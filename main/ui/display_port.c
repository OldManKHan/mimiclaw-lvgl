#include "display_port.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl/lvgl.h"
#include "ui/board_config.h"
#include "ui/xpt2046.h"

static const char *TAG = "display_port";

static spi_device_handle_t s_lcd_spi = NULL;
static esp_timer_handle_t s_lvgl_tick_timer = NULL;

static lv_disp_buf_t s_disp_buf;
static lv_color_t s_disp_line_buf[MIMI_LCD_HOR_RES * 20];

static bool s_inited = false;

static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(MIMI_LCD_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_transmit(s_lcd_spi, &t);
}

static void lcd_data(const uint8_t *data, uint16_t len)
{
    gpio_set_level(MIMI_LCD_DC, 1);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_transmit(s_lcd_spi, &t);
}

static void ili9341_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];

    lcd_cmd(0x2A);
    data[0] = (x0 >> 8);
    data[1] = x0 & 0xFF;
    data[2] = (x1 >> 8);
    data[3] = x1 & 0xFF;
    lcd_data(data, 4);

    lcd_cmd(0x2B);
    data[0] = (y0 >> 8);
    data[1] = y0 & 0xFF;
    data[2] = (y1 >> 8);
    data[3] = y1 & 0xFF;
    lcd_data(data, 4);

    lcd_cmd(0x2C);
}

static esp_err_t lcd_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << MIMI_LCD_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    spi_bus_config_t bus_cfg = {
        .miso_io_num = MIMI_LCD_MISO,
        .mosi_io_num = MIMI_LCD_MOSI,
        .sclk_io_num = MIMI_LCD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MIMI_LCD_HOR_RES * MIMI_LCD_VER_RES * 2,
    };

    esp_err_t ret = spi_bus_initialize(MIMI_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = MIMI_LCD_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(MIMI_LCD_SPI_HOST, &dev_cfg, &s_lcd_spi));

    vTaskDelay(pdMS_TO_TICKS(100));

    const uint8_t init_seq[][17] = {
        { 0x01, 0 },
        { 0x11, 0x80 },
        { 0x3A, 1, 0x55 },
        { 0x36, 1, 0x28 },
        { 0x29, 0 },
        { 0, 0xFF },
    };

    for (int i = 0; init_seq[i][1] != 0xFF; i++) {
        lcd_cmd(init_seq[i][0]);
        if (init_seq[i][1] & 0x1F) {
            lcd_data(&init_seq[i][2], init_seq[i][1] & 0x1F);
        }
        if (init_seq[i][1] & 0x80) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    ESP_LOGI(TAG, "LCD initialized (%dx%d)", MIMI_LCD_HOR_RES, MIMI_LCD_VER_RES);
    return ESP_OK;
}

static void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    ili9341_set_window(area->x1, area->y1, area->x2, area->y2);
    gpio_set_level(MIMI_LCD_DC, 1);

    spi_transaction_t t = {
        .length = w * h * 16,
        .tx_buffer = color_map,
    };
    spi_device_transmit(s_lcd_spi, &t);

    lv_disp_flush_ready(drv);
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static esp_err_t lvgl_tick_init(void)
{
    esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_lvgl_tick_timer);
    if (ret != ESP_OK) {
        return ret;
    }
    return esp_timer_start_periodic(s_lvgl_tick_timer, 1000);
}

esp_err_t display_port_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(lcd_init());

    lv_init();
    ESP_ERROR_CHECK(lvgl_tick_init());

    lv_disp_buf_init(&s_disp_buf, s_disp_line_buf, NULL, MIMI_LCD_HOR_RES * 20);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = MIMI_LCD_HOR_RES;
    disp_drv.ver_res = MIMI_LCD_VER_RES;
    disp_drv.flush_cb = lvgl_flush;
    disp_drv.buffer = &s_disp_buf;
    lv_disp_drv_register(&disp_drv);

    esp_err_t touch_ret = xpt2046_init(MIMI_LCD_SPI_HOST, MIMI_TOUCH_CS, MIMI_TOUCH_IRQ);
    if (touch_ret == ESP_OK) {
        lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = xpt2046_read;
        lv_indev_drv_register(&indev_drv);
    } else {
        ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(touch_ret));
    }

    s_inited = true;
    return ESP_OK;
}
