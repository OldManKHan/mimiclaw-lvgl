#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "lvgl/lvgl.h"

#define XPT2046_CMD_X_READ   0x90
#define XPT2046_CMD_Y_READ   0xD0
#define XPT2046_CMD_Z1_READ  0xB0
#define XPT2046_CMD_Z2_READ  0xC0

#define XPT2046_X_MIN        200
#define XPT2046_X_MAX        1900
#define XPT2046_Y_MIN        120
#define XPT2046_Y_MAX        1900
#define XPT2046_SWAP_XY      0
#define XPT2046_X_INV        1
#define XPT2046_Y_INV        0
#define XPT2046_AVG          4
#define XPT2046_THRESHOLD    400

esp_err_t xpt2046_init(spi_host_device_t host, int cs_pin, int irq_pin);
bool xpt2046_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
