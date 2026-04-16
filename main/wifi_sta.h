#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connect to WiFi in STA mode.
 * Blocks until connected or timeout (30s).
 * Returns ESP_OK on success.
 */
esp_err_t wifi_sta_connect(const char *ssid, const char *password);

/** Disconnect and deinit WiFi */
void frame_wifi_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_STA_H
