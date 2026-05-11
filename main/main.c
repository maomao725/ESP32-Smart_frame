#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "sdcard_bsp.h"
#include "axp2101_bsp.h"
#include "epaper_port.h"
#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "frame_config.h"
#include "wifi_sta.h"
#include "photo_client.h"
#include "mqtt_photo_client.h"
#include "softap_prov.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "smart_frame";

/* ── Display buffer ─────────────────────────────────────────────────────────── */

static uint8_t  *s_img_buf  = NULL;
static uint32_t  s_img_size = 0;
static SemaphoreHandle_t s_display_lock = NULL;

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "%s heap: internal=%u spiram=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static uint8_t *alloc_display_buffer(size_t size)
{
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
        ESP_LOGI(TAG, "Image buffer: %lu bytes (PSRAM)", (unsigned long)size);
        return buf;
    }
    buf = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf)
        ESP_LOGW(TAG, "Image buffer: %lu bytes (internal RAM)", (unsigned long)size);
    return buf;
}

static bool epaper_init_buffer(void)
{
    s_img_size = ((EXAMPLE_LCD_WIDTH % 2 == 0)
                  ? (EXAMPLE_LCD_WIDTH / 2)
                  : (EXAMPLE_LCD_WIDTH / 2 + 1)) * EXAMPLE_LCD_HEIGHT;

    log_heap("before display buffer");
    s_img_buf = alloc_display_buffer(s_img_size);
    if (!s_img_buf) {
        ESP_LOGE(TAG, "Display buffer alloc failed (%lu bytes)", (unsigned long)s_img_size);
        return false;
    }
    memset(s_img_buf, 0x11, s_img_size);
    log_heap("after display buffer");

    Paint_NewImage(s_img_buf, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, 0, EPD_7IN3E_WHITE);
    Paint_SetScale(6);
    Paint_SelectImage(s_img_buf);
    Paint_SetRotate(180);

    if (!s_display_lock) {
        s_display_lock = xSemaphoreCreateMutex();
        if (!s_display_lock) {
            ESP_LOGE(TAG, "Display mutex alloc failed");
            return false;
        }
    }
    return true;
}

static bool take_display_lock(void)
{
    return !s_display_lock || xSemaphoreTake(s_display_lock, portMAX_DELAY) == pdTRUE;
}

static void give_display_lock(void)
{
    if (s_display_lock) {
        xSemaphoreGive(s_display_lock);
    }
}

static void draw_softap_screen_locked(const char *device_id,
                                      const char *ap_name,
                                      const char *ap_ip,
                                      const char *bind_code,
                                      softap_state_t state)
{
    if (!s_img_buf || !take_display_lock()) {
        return;
    }

    softap_prov_draw_screen(s_img_buf, device_id, ap_name, ap_ip, bind_code, state);
    epaper_port_display(s_img_buf);
    give_display_lock();
}

static void display_photo(const char *path)
{
    if (!s_img_buf) return;
    if (!take_display_lock()) return;
    Paint_Clear(EPD_7IN3E_WHITE);
    GUI_ReadBmp_RGB_6Color(path, 0, 0);
    epaper_port_display(s_img_buf);
    give_display_lock();
    ESP_LOGI(TAG, "Display refreshed: %s", path);
}

/* ── SoftAP provisioning ────────────────────────────────────────────────────── */

#define PROV_CREDS_BIT BIT0
static EventGroupHandle_t    s_prov_evt;
static softap_prov_credentials_t s_prov_creds;

#define BIND_CONFIRMED_BIT BIT0
static EventGroupHandle_t s_bind_evt;

static void on_prov_credentials(const softap_prov_credentials_t *creds, void *ctx)
{
    (void)ctx;
    memcpy(&s_prov_creds, creds, sizeof(s_prov_creds));
    xEventGroupSetBits(s_prov_evt, PROV_CREDS_BIT);
}

/* ── Fixed server / MQTT config ─────────────────────────────────────────────── */

#define MQTT_BROKER_URL   "mqtt://47.108.232.40:1883"
#define MQTT_USERNAME     "screen_test1"
#define MQTT_PASSWORD     "screen_test1"
#define BIND_CODE_URL_FMT "http://47.108.232.40:8000/api/devices/%s/bind-code"
#define BIND_STATUS_URL_FMT "http://47.108.232.40:8000/api/status/%s"
#define BIND_STATUS_POLL_INTERVAL_SEC 10
#define BIND_STATE_SETTLE_MS 2000

static void apply_default_mqtt_config(frame_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    strncpy(cfg->mqtt_broker_url, MQTT_BROKER_URL, sizeof(cfg->mqtt_broker_url) - 1);
    strncpy(cfg->mqtt_username, MQTT_USERNAME, sizeof(cfg->mqtt_username) - 1);
    strncpy(cfg->mqtt_password, MQTT_PASSWORD, sizeof(cfg->mqtt_password) - 1);
}

static void apply_custom_mqtt_config(frame_config_t *cfg, const softap_prov_credentials_t *creds)
{
    if (!cfg || !creds || !creds->use_custom_mqtt || creds->mqtt_url[0] == '\0') {
        return;
    }

    memset(cfg->mqtt_broker_url, 0, sizeof(cfg->mqtt_broker_url));
    memset(cfg->mqtt_username, 0, sizeof(cfg->mqtt_username));
    memset(cfg->mqtt_password, 0, sizeof(cfg->mqtt_password));

    strncpy(cfg->mqtt_broker_url, creds->mqtt_url, sizeof(cfg->mqtt_broker_url) - 1);
    strncpy(cfg->mqtt_username, creds->mqtt_username, sizeof(cfg->mqtt_username) - 1);
    strncpy(cfg->mqtt_password, creds->mqtt_password, sizeof(cfg->mqtt_password) - 1);
}

/* ── HTTPS bind code request ────────────────────────────────────────────────── */

typedef struct {
    char resp_buf[256];
    int  resp_len;
} http_response_buffer_t;

typedef struct {
    char bind_code[16];
    int expires_at;
    bool success;
    http_response_buffer_t response;
} bind_code_result_t;

typedef struct {
    bool bound;
    bool success;
    http_response_buffer_t response;
} bind_status_result_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_buffer_t *response = (http_response_buffer_t *)evt->user_data;
    if (response && evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        int copy = evt->data_len;
        if (response->resp_len + copy >= (int)sizeof(response->resp_buf) - 1)
            copy = (int)sizeof(response->resp_buf) - 1 - response->resp_len;
        if (copy > 0) {
            memcpy(response->resp_buf + response->resp_len, evt->data, copy);
            response->resp_len += copy;
            response->resp_buf[response->resp_len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t request_bind_code(const char *device_id, bind_code_result_t *result)
{
    char url[256];
    snprintf(url, sizeof(url), BIND_CODE_URL_FMT, device_id);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &result->response,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Bind code request failed, HTTP %d", status_code);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(result->response.resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse bind code response");
        return ESP_FAIL;
    }
    cJSON *code    = cJSON_GetObjectItem(root, "bind_code");
    cJSON *expires = cJSON_GetObjectItem(root, "expires_at");
    if (cJSON_IsString(code))
        strncpy(result->bind_code, code->valuestring, sizeof(result->bind_code) - 1);
    if (cJSON_IsNumber(expires))
        result->expires_at = (int)expires->valuedouble;
    result->success = result->bind_code[0] != '\0';
    cJSON_Delete(root);

    if (!result->success) {
        ESP_LOGE(TAG, "bind_code missing in response");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Bind code: %s (expires %d)", result->bind_code, result->expires_at);
    return ESP_OK;
}

static esp_err_t request_bind_status(const char *device_id, bind_status_result_t *result)
{
    char url[256];
    snprintf(url, sizeof(url), BIND_STATUS_URL_FMT, device_id);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = &result->response,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Bind status request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status_code == 404) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "Bind status request failed, HTTP %d", status_code);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(result->response.resp_buf);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse bind status response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *bound = cJSON_GetObjectItem(root, "bound");
    if (!cJSON_IsBool(bound)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Bind status response missing bound field");
        return ESP_ERR_INVALID_RESPONSE;
    }

    result->bound = cJSON_IsTrue(bound);
    result->success = true;
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── SoftAP provisioning mode entry ──────────────────────────────────────────── */

static void enter_softap_prov_mode(const char *device_id)
{
    /* SoftAP name: "SmartFrame-" + last 4 hex chars of device_id */
    char ap_name[32];
    size_t id_len = strlen(device_id);
    const char *suffix = id_len >= 4 ? device_id + id_len - 4 : device_id;
    snprintf(ap_name, sizeof(ap_name), "SmartFrame-%s", suffix);

    ESP_LOGI(TAG, "SoftAP prov mode: device_id=%s ap=%s", device_id, ap_name);

    /* Init e-paper */
    epaper_port_init();
    if (!epaper_init_buffer()) {
        ESP_LOGE(TAG, "Display init failed in prov mode");
    }

    /* Start SoftAP provisioning */
    s_prov_evt = xEventGroupCreate();
    if (softap_prov_start(ap_name, NULL, on_prov_credentials, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed — halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    const char *ap_ip = softap_prov_get_ap_ip();
    ESP_LOGI(TAG, "SoftAP started: IP=%s", ap_ip);

    /* Draw initial screen */
    draw_softap_screen_locked(device_id, ap_name, ap_ip, NULL, SOFTAP_STATE_IDLE);
    ESP_LOGI(TAG, "Provisioning screen shown");

    /* Block until credentials arrive via HTTP */
    ESP_LOGI(TAG, "Waiting for WiFi credentials via HTTP...");
    xEventGroupWaitBits(s_prov_evt, PROV_CREDS_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Credentials received: ssid=%s", s_prov_creds.ssid);
    vEventGroupDelete(s_prov_evt);
    s_prov_evt = NULL;

    /* Update screen: connecting */
    draw_softap_screen_locked(device_id, ap_name, ap_ip, NULL, SOFTAP_STATE_CONNECTING);

    /*
     * Stop SoftAP first, then re-enter the official STA init path.
     * Switching directly from a running SoftAP/APSTA session to STA caused
     * esp_netif/DHCP instability on the target board.
     */
    softap_prov_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t ret = wifi_sta_connect(s_prov_creds.ssid, s_prov_creds.password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        draw_softap_screen_locked(device_id, ap_name, ap_ip, NULL, SOFTAP_STATE_FAILED);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG, "WiFi connected");

    /* Prepare config — Wi-Fi comes from provisioning, MQTT defaults can be overridden by the page */
    frame_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        ESP_LOGE(TAG, "Provisioning config alloc failed");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    strncpy(cfg->wifi_ssid,       s_prov_creds.ssid,     sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_password,   s_prov_creds.password, sizeof(cfg->wifi_password) - 1);
    strncpy(cfg->device_id,       device_id,             sizeof(cfg->device_id) - 1);
    apply_default_mqtt_config(cfg);
    apply_custom_mqtt_config(cfg, &s_prov_creds);
    cfg->poll_interval      = 30;
    cfg->mqtt_keepalive_sec = 60;
    cfg->bind_status        = BIND_STATUS_PENDING;

    ESP_LOGI(TAG, "Provisioning MQTT config: mode=%s broker=%s user=%s",
             s_prov_creds.use_custom_mqtt ? "custom" : "default",
             cfg->mqtt_broker_url,
             cfg->mqtt_username[0] ? cfg->mqtt_username : "(empty)");

    /* Request bind code from server */
    draw_softap_screen_locked(device_id, ap_name, ap_ip, NULL, SOFTAP_STATE_GETTING_BIND_CODE);
    bind_code_result_t *bind_result = calloc(1, sizeof(*bind_result));
    if (!bind_result) {
        ESP_LOGE(TAG, "Bind result alloc failed");
        free(cfg);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    ret = request_bind_code(device_id, bind_result);
    if (ret == ESP_OK && bind_result->bind_code[0]) {
        strncpy(cfg->bind_code, bind_result->bind_code, sizeof(cfg->bind_code) - 1);
        cfg->bind_code_expires = bind_result->expires_at;
        ESP_LOGI(TAG, "Bind code obtained: %s", cfg->bind_code);
        draw_softap_screen_locked(device_id, ap_name, ap_ip, cfg->bind_code, SOFTAP_STATE_CONNECTED);
    } else {
        ESP_LOGW(TAG, "Failed to get bind code — continuing without it");
    }
    free(bind_result);

    /* Save config to SD card */
    if (frame_config_save(cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config.txt!");
    }
    free(cfg);

    /* Wait a bit for user to see bind code, then stop SoftAP */
    vTaskDelay(pdMS_TO_TICKS(10000));
    softap_prov_stop();

    ESP_LOGI(TAG, "Provisioning complete — rebooting");
    esp_restart();
}

/* ── Normal photo mode ──────────────────────────────────────────────────────── */

static bool has_mqtt_config(const frame_config_t *cfg)
{
    return cfg->mqtt_broker_url[0] != '\0';
}
static bool has_http_config(const frame_config_t *cfg)
{
    return cfg->server_url[0] != '\0';
}
static bool has_photo_source(const frame_config_t *cfg)
{
    return has_mqtt_config(cfg) || has_http_config(cfg);
}
static bool is_direct_url_mode(const frame_config_t *cfg)
{
    return has_http_config(cfg) && cfg->device_id[0] == '\0';
}

static bool requires_initial_binding(const frame_config_t *cfg)
{
    return cfg && (cfg->bind_status == BIND_STATUS_PENDING || cfg->bind_status == BIND_STATUS_EXPIRED);
}

static void invalidate_cached_photo(void)
{
    if (remove(FRAME_PHOTO_PATH) == 0) {
        ESP_LOGI(TAG, "Cached photo invalidated while waiting for first bind");
    }
}

static void show_bind_waiting_screen(const frame_config_t *cfg)
{
    softap_state_t state = SOFTAP_STATE_WAITING_BIND;

    if (!cfg) {
        return;
    }

    if (cfg->bind_code[0] == '\0') {
        state = SOFTAP_STATE_GETTING_BIND_CODE;
    }

    draw_softap_screen_locked(cfg->device_id,
                              "",
                              "",
                              cfg->bind_code[0] ? cfg->bind_code : NULL,
                              state);
}

static bool refresh_bind_status_from_cloud(frame_config_t *cfg)
{
    bind_status_result_t status_result = {0};
    esp_err_t ret;

    if (!cfg || cfg->device_id[0] == '\0') {
        return false;
    }

    ret = request_bind_status(cfg->device_id, &status_result);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "Bind status not found yet for device %s", cfg->device_id);
        return false;
    }
    if (ret != ESP_OK || !status_result.success || !status_result.bound) {
        return false;
    }

    memset(cfg->bind_code, 0, sizeof(cfg->bind_code));
    cfg->bind_code_expires = 0;
    cfg->bind_status = BIND_STATUS_BOUND;
    if (frame_config_save(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist BIND_STATUS_BOUND to config.txt");
    }

    ESP_LOGI(TAG, "Device %s confirmed as bound by cloud", cfg->device_id);
    return true;
}

static void on_photo_ready(const char *path, void *user_ctx);

static void wait_for_bind_completion(frame_config_t *cfg, bool use_mqtt_wait)
{
    if (use_mqtt_wait) {
        if (!s_bind_evt) {
            ESP_LOGE(TAG, "MQTT bind wait requested without event group");
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }

        /* Poll every 30 s; also exit immediately if MQTT fires BIND_CONFIRMED_BIT */
        const TickType_t poll_ticks = pdMS_TO_TICKS(30 * 1000);
        while (1) {
            ESP_LOGI(TAG, "Waiting for MQTT device_bound event (30 s timeout)...");
            EventBits_t bits = xEventGroupWaitBits(s_bind_evt,
                                                   BIND_CONFIRMED_BIT,
                                                   pdTRUE,
                                                   pdTRUE,
                                                   poll_ticks);
            if (bits & BIND_CONFIRMED_BIT) {
                vTaskDelay(pdMS_TO_TICKS(BIND_STATE_SETTLE_MS));
                ESP_LOGI(TAG, "Initial bind complete via MQTT, rebooting into photo mode");
                esp_restart();
            }
            /* MQTT timed out — fall back to HTTP bind status check */
            ESP_LOGI(TAG, "MQTT bind timeout, checking bind status via HTTP...");
            if (refresh_bind_status_from_cloud(cfg)) {
                draw_softap_screen_locked(cfg->device_id, "", "", NULL, SOFTAP_STATE_BOUND);
                vTaskDelay(pdMS_TO_TICKS(BIND_STATE_SETTLE_MS));
                ESP_LOGI(TAG, "Bind confirmed via HTTP fallback, rebooting");
                esp_restart();
            }
            /* Still not bound — also try fetching latest photo directly */
            if (cfg->server_url[0] != '\0' && cfg->device_id[0] != '\0') {
                ESP_LOGI(TAG, "Attempting HTTP photo fetch as bind fallback...");
                if (photo_client_fetch(cfg->server_url, cfg->device_id) == ESP_OK) {
                    on_photo_ready(photo_client_current_path(), cfg);
                }
            }
        }
    }

    while (1) {
        if (refresh_bind_status_from_cloud(cfg)) {
            draw_softap_screen_locked(cfg->device_id, "", "", NULL, SOFTAP_STATE_BOUND);
            vTaskDelay(pdMS_TO_TICKS(BIND_STATE_SETTLE_MS));
            ESP_LOGI(TAG, "Initial bind complete, rebooting into photo mode");
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(BIND_STATUS_POLL_INTERVAL_SEC * 1000));
    }
}

static void on_photo_ready(const char *path, void *user_ctx)
{
    frame_config_t *cfg = (frame_config_t *)user_ctx;

    display_photo(path);

    if (!cfg || cfg->bind_status == BIND_STATUS_BOUND) {
        return;
    }

    memset(cfg->bind_code, 0, sizeof(cfg->bind_code));
    cfg->bind_code_expires = 0;
    cfg->bind_status = BIND_STATUS_BOUND;

    if (frame_config_save(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist BIND_STATUS_BOUND from first photo");
    }

    if (s_bind_evt) {
        xEventGroupSetBits(s_bind_evt, BIND_CONFIRMED_BIT);
    }

    ESP_LOGI(TAG, "First photo received during bind wait, treating device as bound");
}

static void on_bind_code_received(const char *bind_code, int expires_in, int timestamp, void *user_ctx)
{
    frame_config_t *cfg = (frame_config_t *)user_ctx;

    if (!cfg || !bind_code || bind_code[0] == '\0') {
        return;
    }
    if (cfg->bind_status == BIND_STATUS_BOUND) {
        ESP_LOGW(TAG, "Ignoring dyn_bound_code because device is already marked bound");
        return;
    }

    memset(cfg->bind_code, 0, sizeof(cfg->bind_code));
    strncpy(cfg->bind_code, bind_code, sizeof(cfg->bind_code) - 1);
    cfg->bind_status = BIND_STATUS_PENDING;
    cfg->bind_code_expires = (timestamp > 0 && expires_in > 0)
        ? (timestamp + expires_in)
        : 0;

    if (frame_config_save(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist dyn_bound_code to config.txt");
    }

    ESP_LOGI(TAG, "dyn_bound_code received: code=%s expires_in=%d timestamp=%d",
             cfg->bind_code, expires_in, timestamp);

    invalidate_cached_photo();
    show_bind_waiting_screen(cfg);
}

static void on_bound_received(void *user_ctx)
{
    frame_config_t *cfg = (frame_config_t *)user_ctx;

    if (!cfg) {
        return;
    }
    if (cfg->bind_status == BIND_STATUS_BOUND) {
        ESP_LOGI(TAG, "Ignoring duplicate device_bound event");
        return;
    }

    memset(cfg->bind_code, 0, sizeof(cfg->bind_code));
    cfg->bind_code_expires = 0;
    cfg->bind_status = BIND_STATUS_BOUND;

    if (frame_config_save(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist BIND_STATUS_BOUND from MQTT event");
    }

    draw_softap_screen_locked(cfg->device_id, "", "", NULL, SOFTAP_STATE_BOUND);

    if (s_bind_evt) {
        xEventGroupSetBits(s_bind_evt, BIND_CONFIRMED_BIT);
    }

    ESP_LOGI(TAG, "MQTT confirmed device bound");
}

static void photo_poll_task(void *arg)
{
    frame_config_t *cfg = arg;
    while (1) {
        esp_err_t ret = photo_client_fetch(cfg->server_url, cfg->device_id);
        if (ret == ESP_OK)
            display_photo(FRAME_PHOTO_PATH);
        else if (ret != ESP_ERR_NOT_FOUND)
            ESP_LOGW(TAG, "Fetch failed, retry in %ds", cfg->poll_interval);
        vTaskDelay(pdMS_TO_TICKS(cfg->poll_interval * 1000));
    }
}

static void show_cached_photo_or_clear(void)
{
    FILE *f = fopen(FRAME_PHOTO_PATH, "rb");
    if (f) {
        fclose(f);
        display_photo(FRAME_PHOTO_PATH);
    } else {
        if (s_img_buf && take_display_lock()) {
            epaper_port_clear(s_img_buf, EPD_7IN3E_WHITE);
            give_display_lock();
        }
    }
}

/* ── app_main ───────────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Network + event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* PMU */
    ret = axp2101_board_init();
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "AXP2101 init failed: %s", esp_err_to_name(ret));

    /* SD card */
    if (!_sdcard_init()) {
        ESP_LOGE(TAG, "SD card init failed — halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Try to load config */
    frame_config_t *cfg = frame_config_load();

    if (!cfg) {
        /* No valid config — enter SoftAP provisioning */
        const char *device_id = frame_config_device_id_from_efuse();
        enter_softap_prov_mode(device_id); /* never returns (reboots) */
    }

    /* ── Normal operation ── */
    if (wifi_sta_connect(cfg->wifi_ssid, cfg->wifi_password) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed — halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (cfg->bind_status != BIND_STATUS_BOUND && !has_mqtt_config(cfg)) {
        refresh_bind_status_from_cloud(cfg);
    }

    if (!has_photo_source(cfg)) {
        ESP_LOGI(TAG, "WiFi-only mode (no photo source configured)");
        while (1) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    epaper_port_init();
    if (!epaper_init_buffer()) {
        ESP_LOGE(TAG, "Display buffer init failed — halting");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (requires_initial_binding(cfg)) {
        mqtt_photo_client_config_t bind_only_cfg = {
            .broker_url             = cfg->mqtt_broker_url,
            .username               = cfg->mqtt_username[0]  ? cfg->mqtt_username  : NULL,
            .password               = cfg->mqtt_password[0]  ? cfg->mqtt_password  : NULL,
            .topic                  = cfg->mqtt_topic[0]     ? cfg->mqtt_topic     : NULL,
            .client_id              = cfg->mqtt_client_id[0] ? cfg->mqtt_client_id : NULL,
            .photo_base_url         = cfg->server_url[0]     ? cfg->server_url     : NULL,
            .device_id              = cfg->device_id[0]      ? cfg->device_id      : NULL,
            .keepalive_sec          = cfg->mqtt_keepalive_sec,
            .subscribe_photo_topic  = true,
        };

        invalidate_cached_photo();
        show_bind_waiting_screen(cfg);

        if (has_mqtt_config(cfg)) {
            s_bind_evt = xEventGroupCreate();
            if (!s_bind_evt) {
                ESP_LOGE(TAG, "Bind event group alloc failed");
                while (1) vTaskDelay(pdMS_TO_TICKS(1000));
            }

            ret = mqtt_photo_client_start(&bind_only_cfg,
                                          on_photo_ready,
                                          on_bind_code_received,
                                          on_bound_received,
                                          cfg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "MQTT bind-only client start failed: %s", esp_err_to_name(ret));
                while (1) vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        ESP_LOGI(TAG, "Waiting for initial cloud bind before enabling photo display");
        wait_for_bind_completion(cfg, has_mqtt_config(cfg));
    }

    show_cached_photo_or_clear();

    if (has_mqtt_config(cfg)) {
        mqtt_photo_client_config_t mcfg = {
            .broker_url            = cfg->mqtt_broker_url,
            .username              = cfg->mqtt_username[0]  ? cfg->mqtt_username  : NULL,
            .password              = cfg->mqtt_password[0]  ? cfg->mqtt_password  : NULL,
            .topic                 = cfg->mqtt_topic[0]     ? cfg->mqtt_topic     : NULL,
            .client_id             = cfg->mqtt_client_id[0] ? cfg->mqtt_client_id : NULL,
            .photo_base_url        = cfg->server_url[0]     ? cfg->server_url     : NULL,
            .device_id             = cfg->device_id[0]      ? cfg->device_id      : NULL,
            .keepalive_sec         = cfg->mqtt_keepalive_sec,
            .subscribe_photo_topic = true,
        };
        ret = mqtt_photo_client_start(&mcfg,
                                      on_photo_ready,
                                      on_bind_code_received,
                                      on_bound_received,
                                      cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MQTT client start failed: %s", esp_err_to_name(ret));
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ESP_LOGI(TAG, "MQTT photo mode running");
        while (1) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    if (is_direct_url_mode(cfg)) {
        if (photo_client_fetch(cfg->server_url, NULL) == ESP_OK)
            display_photo(FRAME_PHOTO_PATH);
        while (1) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    xTaskCreate(photo_poll_task, "photo_poll", 8 * 1024, cfg, 3, NULL);
}
