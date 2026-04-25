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
 */
esp_err_t photo_client_fetch(const char *base_url, const char *device_id);

/**
 * Download a photo from a full HTTP(S) URL and save it to the current photo path.
 */
esp_err_t photo_client_fetch_url(const char *url);

/**
 * Return the SD-card path of the current photo file.
 */
const char *photo_client_current_path(void);

#ifdef __cplusplus
}
#endif

#endif // PHOTO_CLIENT_H
