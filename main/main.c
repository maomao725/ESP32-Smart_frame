#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "sdcard_bsp.h"
#include "axp2101_bsp.h"
#include "epaper_port.h"
#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "frame_config.h"
#include "wifi_sta.h"
#include "photo_client.h"

/* AP config mode (reused from original server_bsp) */
#include "server_bsp.h"

static const char *TAG = "smart_frame";

/* Image buffer: prefer PSRAM so Wi-Fi/HTTP can keep internal RAM */
static uint8_t  *s_img_buf  = NULL;
static uint32_t  s_img_size = 0;

static void log_heap_status(const char *stage) {
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t spiram_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "%s heap: internal=%u, spiram=%u",
             stage, (unsigned)internal_free, (unsigned)spiram_free);
}

static uint8_t *alloc_display_buffer(size_t size) {
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
        ESP_LOGI(TAG, "Image buffer ready: %lu bytes (PSRAM)", (unsigned long)size);
        return buf;
    }

    buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf) {
        ESP_LOGW(TAG, "PSRAM allocation failed, image buffer fell back to internal RAM: %lu bytes",
                 (unsigned long)size);
    }
    return buf;
}

static void epaper_init_buffer(void) {
    s_img_size = ((EXAMPLE_LCD_WIDTH % 2 == 0)
                  ? (EXAMPLE_LCD_WIDTH / 2)
                  : (EXAMPLE_LCD_WIDTH / 2 + 1)) * EXAMPLE_LCD_HEIGHT;

    log_heap_status("Before display buffer alloc");
    s_img_buf = alloc_display_buffer(s_img_size);
    if (!s_img_buf) {
        ESP_LOGE(TAG, "Failed to allocate image buffer: %lu bytes", (unsigned long)s_img_size);
        return;
    }
    memset(s_img_buf, 0x11, s_img_size);
    log_heap_status("After display buffer alloc");
    Paint_NewImage(s_img_buf, EXAMPLE_LCD_WIDTH, EXAMPLE_LCD_HEIGHT, 0, EPD_7IN3E_WHITE);
    Paint_SetScale(6);
    Paint_SelectImage(s_img_buf);
    Paint_SetRotate(180);
}

static void display_photo(const char *path) {
    if (!s_img_buf) return;
    GUI_ReadBmp_RGB_6Color(path, 0, 0);
    epaper_port_display(s_img_buf);
    ESP_LOGI(TAG, "Display refreshed: %s", path);
}

static bool has_photo_source_config(const frame_config_t *cfg) {
    return cfg->server_url[0] != '\0';
}

static bool is_direct_photo_url_mode(const frame_config_t *cfg) {
    return cfg->server_url[0] != '\0' && cfg->device_id[0] == '\0';
}

static void enter_ap_config_mode(void) {
    ESP_LOGI(TAG, "No config found — entering AP config mode");
    ESP_LOGI(TAG, "Connect to WiFi: esp_network / 1234567890");
    ESP_LOGI(TAG, "Then open http://192.168.4.1/index.html");
    Network_wifi_ap_init();
    http_server_init();
    /* Block forever — user must reboot after saving config */
    while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
}

static void photo_poll_task(void *arg) {
    frame_config_t *cfg = (frame_config_t *)arg;
    while (1) {
        esp_err_t ret = photo_client_fetch(cfg->server_url, cfg->device_id);
        if (ret == ESP_OK) {
            display_photo(FRAME_PHOTO_PATH);
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "No new photo, keeping current display");
        } else {
            ESP_LOGW(TAG, "Fetch failed, will retry in %ds", cfg->poll_interval);
        }
        vTaskDelay(pdMS_TO_TICKS(cfg->poll_interval * 1000));
    }
}

void app_main(void) {
    /* NVS init (required by WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* netif + event loop — init once here, shared by both STA and AP paths */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Board PMU init: vendor hardware powers panel rails through AXP2101 */
    ret = axp2101_board_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 init failed, continuing without PMU setup: %s",
                 esp_err_to_name(ret));
    }

    /* 1. SD card */
    if (!_sdcard_init()) {
        ESP_LOGE(TAG, "SD card init failed — halting");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* 2. Load config */
    frame_config_t *cfg = frame_config_load();
    if (!cfg) {
        enter_ap_config_mode(); /* never returns */
    }

    /* 3. Connect WiFi */
    if (wifi_sta_connect(cfg->wifi_ssid, cfg->wifi_password) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed — halting");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (!has_photo_source_config(cfg)) {
        ESP_LOGI(TAG, "WiFi test mode enabled");
        ESP_LOGI(TAG, "Connected to SSID: %s", cfg->wifi_ssid);
        ESP_LOGI(TAG, "server_url or device_id not set, skipping server/photo logic");
        while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
    }

    /* 4. Init e-paper */
    epaper_port_init();
    epaper_init_buffer();
    if (!s_img_buf) {
        ESP_LOGE(TAG, "Display buffer init failed - halting");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (is_direct_photo_url_mode(cfg)) {
        ESP_LOGI(TAG, "Direct photo URL mode enabled");
        if (photo_client_fetch(cfg->server_url, NULL) == ESP_OK) {
            display_photo(FRAME_PHOTO_PATH);
        } else {
            ESP_LOGE(TAG, "Direct photo fetch failed");
        }
        while (1) { vTaskDelay(pdMS_TO_TICKS(5000)); }
    }

    /* 5. Show current photo if exists, else clear screen */
    FILE *f = fopen(FRAME_PHOTO_PATH, "rb");
    if (f) {
        fclose(f);
        display_photo(FRAME_PHOTO_PATH);
    } else {
        epaper_port_clear(s_img_buf, EPD_7IN3E_WHITE);
    }

    /* 6. Start polling task */
    xTaskCreate(photo_poll_task, "photo_poll", 8 * 1024, cfg, 3, NULL);
}
