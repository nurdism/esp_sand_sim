#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Measure the raw state of the I2C lines and try to unstick them. Call this
 * before anything configures the bus pins, i.e. before board_i2c_init(). */
void sand_input_bus_check(void);

/* Bring up the IMU and the touch controller (both live on the BSP I2C bus). */
esp_err_t sand_input_init(void);

/* Smoothed gravity direction in screen space: +x right, +y down.
 * Roughly unit length; falls back to straight down if the IMU goes quiet. */
void sand_input_gravity(float *gx, float *gy);

/* Current touch point in screen pixels. Returns false when nothing is
 * pressed. Pixels rather than cells because the grain size is a setting now,
 * and the menus are laid out on the glass, not on the grid. */
bool sand_input_touch(int *screen_x, int *screen_y);

/* Untransformed controller coordinates, for calibration. */
bool sand_input_touch_raw(int *raw_x, int *raw_y);

/* Best orientation over two tapped points, with the residual in pixels
 * written to `error_px`. Two points are the minimum: one point cannot tell a
 * rotation from a mirror through it, and cannot see a flip at all if it
 * happens to sit near the axis of that flip. */
int sand_input_touch_solve(int ax, int ay, int target_ax, int target_ay,
                           int bx, int by, int target_bx, int target_by,
                           int *error_px);

int sand_input_touch_orientation(void);
void sand_input_touch_set_orientation(int orientation);

/* Ask the PMIC to cut the rails. Does not return if it works. */
void sand_input_power_off(void);

/* Battery charge from the PMIC's fuel gauge, 0-100. Returns -1 when there is
 * no PMIC, no battery, or the read fails. */
int sand_input_battery(void);

typedef enum {
    BUTTON_NONE = 0,
    BUTTON_CLICK,   /* released before the hold threshold */
    BUTTON_HOLD,    /* fires once, as soon as the threshold is reached */
} button_event_t;

/* State of the board's Key1. Call once per frame - the debounce and the hold
 * threshold both count frames. */
button_event_t sand_input_button(void);

#ifdef __cplusplus
}
#endif
