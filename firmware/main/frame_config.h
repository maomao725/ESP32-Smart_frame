#ifndef FRAME_CONFIG_H
#define FRAME_CONFIG_H

#include <stdint.h>
#include "esp_err.h"

#define FRAME_CONFIG_PATH "/sdcard/config.txt"
#define FRAME_PHOTO_PATH  "/sdcard/current.bmp"

/**
 * Device binding status for dynamic bind code flow
 */
typedef enum {
    BIND_STATUS_UNBOUND = 0,    // Device not bound to any user
    BIND_STATUS_PENDING = 1,    // Bind code generated, waiting for user input
    BIND_STATUS_BOUND = 2,      // Successfully bound to user account
    BIND_STATUS_EXPIRED = 3     // Bind code expired
} bind_status_t;

/**
 * Smart photo frame configuration loaded from SD card config.txt
 */
typedef struct {
    char wifi_ssid[64];
    char wifi_password[64];
    char server_url[256];      // HTTP base URL, direct image URL, or refresh endpoint
    char device_id[32];        // e.g. frame_001
    char mqtt_broker_url[256]; // e.g. mqtt://1.2.3.4:1883
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_topic[128];      // optional, default: device/{device_id}/image
    char mqtt_client_id[64];   // optional, default: smart-frame-{device_id}
    int  poll_interval;        // seconds between HTTP polls, default 30
    int  mqtt_keepalive_sec;   // MQTT keepalive, default 60

    // SoftAP provisioning fields
    char bind_code[16];        // Dynamic bind code (6-digit or 8-alphanumeric)
    bind_status_t bind_status; // Current binding status
    int bind_code_expires;     // Unix timestamp when bind code expires
} frame_config_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read and parse config.txt from SD card.
 * Caller must free() the returned pointer.
 * Returns NULL if file missing or JSON invalid.
 */
frame_config_t *frame_config_load(void);

/**
 * Write cfg to SD card as config.txt (JSON).
 * Returns ESP_OK on success.
 */
esp_err_t frame_config_save(const frame_config_t *cfg);

/**
 * Return a device_id string derived from eFuse UID (16 hex chars, static buffer).
 * Valid until next call. Thread-unsafe — call before starting tasks.
 */
const char *frame_config_device_id_from_efuse(void);

#ifdef __cplusplus
}
#endif

#endif // FRAME_CONFIG_H
