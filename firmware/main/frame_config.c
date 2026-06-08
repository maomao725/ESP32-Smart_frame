#include "frame_config.h"
#include "sdcard_bsp.h"
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "frame_config";

static char s_efuse_id_buf[32];

const char *frame_config_device_id_from_efuse(void)
{
    uint8_t uid[8] = {0};
    esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 64);
    snprintf(s_efuse_id_buf, sizeof(s_efuse_id_buf),
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             uid[0], uid[1], uid[2], uid[3],
             uid[4], uid[5], uid[6], uid[7]);
    return s_efuse_id_buf;
}

static void fill_device_id_from_chip_id(frame_config_t *cfg) {
    uint8_t uid[8] = {0};
    /* ESP32-S3 eFuse Block3: OPTIONAL_UNIQUE_ID (128-bit), read first 64 bits */
    esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, 64);
    snprintf(cfg->device_id, sizeof(cfg->device_id),
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6], uid[7]);
    ESP_LOGI(TAG, "device_id from eFuse UID: %s", cfg->device_id);
}

esp_err_t frame_config_save(const frame_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(root, "wifi_ssid",       cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password",   cfg->wifi_password);
    cJSON_AddStringToObject(root, "device_id",       cfg->device_id);
    if (cfg->server_url[0])
        cJSON_AddStringToObject(root, "server_url",  cfg->server_url);
    if (cfg->mqtt_broker_url[0])
        cJSON_AddStringToObject(root, "mqtt_broker_url", cfg->mqtt_broker_url);
    if (cfg->mqtt_username[0])
        cJSON_AddStringToObject(root, "mqtt_username",   cfg->mqtt_username);
    if (cfg->mqtt_password[0])
        cJSON_AddStringToObject(root, "mqtt_password",   cfg->mqtt_password);
    if (cfg->mqtt_topic[0])
        cJSON_AddStringToObject(root, "mqtt_topic",      cfg->mqtt_topic);
    if (cfg->mqtt_client_id[0])
        cJSON_AddStringToObject(root, "mqtt_client_id",  cfg->mqtt_client_id);
    cJSON_AddNumberToObject(root, "poll_interval",      cfg->poll_interval > 0 ? cfg->poll_interval : 30);
    cJSON_AddNumberToObject(root, "mqtt_keepalive_sec", cfg->mqtt_keepalive_sec > 0 ? cfg->mqtt_keepalive_sec : 60);
    if (cfg->bind_code[0])
        cJSON_AddStringToObject(root, "bind_code", cfg->bind_code);
    cJSON_AddNumberToObject(root, "bind_status", cfg->bind_status);
    if (cfg->bind_code_expires > 0)
        cJSON_AddNumberToObject(root, "bind_code_expires", cfg->bind_code_expires);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    esp_err_t ret = sdcard_write_file(FRAME_CONFIG_PATH, json_str, strlen(json_str));
    free(json_str);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Config saved: device=%s mqtt=%s",
                 cfg->device_id,
                 cfg->mqtt_broker_url[0] ? cfg->mqtt_broker_url : "(none)");
    }
    return ret;
}

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
    cJSON *ssid            = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *pass            = cJSON_GetObjectItem(root, "wifi_password");
    cJSON *url             = cJSON_GetObjectItem(root, "server_url");
    cJSON *did             = cJSON_GetObjectItem(root, "device_id");
    cJSON *mqtt_broker     = cJSON_GetObjectItem(root, "mqtt_broker_url");
    cJSON *mqtt_user       = cJSON_GetObjectItem(root, "mqtt_username");
    cJSON *mqtt_pass       = cJSON_GetObjectItem(root, "mqtt_password");
    cJSON *mqtt_topic      = cJSON_GetObjectItem(root, "mqtt_topic");
    cJSON *mqtt_client_id  = cJSON_GetObjectItem(root, "mqtt_client_id");
    cJSON *mqtt_keepalive  = cJSON_GetObjectItem(root, "mqtt_keepalive_sec");

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
    if (cfg->device_id[0] == '\0') {
        fill_device_id_from_chip_id(cfg);
    }
    if (cJSON_IsString(mqtt_broker)) {
        strncpy(cfg->mqtt_broker_url, mqtt_broker->valuestring, sizeof(cfg->mqtt_broker_url) - 1);
    }
    if (cJSON_IsString(mqtt_user)) {
        strncpy(cfg->mqtt_username, mqtt_user->valuestring, sizeof(cfg->mqtt_username) - 1);
    }
    if (cJSON_IsString(mqtt_pass)) {
        strncpy(cfg->mqtt_password, mqtt_pass->valuestring, sizeof(cfg->mqtt_password) - 1);
    }
    if (cJSON_IsString(mqtt_topic)) {
        strncpy(cfg->mqtt_topic, mqtt_topic->valuestring, sizeof(cfg->mqtt_topic) - 1);
    }
    if (cJSON_IsString(mqtt_client_id)) {
        strncpy(cfg->mqtt_client_id, mqtt_client_id->valuestring, sizeof(cfg->mqtt_client_id) - 1);
    }

    /* Optional: poll_interval (default 30s) */
    cJSON *interval = cJSON_GetObjectItem(root, "poll_interval");
    cfg->poll_interval = cJSON_IsNumber(interval) ? (int)interval->valuedouble : 30;
    cfg->mqtt_keepalive_sec = cJSON_IsNumber(mqtt_keepalive) ? (int)mqtt_keepalive->valuedouble : 60;

    /* Bind code fields (for SoftAP provisioning) */
    cJSON *bind_code = cJSON_GetObjectItem(root, "bind_code");
    if (cJSON_IsString(bind_code)) {
        strncpy(cfg->bind_code, bind_code->valuestring, sizeof(cfg->bind_code) - 1);
    }
    cJSON *bind_status = cJSON_GetObjectItem(root, "bind_status");
    cfg->bind_status = cJSON_IsNumber(bind_status) ? (bind_status_t)bind_status->valuedouble : BIND_STATUS_UNBOUND;
    cJSON *bind_expires = cJSON_GetObjectItem(root, "bind_code_expires");
    cfg->bind_code_expires = cJSON_IsNumber(bind_expires) ? (int)bind_expires->valuedouble : 0;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config loaded: ssid=%s transport=%s server=%s device=%s topic=%s poll=%ds",
             cfg->wifi_ssid,
             cfg->mqtt_broker_url[0] ? "mqtt" : (cfg->server_url[0] ? "http" : "wifi-only"),
             cfg->server_url[0] ? "configured" : "not set",
             cfg->device_id[0] ? cfg->device_id : "(not set)",
             cfg->mqtt_topic[0] ? cfg->mqtt_topic : "(default)",
             cfg->poll_interval);
    return cfg;
}
