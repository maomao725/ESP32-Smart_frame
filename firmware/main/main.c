#include <stdbool.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
#include "esp_sntp.h"
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
#include "button_bsp.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "smart_frame";

/* ── Display buffer ─────────────────────────────────────────────────────────── */

static uint8_t  *s_img_buf  = NULL;
static uint32_t  s_img_size = 0;
static SemaphoreHandle_t s_display_lock = NULL;

typedef enum {
    PHOTO_MODE_MQTT_LIVE = 0,
    PHOTO_MODE_LOCAL_TIMED = 1,
} photo_mode_t;

#define LOCAL_PHOTO_DIR "/sdcard/LOCAL"
#define LOCAL_PHOTO_NAME_MAX 96
#define LOCAL_DEFAULT_INTERVAL_SEC 300
#define DISPLAY_MIN_STAY_SEC 20
#define LOCAL_MIN_INTERVAL_SEC DISPLAY_MIN_STAY_SEC
#define LOCAL_MAX_INTERVAL_SEC (24 * 3600)
#define LOCAL_COPY_BUF_SIZE 512

static volatile photo_mode_t s_photo_mode = PHOTO_MODE_MQTT_LIVE;
static volatile int s_local_interval_sec = LOCAL_DEFAULT_INTERVAL_SEC;
static volatile TickType_t s_next_local_refresh_tick = 0;
static volatile TickType_t s_last_display_refresh_tick = 0;
static volatile bool s_local_daily_enabled = false;
static volatile int s_local_daily_seconds = -1;
static volatile time_t s_next_daily_refresh_time = 0;
static size_t s_local_next_index = 0;
static uint32_t s_local_store_seq = 0;

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "%s heap: internal=%u spiram=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void start_time_sync(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    if (esp_sntp_enabled()) {
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP time sync started");
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

    TickType_t now = xTaskGetTickCount();
    TickType_t last = s_last_display_refresh_tick;
    const TickType_t min_stay_ticks = pdMS_TO_TICKS(DISPLAY_MIN_STAY_SEC * 1000);

    if (last != 0 && (now - last) < min_stay_ticks) {
        vTaskDelay(min_stay_ticks - (now - last));
    }

    Paint_Clear(EPD_7IN3E_WHITE);
    GUI_ReadBmp_RGB_6Color(path, 0, 0);
    epaper_port_display(s_img_buf);
    s_last_display_refresh_tick = xTaskGetTickCount();
    give_display_lock();
    ESP_LOGI(TAG, "Display refreshed: %s", path);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool is_bmp_filename(const char *name)
{
    size_t len;

    if (!name) {
        return false;
    }

    len = strlen(name);
    return len > 4
        && name[len - 4] == '.'
        && tolower((unsigned char)name[len - 3]) == 'b'
        && tolower((unsigned char)name[len - 2]) == 'm'
        && tolower((unsigned char)name[len - 1]) == 'p';
}

static esp_err_t ensure_local_photo_dir(void)
{
    if (mkdir(LOCAL_PHOTO_DIR, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to create local photo dir: %s errno=%d",
             LOCAL_PHOTO_DIR,
             errno);
    return ESP_FAIL;
}

static esp_err_t copy_file(const char *src_path, const char *dst_path)
{
    FILE *src = NULL;
    FILE *dst = NULL;
    uint8_t buf[LOCAL_COPY_BUF_SIZE];
    esp_err_t ret = ESP_FAIL;

    src = fopen(src_path, "rb");
    if (!src) {
        ESP_LOGE(TAG, "Failed to open source file: %s", src_path);
        goto cleanup;
    }

    dst = fopen(dst_path, "wb");
    if (!dst) {
        ESP_LOGE(TAG, "Failed to open destination file: %s", dst_path);
        goto cleanup;
    }

    while (1) {
        size_t len = fread(buf, 1, sizeof(buf), src);
        if (len > 0 && fwrite(buf, 1, len, dst) != len) {
            ESP_LOGE(TAG, "Failed to write local photo: %s", dst_path);
            goto cleanup;
        }

        if (len < sizeof(buf)) {
            ret = feof(src) ? ESP_OK : ESP_FAIL;
            goto cleanup;
        }
    }

cleanup:
    if (src) {
        fclose(src);
    }
    if (dst) {
        fclose(dst);
    }
    if (ret != ESP_OK) {
        remove(dst_path);
    }
    return ret;
}

static esp_err_t store_current_photo_to_local_pool(void)
{
    char dst_path[LOCAL_PHOTO_NAME_MAX];
    bool found_free_path = false;

    if (!file_exists(FRAME_PHOTO_PATH)) {
        return ESP_ERR_NOT_FOUND;
    }

    if (ensure_local_photo_dir() != ESP_OK) {
        return ESP_FAIL;
    }

    for (int i = 0; i < 10000; ++i) {
        snprintf(dst_path,
                 sizeof(dst_path),
                 LOCAL_PHOTO_DIR "/P%07" PRIu32 ".BMP",
                 ++s_local_store_seq);
        if (!file_exists(dst_path)) {
            found_free_path = true;
            break;
        }
    }

    if (!found_free_path) {
        ESP_LOGE(TAG, "Failed to find free local photo path in %s", LOCAL_PHOTO_DIR);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = copy_file(FRAME_PHOTO_PATH, dst_path);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Cached MQTT photo for local timed mode: %s", dst_path);
    }
    return ret;
}

static bool select_local_photo(char *out_path, size_t out_size)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    size_t count = 0;
    size_t target = 0;
    size_t index = 0;
    bool found = false;

    if (!out_path || out_size == 0 || ensure_local_photo_dir() != ESP_OK) {
        return false;
    }

    dir = opendir(LOCAL_PHOTO_DIR);
    if (!dir) {
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (is_bmp_filename(entry->d_name)) {
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        return false;
    }

    target = s_local_next_index % count;
    dir = opendir(LOCAL_PHOTO_DIR);
    if (!dir) {
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!is_bmp_filename(entry->d_name)) {
            continue;
        }

        if (index == target) {
            snprintf(out_path, out_size, LOCAL_PHOTO_DIR "/%s", entry->d_name);
            found = true;
            break;
        }
        index++;
    }

    closedir(dir);

    if (found) {
        s_local_next_index = (target + 1) % count;
    }
    return found;
}

static bool display_next_local_photo(void)
{
    char path[LOCAL_PHOTO_NAME_MAX];

    if (!select_local_photo(path, sizeof(path))) {
        ESP_LOGW(TAG, "Local timed mode has no BMP files in %s", LOCAL_PHOTO_DIR);
        return false;
    }

    display_photo(path);
    return true;
}

static bool is_wall_clock_ready(void)
{
    return time(NULL) > 1700000000;
}

static time_t next_daily_refresh_time(time_t now, int seconds_since_midnight)
{
    struct tm local_now;

    if (seconds_since_midnight < 0) {
        return 0;
    }

    localtime_r(&now, &local_now);
    local_now.tm_hour = seconds_since_midnight / 3600;
    local_now.tm_min = (seconds_since_midnight % 3600) / 60;
    local_now.tm_sec = seconds_since_midnight % 60;

    time_t target = mktime(&local_now);
    if (target <= now) {
        target += 24 * 3600;
    }
    return target;
}

static void schedule_next_daily_refresh(time_t now)
{
    if (!s_local_daily_enabled || !is_wall_clock_ready()) {
        s_next_daily_refresh_time = 0;
        return;
    }

    s_next_daily_refresh_time = next_daily_refresh_time(now, s_local_daily_seconds);
}

static void set_local_interval_seconds(int seconds)
{
    if (seconds < LOCAL_MIN_INTERVAL_SEC) {
        seconds = LOCAL_MIN_INTERVAL_SEC;
    } else if (seconds > LOCAL_MAX_INTERVAL_SEC) {
        seconds = LOCAL_MAX_INTERVAL_SEC;
    }

    s_local_interval_sec = seconds;
    s_local_daily_enabled = false;
    s_next_daily_refresh_time = 0;
    if (s_photo_mode == PHOTO_MODE_LOCAL_TIMED) {
        s_next_local_refresh_tick = xTaskGetTickCount() + pdMS_TO_TICKS(seconds * 1000);
    }
    ESP_LOGI(TAG, "Local timed refresh interval set to %ds", seconds);
}

static void set_local_daily_time_seconds(int seconds_since_midnight)
{
    if (seconds_since_midnight < 0) {
        seconds_since_midnight = 0;
    } else if (seconds_since_midnight >= 24 * 3600) {
        seconds_since_midnight = (24 * 3600) - 1;
    }

    s_local_daily_seconds = seconds_since_midnight;
    s_local_daily_enabled = true;
    s_next_local_refresh_tick = 0;
    schedule_next_daily_refresh(time(NULL));

    ESP_LOGI(TAG, "Local daily refresh time set to %02d:%02d:%02d, next=%lld",
             seconds_since_midnight / 3600,
             (seconds_since_midnight % 3600) / 60,
             seconds_since_midnight % 60,
             (long long)s_next_daily_refresh_time);
}

static void local_photo_timer_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_photo_mode == PHOTO_MODE_LOCAL_TIMED) {
            if (s_local_daily_enabled) {
                time_t now = time(NULL);

                if (is_wall_clock_ready()) {
                    if (s_next_daily_refresh_time == 0) {
                        schedule_next_daily_refresh(now);
                    }
                    if (s_next_daily_refresh_time > 0 && now >= s_next_daily_refresh_time) {
                        display_next_local_photo();
                        s_next_daily_refresh_time = next_daily_refresh_time(now, s_local_daily_seconds);
                    }
                }
            } else {
                TickType_t now = xTaskGetTickCount();
                TickType_t next = s_next_local_refresh_tick;

                if (next == 0 || now >= next) {
                    display_next_local_photo();
                    s_next_local_refresh_tick = now + pdMS_TO_TICKS(s_local_interval_sec * 1000);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void toggle_photo_mode(void)
{
    if (s_photo_mode == PHOTO_MODE_MQTT_LIVE) {
        s_photo_mode = PHOTO_MODE_LOCAL_TIMED;
        if (s_local_daily_enabled) {
            schedule_next_daily_refresh(time(NULL));
        } else {
            s_next_local_refresh_tick = xTaskGetTickCount();
        }
        ESP_LOGI(TAG, "Photo mode switched to LOCAL_TIMED; interval=%ds dir=%s",
                 s_local_interval_sec,
                 LOCAL_PHOTO_DIR);
        return;
    }

    s_photo_mode = PHOTO_MODE_MQTT_LIVE;
    s_next_local_refresh_tick = 0;
    ESP_LOGI(TAG, "Photo mode switched to MQTT_LIVE");
    if (file_exists(FRAME_PHOTO_PATH)) {
        display_photo(FRAME_PHOTO_PATH);
    }
}

static void mode_button_task(void *arg)
{
    (void)arg;

    xEventGroupClearBits(key_groups, set_bit_button(0) | set_bit_button(1) | set_bit_button(2));
    ESP_LOGI(TAG, "KEY1 long press toggles photo mode");

    while (1) {
        xEventGroupWaitBits(key_groups,
                            set_bit_button(1),
                            pdTRUE,
                            pdFALSE,
                            portMAX_DELAY);
        ESP_LOGI(TAG, "KEY1 long press detected");
        toggle_photo_mode();
    }
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

    esp_err_t store_ret = store_current_photo_to_local_pool();

    if (s_photo_mode == PHOTO_MODE_LOCAL_TIMED) {
        if (store_ret == ESP_OK) {
            ESP_LOGI(TAG, "Local timed mode: MQTT photo cached without display refresh");
        }
    } else {
        if (store_ret == ESP_OK) {
            ESP_LOGI(TAG, "MQTT live mode: photo cached for local timed mode");
        }
        display_photo(path);
    }

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

static void on_local_interval_received(int interval_seconds, void *user_ctx)
{
    (void)user_ctx;
    set_local_interval_seconds(interval_seconds);
}

static void on_local_daily_time_received(int seconds_since_midnight, void *user_ctx)
{
    (void)user_ctx;
    set_local_daily_time_seconds(seconds_since_midnight);
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
    start_time_sync();

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

    button_Init();
    xTaskCreate(mode_button_task, "mode_button", 4 * 1024, NULL, 3, NULL);
    xTaskCreate(local_photo_timer_task, "local_photo", 6 * 1024, NULL, 2, NULL);

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
                                          on_local_interval_received,
                                          on_local_daily_time_received,
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
                                      on_local_interval_received,
                                      on_local_daily_time_received,
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
