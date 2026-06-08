#ifndef BLE_PROV_H
#define BLE_PROV_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Credentials received from web via BLE */
typedef struct {
    char ssid[64];
    char password[64];
    char mqtt_url[256];  // e.g. "mqtt://1.2.3.4:1883"
    char mqtt_user[64];
    char mqtt_pass[64];
} ble_prov_credentials_t;

typedef void (*ble_prov_credentials_cb_t)(const ble_prov_credentials_t *creds, void *ctx);

/**
 * Start BLE GATT provisioning server.
 * Advertises as `ble_name`. On credentials received, calls `cb`.
 */
esp_err_t ble_prov_start(const char *ble_name,
                          ble_prov_credentials_cb_t cb,
                          void *ctx);

/**
 * Draw QR code + instructions onto e-paper paint buffer.
 * Call epaper_port_display() after this to refresh the screen.
 *
 * QR encodes: {"id":"<device_id>","ble":"<ble_name>"}
 */
void ble_prov_draw_qr_screen(uint8_t *img_buf,
                              const char *device_id,
                              const char *ble_name);

/**
 * Send a short status string as a BLE notification to the connected client.
 * status: e.g. "wifi", "wifi_ok", "wifi_fail", "binding", "bound", "bind_timeout"
 * Returns ESP_ERR_INVALID_STATE if no client connected.
 */
esp_err_t ble_prov_notify_status(const char *status);

/**
 * Stop BLE stack and free resources.
 */
void ble_prov_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PROV_H */
