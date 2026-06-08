"""
Smart Photo Frame - Backend Server
FastAPI server that receives photos from WeChat Mini Program
and serves them to ESP32 photo frames.

Run:
    pip install -r requirements.txt
    uvicorn main:app --host 0.0.0.0 --port 8000
"""
import os
import json
import random
import string
import time
from pathlib import Path
from threading import Thread
from typing import Optional

from fastapi import FastAPI, File, UploadFile, HTTPException, Form
from fastapi.responses import FileResponse, JSONResponse
import aiofiles
import paho.mqtt.client as mqtt_lib

from image_processor import convert_to_e6_bmp

app = FastAPI(title="Smart Photo Frame API", version="1.0.0")

# ── MQTT bind config (override via env vars) ─────────────────────────────────
MQTT_BROKER   = os.getenv("MQTT_BROKER_HOST", "localhost")
MQTT_PORT     = int(os.getenv("MQTT_BROKER_PORT", "1883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "")
PUBLIC_BASE_URL = os.getenv("PUBLIC_BASE_URL", "http://47.108.232.40:8000").rstrip("/")

_mqtt_client: Optional[mqtt_lib.Client] = None

# ── Storage layout ──────────────────────────────────────────────────────────
BASE_DIR    = Path(__file__).parent
PHOTOS_DIR  = BASE_DIR / "photos"   # photos/{device_id}/current.bmp
DEVICES_DB  = BASE_DIR / "devices.json"  # simple JSON device registry

PHOTOS_DIR.mkdir(exist_ok=True)


# ── Device registry helpers ──────────────────────────────────────────────────
def _load_devices() -> dict:
    if DEVICES_DB.exists():
        return json.loads(DEVICES_DB.read_text())
    return {}


def _save_devices(db: dict) -> None:
    DEVICES_DB.write_text(json.dumps(db, indent=2, ensure_ascii=False))


def _gen_bind_code() -> str:
    """Generate a 6-digit numeric bind code."""
    return "".join(random.choices(string.digits, k=6))


def _parse_daily_time_to_seconds(value: str) -> int:
    text = value.strip()
    if text.isdigit():
        seconds = int(text)
        if 0 <= seconds < 24 * 3600:
            return seconds
        raise ValueError("seconds must be in [0, 86400)")

    parts = text.split(":")
    if len(parts) not in (2, 3):
        raise ValueError("daily_time must be HH:MM or HH:MM:SS")

    hour = int(parts[0])
    minute = int(parts[1])
    second = int(parts[2]) if len(parts) == 3 else 0
    if not (0 <= hour < 24 and 0 <= minute < 60 and 0 <= second < 60):
        raise ValueError("daily_time is out of range")
    return hour * 3600 + minute * 60 + second


def _format_daily_time(seconds: int) -> str:
    return f"{seconds // 3600:02d}:{(seconds % 3600) // 60:02d}:{seconds % 60:02d}"


def _get_bind_codes_db() -> dict:
    """Load bind codes database."""
    bind_codes_path = BASE_DIR / "bind_codes.json"
    if bind_codes_path.exists():
        return json.loads(bind_codes_path.read_text())
    return {}


def _save_bind_codes_db(db: dict) -> None:
    """Save bind codes database."""
    bind_codes_path = BASE_DIR / "bind_codes.json"
    bind_codes_path.write_text(json.dumps(db, indent=2, ensure_ascii=False))


def _mqtt_publish(topic: str, payload: dict, retain: bool = False) -> bool:
    if _mqtt_client is None:
        print(f"[MQTT] skip publish, client unavailable: topic={topic}")
        return False

    try:
        message = json.dumps(payload, ensure_ascii=False)
        result = _mqtt_client.publish(topic, message, qos=1, retain=retain)
        if result.rc != mqtt_lib.MQTT_ERR_SUCCESS:
            print(f"[MQTT] publish failed: topic={topic} rc={result.rc}")
            return False
        print(f"[MQTT] published: topic={topic} payload={message}")
        return True
    except Exception as exc:
        print(f"[MQTT] publish error: topic={topic} exc={exc}")
        return False


def _publish_device_bound(device_id: str) -> None:
    topic = f"device/{device_id}/bound"
    _mqtt_publish(
        topic,
        {
            "event": "device_bound",
            "device_uid": device_id,
            "timestamp": int(time.time()),
        },
        retain=True,
    )


def _publish_photo_refresh(device_id: str) -> None:
    topic = f"device/{device_id}/image"
    _mqtt_publish(
        topic,
        {
            "url": f"{PUBLIC_BASE_URL}/api/photo/{device_id}/latest.bmp",
            "device_uid": device_id,
            "timestamp": int(time.time()),
        },
    )


# ── MQTT bind handler ────────────────────────────────────────────────────────

def _on_mqtt_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        client.subscribe("smartframe/bind/request", qos=1)
        client.subscribe("device/+/bound", qos=1)
        print("[MQTT] connected, subscribed to smartframe/bind/request and device/+/bound")
    else:
        print(f"[MQTT] connect failed, rc={rc}")


def _on_bind_request(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        device_id = payload.get("device_id", "").strip()
        if not device_id:
            return

        db = _load_devices()
        if device_id not in db:
            db[device_id] = {
                "bind_code": _gen_bind_code(),
                "owner_openid": None,
                "registered_at": int(time.time()),
                "last_seen": int(time.time()),
            }
            (PHOTOS_DIR / device_id).mkdir(exist_ok=True)
            print(f"[MQTT] registered new device: {device_id}")
        else:
            db[device_id]["last_seen"] = int(time.time())

        _save_devices(db)

        ack_topic   = f"smartframe/bind/ack/{device_id}"
        ack_payload = json.dumps({"status": "ok", "device_id": device_id})
        client.publish(ack_topic, ack_payload, qos=1)
        print(f"[MQTT] bind ack sent → {ack_topic}")

    except Exception as exc:
        print(f"[MQTT] bind/request error: {exc}")


def _on_device_bound_topic(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        event = str(payload.get("event", "")).strip()
        device_id = str(payload.get("device_uid", "")).strip()

        if not device_id:
            topic_parts = msg.topic.split("/")
            if len(topic_parts) >= 3:
                device_id = topic_parts[1].strip()

        if not device_id:
            print(f"[MQTT] ignore bound topic without device_id: topic={msg.topic}")
            return

        if event != "reg_new_device":
            print(f"[MQTT] ignore bound topic event={event} for device={device_id}")
            return

        db = _load_devices()
        info = db.get(device_id)
        if not info:
            print(f"[MQTT] reg_new_device received for unknown device: {device_id}")
            return

        info["last_seen"] = int(time.time())
        _save_devices(db)

        if info.get("owner_openid"):
            print(f"[MQTT] device already bound, publishing device_bound for {device_id}")
            _publish_device_bound(device_id)
            return

        print(f"[MQTT] device not bound yet, waiting for /api/bind: {device_id}")
    except Exception as exc:
        print(f"[MQTT] device bound topic error: {exc}")


def _mqtt_thread():
    global _mqtt_client
    try:
        client = mqtt_lib.Client(
            client_id="smart-frame-server",
            callback_api_version=mqtt_lib.CallbackAPIVersion.VERSION2,
        )
        if MQTT_USERNAME:
            client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        client.on_connect = _on_mqtt_connect
        client.message_callback_add("smartframe/bind/request", _on_bind_request)
        client.message_callback_add("device/+/bound", _on_device_bound_topic)
        client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        _mqtt_client = client
        client.loop_forever()
    except Exception as exc:
        print(f"[MQTT] thread error: {exc}")


@app.on_event("startup")
async def _startup():
    Thread(target=_mqtt_thread, daemon=True, name="mqtt-bind").start()


# ── Routes ───────────────────────────────────────────────────────────────────

@app.get("/")
def root():
    return {"service": "Smart Photo Frame", "status": "ok"}


@app.post("/api/register")
def register_device(device_id: str = Form(...)):
    """
    Register a new frame device and return a bind code.
    Called by the ESP32 on first boot (or via AP config page).
    """
    db = _load_devices()
    if device_id not in db:
        db[device_id] = {
            "bind_code": _gen_bind_code(),
            "owner_openid": None,
            "registered_at": int(time.time()),
            "last_seen": None,
        }
        _save_devices(db)
        # Create photo directory for this device
        (PHOTOS_DIR / device_id).mkdir(exist_ok=True)

    return {"device_id": device_id, "bind_code": db[device_id]["bind_code"]}


@app.post("/api/devices/{device_id}/bind-code")
def request_bind_code(device_id: str):
    """
    Request a new dynamic bind code for SoftAP provisioning.
    Called by ESP32 after successful Wi-Fi connection.

    Returns:
        - bind_code: 6-digit numeric code
        - expires_at: Unix timestamp when code expires (5 minutes)
    """
    db = _load_devices()
    bind_codes_db = _get_bind_codes_db()

    # Generate new bind code
    bind_code = _gen_bind_code()
    expires_at = int(time.time()) + 300  # 5 minutes

    # Save to bind codes database
    bind_codes_db[bind_code] = {
        "device_id": device_id,
        "status": "pending",
        "created_at": int(time.time()),
        "expires_at": expires_at,
    }
    _save_bind_codes_db(bind_codes_db)

    # Also update main devices database for compatibility
    if device_id not in db:
        db[device_id] = {
            "bind_code": bind_code,
            "owner_openid": None,
            "registered_at": int(time.time()),
            "last_seen": int(time.time()),
        }
        (PHOTOS_DIR / device_id).mkdir(exist_ok=True)
    else:
        db[device_id]["bind_code"] = bind_code
        db[device_id]["last_seen"] = int(time.time())

    _save_devices(db)

    return {
        "bind_code": bind_code,
        "expires_at": expires_at,
        "device_id": device_id
    }


@app.post("/api/bind")
def bind_device(bind_code: str = Form(...), openid: str = Form(...)):
    """
    Bind a frame to a WeChat user via bind code.
    Mini Program calls this after user enters the 6-digit code shown on frame.
    """
    db = _load_devices()
    bind_codes_db = _get_bind_codes_db()

    # First try to find in bind codes database (new method)
    if bind_code in bind_codes_db:
        bind_info = bind_codes_db[bind_code]

        # Check if code is expired
        if bind_info["status"] != "pending" or time.time() > bind_info["expires_at"]:
            raise HTTPException(status_code=400, detail="Bind code expired or already used")

        device_id = bind_info["device_id"]

        # Update bind code status
        bind_info["status"] = "used"
        bind_info["used_at"] = int(time.time())
        bind_info["user_id"] = openid
        _save_bind_codes_db(bind_codes_db)

        # Update main devices database
        if device_id in db:
            db[device_id]["owner_openid"] = openid
            _save_devices(db)
            _publish_device_bound(device_id)

        return {"success": True, "device_id": device_id}

    # Fallback to old method (backward compatibility)
    for device_id, info in db.items():
        if info.get("bind_code") == bind_code.upper():
            info["owner_openid"] = openid
            _save_devices(db)
            _publish_device_bound(device_id)
            return {"success": True, "device_id": device_id}

    raise HTTPException(status_code=404, detail="Bind code not found")


@app.post("/api/upload")
async def upload_photo(
    openid: str = Form(...),
    file: UploadFile = File(...),
):
    """
    Receive a photo from WeChat Mini Program.
    Converts to 800x480 6-color BMP and saves for the bound frame.

    Form fields:
        openid  - WeChat user openid (from wx.login)
        file    - image file (JPEG/PNG)
    """
    db = _load_devices()

    # Find device bound to this user
    device_id: Optional[str] = None
    for did, info in db.items():
        if info.get("owner_openid") == openid:
            device_id = did
            break

    if not device_id:
        raise HTTPException(status_code=403, detail="No frame bound to this user")

    # Read and convert image
    raw = await file.read()
    try:
        bmp_bytes = convert_to_e6_bmp(raw)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Image conversion failed: {e}")

    # Save atomically: write to .tmp then rename
    device_dir = PHOTOS_DIR / device_id
    device_dir.mkdir(exist_ok=True)
    tmp_path     = device_dir / "incoming.bmp"
    current_path = device_dir / "current.bmp"

    async with aiofiles.open(tmp_path, "wb") as f:
        await f.write(bmp_bytes)
    tmp_path.rename(current_path)

    # Update last_seen timestamp
    db[device_id]["last_seen"] = int(time.time())
    _save_devices(db)
    _publish_photo_refresh(device_id)

    return {"success": True, "device_id": device_id, "size": len(bmp_bytes)}


@app.get("/api/photo/{device_id}/latest.bmp")
def get_latest_photo(device_id: str):
    """
    ESP32 polls this endpoint every N seconds.
    Returns the current BMP file, or 404 if no photo yet.
    """
    photo_path = PHOTOS_DIR / device_id / "current.bmp"
    if not photo_path.exists():
        raise HTTPException(status_code=404, detail="No photo available")

    # Update heartbeat
    db = _load_devices()
    if device_id in db:
        db[device_id]["last_seen"] = int(time.time())
        _save_devices(db)

    return FileResponse(
        path=str(photo_path),
        media_type="image/bmp",
        filename="current.bmp",
    )


@app.post("/api/devices/{device_id}/local-schedule")
def set_local_photo_schedule(device_id: str, daily_time: str = Form(...)):
    """
    Set local timed mode to switch photos once per day at HH:MM[:SS].
    Publishes the setting to device/{device_id}/image via MQTT.
    """
    db = _load_devices()
    if device_id not in db:
        raise HTTPException(status_code=404, detail="Device not found")

    try:
        seconds = _parse_daily_time_to_seconds(daily_time)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc))

    formatted = _format_daily_time(seconds)
    db[device_id]["local_daily_time"] = formatted
    db[device_id]["updated_at"] = int(time.time())
    _save_devices(db)

    ok = _mqtt_publish(
        f"device/{device_id}/image",
        {
            "daily_time": formatted,
            "device_uid": device_id,
            "timestamp": int(time.time()),
        },
        retain=True,
    )
    if not ok:
        raise HTTPException(status_code=503, detail="MQTT client unavailable")

    return {"success": True, "device_id": device_id, "daily_time": formatted}


@app.get("/api/status/{device_id}")
def get_device_status(device_id: str):
    """
    Check if a frame is online (last seen within 2 minutes).
    Mini Program can poll this to show "奶奶的相框在线" status.
    """
    db = _load_devices()
    if device_id not in db:
        raise HTTPException(status_code=404, detail="Device not found")

    info      = db[device_id]
    last_seen = info.get("last_seen")
    online    = last_seen is not None and (time.time() - last_seen) < 120

    return {
        "device_id": device_id,
        "online": online,
        "last_seen": last_seen,
        "bound": info.get("owner_openid") is not None,
        "local_daily_time": info.get("local_daily_time"),
    }
