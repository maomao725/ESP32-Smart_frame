#ifndef FRAME_CONFIG_H
#define FRAME_CONFIG_H

#include <stdint.h>

#define FRAME_CONFIG_PATH "/sdcard/config.txt"
#define FRAME_PHOTO_PATH  "/sdcard/current.bmp"

/**
 * Smart photo frame configuration loaded from SD card config.txt
 */
typedef struct {
    char wifi_ssid[64];
    char wifi_password[64];
    char server_url[256];   // e.g. http://1.2.3.4:8000/api/photo
    char device_id[32];     // e.g. frame_001
    int  poll_interval;     // seconds between polls, default 30
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

#ifdef __cplusplus
}
#endif

#endif // FRAME_CONFIG_H
