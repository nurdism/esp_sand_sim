#pragma once

/* ------------------------------------------------------------------------
 * Tuning knobs for the falling-sand simulation.
 * Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *   466x466 round QSPI AMOLED (CO5300), CST9217 touch, QMI8658 IMU.
 * ---------------------------------------------------------------------- */

/* Panel geometry. */
#define FB_W                466
#define FB_H                466

/* Grain size in screen pixels, chosen at runtime from the settings ring.
 * Arrays are sized for the finest setting and the rest is worked out from
 * whichever is active, so nothing here is a compile-time grid dimension any
 * more - see the g_* globals in sand_sim.h. */
/* 1 px grains were tried and dropped: 217k cells is 16x the work of 4 px and
 * no longer fits in internal RAM, for a grain nobody can see anyway. */
#define CELL_PX_MIN         2
#define CELL_PX_MAX         6
#define CELL_PX_DEFAULT     4

#define GRID_W_MAX          (FB_W / CELL_PX_MIN)
#define GRID_H_MAX          (FB_H / CELL_PX_MIN)
#define GRID_CELLS_MAX      (GRID_W_MAX * GRID_H_MAX)

/* Height of a transfer band, in screen pixels rather than cells so it stays
 * put when the grain size changes.
 *
 * The frame is painted and pushed a band at a time out of a small internal
 * DMA buffer rather than one big framebuffer in PSRAM. esp_lcd never sets
 * SPI_TRANS_DMA_USE_PSRAM, so a PSRAM source buffer makes the SPI driver
 * bounce the whole transfer through internal DMA memory - which simply fails
 * once a transfer is larger than the internal heap. */
#define BAND_PX             64
#define BAND_BYTES          ((size_t)FB_W * BAND_PX * 2)

/* Target frame period in milliseconds. */
#define FRAME_PERIOD_MS     20

/* AMOLED brightness, 0-100. */
#define DISPLAY_BRIGHTNESS  90

/* Brush radius in screen pixels, so the stroke stays the same width on the
 * glass whatever the grain size is set to. */
#define TOUCH_DRAW_RADIUS_PX 12



/* ------------------------------------------------------------------------
 * Touch panel orientation relative to the display.
 *
 * This is only the starting guess and the fallback: the truth is measured
 * on-device by the guided calibration (two dots, instructions on the glass)
 * that runs on first boot and whenever Key1 is held for a second while
 * powering up. A good fit is stored in NVS; a bad or abandoned run changes
 * nothing and boots on with this value. Guessing it remotely from symptom
 * descriptions was tried at length, and the eight candidates are just close
 * enough relatives to make that a coin-toss - hence: measure, on the glass,
 * with words on screen so it cannot run unnoticed.
 *
 * 0-3 are the four rotations, 4-7 those mirrored:
 *
 *   0: (x, y)        4: (y, x)         <- transpose, the usual fix
 *   1: (y, M-x)      5: (M-x, y)
 *   2: (M-x, M-y)    6: (M-y, M-x)
 *   3: (M-y, x)      7: (x, M-y)
 *
 * where M is the last pixel index. */
#define TOUCH_ORIENTATION   5

/* Flip one mapped axis afterwards, composed onto TOUCH_ORIENTATION. The
 * quickest fix when exactly one axis runs backwards: up-down wrong, toggle
 * TOUCH_FLIP_Y; left-right wrong, toggle TOUCH_FLIP_X. */
#define TOUCH_FLIP_X        0
#define TOUCH_FLIP_Y        0

/* Print every touch-down with its raw and mapped coordinates. */
#define TOUCH_LOG_RAW       1

/* How long the button must be held to open the menu rather than register a
 * click. Real milliseconds, not frames: the frame period stretches with
 * whatever the simulation currently costs, so counting frames would make the
 * gesture need a noticeably longer hold in fluid mode than in sand. */
#define BUTTON_HOLD_MS      500

/* The board's Key1, wired to GPIO0 through a pull-up (Key2 is reset). Also
 * the strapping pin for download mode, which only matters at boot - reading
 * it at runtime is fine. Pressed reads low. */
#define BUTTON_GPIO         GPIO_NUM_0

/* Only talk to the touch controller when its interrupt line says a finger is
 * down.
 *
 * This is not an optimisation, it is damage control. esp_lcd's I2C layer
 * passes -1 (wait forever) for every transaction, and the CST9217 driver
 * wraps that in five retries with 3 ms sleeps, a 2 ms sleep between the
 * address write and the read, and - if it still fails - a chip reset costing
 * vTaskDelay(10) + vTaskDelay(50). Polling that blind at 50 Hz means one
 * unhappy read turns into 60+ ms of dead time every single frame.
 *
 * Set to 0 if touch stops responding, which would mean this board's INT line
 * does not idle high. */
#define TOUCH_USE_INT_GATE  1

/* ------------------------------------------------------------------------
 * IMU -> screen orientation.
 *
 * Screen axes: +x is right, +y is down, origin top-left. An accelerometer at
 * rest reads +1 g along the axis pointing UP, so gravity is the negated
 * reading.
 *
 * Which sensor axis lines up with which screen axis is a board layout fact,
 * not something derivable, so it is just a number to turn. 0-3 are the four
 * 90-degree rotations; 4-7 are those same four mirrored left-to-right.
 *
 *   0: (-ax, -ay)    4: ( ax, -ay)
 *   1: ( ay, -ax)    5: (-ay, -ax)
 *   2: ( ax,  ay)    6: (-ax,  ay)
 *   3: (-ay,  ax)    7: ( ay,  ax)
 *
 *   - sand runs to an edge 90 degrees off?   step within the group of four
 *   - sand runs to the exactly opposite edge? add 2, wrapping inside the
 *                                             group (0-3 or 4-7)
 *   - up/down right but left/right backwards? add 4
 *
 * The stats line prints the resulting screen-space gravity, and IMU_LOG_RAW
 * prints the raw axes.
 * ---------------------------------------------------------------------- */
#define IMU_ORIENTATION     7

#if   IMU_ORIENTATION == 0
#define GRAVITY_X(ax, ay, az)   (-(ax))
#define GRAVITY_Y(ax, ay, az)   (-(ay))
#elif IMU_ORIENTATION == 1
#define GRAVITY_X(ax, ay, az)   ( (ay))
#define GRAVITY_Y(ax, ay, az)   (-(ax))
#elif IMU_ORIENTATION == 2
#define GRAVITY_X(ax, ay, az)   ( (ax))
#define GRAVITY_Y(ax, ay, az)   ( (ay))
#elif IMU_ORIENTATION == 3
#define GRAVITY_X(ax, ay, az)   (-(ay))
#define GRAVITY_Y(ax, ay, az)   ( (ax))
#elif IMU_ORIENTATION == 4
#define GRAVITY_X(ax, ay, az)   ( (ax))
#define GRAVITY_Y(ax, ay, az)   (-(ay))
#elif IMU_ORIENTATION == 5
#define GRAVITY_X(ax, ay, az)   (-(ay))
#define GRAVITY_Y(ax, ay, az)   (-(ax))
#elif IMU_ORIENTATION == 6
#define GRAVITY_X(ax, ay, az)   (-(ax))
#define GRAVITY_Y(ax, ay, az)   ( (ay))
#elif IMU_ORIENTATION == 7
#define GRAVITY_X(ax, ay, az)   ( (ay))
#define GRAVITY_Y(ax, ay, az)   ( (ax))
#else
#error "IMU_ORIENTATION must be 0..7"
#endif

/* Print raw accelerometer readings once a second. */
#define IMU_LOG_RAW         1

/* Low-pass factor for the gravity vector, 0..1. Lower = smoother/laggier. */
#define GRAVITY_SMOOTHING   0.25f

/* ------------------------------------------------------------------------
 * Colour byte order.
 *
 * This panel reads the high byte off the wire first, so palette entries are
 * pre-swapped. (The vendor BSP declared the panel little-endian, which is
 * simply wrong for this hardware - with no swap the blues came out green,
 * which is the signature of the two bytes being transposed: 0x66BF read as
 * 0xBF66 decodes to a yellow-green.)
 * ---------------------------------------------------------------------- */
#define LCD_SWAP_BYTES      1
