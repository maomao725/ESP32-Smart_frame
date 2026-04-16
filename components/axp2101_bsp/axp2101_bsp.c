#include "axp2101_bsp.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bsp.h"

#define AXP2101_REG_CHIP_ID              0x03
#define AXP2101_REG_CHG_GAUGE_WDT_CTRL   0x18
#define AXP2101_REG_SLEEP_WAKEUP_CTRL    0x26
#define AXP2101_REG_IRQ_OFF_ON_LEVEL     0x27
#define AXP2101_REG_ADC_CHANNEL_CTRL     0x30
#define AXP2101_REG_CV_CHG_VOL_SET       0x64
#define AXP2101_REG_CHGLED_SET_CTRL      0x69
#define AXP2101_REG_BTN_BAT_CHG_VOL_SET  0x6A
#define AXP2101_REG_DC_ONOFF_DVM_CTRL    0x80
#define AXP2101_REG_DC_VOL0_CTRL         0x82
#define AXP2101_REG_LDO_ONOFF_CTRL0      0x90
#define AXP2101_REG_LDO_VOL2_CTRL        0x94
#define AXP2101_REG_LDO_VOL3_CTRL        0x95
#define AXP2101_CHIP_ID                  0x4A

#define BIT0                             0x01
#define BIT1                             0x02
#define BIT2                             0x04
#define BIT3                             0x08

static const char *TAG = "axp2101_bsp";

static esp_err_t axp2101_read_u8(uint8_t reg, uint8_t *value) {
    return i2c_axp2101_read(reg, value, 1);
}

static esp_err_t axp2101_write_u8(uint8_t reg, uint8_t value) {
    return i2c_axp2101_write(reg, &value, 1);
}

static esp_err_t axp2101_update_bits(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t cur = 0;
    esp_err_t err = axp2101_read_u8(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t next = (cur & (uint8_t)~mask) | (value & mask);
    if (next == cur) {
        return ESP_OK;
    }
    return axp2101_write_u8(reg, next);
}

static esp_err_t axp2101_set_voltage_reg(uint8_t reg, uint16_t mv,
                                         uint16_t min_mv, uint16_t step_mv,
                                         uint8_t mask, bool preserve_high_bits) {
    if (mv < min_mv || ((mv - min_mv) % step_mv) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw = (uint8_t)((mv - min_mv) / step_mv);
    if ((raw & (uint8_t)~mask) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!preserve_high_bits) {
        return axp2101_write_u8(reg, raw);
    }

    return axp2101_update_bits(reg, mask, raw);
}

static void axp2101_log_key_regs(const char *stage) {
    uint8_t reg26 = 0;
    uint8_t reg80 = 0;
    uint8_t reg90 = 0;
    uint8_t reg82 = 0;
    uint8_t reg94 = 0;
    uint8_t reg95 = 0;

    if (axp2101_read_u8(AXP2101_REG_SLEEP_WAKEUP_CTRL, &reg26) == ESP_OK &&
        axp2101_read_u8(AXP2101_REG_DC_ONOFF_DVM_CTRL, &reg80) == ESP_OK &&
        axp2101_read_u8(AXP2101_REG_LDO_ONOFF_CTRL0, &reg90) == ESP_OK &&
        axp2101_read_u8(AXP2101_REG_DC_VOL0_CTRL, &reg82) == ESP_OK &&
        axp2101_read_u8(AXP2101_REG_LDO_VOL2_CTRL, &reg94) == ESP_OK &&
        axp2101_read_u8(AXP2101_REG_LDO_VOL3_CTRL, &reg95) == ESP_OK) {
        ESP_LOGI(TAG, "%s regs: 26=0x%02X 80=0x%02X 90=0x%02X 82=0x%02X 94=0x%02X 95=0x%02X",
                 stage, reg26, reg80, reg90, reg82, reg94, reg95);
    }
}

esp_err_t axp2101_board_init(void) {
    ESP_RETURN_ON_ERROR(i2c_master_Init(), TAG, "I2C init failed");

    uint8_t chip_id = 0;
    esp_err_t err = axp2101_read_u8(AXP2101_REG_CHIP_ID, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read chip id failed: %s", esp_err_to_name(err));
        return err;
    }
    if (chip_id != AXP2101_CHIP_ID) {
        ESP_LOGW(TAG, "unexpected PMU chip id: 0x%02X", chip_id);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "AXP2101 detected");
    axp2101_log_key_regs("Before PMU init");

    uint8_t sleep_reg = 0;
    ESP_RETURN_ON_ERROR(axp2101_read_u8(AXP2101_REG_SLEEP_WAKEUP_CTRL, &sleep_reg), TAG,
                        "read sleep/wakeup ctrl failed");

    ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_ADC_CHANNEL_CTRL, BIT1, 0), TAG,
                        "disable TS pin measure failed");

    if (sleep_reg & BIT0) {
        ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_SLEEP_WAKEUP_CTRL, BIT1, BIT1), TAG,
                            "enable wakeup failed");
    }
    if (sleep_reg & BIT3) {
        ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_SLEEP_WAKEUP_CTRL, BIT3, 0), TAG,
                            "clear PWROK-to-low wakeup failed");
    }

    /* Power key: 128 ms power-on, 4 s power-off */
    ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_IRQ_OFF_ON_LEVEL, 0x03, 0x00), TAG,
                        "set power-key on-time failed");
    ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_IRQ_OFF_ON_LEVEL, 0x0C, 0x00), TAG,
                        "set power-key off-time failed");

    /* Charge LED: manual control off */
    uint8_t chgled_reg = 0;
    ESP_RETURN_ON_ERROR(axp2101_read_u8(AXP2101_REG_CHGLED_SET_CTRL, &chgled_reg), TAG,
                        "read CHGLED ctrl failed");
    chgled_reg &= 0xC8;
    chgled_reg |= 0x05;
    ESP_RETURN_ON_ERROR(axp2101_write_u8(AXP2101_REG_CHGLED_SET_CTRL, chgled_reg), TAG,
                        "set CHGLED mode failed");

    /* Charge target 4.1V, button battery charge 3.3V, and enable button battery charge */
    ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_CV_CHG_VOL_SET, 0x07, 0x02), TAG,
                        "set charge target voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_set_voltage_reg(AXP2101_REG_BTN_BAT_CHG_VOL_SET, 3300, 2600, 100,
                                                0x07, true), TAG,
                        "set button battery charge voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_CHG_GAUGE_WDT_CTRL, BIT2, BIT2), TAG,
                        "enable button battery charge failed");

    /* Match vendor PMU rails and force the likely panel rails on */
    ESP_RETURN_ON_ERROR(axp2101_set_voltage_reg(AXP2101_REG_DC_VOL0_CTRL, 3300, 1500, 100,
                                                0x1F, false), TAG,
                        "set DC1 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_set_voltage_reg(AXP2101_REG_LDO_VOL2_CTRL, 3300, 500, 100,
                                                0x1F, true), TAG,
                        "set ALDO3 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_set_voltage_reg(AXP2101_REG_LDO_VOL3_CTRL, 3300, 500, 100,
                                                0x1F, true), TAG,
                        "set ALDO4 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_DC_ONOFF_DVM_CTRL, BIT0, BIT0), TAG,
                        "enable DC1 failed");
    ESP_RETURN_ON_ERROR(axp2101_update_bits(AXP2101_REG_LDO_ONOFF_CTRL0, BIT2 | BIT3, BIT2 | BIT3), TAG,
                        "enable ALDO3/ALDO4 failed");

    axp2101_log_key_regs("After PMU init");
    return ESP_OK;
}
