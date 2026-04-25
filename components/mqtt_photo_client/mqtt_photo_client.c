#include "mqtt_photo_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "photo_client.h"

static const char *TAG = "mqtt_photo";

#define MQTT_PHOTO_BROKER_URL_MAX_LEN 256
#define MQTT_PHOTO_USERNAME_MAX_LEN    64
#define MQTT_PHOTO_PASSWORD_MAX_LEN    64
#define MQTT_PHOTO_TOPIC_MAX_LEN      128
#define MQTT_PHOTO_CLIENT_ID_MAX_LEN   64
#define MQTT_PHOTO_URL_MAX_LEN        320
#define MQTT_PHOTO_PAYLOAD_MAX_LEN    512
#define MQTT_PHOTO_BOUND_TOPIC_MAX_LEN 128
#define MQTT_PHOTO_BIND_CODE_MAX_LEN   16
#define MQTT_PHOTO_DEFAULT_KEEPALIVE   60
#define MQTT_PHOTO_DEFAULT_QOS          1

typedef enum {
    MQTT_PHOTO_REQUEST_NONE = 0,
    MQTT_PHOTO_REQUEST_REFRESH,
    MQTT_PHOTO_REQUEST_URL,
    MQTT_PHOTO_REQUEST_RELATIVE_PATH,
    MQTT_PHOTO_REQUEST_BIND_CODE,
    MQTT_PHOTO_REQUEST_BIND_CONFIRMED,
} mqtt_photo_request_type_t;

typedef struct {
    mqtt_photo_request_type_t type;
    char value[MQTT_PHOTO_URL_MAX_LEN];
    char bind_code[MQTT_PHOTO_BIND_CODE_MAX_LEN];
    int expires_in;
    int timestamp;
} mqtt_photo_request_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    QueueHandle_t request_queue;
    TaskHandle_t worker_task;
    mqtt_photo_client_ready_cb_t on_photo_ready;
    mqtt_photo_client_bind_code_cb_t on_bind_code;
    mqtt_photo_client_bound_cb_t on_bound;
    void *user_ctx;
    char broker_url[MQTT_PHOTO_BROKER_URL_MAX_LEN];
    char username[MQTT_PHOTO_USERNAME_MAX_LEN];
    char password[MQTT_PHOTO_PASSWORD_MAX_LEN];
    char topic[MQTT_PHOTO_TOPIC_MAX_LEN];
    char bound_topic[MQTT_PHOTO_BOUND_TOPIC_MAX_LEN];
    char client_id[MQTT_PHOTO_CLIENT_ID_MAX_LEN];
    char photo_base_url[MQTT_PHOTO_URL_MAX_LEN];
    char device_id[32];
    char inbound_topic[MQTT_PHOTO_BOUND_TOPIC_MAX_LEN];
    char payload_buffer[MQTT_PHOTO_PAYLOAD_MAX_LEN];
    size_t payload_expected_len;
    size_t payload_received_len;
    bool payload_drop;
    bool subscribe_photo_topic;
    int keepalive_sec;
} mqtt_photo_client_ctx_t;

static mqtt_photo_client_ctx_t s_ctx = {0};

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static bool is_absolute_url(const char *value) {
    return value
        && (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0);
}

static void trim_inplace(char *value) {
    char *start = value;
    char *end = NULL;

    if (!value) {
        return;
    }

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        ++start;
    }

    if (start != value) {
        memmove(value, start, strlen(start) + 1);
    }

    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
}

static esp_err_t join_url(const char *base_url, const char *path, char *out, size_t out_size) {
    size_t base_len = 0;
    const char *path_part = path;

    if (!base_url || base_url[0] == '\0' || !path || path[0] == '\0' || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    base_len = strlen(base_url);
    while (*path_part == '/') {
        ++path_part;
    }

    if (base_len > 0 && base_url[base_len - 1] == '/') {
        if (snprintf(out, out_size, "%s%s", base_url, path_part) >= (int)out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        if (snprintf(out, out_size, "%s/%s", base_url, path_part) >= (int)out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return ESP_OK;
}

static void derive_default_topic(char *topic, size_t topic_size, const char *device_id) {
    if (!topic || topic_size == 0) {
        return;
    }

    if (device_id && device_id[0] != '\0') {
        snprintf(topic, topic_size, "device/%s/image", device_id);
        return;
    }

    topic[0] = '\0';
}

static void derive_default_client_id(char *client_id, size_t client_id_size, const char *device_id) {
    if (!client_id || client_id_size == 0) {
        return;
    }

    if (device_id && device_id[0] != '\0') {
        snprintf(client_id, client_id_size, "smart-frame-%s", device_id);
        return;
    }

    snprintf(client_id, client_id_size, "smart-frame");
}

static esp_err_t derive_bound_topic(const char *device_id, char *topic, size_t topic_size) {
    if (!device_id || device_id[0] == '\0' || !topic || topic_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(topic, topic_size, "device/%s/bound", device_id) >= (int)topic_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static int64_t current_registration_timestamp(void) {
    time_t now = time(NULL);

    if (now > 0) {
        return (int64_t)now;
    }

    /* The backend only requires a numeric timestamp; fall back to uptime
     * seconds when RTC/NTP is not available yet. */
    int64_t fallback = (int64_t)(esp_log_timestamp() / 1000);
    return fallback > 0 ? fallback : 1;
}

static void publish_device_registration(mqtt_photo_client_ctx_t *ctx) {
    char topic[MQTT_PHOTO_BOUND_TOPIC_MAX_LEN];
    char *payload = NULL;
    cJSON *root = NULL;
    int msg_id = -1;

    if (!ctx || !ctx->client) {
        return;
    }

    if (ctx->device_id[0] == '\0') {
        ESP_LOGW(TAG, "Skip device registration publish: device_id is empty");
        return;
    }

    if (derive_bound_topic(ctx->device_id, topic, sizeof(topic)) != ESP_OK) {
        ESP_LOGW(TAG, "Skip device registration publish: bound topic too long");
        return;
    }

    root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGW(TAG, "Skip device registration publish: JSON alloc failed");
        return;
    }

    cJSON_AddStringToObject(root, "event", "reg_new_device");
    cJSON_AddStringToObject(root, "device_uid", ctx->device_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)current_registration_timestamp());

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        ESP_LOGW(TAG, "Skip device registration publish: JSON encode failed");
        return;
    }

    msg_id = esp_mqtt_client_publish(ctx->client, topic, payload, 0, MQTT_PHOTO_DEFAULT_QOS, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Device registration publish failed: topic=%s", topic);
    } else {
        ESP_LOGI(TAG, "Device registration published: topic=%s msg_id=%d", topic, msg_id);
    }

    free(payload);
}

static bool topic_equals(const char *lhs, const char *rhs) {
    return lhs && rhs && strcmp(lhs, rhs) == 0;
}

static const cJSON *find_first_string_field(const cJSON *root,
                                            const char *key1,
                                            const char *key2,
                                            const char *key3) {
    const cJSON *item = NULL;

    if (!root) {
        return NULL;
    }

    if (key1) {
        item = cJSON_GetObjectItemCaseSensitive(root, key1);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
            return item;
        }
    }
    if (key2) {
        item = cJSON_GetObjectItemCaseSensitive(root, key2);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
            return item;
        }
    }
    if (key3) {
        item = cJSON_GetObjectItemCaseSensitive(root, key3);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
            return item;
        }
    }

    return NULL;
}

static esp_err_t resolve_payload_to_request(const char *payload, mqtt_photo_request_t *request) {
    char payload_copy[MQTT_PHOTO_PAYLOAD_MAX_LEN];
    cJSON *root = NULL;

    if (!payload || !request) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(request, 0, sizeof(*request));
    copy_string(payload_copy, sizeof(payload_copy), payload);
    trim_inplace(payload_copy);
    if (payload_copy[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(payload_copy);
    if (root) {
        const cJSON *url_item = find_first_string_field(root, "url", "photo_url", "image_url");
        const cJSON *path_item = find_first_string_field(root, "path", "file", NULL);
        const cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
        const cJSON *refresh_item = cJSON_GetObjectItemCaseSensitive(root, "refresh");

        if (url_item) {
            request->type = MQTT_PHOTO_REQUEST_URL;
            copy_string(request->value, sizeof(request->value), url_item->valuestring);
            cJSON_Delete(root);
            return ESP_OK;
        }

        if (path_item) {
            request->type = is_absolute_url(path_item->valuestring)
                ? MQTT_PHOTO_REQUEST_URL
                : MQTT_PHOTO_REQUEST_RELATIVE_PATH;
            copy_string(request->value, sizeof(request->value), path_item->valuestring);
            cJSON_Delete(root);
            return ESP_OK;
        }

        if ((cJSON_IsString(action_item) && action_item->valuestring
             && strcasecmp(action_item->valuestring, "refresh") == 0)
            || cJSON_IsTrue(refresh_item)) {
            request->type = MQTT_PHOTO_REQUEST_REFRESH;
            cJSON_Delete(root);
            return ESP_OK;
        }

        cJSON_Delete(root);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (strcasecmp(payload_copy, "refresh") == 0 || strcasecmp(payload_copy, "latest") == 0) {
        request->type = MQTT_PHOTO_REQUEST_REFRESH;
        return ESP_OK;
    }

    if (is_absolute_url(payload_copy)) {
        request->type = MQTT_PHOTO_REQUEST_URL;
        copy_string(request->value, sizeof(request->value), payload_copy);
        return ESP_OK;
    }

    request->type = MQTT_PHOTO_REQUEST_RELATIVE_PATH;
    copy_string(request->value, sizeof(request->value), payload_copy);
    return ESP_OK;
}

static esp_err_t resolve_bound_payload_to_request(const char *payload,
                                                  const char *device_id,
                                                  mqtt_photo_request_t *request) {
    cJSON *root = NULL;
    const cJSON *event_item = NULL;
    const cJSON *device_uid_item = NULL;
    const cJSON *bind_code_item = NULL;
    const cJSON *expires_in_item = NULL;
    const cJSON *timestamp_item = NULL;

    if (!payload || !device_id || !request) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(request, 0, sizeof(*request));
    root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGW(TAG, "Ignoring bound payload that is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    event_item = cJSON_GetObjectItemCaseSensitive(root, "event");
    device_uid_item = cJSON_GetObjectItemCaseSensitive(root, "device_uid");
    if (!cJSON_IsString(event_item) || !cJSON_IsString(device_uid_item)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Ignoring bound payload missing event/device_uid");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strcmp(device_uid_item->valuestring, device_id) != 0) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Ignoring bound payload for another device: %s", device_uid_item->valuestring);
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(event_item->valuestring, "reg_new_device") == 0) {
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Ignoring echoed reg_new_device payload on bound topic");
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(event_item->valuestring, "device_bound") == 0) {
        request->type = MQTT_PHOTO_REQUEST_BIND_CONFIRMED;
        cJSON_Delete(root);
        return ESP_OK;
    }

    if (strcmp(event_item->valuestring, "dyn_bound_code") != 0) {
        cJSON_Delete(root);
        ESP_LOGI(TAG, "Ignoring unsupported bound event: %s", event_item->valuestring);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bind_code_item = cJSON_GetObjectItemCaseSensitive(root, "dyn_bound_code");
    expires_in_item = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    timestamp_item = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    if (!cJSON_IsString(bind_code_item)
        || !cJSON_IsNumber(expires_in_item)
        || !cJSON_IsNumber(timestamp_item)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Ignoring dyn_bound_code payload missing required fields");
        return ESP_ERR_INVALID_RESPONSE;
    }

    request->type = MQTT_PHOTO_REQUEST_BIND_CODE;
    copy_string(request->bind_code, sizeof(request->bind_code), bind_code_item->valuestring);
    request->expires_in = (int)expires_in_item->valuedouble;
    request->timestamp = (int)timestamp_item->valuedouble;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t execute_request(mqtt_photo_client_ctx_t *ctx, const mqtt_photo_request_t *request) {
    char url[MQTT_PHOTO_URL_MAX_LEN];

    if (!ctx || !request) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (request->type) {
        case MQTT_PHOTO_REQUEST_REFRESH:
            if (ctx->photo_base_url[0] == '\0') {
                ESP_LOGW(TAG, "Received refresh command but server_url is not configured");
                return ESP_ERR_INVALID_STATE;
            }
            return photo_client_fetch(ctx->photo_base_url,
                                      ctx->device_id[0] ? ctx->device_id : NULL);

        case MQTT_PHOTO_REQUEST_URL:
            return photo_client_fetch_url(request->value);

        case MQTT_PHOTO_REQUEST_RELATIVE_PATH:
            if (ctx->photo_base_url[0] == '\0') {
                ESP_LOGW(TAG, "Received relative photo path but server_url is not configured");
                return ESP_ERR_INVALID_STATE;
            }
            if (join_url(ctx->photo_base_url, request->value, url, sizeof(url)) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to build photo URL from base and relative path");
                return ESP_ERR_INVALID_SIZE;
            }
            return photo_client_fetch_url(url);

        case MQTT_PHOTO_REQUEST_NONE:
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static void mqtt_photo_worker_task(void *arg) {
    mqtt_photo_client_ctx_t *ctx = (mqtt_photo_client_ctx_t *)arg;
    mqtt_photo_request_t request;

    while (xQueueReceive(ctx->request_queue, &request, portMAX_DELAY) == pdTRUE) {
        if (request.type == MQTT_PHOTO_REQUEST_BIND_CODE) {
            ESP_LOGI(TAG, "Received dyn_bound_code via MQTT: code=%s expires_in=%d",
                     request.bind_code, request.expires_in);
            if (ctx->on_bind_code) {
                ctx->on_bind_code(request.bind_code,
                                  request.expires_in,
                                  request.timestamp,
                                  ctx->user_ctx);
            }
            continue;
        }

        if (request.type == MQTT_PHOTO_REQUEST_BIND_CONFIRMED) {
            ESP_LOGI(TAG, "Received device_bound via MQTT");
            if (ctx->on_bound) {
                ctx->on_bound(ctx->user_ctx);
            }
            continue;
        }

        esp_err_t err = execute_request(ctx, &request);
        if (err == ESP_OK) {
            if (ctx->on_photo_ready) {
                ctx->on_photo_ready(photo_client_current_path(), ctx->user_ctx);
            }
            continue;
        }

        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "MQTT-triggered refresh found no new photo");
            continue;
        }

        ESP_LOGW(TAG, "Photo update failed after MQTT message: %s", esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

static void reset_payload_buffer(mqtt_photo_client_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    ctx->payload_expected_len = 0;
    ctx->payload_received_len = 0;
    ctx->payload_drop = false;
    ctx->payload_buffer[0] = '\0';
}

static void handle_complete_payload(mqtt_photo_client_ctx_t *ctx) {
    mqtt_photo_request_t request;
    esp_err_t err;

    if (!ctx) {
        return;
    }

    if (topic_equals(ctx->inbound_topic, ctx->bound_topic)) {
        ESP_LOGI(TAG, "MQTT bound topic payload: %s", ctx->payload_buffer);
        err = resolve_bound_payload_to_request(ctx->payload_buffer, ctx->device_id, &request);
    } else {
        err = resolve_payload_to_request(ctx->payload_buffer, &request);
    }

    if (err != ESP_OK) {
        reset_payload_buffer(ctx);
        return;
    }

    if (xQueueOverwrite(ctx->request_queue, &request) != pdPASS) {
        ESP_LOGW(TAG, "Failed to enqueue MQTT photo request");
    }

    reset_payload_buffer(ctx);
}

static void handle_mqtt_data_event(mqtt_photo_client_ctx_t *ctx, esp_mqtt_event_handle_t event) {
    size_t next_len = 0;

    if (!ctx || !event) {
        return;
    }

    if (event->current_data_offset == 0) {
        reset_payload_buffer(ctx);
        if (event->topic_len <= 0 || event->topic_len >= MQTT_PHOTO_BOUND_TOPIC_MAX_LEN) {
            ESP_LOGW(TAG, "MQTT topic too large: %d bytes", event->topic_len);
            ctx->payload_drop = true;
            return;
        }
        memcpy(ctx->inbound_topic, event->topic, event->topic_len);
        ctx->inbound_topic[event->topic_len] = '\0';
        if (event->total_data_len <= 0 || event->total_data_len >= MQTT_PHOTO_PAYLOAD_MAX_LEN) {
            ESP_LOGW(TAG, "MQTT payload too large: %d bytes", event->total_data_len);
            ctx->payload_drop = true;
            return;
        }
        ctx->payload_expected_len = (size_t)event->total_data_len;
    }

    if (ctx->payload_drop) {
        return;
    }

    next_len = (size_t)event->current_data_offset + (size_t)event->data_len;
    if (next_len >= MQTT_PHOTO_PAYLOAD_MAX_LEN) {
        ESP_LOGW(TAG, "MQTT payload buffer overflow while receiving message");
        ctx->payload_drop = true;
        return;
    }

    memcpy(ctx->payload_buffer + event->current_data_offset, event->data, event->data_len);
    if (next_len > ctx->payload_received_len) {
        ctx->payload_received_len = next_len;
    }

    if (ctx->payload_expected_len > 0 && ctx->payload_received_len >= ctx->payload_expected_len) {
        ctx->payload_buffer[ctx->payload_expected_len] = '\0';
        handle_complete_payload(ctx);
    }
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data) {
    mqtt_photo_client_ctx_t *ctx = (mqtt_photo_client_ctx_t *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)base;

    if (!ctx || !event) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            if (ctx->subscribe_photo_topic && ctx->topic[0] != '\0') {
                ESP_LOGI(TAG, "MQTT connected, subscribing to topic: %s", ctx->topic);
                esp_mqtt_client_subscribe(ctx->client, ctx->topic, MQTT_PHOTO_DEFAULT_QOS);
            }
            if (ctx->bound_topic[0] != '\0') {
                ESP_LOGI(TAG, "MQTT connected, subscribing to bound topic: %s", ctx->bound_topic);
                esp_mqtt_client_subscribe(ctx->client, ctx->bound_topic, MQTT_PHOTO_DEFAULT_QOS);
            }
            publish_device_registration(ctx);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_DATA:
            handle_mqtt_data_event(ctx, event);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT transport error");
            break;

        default:
            break;
    }
}

static void cleanup_start_failure(mqtt_photo_client_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->client) {
        esp_mqtt_client_stop(ctx->client);
        esp_mqtt_client_destroy(ctx->client);
        ctx->client = NULL;
    }
    if (ctx->worker_task) {
        vTaskDelete(ctx->worker_task);
        ctx->worker_task = NULL;
    }
    if (ctx->request_queue) {
        vQueueDelete(ctx->request_queue);
        ctx->request_queue = NULL;
    }
}

esp_err_t mqtt_photo_client_start(const mqtt_photo_client_config_t *config,
                                  mqtt_photo_client_ready_cb_t on_photo_ready,
                                  mqtt_photo_client_bind_code_cb_t on_bind_code,
                                  mqtt_photo_client_bound_cb_t on_bound,
                                  void *user_ctx) {
    esp_err_t err = ESP_OK;
    esp_mqtt_client_config_t mqtt_cfg = {0};

    if (!config || !config->broker_url || config->broker_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.client) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    copy_string(s_ctx.broker_url, sizeof(s_ctx.broker_url), config->broker_url);
    copy_string(s_ctx.username, sizeof(s_ctx.username), config->username);
    copy_string(s_ctx.password, sizeof(s_ctx.password), config->password);
    copy_string(s_ctx.photo_base_url, sizeof(s_ctx.photo_base_url), config->photo_base_url);
    copy_string(s_ctx.device_id, sizeof(s_ctx.device_id), config->device_id);
    copy_string(s_ctx.topic, sizeof(s_ctx.topic), config->topic);
    copy_string(s_ctx.client_id, sizeof(s_ctx.client_id), config->client_id);
    s_ctx.subscribe_photo_topic = config->subscribe_photo_topic;
    s_ctx.keepalive_sec = config->keepalive_sec > 0 ? config->keepalive_sec : MQTT_PHOTO_DEFAULT_KEEPALIVE;
    s_ctx.on_photo_ready = on_photo_ready;
    s_ctx.on_bind_code = on_bind_code;
    s_ctx.on_bound = on_bound;
    s_ctx.user_ctx = user_ctx;

    if (s_ctx.subscribe_photo_topic && s_ctx.topic[0] == '\0') {
        derive_default_topic(s_ctx.topic, sizeof(s_ctx.topic), s_ctx.device_id);
    }
    if (s_ctx.device_id[0] != '\0') {
        if (derive_bound_topic(s_ctx.device_id, s_ctx.bound_topic, sizeof(s_ctx.bound_topic)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to derive bound topic from device_id");
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (s_ctx.client_id[0] == '\0') {
        derive_default_client_id(s_ctx.client_id, sizeof(s_ctx.client_id), s_ctx.device_id);
    }
    if (s_ctx.subscribe_photo_topic && s_ctx.topic[0] == '\0') {
        ESP_LOGE(TAG, "mqtt_topic is not configured and device_id is empty");
        return ESP_ERR_INVALID_ARG;
    }

    s_ctx.request_queue = xQueueCreate(1, sizeof(mqtt_photo_request_t));
    if (!s_ctx.request_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(mqtt_photo_worker_task, "mqtt_photo", 6 * 1024, &s_ctx, 4, &s_ctx.worker_task) != pdPASS) {
        cleanup_start_failure(&s_ctx);
        return ESP_ERR_NO_MEM;
    }

    mqtt_cfg.broker.address.uri = s_ctx.broker_url;
    mqtt_cfg.credentials.client_id = s_ctx.client_id;
    mqtt_cfg.session.keepalive = s_ctx.keepalive_sec;
    if (s_ctx.username[0] != '\0') {
        mqtt_cfg.credentials.username = s_ctx.username;
    }
    if (s_ctx.password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = s_ctx.password;
    }

    s_ctx.client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_ctx.client) {
        cleanup_start_failure(&s_ctx);
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(s_ctx.client, ESP_EVENT_ANY_ID, mqtt_event_handler, &s_ctx);
    if (err != ESP_OK) {
        cleanup_start_failure(&s_ctx);
        return err;
    }

    err = esp_mqtt_client_start(s_ctx.client);
    if (err != ESP_OK) {
        cleanup_start_failure(&s_ctx);
        return err;
    }

    ESP_LOGI(TAG, "MQTT photo client started: broker=%s topic=%s bound_topic=%s client_id=%s",
             s_ctx.broker_url,
             s_ctx.subscribe_photo_topic ? s_ctx.topic : "(disabled)",
             s_ctx.bound_topic[0] ? s_ctx.bound_topic : "(none)",
             s_ctx.client_id);
    return ESP_OK;
}
