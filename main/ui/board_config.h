#pragma once

#include "driver/spi_master.h"

/*
 * Hardware mapping aligned with ../esp32_lvgl7_video_player.
 */
#define MIMI_LCD_SPI_HOST SPI2_HOST

#define MIMI_LCD_SCK      12
#define MIMI_LCD_MOSI     11
#define MIMI_LCD_MISO     13
#define MIMI_LCD_DC       9
#define MIMI_LCD_CS       10

#define MIMI_TOUCH_CS     46
#define MIMI_TOUCH_IRQ    2

#define MIMI_LCD_HOR_RES  320
#define MIMI_LCD_VER_RES  240
