#include "frame_config.h"
#include "sdcard_bsp.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "frame_config";

frame_config_t *frame_config_load(void) {
    uint8_t buf[1024] = {0};
    size_t  out_len   = 0;

    if (sdcard_read_file(FRAME_CONFIG_PATH, buf, &out_len) != ESP_OK) {
        ESP_LOGW(TAG, "config.txt not found, entering AP config mode");
        return NULL;
    }

    cJSON *root = cJSON_Parse((const char *)buf);
    if (!root) {
        ESP_LOGE(TAG, "config.txt JSON parse failed");
        return NULL;
    }

    frame_config_t *cfg = (frame_config_t *)calloc(1, sizeof(frame_config_t));
    if (!cfg) { cJSON_Delete(root); return NULL; }

    /* Required fields for basic WiFi testing */
    cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "wifi_password");
    cJSON *url  = cJSON_GetObjectItem(root, "server_url");
    cJSON *did  = cJSON_GetObjectItem(root, "device_id");

    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        ESP_LOGE(TAG, "config.txt missing required WiFi fields");
        free(cfg);
        cJSON_Delete(root);
        return NULL;
    }

    strncpy(cfg->wifi_ssid,     ssid->valuestring, sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_password, pass->valuestring, sizeof(cfg->wifi_password) - 1);
    if (cJSON_IsString(url)) {
        strncpy(cfg->server_url, url->valuestring, sizeof(cfg->server_url) - 1);
    }
    if (cJSON_IsString(did)) {
        strncpy(cfg->device_id, did->valuestring, sizeof(cfg->device_id) - 1);
    }

    /* Optional: poll_interval (default 30s) */
    cJSON *interval = cJSON_GetObjectItem(root, "poll_interval");
    cfg->poll_interval = cJSON_IsNumber(interval) ? (int)interval->valuedouble : 30;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config loaded: ssid=%s server=%s device=%s interval=%ds",
             cfg->wifi_ssid,
             cfg->server_url[0] ? "configured" : "not set",
             cfg->device_id[0] ? cfg->device_id : "(not set)",
             cfg->poll_interval);
    return cfg;
}
