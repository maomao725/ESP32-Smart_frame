#include "ble_prov.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* JSON */
#include "cJSON.h"

/* QR code generator (from esp_qrcode component) */
#include "qrcodegen.h"

/* E-paper display */
#include "epaper_port.h"
#include "GUI_Paint.h"

static const char *TAG = "ble_prov";

/* ── BLE state ─────────────────────────────────────────────────────────────── */

static char s_ble_name[32];
static uint16_t s_conn_handle       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_stat_val_handle   = 0;
static ble_prov_credentials_cb_t s_creds_cb  = NULL;
static void    *s_creds_ctx                  = NULL;
static char     s_rx_buf[512];

/* ── Service / characteristic UUIDs ────────────────────────────────────────── *
 * Service:   4FAFC201-1FB5-459E-8FCC-C5C9C331914B
 * WiFi char: BEB5483E-36E1-4688-B7F5-EA07361B26A8  (Write | WriteNoRsp)
 * Stat char: 0617D8B4-E9D0-4FD1-9E7D-1A3BB012F7EB  (Notify)
 * UUIDs stored little-endian per NimBLE convention.
 * ─────────────────────────────────────────────────────────────────────────── */

static const ble_uuid128_t s_svc_uuid = {
    .u.type = BLE_UUID_TYPE_128,
    .value  = {0x4B,0x91,0x31,0xC3, 0xC9,0xC5,0xCC,0x8F,
               0x9E,0x45,0xB5,0x1F, 0x01,0xC2,0xAF,0x4F}
};

static const ble_uuid128_t s_wifi_chr_uuid = {
    .u.type = BLE_UUID_TYPE_128,
    .value  = {0xA8,0x26,0x1B,0x36, 0x07,0xEA,0xF5,0xB7,
               0x88,0x46,0xE1,0x36, 0x3E,0x48,0xB5,0xBE}
};

static const ble_uuid128_t s_stat_chr_uuid = {
    .u.type = BLE_UUID_TYPE_128,
    .value  = {0xEB,0xF7,0x12,0xB0, 0x3B,0x1A,0x7D,0x9E,
               0xD1,0x4F,0xD0,0xE9, 0xB4,0xD8,0x17,0x06}
};

/* ── GATT callbacks ─────────────────────────────────────────────────────────── */

static int wifi_cfg_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t data_len = OS_MBUF_PKTLEN(ctxt->om);
    if (data_len >= sizeof(s_rx_buf)) {
        ESP_LOGE(TAG, "BLE write too large: %u bytes", data_len);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    memset(s_rx_buf, 0, sizeof(s_rx_buf));
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof(s_rx_buf) - 1, &out_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbuf_to_flat failed: %d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_rx_buf[out_len] = '\0';
    ESP_LOGI(TAG, "BLE received: %s", s_rx_buf);

    cJSON *root = cJSON_Parse(s_rx_buf);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON from BLE");
        return BLE_ATT_ERR_UNLIKELY;
    }

    ble_prov_credentials_t creds = {0};
    cJSON *j_ssid      = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass      = cJSON_GetObjectItem(root, "password");
    cJSON *j_mqtt      = cJSON_GetObjectItem(root, "mqtt");
    cJSON *j_mqtt_user = cJSON_GetObjectItem(root, "mqtt_user");
    cJSON *j_mqtt_pass = cJSON_GetObjectItem(root, "mqtt_pass");

    if (!cJSON_IsString(j_ssid) || !cJSON_IsString(j_pass)) {
        ESP_LOGE(TAG, "Missing ssid/password in BLE payload");
        cJSON_Delete(root);
        return BLE_ATT_ERR_UNLIKELY;
    }

    strncpy(creds.ssid,     j_ssid->valuestring, sizeof(creds.ssid) - 1);
    strncpy(creds.password, j_pass->valuestring, sizeof(creds.password) - 1);
    if (cJSON_IsString(j_mqtt))
        strncpy(creds.mqtt_url,  j_mqtt->valuestring,      sizeof(creds.mqtt_url) - 1);
    if (cJSON_IsString(j_mqtt_user))
        strncpy(creds.mqtt_user, j_mqtt_user->valuestring, sizeof(creds.mqtt_user) - 1);
    if (cJSON_IsString(j_mqtt_pass))
        strncpy(creds.mqtt_pass, j_mqtt_pass->valuestring, sizeof(creds.mqtt_pass) - 1);

    cJSON_Delete(root);

    if (s_creds_cb) {
        s_creds_cb(&creds, s_creds_ctx);
    }
    return 0;
}

static int stat_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0; /* notify-only, no read handler needed */
}

/* ── GATT service table ─────────────────────────────────────────────────────── */

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   /* WiFi config — writable by central */
                .uuid       = &s_wifi_chr_uuid.u,
                .access_cb  = wifi_cfg_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   /* Status — notifies central */
                .uuid       = &s_stat_chr_uuid.u,
                .access_cb  = stat_access_cb,
                .val_handle = &s_stat_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ── GAP / advertising ──────────────────────────────────────────────────────── */

static int gap_event_handler(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags                    = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present    = 1;
    fields.tx_pwr_lvl               = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name                     = (uint8_t *)s_ble_name;
    fields.name_len                 = (uint8_t)strlen(s_ble_name);
    fields.name_is_complete         = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_handler, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising: %s", s_ble_name);
    }
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "BLE connected, handle=%u", s_conn_handle);
            } else {
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "BLE disconnected, reason=%d",
                     event->disconnect.reason);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            start_advertising();
            break;

        default:
            break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t ble_prov_start(const char *ble_name,
                          ble_prov_credentials_cb_t cb, void *ctx)
{
    strncpy(s_ble_name, ble_name, sizeof(s_ble_name) - 1);
    s_creds_cb  = cb;
    s_creds_ctx = ctx;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(s_ble_name);

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE prov started, name=%s", s_ble_name);
    return ESP_OK;
}

esp_err_t ble_prov_notify_status(const char *status)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_stat_val_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, strlen(status));
    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_stat_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ble_prov_stop(void)
{
    nimble_port_stop();
    nimble_port_deinit();
    ESP_LOGI(TAG, "BLE prov stopped");
}

/* ── QR code display ────────────────────────────────────────────────────────── */

#define QR_MODULE_PX   8          /* pixels per QR module */
#define QR_QUIET_MODS  4          /* quiet-zone width in modules */
#define QR_BUF_VER     10         /* max QR version to attempt */
#define QR_BUF_LEN     ((((QR_BUF_VER)*4+17)*((QR_BUF_VER)*4+17)+7)/8+1)

void ble_prov_draw_qr_screen(uint8_t *img_buf,
                              const char *device_id,
                              const char *ble_name)
{
    (void)img_buf; /* Paint API uses global Paint state, img_buf already selected */

    /* Build QR payload: {"id":"<device_id>","ble":"<ble_name>"} */
    char qr_text[128];
    snprintf(qr_text, sizeof(qr_text),
             "{\"id\":\"%s\",\"ble\":\"%s\"}", device_id, ble_name);

    /* Allocate QR buffers on heap to avoid stack overflow */
    uint8_t *tmp = (uint8_t *)malloc(QR_BUF_LEN);
    uint8_t *qrc = (uint8_t *)malloc(QR_BUF_LEN);
    if (!tmp || !qrc) {
        ESP_LOGE(TAG, "QR buffer alloc failed");
        free(tmp); free(qrc);
        return;
    }

    bool ok = qrcodegen_encodeText(qr_text, tmp, qrc,
                                   qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, QR_BUF_VER,
                                   qrcodegen_Mask_AUTO, true);
    free(tmp);

    if (!ok) {
        ESP_LOGE(TAG, "QR encode failed");
        free(qrc);
        return;
    }

    int qr_size    = qrcodegen_getSize(qrc); /* modules per side */
    int qr_px      = qr_size * QR_MODULE_PX; /* QR area in pixels */
    int quiet_px   = QR_QUIET_MODS * QR_MODULE_PX;

    /* Center QR on 800×480 screen, shifted slightly up for text below */
    int qr_x0 = (EXAMPLE_LCD_WIDTH  - qr_px) / 2;
    int qr_y0 = 80; /* start y for QR modules */

    int bg_x0 = qr_x0 - quiet_px;
    int bg_y0 = qr_y0 - quiet_px;
    int bg_x1 = qr_x0 + qr_px + quiet_px - 1;
    int bg_y1 = qr_y0 + qr_px + quiet_px - 1;

    /* Clear screen */
    Paint_Clear(EPD_7IN3E_WHITE);

    /* White quiet-zone background */
    Paint_DrawRectangle(bg_x0, bg_y0, bg_x1, bg_y1,
                        EPD_7IN3E_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    /* Draw QR modules */
    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (qrcodegen_getModule(qrc, x, y)) {
                int px = qr_x0 + x * QR_MODULE_PX;
                int py = qr_y0 + y * QR_MODULE_PX;
                Paint_DrawRectangle(px, py,
                                    px + QR_MODULE_PX - 1,
                                    py + QR_MODULE_PX - 1,
                                    EPD_7IN3E_BLACK,
                                    DOT_PIXEL_1X1, DRAW_FILL_FULL);
            }
        }
    }

    free(qrc);

    /* ── Text labels (Font24: ~17px wide × 24px tall) ── */
    int text_y = qr_y0 + qr_px + quiet_px + 16;

    /* Title: center "Smart Photo Frame" */
    const char *title = "Smart Photo Frame";
    int title_x = (EXAMPLE_LCD_WIDTH - (int)strlen(title) * 17) / 2;
    if (title_x < 0) title_x = 4;
    Paint_DrawString_EN(title_x, 20,
                        (char *)title, &Font24,
                        EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);

    /* Subtitle: "Scan QR code to set up WiFi" */
    const char *sub = "Scan QR code to set up WiFi";
    int sub_x = (EXAMPLE_LCD_WIDTH - (int)strlen(sub) * 17) / 2;
    if (sub_x < 0) sub_x = 4;
    Paint_DrawString_EN(sub_x, 48,
                        (char *)sub, &Font24,
                        EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);

    /* BLE name */
    char ble_line[64];
    snprintf(ble_line, sizeof(ble_line), "BT: %s", ble_name);
    int ble_x = (EXAMPLE_LCD_WIDTH - (int)strlen(ble_line) * 17) / 2;
    if (ble_x < 0) ble_x = 4;
    Paint_DrawString_EN(ble_x, text_y,
                        ble_line, &Font24,
                        EPD_7IN3E_BLUE, EPD_7IN3E_WHITE);

    /* Device ID */
    char id_line[48];
    snprintf(id_line, sizeof(id_line), "ID: %s", device_id);
    int id_x = (EXAMPLE_LCD_WIDTH - (int)strlen(id_line) * 17) / 2;
    if (id_x < 0) id_x = 4;
    if (text_y + 32 < EXAMPLE_LCD_HEIGHT) {
        Paint_DrawString_EN(id_x, text_y + 32,
                            id_line, &Font24,
                            EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
    }

    ESP_LOGI(TAG, "QR screen drawn: size=%d modules (%dx%d px), y0=%d",
             qr_size, qr_px, qr_px, qr_y0);
}
