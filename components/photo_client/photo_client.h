#ifndef PHOTO_CLIENT_H
#define PHOTO_CLIENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Download the latest photo for device_id from server and save to SD card.
 *
 * URL pattern: {base_url}/{device_id}/latest.bmp
 * Saves to: /sdcard/current.bmp
 *
 * Returns ESP_OK if a new photo was downloaded and saved.
 * Returns ESP_ERR_NOT_FOUND if server returns 404 (no new photo).
 * Returns ESP_FAIL on network or write error.
 */
esp_err_t photo_client_fetch(const char *base_url, const char *device_id);

#ifdef __cplusplus
}
#endif

#endif // PHOTO_CLIENT_H
