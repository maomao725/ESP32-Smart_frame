#include "photo_client.h"
#include "sdcard_bsp.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdint.h>

static const char *TAG = "photo_client";

#define PHOTO_HTTP_CLIENT_BUF_SIZE 2048
#define PHOTO_HTTP_RX_BUF_SIZE     4096
#define PHOTO_FILE_IO_BUF_SIZE     2048
#define PHOTO_HTTP_TIMEOUT_MS     15000    /* socket-level recv timeout */
#define PHOTO_FETCH_MAX_ATTEMPTS  3
#define PHOTO_PROGRESS_LOG_BYTES  102400   /* log every 100 KB */
#define PHOTO_STALL_TIMEOUT_MS    20000    /* watchdog: abort if no progress for 20s */

/* Watchdog shared state — written by download task, read by watchdog task */
typedef struct {
    volatile size_t   bytes_written;   /* updated on every fwrite */
    volatile bool     done;            /* set true when download loop exits */
    volatile bool     wd_fired;        /* set true when watchdog force-closed client */
    esp_http_client_handle_t client;   /* closed by watchdog to unblock read */
} dl_watchdog_ctx_t;

static void download_watchdog_task(void *arg) {
    dl_watchdog_ctx_t *ctx = (dl_watchdog_ctx_t *)arg;
    size_t last_bytes = 0;
    const TickType_t check_interval = pdMS_TO_TICKS(PHOTO_STALL_TIMEOUT_MS);

    while (!ctx->done) {
        vTaskDelay(check_interval);
        if (ctx->done) break;
        if (ctx->bytes_written == last_bytes) {
            ESP_LOGW(TAG, "Download stalled at %u bytes — forcing abort",
                     (unsigned)ctx->bytes_written);
            ctx->wd_fired = true;
            esp_http_client_cleanup(ctx->client);
            break;
        }
        last_bytes = ctx->bytes_written;
    }
    vTaskDelete(NULL);
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static bool header_contains_token(const char *header, const char *token) {
    return header && token && strstr(header, token) != NULL;
}

static bool is_retryable_http_error(esp_err_t err) {
    return err == ESP_FAIL
        || err == ESP_ERR_HTTP_CONNECT
        || err == ESP_ERR_HTTP_FETCH_HEADER
        || err == ESP_ERR_HTTP_CONNECTION_CLOSED
        || err == ESP_ERR_HTTP_READ_TIMEOUT
        || err == ESP_ERR_HTTP_INCOMPLETE_DATA;
}

static void log_heap_status(const char *stage) {
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t spiram_free   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "%s heap: internal=%u, spiram=%u",
             stage, (unsigned)internal_free, (unsigned)spiram_free);
}

static void *alloc_stream_buffer(size_t size, const char *label) {
    void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) {
        ESP_LOGI(TAG, "%s allocated in PSRAM: %u bytes", label, (unsigned)size);
        return buf;
    }

    buf = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf) {
        ESP_LOGW(TAG, "%s fell back to internal RAM: %u bytes", label, (unsigned)size);
    }
    return buf;
}

static esp_err_t validate_bmp_file(const char *path, size_t actual_size) {
    uint8_t header[54] = {0};
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot reopen %s for validation", path);
        return ESP_FAIL;
    }

    size_t header_len = fread(header, 1, sizeof(header), fp);
    fclose(fp);
    if (header_len < sizeof(header)) {
        ESP_LOGE(TAG, "BMP header too short: %zu bytes", header_len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (header[0] != 'B' || header[1] != 'M') {
        ESP_LOGE(TAG, "Downloaded file is not a BMP");
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t file_size   = read_le32(&header[2]);
    uint32_t pixel_offset = read_le32(&header[10]);
    uint32_t dib_size    = read_le32(&header[14]);
    uint32_t width       = read_le32(&header[18]);
    uint32_t height      = read_le32(&header[22]);
    uint16_t bit_count   = read_le16(&header[28]);
    uint32_t compression = read_le32(&header[30]);

    if (dib_size < 40 || pixel_offset < 54) {
        ESP_LOGE(TAG, "Unsupported BMP header: dib=%" PRIu32 ", offset=%" PRIu32, dib_size, pixel_offset);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (bit_count != 24 || compression != 0) {
        ESP_LOGE(TAG, "Unsupported BMP format: %u-bit compression=%" PRIu32, bit_count, compression);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (width == 0 || height == 0) {
        ESP_LOGE(TAG, "Invalid BMP dimensions: %" PRIu32 "x%" PRIu32, width, height);
        return ESP_ERR_INVALID_SIZE;
    }

    if (file_size != 0 && file_size != actual_size) {
        ESP_LOGW(TAG, "BMP header size mismatch: header=%" PRIu32 ", actual=%u",
                 file_size, (unsigned)actual_size);
    }

    ESP_LOGI(TAG, "BMP validated: %" PRIu32 "x%" PRIu32 ", %u-bit, %u bytes",
             width, height, bit_count, (unsigned)actual_size);
    return ESP_OK;
}

static esp_err_t download_once(const char *url) {
    const char *tmp_path = "/sdcard/incoming.bmp";
    esp_err_t err = ESP_OK;
    esp_http_client_handle_t client = NULL;
    uint8_t *rx_buf = NULL;
    char *file_buf = NULL;
    FILE *fp = NULL;
    size_t total_written = 0;
    dl_watchdog_ctx_t wd_ctx = { .bytes_written = 0, .done = false, .wd_fired = false, .client = NULL };
    TaskHandle_t wd_task_handle = NULL;
    int status = 0;
    int64_t fetch_len = 0;
    int64_t content_length = 0;
    bool chunked = false;

    remove(tmp_path);

    esp_http_client_config_t config = {
        .url                   = url,
        .timeout_ms            = PHOTO_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size           = PHOTO_HTTP_CLIENT_BUF_SIZE,
        .buffer_size_tx        = 1024,
        .keep_alive_enable     = false,
    };

    log_heap_status("Before HTTP open");
    client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    fetch_len = esp_http_client_fetch_headers(client);
    if (fetch_len < 0 && fetch_len != -ESP_ERR_HTTP_EAGAIN) {
        err = (esp_err_t)(-fetch_len);
        ESP_LOGE(TAG, "HTTP fetch headers failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    status = esp_http_client_get_status_code(client);
    content_length = esp_http_client_get_content_length(client);
    chunked = esp_http_client_is_chunked_response(client);

    char *content_type = NULL;
    if (esp_http_client_get_header(client, "Content-Type", &content_type) != ESP_OK) {
        content_type = NULL;
    }

    ESP_LOGI(TAG, "HTTP %d, content-type=%s, content-length=%lld, chunked=%s",
             status,
             content_type ? content_type : "(unknown)",
             (long long)content_length,
             chunked ? "yes" : "no");

    if (status == 304) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (status == 404) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (status != 200) {
        err = ESP_FAIL;
        goto cleanup;
    }

    if (content_type && header_contains_token(content_type, "image/") && !header_contains_token(content_type, "bmp")) {
        ESP_LOGE(TAG, "Unsupported content-type for e-paper pipeline: %s", content_type);
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    fp = fopen(tmp_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open %s for writing", tmp_path);
        err = ESP_FAIL;
        goto cleanup;
    }
    file_buf = (char *)alloc_stream_buffer(PHOTO_FILE_IO_BUF_SIZE, "File I/O buffer");
    if (file_buf && setvbuf(fp, file_buf, _IOFBF, PHOTO_FILE_IO_BUF_SIZE) != 0) {
        ESP_LOGW(TAG, "setvbuf failed, using default stdio buffering");
        heap_caps_free(file_buf);
        file_buf = NULL;
    }

    /* Start stall watchdog — it will force-close client if no progress for 20s */
    wd_ctx.client = client;
    xTaskCreate(download_watchdog_task, "dl_wd", 2048, &wd_ctx, 5, &wd_task_handle);

    rx_buf = (uint8_t *)alloc_stream_buffer(PHOTO_HTTP_RX_BUF_SIZE, "HTTP RX buffer");
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate HTTP RX buffer");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    log_heap_status("After download buffer alloc");

    while (1) {
        int read_len = esp_http_client_read(client, (char *)rx_buf, PHOTO_HTTP_RX_BUF_SIZE);
        if (read_len > 0) {
            size_t written = fwrite(rx_buf, 1, (size_t)read_len, fp);
            if (written != (size_t)read_len) {
                ESP_LOGE(TAG, "SD write error (%zu/%d)", written, read_len);
                err = ESP_FAIL;
                goto cleanup;
            }
            total_written += written;
            wd_ctx.bytes_written = total_written;  /* update watchdog */

            /* Progress log every 100 KB */
            size_t prev = total_written - written;
            if (total_written / PHOTO_PROGRESS_LOG_BYTES != prev / PHOTO_PROGRESS_LOG_BYTES) {
                ESP_LOGI(TAG, "Download progress: %u / %lld bytes",
                         (unsigned)total_written, (long long)content_length);
            }
            continue;
        }

        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            ESP_LOGE(TAG, "HTTP body ended early: %u/%lld bytes",
                     (unsigned)total_written,
                     (long long)content_length);
            err = ESP_ERR_HTTP_INCOMPLETE_DATA;
            goto cleanup;
        }

        if (read_len == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }

        ESP_LOGE(TAG, "HTTP read failed: %d", read_len);
        err = ESP_FAIL;
        goto cleanup;
    }

    wd_ctx.done = true;  /* signal watchdog to exit */

    fflush(fp);
    fclose(fp);
    fp = NULL;   /* closed — FATFS now commits directory entry with correct file size */

    if (total_written == 0) {
        ESP_LOGE(TAG, "Zero bytes received from server");
        err = ESP_FAIL;
        goto cleanup;
    }

    if (content_length > 0 && (int64_t)total_written != content_length) {
        ESP_LOGE(TAG, "Content-Length mismatch: %u/%lld bytes",
                 (unsigned)total_written,
                 (long long)content_length);
        err = ESP_ERR_HTTP_INCOMPLETE_DATA;
        goto cleanup;
    }

    err = validate_bmp_file(tmp_path, total_written);
    if (err != ESP_OK) {
        goto cleanup;
    }

cleanup:
    wd_ctx.done = true;
    if (wd_task_handle && !wd_ctx.wd_fired) {
        /* Watchdog hasn't fired yet — safe to delete it */
        vTaskDelete(wd_task_handle);
        wd_task_handle = NULL;
    } else if (wd_ctx.wd_fired) {
        /* Watchdog fired and called vTaskDelete(NULL) on itself — already gone */
        vTaskDelay(pdMS_TO_TICKS(10)); /* brief yield to let it finish */
        wd_task_handle = NULL;
    }
    if (fp) {
        fclose(fp);
    }
    if (file_buf) {
        heap_caps_free(file_buf);
    }
    if (rx_buf) {
        heap_caps_free(rx_buf);
    }
    if (client && !wd_ctx.wd_fired) {
        /* Watchdog didn't fire — we own the cleanup */
        esp_http_client_cleanup(client);
    }
    /* If wd_fired, watchdog already called cleanup — skip to avoid double-free */
    if (wd_ctx.wd_fired) {
        err = ESP_FAIL;  /* treat stall as retryable failure */
    }
    if (err != ESP_OK) {
        remove(tmp_path);
    }
    return err;
}

esp_err_t photo_client_fetch(const char *base_url, const char *device_id) {
    char url[320];
    if (device_id && device_id[0] != '\0') {
        snprintf(url, sizeof(url), "%s/%s/latest.bmp", base_url, device_id);
    } else {
        snprintf(url, sizeof(url), "%s", base_url);
    }
    ESP_LOGI(TAG, "Fetching: %s", url);

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= PHOTO_FETCH_MAX_ATTEMPTS; ++attempt) {
        err = download_once(url);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
            break;
        }

        ESP_LOGW(TAG, "Download attempt %d/%d failed: %s",
                 attempt, PHOTO_FETCH_MAX_ATTEMPTS, esp_err_to_name(err));
        if (attempt < PHOTO_FETCH_MAX_ATTEMPTS && is_retryable_http_error(err)) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        break;
    }

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No new photo available");
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Atomically replace current photo */
    remove("/sdcard/current.bmp");
    if (rename("/sdcard/incoming.bmp", "/sdcard/current.bmp") != 0) {
        ESP_LOGE(TAG, "Failed to rename incoming.bmp -> current.bmp");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Photo saved to /sdcard/current.bmp");
    return ESP_OK;
}
