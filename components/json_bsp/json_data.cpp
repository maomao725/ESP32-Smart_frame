/**
 * json_bsp — minimal stub for smart photo frame.
 *
 * Original weather/RTC parsing removed (depended on i2c_equipment which is
 * not part of this project). Only json_sdcard_txt_aimodel() is kept because
 * it reads the SD-card config used by the AP-config flow.
 *
 * NOTE: frame_config.c already handles config.txt parsing via cJSON for the
 * normal boot path. This function is retained for the http_server_bsp AP
 * config flow that may call it.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "json_data.h"
#include "ArduinoJson-v7.4.1.h"
#include "esp_log.h"
#include "sdcard_bsp.h"

static JsonDocument doc;

ai_model_t *json_sdcard_txt_aimodel(void) {
    ai_model_t *data = (ai_model_t *)malloc(sizeof(ai_model_t));
    uint8_t *sdcard_buffer = (uint8_t *)malloc(1024);
    if (!sdcard_buffer || !data) {
        free(sdcard_buffer);
        free(data);
        return NULL;
    }
    if (sdcard_read_file("/sdcard/config.txt", sdcard_buffer, NULL) != ESP_OK) {
        free(sdcard_buffer);
        free(data);
        return NULL;
    }
    DeserializationError error = deserializeJson(doc, sdcard_buffer);
    free(sdcard_buffer);
    if (error) {
        ESP_LOGE("json_bsp", "Config parse failed");
        free(data);
        return NULL;
    }
    data->time = doc["poll_interval"] | 30;
    const char *str = doc["server_url"];
    if (str) strncpy(data->url, str, sizeof(data->url) - 1);
    str = doc["device_id"];
    if (str) strncpy(data->model, str, sizeof(data->model) - 1);
    data->key[0] = '\0';
    return data;
}
