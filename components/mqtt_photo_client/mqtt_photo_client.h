#ifndef MQTT_PHOTO_CLIENT_H
#define MQTT_PHOTO_CLIENT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *broker_url;
    const char *username;
    const char *password;
    const char *topic;
    const char *client_id;
    const char *photo_base_url;
    const char *device_id;
    int keepalive_sec;
    bool subscribe_photo_topic;
} mqtt_photo_client_config_t;

typedef void (*mqtt_photo_client_ready_cb_t)(const char *photo_path, void *user_ctx);
typedef void (*mqtt_photo_client_bind_code_cb_t)(const char *bind_code,
                                                 int expires_in,
                                                 int timestamp,
                                                 void *user_ctx);
typedef void (*mqtt_photo_client_bound_cb_t)(void *user_ctx);
typedef void (*mqtt_photo_client_interval_cb_t)(int interval_seconds, void *user_ctx);
typedef void (*mqtt_photo_client_daily_time_cb_t)(int seconds_since_midnight, void *user_ctx);

esp_err_t mqtt_photo_client_start(const mqtt_photo_client_config_t *config,
                                  mqtt_photo_client_ready_cb_t on_photo_ready,
                                  mqtt_photo_client_bind_code_cb_t on_bind_code,
                                  mqtt_photo_client_bound_cb_t on_bound,
                                  mqtt_photo_client_interval_cb_t on_interval,
                                  mqtt_photo_client_daily_time_cb_t on_daily_time,
                                  void *user_ctx);

#ifdef __cplusplus
}
#endif

#endif // MQTT_PHOTO_CLIENT_H
