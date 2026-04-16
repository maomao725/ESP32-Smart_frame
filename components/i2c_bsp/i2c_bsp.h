#ifndef I2C_BSP_H
#define I2C_BSP_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_master_Init(void);
esp_err_t i2c_axp2101_read(uint8_t reg, uint8_t *data, size_t len);
esp_err_t i2c_axp2101_write(uint8_t reg, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
