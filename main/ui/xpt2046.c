#include "xpt2046.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "ui/board_config.h"

static const char *TAG = "xpt2046";

static spi_device_handle_t s_spi = NULL;
static int s_irq_pin = -1;

static int16_t s_avg_buf_x[XPT2046_AVG];
static int16_t s_avg_buf_y[XPT2046_AVG];
static uint8_t s_avg_last = 0;

static int16_t xpt2046_cmd(uint8_t cmd)
{
    uint8_t tx_data[3] = { cmd, 0x00, 0x00 };
    uint8_t rx_data[3] = { 0 };

    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    esp_err_t ret = spi_device_transmit(s_spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI read failed: %s", esp_err_to_name(ret));
        return 0;
    }

    return ((rx_data[1] << 8) | rx_data[2]) >> 4;
}

static bool xpt2046_is_touch_detected(void)
{
    if (s_irq_pin >= 0 && gpio_get_level(s_irq_pin) != 0) {
        return false;
    }

    int16_t z1 = xpt2046_cmd(XPT2046_CMD_Z1_READ);
    int16_t z2 = xpt2046_cmd(XPT2046_CMD_Z2_READ);
    int16_t z = z1 + 4096 - z2;
    return z >= XPT2046_THRESHOLD;
}

static void xpt2046_corr(int16_t *x, int16_t *y)
{
#if XPT2046_SWAP_XY
    int16_t tmp = *x;
    *x = *y;
    *y = tmp;
#endif

    *x = (*x > XPT2046_X_MIN) ? (*x - XPT2046_X_MIN) : 0;
    *y = (*y > XPT2046_Y_MIN) ? (*y - XPT2046_Y_MIN) : 0;

    *x = (int32_t)(*x) * MIMI_LCD_HOR_RES / (XPT2046_X_MAX - XPT2046_X_MIN);
    *y = (int32_t)(*y) * MIMI_LCD_VER_RES / (XPT2046_Y_MAX - XPT2046_Y_MIN);

#if XPT2046_X_INV
    *x = MIMI_LCD_HOR_RES - *x;
#endif
#if XPT2046_Y_INV
    *y = MIMI_LCD_VER_RES - *y;
#endif

    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= MIMI_LCD_HOR_RES) *x = MIMI_LCD_HOR_RES - 1;
    if (*y >= MIMI_LCD_VER_RES) *y = MIMI_LCD_VER_RES - 1;
}

static void xpt2046_avg(int16_t *x, int16_t *y)
{
    for (int i = XPT2046_AVG - 1; i > 0; i--) {
        s_avg_buf_x[i] = s_avg_buf_x[i - 1];
        s_avg_buf_y[i] = s_avg_buf_y[i - 1];
    }

    s_avg_buf_x[0] = *x;
    s_avg_buf_y[0] = *y;
    if (s_avg_last < XPT2046_AVG) {
        s_avg_last++;
    }

    int32_t x_sum = 0;
    int32_t y_sum = 0;
    for (int i = 0; i < s_avg_last; i++) {
        x_sum += s_avg_buf_x[i];
        y_sum += s_avg_buf_y[i];
    }

    *x = x_sum / s_avg_last;
    *y = y_sum / s_avg_last;
}

esp_err_t xpt2046_init(spi_host_device_t host, int cs_pin, int irq_pin)
{
    s_irq_pin = irq_pin;

    if (s_irq_pin >= 0) {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = BIT64(s_irq_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&irq_cfg));
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = cs_pin,
        .queue_size = 1,
    };

    esp_err_t ret = spi_bus_add_device(host, &dev_cfg, &s_spi);
    if (ret == ESP_ERR_INVALID_STATE) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = MIMI_LCD_MOSI,
            .miso_io_num = MIMI_LCD_MISO,
            .sclk_io_num = MIMI_LCD_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 32,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(host, &bus_cfg, SPI_DMA_CH_AUTO));
        ret = spi_bus_add_device(host, &dev_cfg, &s_spi);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add touch SPI device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    xpt2046_cmd(XPT2046_CMD_X_READ);
    ESP_LOGI(TAG, "Touch initialized on SPI%d, CS=%d IRQ=%d", host + 1, cs_pin, s_irq_pin);
    return ESP_OK;
}

bool xpt2046_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    static int16_t last_x = 0;
    static int16_t last_y = 0;

    bool valid = false;
    int16_t x = last_x;
    int16_t y = last_y;

    if (xpt2046_is_touch_detected()) {
        valid = true;
        x = xpt2046_cmd(XPT2046_CMD_X_READ);
        y = xpt2046_cmd(XPT2046_CMD_Y_READ);

        xpt2046_corr(&x, &y);
        xpt2046_avg(&x, &y);

        last_x = x;
        last_y = y;
    } else {
        s_avg_last = 0;
    }

    data->point.x = x;
    data->point.y = y;
    data->state = valid ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    return false;
}
