# 智能相框 Smart Photo Frame

面向养老院老人的远程照片相框。家人通过微信小程序发送照片，相框实时显示。

---

## 系统架构

```
微信小程序（家人手机）
       │
       │  POST /api/upload  (multipart, JPEG/PNG)
       ▼
腾讯云服务器  (Python FastAPI)
  ├── 图片转换：任意格式 → 800×480 6色BMP (Pillow)
  ├── 设备绑定表：devices.json
  └── 图片存储：photos/{device_id}/current.bmp
       │
       │  GET /api/photo/{device_id}/latest.bmp  (每30秒轮询)
       ▼
ESP32-S3-PhotoPainter（相框）
  ├── WiFi STA 连接养老院网络
  ├── HTTP 下载 BMP → 写入 SD 卡
  └── GUI_ReadBmp_RGB_6Color → epaper_port_display
       │
       ▼
7.3寸 E6 全彩电子墨水屏（800×480，6色）
```

---

## 目录结构

```
smart-frame/
├── firmware/                  # ESP32 固件 (ESP-IDF)
│   ├── CMakeLists.txt
│   ├── main/
│   │   ├── main.c             # 主程序入口
│   │   ├── frame_config.c/h   # 读取 SD 卡 config.txt
│   │   ├── wifi_sta.c/h       # WiFi STA 连接
│   │   └── photo_client.c/h   # HTTP 图片下载（在 components/photo_client/）
│   └── components/            # 复用自微雪原版 SDK
│       ├── epaper_src/        # 电子墨水屏驱动 + BMP 解码
│       ├── epaper_port/       # SPI 硬件抽象层
│       ├── sdcard_bsp/        # SD 卡读写
│       ├── photo_client/      # 图片下载组件（新增）
│       ├── http_server_bsp/   # AP 配网 HTTP 服务器（复用）
│       ├── json_bsp/          # ArduinoJson v7
│       └── ListLib/           # 链表库
├── server/                    # 后端服务器
│   ├── main.py                # FastAPI 应用
│   ├── image_processor.py     # 图片转换（Pillow）
│   └── requirements.txt
├── miniprogram/               # 微信小程序
│   ├── app.js / app.json
│   ├── pages/index/           # 发送照片页
│   └── pages/bind/            # 绑定相框页
└── docs/
    └── README.md              # 本文档
```

---

## 硬件规格

| 参数 | 值 |
|------|----|
| 主控 | ESP32-S3（双核 LX7，240MHz） |
| 屏幕 | 7.3寸 E6 全彩电子墨水屏（Spectra 6） |
| 分辨率 | 800 × 480 |
| 显示色彩 | 6色：黑 / 白 / 绿 / 蓝 / 红 / 黄 |
| 无线 | 2.4GHz Wi-Fi + BLE 5 |
| 存储 | MicroSD 卡（4-bit SDMMC） |
| 外壳 | 实木相框 |

### SPI 引脚（电子墨水屏）

| 信号 | GPIO |
|------|------|
| DC   | 8    |
| CS   | 9    |
| SCK  | 10   |
| MOSI | 11   |
| RST  | 12   |
| BUSY | 13   |

### SDMMC 引脚

| 信号 | GPIO |
|------|------|
| CLK  | 39   |
| CMD  | 41   |
| D0   | 40   |
| D1   | 1    |
| D2   | 2    |
| D3   | 38   |

---

## SD 卡文件结构

```
/sdcard/
├── config.txt        # 设备配置（JSON）
├── current.bmp       # 当前显示的照片（800×480，6色BMP）
└── incoming.bmp      # 下载临时文件（下载完成后重命名）
```

### config.txt 格式

```json
{
  "wifi_ssid":     "养老院WiFi名称",
  "wifi_password": "WiFi密码",
  "server_url":    "http://1.2.3.4:8000/api/photo",
  "device_id":     "frame_001",
  "poll_interval": 30
}
```

| 字段 | 说明 | 默认值 |
|------|------|--------|
| wifi_ssid | 养老院 WiFi SSID | 必填 |
| wifi_password | WiFi 密码 | 必填 |
| server_url | 服务器图片接口基础 URL | 必填 |
| device_id | 相框唯一标识符 | 必填 |
| poll_interval | 轮询间隔（秒） | 30 |

---

## MQTT 约定

启用 MQTT 模式时，固件当前默认使用以下 topic：

| 用途 | Topic | 说明 |
|------|-------|------|
| 图片下发订阅 | `device/<DEVICE_UID>/image` | 设备上线后订阅，用于接收图片刷新消息 |
| 设备入网上报 | `device/<DEVICE_UID>/bound` | MQTT 连接成功后立即上报一次新设备注册消息 |

### 设备入网上报 payload

设备会向 `device/<DEVICE_UID>/bound` 发布以下 JSON 结构：

```json
{
  "event": "reg_new_device",
  "device_uid": "AC12EF34AB56CD78",
  "timestamp": 1776530000
}
```

说明：

- `event` 固定为 `reg_new_device`
- `device_uid` 使用设备当前 `device_id`
- `timestamp` 优先使用设备系统时间；若设备尚未完成授时，则回退为开机秒数，确保始终为数字

### 云端下发动态绑定码

设备也会订阅同一个 topic：`device/<DEVICE_UID>/bound`

当收到以下 JSON 时，设备会：

- 仅在 `event = dyn_bound_code` 时处理
- 校验 `device_uid` 必须和当前设备一致
- 将绑定码写回 `config.txt`
- 在墨水屏上显示绑定码与等待绑定状态

```json
{
  "event": "dyn_bound_code",
  "device_uid": "AC12EF34AB56CD78",
  "dyn_bound_code": "483726",
  "expires_in": 300,
  "timestamp": 1776530000
}
```

设备会忽略以下消息：

- `event = reg_new_device` 的自发上行回环消息
- `device_uid` 与当前设备不一致的消息
- 缺少 `dyn_bound_code` / `expires_in` / `timestamp` 的非法消息

---

## 图片格式规范

| 参数 | 要求 |
|------|------|
| 分辨率 | 800 × 480 像素 |
| 格式 | BMP，无压缩 |
| 色彩模式 | 6色调色板（Floyd-Steinberg 抖动） |
| 文件大小 | 约 192KB |

### 6色调色板定义

| 索引 | 颜色 | RGB 值 |
|------|------|--------|
| 0 | 黑 | (0, 0, 0) |
| 1 | 白 | (255, 255, 255) |
| 2 | 黄 | (255, 255, 0) |
| 3 | 红 | (255, 0, 0) |
| 5 | 蓝 | (0, 0, 255) |
| 6 | 绿 | (0, 128, 0) |

---

## API 文档

### POST `/api/register`

注册相框设备，返回绑定码。

**请求（form-data）：**
```
device_id: frame_001
```

**响应：**
```json
{ "device_id": "frame_001", "bind_code": "A3F7K2" }
```

---

### POST `/api/bind`

家人通过小程序输入绑定码，将相框与微信账号绑定。

**请求（form-data）：**
```
bind_code: A3F7K2
openid:    wx_user_openid_xxx
```

**响应：**
```json
{ "success": true, "device_id": "frame_001" }
```

---

### POST `/api/upload`

微信小程序上传照片。服务器自动转换为 800×480 6色 BMP。

**请求（multipart/form-data）：**
```
openid: wx_user_openid_xxx
file:   <图片文件，支持 JPEG/PNG/HEIC 等>
```

**响应：**
```json
{ "success": true, "device_id": "frame_001", "size": 196662 }
```

---

### GET `/api/photo/{device_id}/latest.bmp`

ESP32 轮询此接口下载最新照片。

**响应：**
- `200 OK`：返回 BMP 文件（`Content-Type: image/bmp`）
- `304 Not Modified`：无新照片（暂未实现 ETag，当前始终返回 200）
- `404 Not Found`：该设备尚无照片

---

### GET `/api/status/{device_id}`

查询相框在线状态（最近 2 分钟内有轮询则视为在线）。

**响应：**
```json
{
  "device_id": "frame_001",
  "online":    true,
  "last_seen": 1713100800,
  "bound":     true
}
```

---

## 部署说明

### 服务器部署（腾讯云）

```bash
# 1. 安装依赖
pip install -r requirements.txt

# 2. 启动服务（生产环境建议用 systemd 或 supervisor 管理）
uvicorn main:app --host 0.0.0.0 --port 8000

# 3. 开放腾讯云安全组端口 8000（TCP 入站）
```

### ESP32 固件编译

```bash
# 环境要求：ESP-IDF v5.x
cd smart-frame/firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM_PORT flash monitor
```

### SD 卡初始化

1. 格式化为 FAT32
2. 创建 `config.txt`（参考上方格式）
3. 插入相框

### 首次配网（无 config.txt 时）

1. 相框开机自动进入 AP 模式
2. 手机连接热点：`esp_network`（密码：`1234567890`）
3. 浏览器访问 `http://192.168.4.1/index.html`
4. 上传填写好的 `config.txt`
5. 重启相框

### 微信小程序配置

1. 在 `miniprogram/app.js` 中修改 `serverUrl` 为腾讯云服务器 IP
2. 在微信开发者工具中填写 AppID
3. 在小程序后台配置合法域名（服务器域名）
4. 上传发布

---

## 工作流程

```
首次使用：
  相框开机 → 无 config.txt → AP 热点配网 → 写入 config.txt → 重启
  → 连接 WiFi → 注册设备 → 屏幕显示绑定码
  → 家人打开小程序 → 输入绑定码 → 绑定成功

日常使用：
  家人打开小程序 → 选择照片 → 点击发送
  → 服务器转换图片 → 保存 BMP
  → 相框每30秒轮询 → 下载新 BMP → 刷新屏幕（约15秒刷新时间）
  → 奶奶看到照片 ❤️
```

---

## 关键代码位置

| 功能 | 文件 |
|------|------|
| 主程序入口 | `firmware/main/main.c` |
| WiFi 连接 | `firmware/main/wifi_sta.c` |
| 配置读取 | `firmware/main/frame_config.c` |
| 图片下载 | `firmware/components/photo_client/photo_client.c` |
| 电子墨水屏驱动 | `firmware/components/epaper_port/epaper_port.c` |
| BMP 解码显示 | `firmware/components/epaper_src/GUI_BMPfile.c` |
| 图片转换（服务器） | `server/image_processor.py` |
| API 接口（服务器） | `server/main.py` |
| 发送照片（小程序） | `miniprogram/pages/index/index.js` |
| 绑定相框（小程序） | `miniprogram/pages/bind/bind.js` |

---

## 注意事项

- 电子墨水屏每次刷新约需 **15秒**，属正常现象
- 图片建议使用色彩鲜明的照片，避免大面积渐变（6色限制）
- 相框需保持 WiFi 连接，断网后无法接收新照片
- 服务器 `devices.json` 为简单文件存储，生产环境建议替换为 SQLite 或 MySQL
