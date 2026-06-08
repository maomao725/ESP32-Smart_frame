#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connect to WiFi in STA mode (full init).
 * Blocks until connected or timeout (30s).
 */
esp_err_t wifi_sta_connect(const char *ssid, const char *password);

/**
 * Switch from SoftAP/APSTA to STA mode and connect.
 * Use this after softap_prov_start() — skips esp_wifi_init/start.
 * Blocks until connected or timeout (30s).
 */
esp_err_t wifi_sta_connect_after_softap(const char *ssid, const char *password);

/** Disconnect and deinit WiFi */
void frame_wifi_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_STA_H
