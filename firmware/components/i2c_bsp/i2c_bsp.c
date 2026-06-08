#include "i2c_bsp.h"

#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define AXP2101_ADDR      0x34
#define FRAME_I2C_PORT    0
#define FRAME_I2C_SCL     48
#define FRAME_I2C_SDA     47
#define FRAME_I2C_SPEED   300000

static const char *TAG = "i2c_bsp";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_axp2101_dev = NULL;

static esp_err_t i2c_wait_idle(void) {
    if (!s_i2c_bus) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_bus_wait_all_done(s_i2c_bus, pdMS_TO_TICKS(1000));
}

esp_err_t i2c_master_Init(void) {
    if (s_i2c_bus && s_axp2101_dev) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = FRAME_I2C_PORT,
        .scl_io_num = FRAME_I2C_SCL,
        .sda_io_num = FRAME_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
                        "create I2C bus failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = FRAME_I2C_SPEED,
    };

    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_axp2101_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add AXP2101 device failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C bus ready: port=%d, SDA=%d, SCL=%d, speed=%d",
             FRAME_I2C_PORT, FRAME_I2C_SDA, FRAME_I2C_SCL, FRAME_I2C_SPEED);
    return ESP_OK;
}

esp_err_t i2c_axp2101_read(uint8_t reg, uint8_t *data, size_t len) {
    if (!s_axp2101_dev || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = i2c_wait_idle();
    if (err != ESP_OK) {
        return err;
    }

    return i2c_master_transmit_receive(
        s_axp2101_dev, &reg, 1, data, len, pdMS_TO_TICKS(1000));
}

esp_err_t i2c_axp2101_write(uint8_t reg, const uint8_t *data, size_t len) {
    if (!s_axp2101_dev || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = i2c_wait_idle();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *tx = (uint8_t *)malloc(len + 1);
    if (!tx) {
        return ESP_ERR_NO_MEM;
    }

    tx[0] = reg;
    memcpy(tx + 1, data, len);
    err = i2c_master_transmit(s_axp2101_dev, tx, len + 1, pdMS_TO_TICKS(5000));
    free(tx);
    return err;
}
