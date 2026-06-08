#include "softap_prov.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "epaper_port.h"
#include "GUI_Paint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "softap_prov";

#define PROV_CREDENTIALS_BIT BIT0
#define MAX_AP_LIST 20
#define CONNECT_RESPONSE_SETTLE_MS 800

typedef struct {
    wifi_ap_record_t ap_records[MAX_AP_LIST];
    uint16_t ap_count;
    char ap_ip[16];
    softap_state_t state;
    EventGroupHandle_t evt_grp;
    softap_prov_credentials_cb_t cred_cb;
    void *cred_ctx;
    httpd_handle_t server;
    esp_netif_t *netif;
} softap_prov_ctx_t;

static softap_prov_ctx_t *s_prov_ctx = NULL;

static void update_state(softap_state_t new_state)
{
    if (s_prov_ctx) {
        s_prov_ctx->state = new_state;
        ESP_LOGI(TAG, "State changed: %d", new_state);
    }
}

static void copy_json_string(char *dst, size_t dst_size, const cJSON *item)
{
    if (!dst || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (!cJSON_IsString(item) || !item->valuestring) {
        return;
    }

    strncpy(dst, item->valuestring, dst_size - 1);
}

static bool should_use_custom_mqtt(const cJSON *root)
{
    const cJSON *custom_flag = cJSON_GetObjectItemCaseSensitive(root, "use_custom_mqtt");
    const cJSON *legacy_flag = cJSON_GetObjectItemCaseSensitive(root, "use_mqtt");
    const cJSON *mqtt_url = cJSON_GetObjectItemCaseSensitive(root, "mqtt_url");

    if (cJSON_IsBool(custom_flag)) {
        return cJSON_IsTrue(custom_flag);
    }
    if (cJSON_IsBool(legacy_flag)) {
        return cJSON_IsTrue(legacy_flag);
    }

    return cJSON_IsString(mqtt_url)
        && mqtt_url->valuestring
        && mqtt_url->valuestring[0] != '\0';
}

static esp_err_t scan_wifi_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!s_prov_ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Provisioning not started");
        return ESP_FAIL;
    }

    update_state(SOFTAP_STATE_SCANNING);

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false
    };

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Get WiFi mode failed: %s", esp_err_to_name(ret));
        update_state(SOFTAP_STATE_IDLE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Get WiFi mode failed");
        return ret;
    }

    if (mode == WIFI_MODE_AP) {
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Switch to APSTA for scan failed: %s", esp_err_to_name(ret));
            update_state(SOFTAP_STATE_IDLE);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Enable scan mode failed");
            return ret;
        }
        ESP_LOGI(TAG, "Switched WiFi mode to APSTA for scanning");
    } else if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        ESP_LOGE(TAG, "WiFi mode %d does not support scanning", mode);
        update_state(SOFTAP_STATE_IDLE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi mode does not support scanning");
        return ESP_FAIL;
    }

    ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        update_state(SOFTAP_STATE_IDLE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ret;
    }

    s_prov_ctx->ap_count = MAX_AP_LIST;
    ret = esp_wifi_scan_get_ap_records(&s_prov_ctx->ap_count, s_prov_ctx->ap_records);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Get scan results failed: %s", esp_err_to_name(ret));
        update_state(SOFTAP_STATE_IDLE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Get scan results failed");
        return ret;
    }

    cJSON *root = cJSON_CreateArray();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON create failed");
        return ESP_ERR_NO_MEM;
    }

    for (uint16_t i = 0; i < s_prov_ctx->ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)s_prov_ctx->ap_records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", s_prov_ctx->ap_records[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", s_prov_ctx->ap_records[i].primary);
        cJSON_AddNumberToObject(ap, "auth", s_prov_ctx->ap_records[i].authmode);
        cJSON_AddItemToArray(root, ap);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    update_state(SOFTAP_STATE_IDLE);
    return ESP_OK;
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory alloc failed");
        return ESP_ERR_NO_MEM;
    }

    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Connection error");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *password = cJSON_GetObjectItem(root, "password");
    cJSON *mqtt_url = cJSON_GetObjectItem(root, "mqtt_url");
    cJSON *mqtt_user = cJSON_GetObjectItem(root, "mqtt_user");
    cJSON *mqtt_pass = cJSON_GetObjectItem(root, "mqtt_pass");

    if (!cJSON_IsString(ssid)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    if (s_prov_ctx && s_prov_ctx->cred_cb) {
        softap_prov_credentials_t creds = {0};
        strncpy(creds.ssid, ssid->valuestring, sizeof(creds.ssid) - 1);
        if (cJSON_IsString(password)) {
            strncpy(creds.password, password->valuestring, sizeof(creds.password) - 1);
        }
        creds.use_custom_mqtt = should_use_custom_mqtt(root);
        if (creds.use_custom_mqtt) {
            copy_json_string(creds.mqtt_url, sizeof(creds.mqtt_url), mqtt_url);
            copy_json_string(creds.mqtt_username, sizeof(creds.mqtt_username), mqtt_user);
            copy_json_string(creds.mqtt_password, sizeof(creds.mqtt_password), mqtt_pass);

            if (creds.mqtt_url[0] == '\0') {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing MQTT broker URL");
                return ESP_FAIL;
            }
        }

        cJSON_Delete(root);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "message", "Credentials received, connecting...");
        char *resp_str = cJSON_PrintUnformatted(resp);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp_str, strlen(resp_str));
        free(resp_str);
        cJSON_Delete(resp);

        /*
         * Keep SoftAP alive briefly after replying. Otherwise the browser often
         * reports "Failed to fetch" while the device switches to STA mode.
         */
        vTaskDelay(pdMS_TO_TICKS(CONNECT_RESPONSE_SETTLE_MS));
        update_state(SOFTAP_STATE_CONNECTING);
        s_prov_ctx->cred_cb(&creds, s_prov_ctx->cred_ctx);
        xEventGroupSetBits(s_prov_ctx->evt_grp, PROV_CREDENTIALS_BIT);

        return ESP_OK;
    }

    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Provisioning not ready");
    return ESP_FAIL;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!s_prov_ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Provisioning not started");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", "provisioning");
    cJSON_AddStringToObject(root, "ap_ip", s_prov_ctx->ap_ip);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

static httpd_uri_t uri_handlers[] = {
    {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    },
    {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = scan_wifi_handler,
        .user_ctx = NULL
    },
    {
        .uri = "/api/connect",
        .method = HTTP_POST,
        .handler = connect_handler,
        .user_ctx = NULL
    },
    {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    }
};

esp_err_t softap_prov_start(const char *ap_name,
                            const char *ap_password,
                            softap_prov_credentials_cb_t cb,
                            void *ctx)
{
    if (s_prov_ctx) {
        ESP_LOGE(TAG, "Provisioning already started");
        return ESP_ERR_INVALID_STATE;
    }

    s_prov_ctx = calloc(1, sizeof(softap_prov_ctx_t));
    if (!s_prov_ctx) {
        return ESP_ERR_NO_MEM;
    }

    s_prov_ctx->cred_cb = cb;
    s_prov_ctx->cred_ctx = ctx;
    s_prov_ctx->evt_grp = xEventGroupCreate();
    if (!s_prov_ctx->evt_grp) {
        free(s_prov_ctx);
        s_prov_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    s_prov_ctx->netif = esp_netif_create_default_wifi_ap();
    if (!s_prov_ctx->netif) {
        ESP_LOGE(TAG, "Create AP netif failed");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ap_name),
            .channel = 1,
            .max_connection = 4,
            .authmode = (ap_password && ap_password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .beacon_interval = 100,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, ap_name, sizeof(wifi_config.ap.ssid) - 1);
    if (ap_password && ap_password[0]) {
        strncpy((char *)wifi_config.ap.password, ap_password, sizeof(wifi_config.ap.password) - 1);
    }

    ESP_LOGI(TAG, "Starting SoftAP: SSID=%s channel=%d auth=%d",
             ap_name, wifi_config.ap.channel, wifi_config.ap.authmode);

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set mode failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set AP config failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_prov_ctx->netif, &ip_info);
    snprintf(s_prov_ctx->ap_ip, sizeof(s_prov_ctx->ap_ip),
             IPSTR, IP2STR(&ip_info.ip));

    ESP_LOGI(TAG, "SoftAP started: SSID=%s IP=%s", ap_name, s_prov_ctx->ap_ip);

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = 80;
    http_config.lru_purge_enable = true;

    ret = httpd_start(&s_prov_ctx->server, &http_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    for (size_t i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); i++) {
        ret = httpd_register_uri_handler(s_prov_ctx->server, &uri_handlers[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Register URI handler failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }
    }

    update_state(SOFTAP_STATE_IDLE);
    ESP_LOGI(TAG, "SoftAP provisioning started successfully");
    return ESP_OK;

cleanup:
    if (s_prov_ctx) {
        if (s_prov_ctx->evt_grp) vEventGroupDelete(s_prov_ctx->evt_grp);
        if (s_prov_ctx->server) httpd_stop(s_prov_ctx->server);
        if (s_prov_ctx->netif) esp_netif_destroy(s_prov_ctx->netif);
        esp_wifi_stop();
        esp_wifi_deinit();
        free(s_prov_ctx);
        s_prov_ctx = NULL;
    }
    return ret;
}

softap_state_t softap_prov_get_state(void)
{
    return s_prov_ctx ? s_prov_ctx->state : SOFTAP_STATE_IDLE;
}

const char *softap_prov_get_ap_ip(void)
{
    return s_prov_ctx ? s_prov_ctx->ap_ip : NULL;
}

static void draw_cn_line(UWORD y, const char *text)
{
    if (text && text[0]) {
        Paint_DrawString_CN(50, y, text, &Font24CN, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
    }
}

static void draw_en_line(UWORD y, const char *text)
{
    if (text && text[0]) {
        Paint_DrawString_EN(50, y, text, &Font24, EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
    }
}

void softap_prov_stop(void)
{
    if (!s_prov_ctx) return;

    if (s_prov_ctx->server) {
        httpd_stop(s_prov_ctx->server);
        s_prov_ctx->server = NULL;
    }

    if (s_prov_ctx->evt_grp) {
        vEventGroupDelete(s_prov_ctx->evt_grp);
        s_prov_ctx->evt_grp = NULL;
    }

    if (s_prov_ctx->netif) {
        esp_netif_destroy(s_prov_ctx->netif);
        s_prov_ctx->netif = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    free(s_prov_ctx);
    s_prov_ctx = NULL;

    ESP_LOGI(TAG, "SoftAP provisioning stopped");
}

void softap_prov_draw_screen(uint8_t *img_buf,
                              const char *device_id,
                              const char *ap_name,
                              const char *ap_ip,
                              const char *bind_code,
                              softap_state_t state)
{
    if (!img_buf) return;
    Paint_SelectImage(img_buf);

    Paint_Clear(EPD_7IN3E_WHITE);

    switch (state) {
        case SOFTAP_STATE_IDLE:
        case SOFTAP_STATE_SCANNING:
            draw_cn_line(100, "连接相框热点");
            draw_en_line(150, ap_name);
            draw_cn_line(200, "浏览器打开地址");
            draw_en_line(250, ap_ip);
            break;

        case SOFTAP_STATE_CONNECTING:
            draw_cn_line(100, "正在连接无线");
            draw_cn_line(150, "请稍等");
            break;

        case SOFTAP_STATE_GETTING_BIND_CODE:
            draw_cn_line(100, "无线已连接");
            draw_cn_line(150, "正在获取绑定码");
            break;

        case SOFTAP_STATE_CONNECTED:
            if (bind_code && bind_code[0]) {
                draw_cn_line(100, "无线已连接");
                draw_cn_line(150, "绑定码");
                draw_en_line(200, bind_code);
                draw_cn_line(250, "五分钟内有效");
                draw_en_line(300, device_id);
            } else {
                draw_cn_line(100, "无线已连接");
                draw_cn_line(150, "正在获取绑定码");
            }
            break;

        case SOFTAP_STATE_WAITING_BIND:
            if (bind_code && bind_code[0]) {
                draw_cn_line(100, "请输入绑定码");
                draw_en_line(150, bind_code);
                draw_cn_line(200, "五分钟内有效");
                draw_en_line(250, device_id);
            } else {
                draw_cn_line(100, "无线已连接");
                draw_cn_line(150, "等待绑定");
            }
            break;

        case SOFTAP_STATE_BOUND:
            draw_cn_line(100, "绑定成功");
            draw_cn_line(150, "设备");
            draw_en_line(200, device_id);
            break;

        case SOFTAP_STATE_FAILED:
            draw_cn_line(100, "无线连接失败");
            draw_cn_line(150, "请检查密码");
            draw_cn_line(200, "即将重启");
            break;

        default:
            draw_cn_line(100, "配网中");
            break;
    }
}
