#include <math.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "esp_lcd_touch_cst9217.h"

#include "sand_board.h"
#include "sand_config.h"
#include "sand_input.h"

static const char *TAG = "sand_input";

/* ------------------------------------------------------------------------
 * QMI8658, accelerometer only.
 *
 * No vendor driver: the published waveshare/qmi8658 component declares a
 * pile of functions its .c never defines. All this simulation needs is a
 * direction, so here are the five registers that takes.
 * ---------------------------------------------------------------------- */
#define QMI8658_ADDR_LOW    0x6A   /* what this board uses */
#define QMI8658_ADDR_HIGH   0x6B

#define QMI8658_REG_WHO_AM_I  0x00
#define QMI8658_REG_CTRL1     0x02
#define QMI8658_REG_CTRL2     0x03
#define QMI8658_REG_CTRL5     0x06
#define QMI8658_REG_CTRL7     0x08
#define QMI8658_REG_AX_L      0x35

#define QMI8658_WHO_AM_I_VALUE 0x05

/* CTRL2 = accel full scale in bits 6:4, output data rate in bits 3:0.
 * 0x1 = +-4 g (8192 LSB/g), 0x4 = 500 Hz. */
#define QMI8658_ACC_FS_4G     0x1
#define QMI8658_ACC_ODR_500HZ 0x4
#define QMI8658_ACC_LSB_PER_G 8192.0f

#define AXP2101_ADDR        0x34
#define AXP2101_REG_COMMON  0x10    /* bit 0 requests a soft power off */
#define AXP2101_REG_STATUS1 0x00    /* bit 3 reads high with a battery fitted */
#define AXP2101_REG_BAT_PCT 0xA4    /* fuel gauge output, 0-100 */

static i2c_master_dev_handle_t s_imu;
static i2c_master_dev_handle_t s_pmic;
static esp_lcd_touch_handle_t s_touch;

static IRAM_ATTR void touch_isr(esp_lcd_touch_handle_t tp);

/* Smoothed gravity direction, starts pointing at the bottom of the screen. */
static float s_gx = 0.0f;
static float s_gy = 1.0f;

static esp_err_t imu_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_imu, buf, sizeof(buf), 100);
}

static esp_err_t imu_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_imu, &reg, 1, out, len, 100);
}

/* ------------------------------------------------------------------------ */

/* Short, bounded probe. Unlike a register read it can never block forever,
 * which matters because the bus is the thing we are trying to vet. */
static bool i2c_present(i2c_master_bus_handle_t bus, uint8_t addr)
{
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    const bool ok = i2c_master_probe(bus, addr, 20) == ESP_OK;
    esp_log_level_set("i2c.master", ESP_LOG_WARN);
    return ok;
}

/* Log every address that answers. Cheap, and the only way to tell "wrong
 * address" apart from "dead bus" without a scope. */
static void i2c_scan(i2c_master_bus_handle_t bus)
{
    char found[128];
    int len = 0;

    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    const int64_t started_us = esp_timer_get_time();
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 10) == ESP_OK && len < (int)sizeof(found) - 6) {
            len += snprintf(found + len, sizeof(found) - len, " 0x%02X", addr);
        }
    }
    const int elapsed_ms = (int)((esp_timer_get_time() - started_us) / 1000);
    esp_log_level_set("i2c.master", ESP_LOG_WARN);

    /* 112 probes. Near 1120 ms means every one hit its 10 ms timeout, i.e. the
     * bus is stuck. Fast means clean NACKs, i.e. the bus works and nothing is
     * listening - completely different problems. */
    if (len == 0) {
        ESP_LOGE(TAG, "I2C scan found nothing in %d ms (%s)", elapsed_ms,
                 elapsed_ms > 500 ? "timeouts: bus stuck" : "NACKs: bus alive, no devices");
    } else {
        ESP_LOGI(TAG, "I2C devices:%s (scan %d ms)", found, elapsed_ms);
    }
}

static bool imu_init(i2c_master_bus_handle_t bus)
{
    static const uint8_t addresses[] = { QMI8658_ADDR_LOW, QMI8658_ADDR_HIGH };

    for (size_t i = 0; i < sizeof(addresses); i++) {
        if (!i2c_present(bus, addresses[i])) {
            continue;
        }

        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[i],
            .scl_speed_hz = BOARD_I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &s_imu) != ESP_OK) {
            continue;
        }

        uint8_t who = 0;
        if (imu_read(QMI8658_REG_WHO_AM_I, &who, 1) != ESP_OK || who != QMI8658_WHO_AM_I_VALUE) {
            ESP_LOGW(TAG, "0x%02X answered but WHO_AM_I is 0x%02X, not 0x%02X",
                     addresses[i], who, QMI8658_WHO_AM_I_VALUE);
            i2c_master_bus_rm_device(s_imu);
            s_imu = NULL;
            continue;
        }

        /* Address auto-increment so the six accel bytes come out in one read,
         * and little-endian so the halves assemble the obvious way. */
        esp_err_t ret = imu_write(QMI8658_REG_CTRL1, 0x40);
        ret |= imu_write(QMI8658_REG_CTRL2, (QMI8658_ACC_FS_4G << 4) | QMI8658_ACC_ODR_500HZ);
        ret |= imu_write(QMI8658_REG_CTRL5, 0x03);  /* accel low-pass filter on */
        ret |= imu_write(QMI8658_REG_CTRL7, 0x01);  /* accelerometer enable */
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "QMI8658 at 0x%02X failed to configure", addresses[i]);
            i2c_master_bus_rm_device(s_imu);
            s_imu = NULL;
            continue;
        }

        ESP_LOGI(TAG, "QMI8658 up at 0x%02X", addresses[i]);
        return true;
    }

    return false;
}

static void line_report(const char *what)
{
    ESP_LOGI(TAG, "  %-14s SDA=%d SCL=%d", what,
             gpio_get_level(BOARD_I2C_SDA), gpio_get_level(BOARD_I2C_SCL));
}

void sand_input_bus_check(void)
{
    ESP_LOGI(TAG, "I2C line check on SDA=GPIO%d SCL=GPIO%d", BOARD_I2C_SDA, BOARD_I2C_SCL);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_I2C_SDA) | (1ULL << BOARD_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    esp_rom_delay_us(2000);
    /* High here means the board has its own pull-up resistors. */
    line_report("no pull-up:");

    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);
    esp_rom_delay_us(2000);
    /* Low here means something is clamping the line, not a missing pull-up. */
    line_report("internal pu:");

    /* Nine clocks walk a slave that is stuck mid-byte off the bus. */
    cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    gpio_config(&cfg);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(BOARD_I2C_SCL, 0);
        esp_rom_delay_us(5);
        gpio_set_level(BOARD_I2C_SCL, 1);
        esp_rom_delay_us(5);
    }
    esp_rom_delay_us(1000);
    line_report("post-recovery:");
}

button_event_t sand_input_button(void)
{
    /* Two agreeing samples debounces the switch; the hold itself is timed off
     * the clock so it does not stretch when frames get expensive. */
    static uint8_t low_frames;
    static int64_t pressed_at_us;
    static bool hold_fired;

    if (gpio_get_level(BUTTON_GPIO) == 0) {
        if (low_frames < 2) {
            low_frames++;
            if (low_frames == 2) {
                pressed_at_us = esp_timer_get_time();
            }
            return BUTTON_NONE;
        }
        /* Fire on the way down rather than on release, so the gesture feels
         * like it responded to the press. */
        if (!hold_fired && esp_timer_get_time() - pressed_at_us >= BUTTON_HOLD_MS * 1000) {
            hold_fired = true;
            return BUTTON_HOLD;
        }
        return BUTTON_NONE;
    }

    const bool was_click = (low_frames >= 2) && !hold_fired;
    low_frames = 0;
    hold_fired = false;
    return was_click ? BUTTON_CLICK : BUTTON_NONE;
}

esp_err_t sand_input_init(void)
{
    i2c_master_bus_handle_t bus = board_i2c_handle();

    const gpio_config_t button_cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* R7 already does this; belt and braces */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&button_cfg);

    /* The bus is built without flags.enable_internal_pullup. Waveshare's own
     * AXP2101 example sets it, so match that. Harmless if the board also has
     * external pull-ups - it just sharpens the rise time. */
    gpio_set_pull_mode(BOARD_I2C_SDA, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(BOARD_I2C_SCL, GPIO_PULLUP_ONLY);

    i2c_scan(bus);

    /* Probe first: esp_lcd_touch_new_i2c_cst9217() reads the chip ID with an
     * infinite timeout, so on a dead bus it hangs the app with no diagnostic. */
    if (i2c_present(bus, ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS)) {
        esp_err_t ret = board_touch_new(&s_touch);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "touch init failed (%s)", esp_err_to_name(ret));
            s_touch = NULL;
        } else if (esp_lcd_touch_register_interrupt_callback(s_touch, touch_isr) != ESP_OK) {
            ESP_LOGW(TAG, "no touch interrupt, falling back to polling the line");
        }
    } else {
        ESP_LOGW(TAG, "no CST9217 at 0x%02X, pouring sand disabled",
                 ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS);
    }

    /* The PMIC is only needed to turn the board off, so a failure here is
     * not worth refusing to run over. */
    if (i2c_present(bus, AXP2101_ADDR)) {
        const i2c_device_config_t pmic_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = AXP2101_ADDR,
            .scl_speed_hz = BOARD_I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &pmic_cfg, &s_pmic) != ESP_OK) {
            s_pmic = NULL;
        }
    }

    if (!imu_init(bus)) {
        /* Not fatal: the sand still falls, it just always falls down. */
        ESP_LOGE(TAG, "QMI8658 not responding, falling back to fixed gravity");
    }

    return ESP_OK;
}

void sand_input_gravity(float *gx, float *gy)
{
    uint8_t raw[6];

    if (s_imu && imu_read(QMI8658_REG_AX_L, raw, sizeof(raw)) == ESP_OK) {
        const float ax = (int16_t)(raw[0] | (raw[1] << 8)) / QMI8658_ACC_LSB_PER_G;
        const float ay = (int16_t)(raw[2] | (raw[3] << 8)) / QMI8658_ACC_LSB_PER_G;
        const float az = (int16_t)(raw[4] | (raw[5] << 8)) / QMI8658_ACC_LSB_PER_G;
        (void)az;  /* only referenced by some GRAVITY_* mappings */
#if IMU_LOG_RAW
        static int64_t last_log_us;
        const int64_t now = esp_timer_get_time();
        if (now - last_log_us > 1000000) {
            last_log_us = now;
            ESP_LOGI(TAG, "accel  x=%+5.2fg  y=%+5.2fg  z=%+5.2fg", ax, ay, az);
        }
#endif
        const float tx = GRAVITY_X(ax, ay, az);
        const float ty = GRAVITY_Y(ax, ay, az);
        const float mag = sqrtf(tx * tx + ty * ty);

        /* Below this the board is face up (or in free fall) and the in-plane
         * direction is just noise, so hold the last one. */
        if (mag > 0.04f) {
            s_gx += (tx / mag - s_gx) * GRAVITY_SMOOTHING;
            s_gy += (ty / mag - s_gy) * GRAVITY_SMOOTHING;
        }
    }

    *gx = s_gx;
    *gy = s_gy;
}

_Static_assert(FB_W == FB_H, "the touch transform assumes a square panel");

#if TOUCH_LOG_RAW
static bool s_touch_was_down;   /* so only the first frame of a press logs */
#define TOUCH_RELEASED()  (s_touch_was_down = false)
#else
#define TOUCH_RELEASED()  ((void)0)
#endif

/* Set from the controller's own interrupt line. */
static volatile bool s_touch_pending;

/* Runtime rather than compile time, so calibration can settle it without a
 * reflash. TOUCH_ORIENTATION is only the starting guess and the fallback. */
static int s_orientation = TOUCH_ORIENTATION;

int sand_input_touch_orientation(void)
{
    return s_orientation;
}

void sand_input_touch_set_orientation(int orientation)
{
    if (orientation >= 0 && orientation < 8) {
        s_orientation = orientation;
    }
}

/* The eight ways a square panel can be mounted: four rotations, then those
 * same four mirrored, with the FLIP knobs composed on top. The solver runs
 * candidates through this same function, so whatever the flips are set to,
 * calibration stays self-consistent. */
static void apply_orientation(int orientation, int rx, int ry, int *sx, int *sy)
{
    const int m = FB_W - 1;

    switch (orientation) {
    default:
    case 0: *sx = rx;       *sy = ry;       break;
    case 1: *sx = ry;       *sy = m - rx;   break;
    case 2: *sx = m - rx;   *sy = m - ry;   break;
    case 3: *sx = m - ry;   *sy = rx;       break;
    case 4: *sx = ry;       *sy = rx;       break;
    case 5: *sx = m - rx;   *sy = ry;       break;
    case 6: *sx = m - ry;   *sy = m - rx;   break;
    case 7: *sx = rx;       *sy = m - ry;   break;
    }

#if TOUCH_FLIP_X
    *sx = m - *sx;
#endif
#if TOUCH_FLIP_Y
    *sy = m - *sy;
#endif
}

static long point_error2(int orientation, int rx, int ry, int tx, int ty)
{
    int sx, sy;
    apply_orientation(orientation, rx, ry, &sx, &sy);
    const long dx = sx - tx;
    const long dy = sy - ty;
    return dx * dx + dy * dy;
}

int sand_input_touch_solve(int ax, int ay, int target_ax, int target_ay,
                           int bx, int by, int target_bx, int target_by,
                           int *error_px)
{
    int best = 0;
    long best_total = -1;

    for (int o = 0; o < 8; o++) {
        /* Scored on both taps at once. Solving from one and merely checking
         * with the other lets a candidate that fits the first point win even
         * when it is hopeless on the second. */
        const long total = point_error2(o, ax, ay, target_ax, target_ay) +
                           point_error2(o, bx, by, target_bx, target_by);

        ESP_LOGI(TAG, "  orientation %d: total miss %ld px", o,
                 lroundf(sqrtf((float)total)));

        if (best_total < 0 || total < best_total) {
            best_total = total;
            best = o;
        }
    }

    if (error_px) {
        /* Per point, so the number means the same thing as a tolerance. */
        *error_px = (int)lroundf(sqrtf((float)best_total / 2.0f));
    }
    return best;
}

/* Fires on the controller's INT edge. Only sets a flag - the I2C read itself
 * happens in the frame loop, since the driver's read path sleeps. */
/* IRAM_ATTR is on the forward declaration above; repeating it here would
 * emit a second, conflicting section name. */
static void touch_isr(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    s_touch_pending = true;
}

/* Read the controller and hand back its own coordinates, untransformed. */
static bool touch_read_raw(int *raw_x, int *raw_y)
{
    if (!s_touch) {
        return false;
    }

#if TOUCH_USE_INT_GATE
    /* Trust either the latched interrupt or the current line level. A level
     * alone can miss a controller that only pulses; a latch alone can miss a
     * finger already down before the handler was installed. */
    const bool signalled = s_touch_pending || (gpio_get_level(BOARD_TOUCH_INT) == 0);
    s_touch_pending = false;
    if (!signalled) {
        return false;
    }
#endif

    esp_lcd_touch_read_data(s_touch);

    esp_lcd_touch_point_data_t point;
    uint8_t point_count = 0;
    if (esp_lcd_touch_get_data(s_touch, &point, &point_count, 1) != ESP_OK || point_count == 0) {
        return false;
    }

    *raw_x = point.x;
    *raw_y = point.y;
    return true;
}

bool sand_input_touch_raw(int *raw_x, int *raw_y)
{
    return touch_read_raw(raw_x, raw_y);
}

bool sand_input_touch(int *screen_x, int *screen_y)
{
    int rx, ry;
    if (!touch_read_raw(&rx, &ry)) {
        TOUCH_RELEASED();
        return false;
    }

    int sx, sy;
    apply_orientation(s_orientation, rx, ry, &sx, &sy);

    /* A touch right on the bezel can land just off the panel. */
    if (sx < 0) sx = 0; else if (sx >= FB_W) sx = FB_W - 1;
    if (sy < 0) sy = 0; else if (sy >= FB_H) sy = FB_H - 1;
    *screen_x = sx;
    *screen_y = sy;

#if TOUCH_LOG_RAW
    if (!s_touch_was_down) {
        ESP_LOGI(TAG, "touch raw=(%d,%d) orientation=%d -> screen=(%d,%d)",
                 rx, ry, s_orientation, sx, sy);
    }
    s_touch_was_down = true;
#endif
    return true;
}

int sand_input_battery(void)
{
    if (!s_pmic) {
        return -1;
    }

    /* No battery means the gauge output is meaningless, so check first. */
    uint8_t reg = AXP2101_REG_STATUS1;
    uint8_t status = 0;
    if (i2c_master_transmit_receive(s_pmic, &reg, 1, &status, 1, 100) != ESP_OK ||
        !(status & (1u << 3))) {
        return -1;
    }

    reg = AXP2101_REG_BAT_PCT;
    uint8_t pct = 0;
    if (i2c_master_transmit_receive(s_pmic, &reg, 1, &pct, 1, 100) != ESP_OK || pct > 100) {
        return -1;
    }
    return pct;
}

void sand_input_power_off(void)
{
    if (!s_pmic) {
        ESP_LOGE(TAG, "no AXP2101, cannot power down - hold Key2 instead");
        return;
    }

    /* Read-modify-write: the rest of this register configures the PMIC, so
     * only the power-off request bit is touched. */
    uint8_t reg = AXP2101_REG_COMMON;
    uint8_t value = 0;
    if (i2c_master_transmit_receive(s_pmic, &reg, 1, &value, 1, 100) != ESP_OK) {
        ESP_LOGE(TAG, "PMIC read failed, cannot power down");
        return;
    }

    const uint8_t buf[2] = { AXP2101_REG_COMMON, (uint8_t)(value | 0x01) };
    if (i2c_master_transmit(s_pmic, buf, sizeof(buf), 100) != ESP_OK) {
        ESP_LOGE(TAG, "PMIC write failed, cannot power down");
    }
}
