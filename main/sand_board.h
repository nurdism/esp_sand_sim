#pragma once

#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"

/* The slice of the Waveshare ESP32-S3-Touch-AMOLED-1.75C this project
 * actually uses: the CO5300 panel over QSPI, the CST9217 touch controller,
 * and the shared I2C bus. The vendor BSP provided the same things wrapped
 * around LVGL, an audio codec stack, and SPIFFS - none of which ever ran -
 * so the handful of init calls live here instead and the dependency tree
 * lost its biggest branches. Pin numbers are from the vendor BSP. */

#define BOARD_I2C_SDA       (GPIO_NUM_15)
#define BOARD_I2C_SCL       (GPIO_NUM_14)
#define BOARD_I2C_NUM       (1)
#define BOARD_I2C_HZ        (400000)

#define BOARD_TOUCH_INT     (GPIO_NUM_11)
#define BOARD_TOUCH_RST     (GPIO_NUM_2)     /* shared with the panel reset */

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the shared I2C bus (touch, IMU, PMIC). Safe to call more than
 * once; the handle getter initialises on first use anyway. */
esp_err_t board_i2c_init(void);
i2c_master_bus_handle_t board_i2c_handle(void);

/* Reset and initialise the CO5300 over QSPI and switch it on. The panel is
 * left at whatever brightness its init table set; call
 * board_brightness_set() to take control of it. */
esp_err_t board_display_new(size_t max_transfer_sz,
                            esp_lcd_panel_handle_t *ret_panel,
                            esp_lcd_panel_io_handle_t *ret_io);

/* AMOLED brightness, 0-100, via the panel's own DCS register - there is no
 * backlight pin on this board. */
esp_err_t board_brightness_set(int percent);

/* Bring up the CST9217 on the shared I2C bus, coordinates unrotated - the
 * application applies its own orientation mapping. */
esp_err_t board_touch_new(esp_lcd_touch_handle_t *ret_touch);

#ifdef __cplusplus
}
#endif
