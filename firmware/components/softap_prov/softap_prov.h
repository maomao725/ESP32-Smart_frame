#ifndef SOFTAP_PROV_H
#define SOFTAP_PROV_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Credentials received from HTTP provisioning
 */
#define SOFTAP_PROV_SSID_MAX_LEN          64
#define SOFTAP_PROV_PASSWORD_MAX_LEN      64
#define SOFTAP_PROV_MQTT_URL_MAX_LEN     256
#define SOFTAP_PROV_MQTT_USER_MAX_LEN     64
#define SOFTAP_PROV_MQTT_PASSWORD_MAX_LEN 64

typedef struct {
    char ssid[SOFTAP_PROV_SSID_MAX_LEN];
    char password[SOFTAP_PROV_PASSWORD_MAX_LEN];
    bool use_custom_mqtt;
    char mqtt_url[SOFTAP_PROV_MQTT_URL_MAX_LEN];
    char mqtt_username[SOFTAP_PROV_MQTT_USER_MAX_LEN];
    char mqtt_password[SOFTAP_PROV_MQTT_PASSWORD_MAX_LEN];
} softap_prov_credentials_t;

/**
 * Provisioning state for UI display
 */
typedef enum {
    SOFTAP_STATE_IDLE = 0,
    SOFTAP_STATE_SCANNING,
    SOFTAP_STATE_CONNECTING,
    SOFTAP_STATE_CONNECTED,
    SOFTAP_STATE_GETTING_BIND_CODE,
    SOFTAP_STATE_WAITING_BIND,
    SOFTAP_STATE_BOUND,
    SOFTAP_STATE_FAILED
} softap_state_t;

typedef void (*softap_prov_credentials_cb_t)(const softap_prov_credentials_t *creds, void *ctx);

/**
 * Start SoftAP provisioning mode.
 *
 * Creates Wi-Fi SoftAP, starts HTTP server for configuration,
 * and scans for nearby Wi-Fi networks.
 *
 * @param ap_name     SoftAP SSID (e.g. "SmartFrame-8F21")
 * @param ap_password SoftAP password (NULL for open network)
 * @param cb          Callback when credentials are submitted
 * @param ctx         User context for callback
 *
 * @return ESP_OK on success
 */
esp_err_t softap_prov_start(const char *ap_name,
                            const char *ap_password,
                            softap_prov_credentials_cb_t cb,
                            void *ctx);

/**
 * Draw provisioning screen onto e-paper paint buffer.
 * Call epaper_port_display() after this to refresh the screen.
 *
 * @param img_buf    Paint buffer
 * @param device_id  Device ID
 * @param ap_name    SoftAP SSID
 * @param ap_ip      SoftAP IP address (e.g. "192.168.4.1")
 * @param bind_code  Dynamic bind code (NULL if not available)
 * @param state      Current provisioning state
 */
void softap_prov_draw_screen(uint8_t *img_buf,
                              const char *device_id,
                              const char *ap_name,
                              const char *ap_ip,
                              const char *bind_code,
                              softap_state_t state);

/**
 * Get current provisioning state
 */
softap_state_t softap_prov_get_state(void);

/**
 * Get SoftAP IP address
 * Returns NULL if not started
 */
const char *softap_prov_get_ap_ip(void);

/**
 * Stop SoftAP provisioning and free resources
 */
void softap_prov_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* SOFTAP_PROV_H */
