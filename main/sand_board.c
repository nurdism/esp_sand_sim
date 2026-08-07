#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"

#include "sand_board.h"
#include "sand_config.h"

static const char *TAG = "sand_board";

/* Display wiring, used nowhere else. */
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_PIN_CS      (GPIO_NUM_12)
#define LCD_PIN_PCLK    (GPIO_NUM_38)
#define LCD_PIN_DATA0   (GPIO_NUM_4)
#define LCD_PIN_DATA1   (GPIO_NUM_5)
#define LCD_PIN_DATA2   (GPIO_NUM_6)
#define LCD_PIN_DATA3   (GPIO_NUM_7)
#define LCD_PIN_RST     (GPIO_NUM_1)

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_io_handle_t s_panel_io;

/* The vendor's power-up sequence for this panel, verbatim. The 0x2A/0x2B
 * window plus the set_gap(6, 0) in board_display_new centre the 466 px
 * frame in the controller's 472 px memory. */
static const co5300_lcd_init_cmd_t LCD_INIT[] = {
    { 0xFE, (uint8_t[]){ 0x20 }, 1, 0 },
    { 0x19, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0x1C, (uint8_t[]){ 0xA0 }, 1, 0 },

    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
    { 0x51, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x2A, (uint8_t[]){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xD1 }, 4, 600 },
    { 0x11, NULL, 0, 600 },
    { 0x29, NULL, 0, 0 },
};

esp_err_t board_i2c_init(void)
{
    if (s_i2c) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .i2c_port = BOARD_I2C_NUM,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c);
}

i2c_master_bus_handle_t board_i2c_handle(void)
{
    board_i2c_init();
    return s_i2c;
}

esp_err_t board_display_new(size_t max_transfer_sz,
                            esp_lcd_panel_handle_t *ret_panel,
                            esp_lcd_panel_io_handle_t *ret_io)
{
    const spi_bus_config_t bus_cfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        LCD_PIN_PCLK, LCD_PIN_DATA0, LCD_PIN_DATA1, LCD_PIN_DATA2,
        LCD_PIN_DATA3, max_transfer_sz);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(LCD_PIN_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                 &io_cfg, &s_panel_io),
                        TAG, "panel IO failed");

    const co5300_vendor_config_t vendor_cfg = {
        .init_cmds = LCD_INIT,
        .init_cmds_size = sizeof(LCD_INIT) / sizeof(LCD_INIT[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_panel_io, &panel_cfg, &panel),
                        TAG, "panel create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, 0x06, 0), TAG, "set_gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on failed");

    if (ret_panel) {
        *ret_panel = panel;
    }
    if (ret_io) {
        *ret_io = s_panel_io;
    }
    return ESP_OK;
}

esp_err_t board_brightness_set(int percent)
{
    if (!s_panel_io) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* DCS write-display-brightness, addressed the way the CO5300 expects
     * commands over QSPI: the 0x02 page in the top byte, the command in the
     * second. */
    const uint8_t level = (uint8_t)(percent * 255 / 100);
    const uint32_t cmd = (0x02 << 24) | (0x51 << 8);
    return esp_lcd_panel_io_tx_param(s_panel_io, cmd, &level, 1);
}

esp_err_t board_touch_new(esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "I2C init failed");

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = FB_W,
        .y_max = FB_H,
        .rst_gpio_num = BOARD_TOUCH_RST,
        .int_gpio_num = BOARD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
    };
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    io_cfg.scl_speed_hz = BOARD_I2C_HZ;

    esp_lcd_panel_io_handle_t touch_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &touch_io),
                        TAG, "touch IO failed");
    return esp_lcd_touch_new_i2c_cst9217(touch_io, &touch_cfg, ret_touch);
}
