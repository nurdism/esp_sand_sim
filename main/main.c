/*
 * Falling-sand simulation for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
 *
 * The QMI8658 IMU sets which way is "down", so tilting the board makes the
 * sand pour towards the low edge. Touching the screen pours in more sand.
 *
 * Rendering goes straight to the CO5300 over QSPI - no LVGL in the loop -
 * out of a single 466x466 RGB565 framebuffer in PSRAM.
 */

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "sand_board.h"
#include "sand_config.h"
#include "sand_input.h"
#include "sand_menu.h"
#include "sand_render.h"
#include "sand_scene.h"
#include "sand_sim.h"
#include "sand_trace.h"
#include "sand_usb.h"

static const char *TAG = "sand";

#define NVS_NAMESPACE   "sand"
#define NVS_KEY_BRIGHT  "bright"
/* Deliberately not the old "touch_or": that key spent a while collecting a
 * corrupted value from the era when a failed calibration stored its least-bad
 * guess, and must never be read again. */
#define NVS_KEY_TOUCH   "touch_or2"

/* How often the frame timing summary is printed, in frames. */
#define STATS_INTERVAL      200

volatile const char *g_sand_stage = "boot";
volatile int32_t g_sand_detail;
volatile uint32_t g_sand_frame;

/* Runs on the other core at the lowest useful priority, so it still gets
 * scheduled while the frame loop is parked in a driver call. */
static void watch_task(void *arg)
{
    uint32_t last_frame = 0;
    bool reported = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        const uint32_t frame = g_sand_frame;
        if (frame != last_frame) {
            last_frame = frame;
            reported = false;
            continue;
        }
        if (reported) {
            continue;
        }
        reported = true;
        ESP_LOGE(TAG, "frame loop STUCK in stage '%s' (detail %ld) at frame %lu",
                 g_sand_stage, (long)g_sand_detail, (unsigned long)frame);
    }
}

static void menu_repaint(void);

/* What a drag lays down, and whether it lays anything down at all. Both come
 * from the menus and then stick. */
static int s_material = MAT_SAND;
static bool s_erasing;
static bool s_filling;
/* Touch does nothing at all. Tilting the board is half of what it is for, and
 * you cannot tilt it without holding it, so there has to be a way to put a
 * thumb on the glass without ploughing a furrow through the scene. */
static bool s_inert;
static bool s_paused;
static int s_brush_px = 12;

/* Panel brightness, as a percentage. A setting rather than a constant now, so
 * it lives in NVS and survives a power cycle - having to re-dim the thing at
 * every boot would defeat the point of it. */
static const uint8_t BRIGHT_LEVELS[4] = { 20, 45, 70, 100 };
static int s_brightness = DISPLAY_BRIGHTNESS;

/* Touch arrives in screen pixels; the grid is whatever the current grain
 * size makes it. */
static void screen_to_cell(int px, int py, int *cx, int *cy)
{
    int x = (px - g_origin_x) / g_cell_px;
    int y = (py - g_origin_y) / g_cell_px;
    if (x < 0) x = 0; else if (x >= g_grid_w) x = g_grid_w - 1;
    if (y < 0) y = 0; else if (y >= g_grid_h) y = g_grid_h - 1;
    *cx = x;
    *cy = y;
}

/* Brush is set on the glass, so it covers the same area whatever the grain. */
static int brush_cells(void)
{
    const int r = s_brush_px / g_cell_px;
    return (r < 1) ? 1 : r;
}

/* Save and load both ask "which slot?", so the choice is parked here while
 * the slot ring is up. */
static menu_icon_t s_pending = ICON_NONE;

/* Which slot the scene on screen came from, so the ring can say where you
 * are as well as what is filled. Boot restores the first one. */
static int s_slot;

static const char *action_name(menu_icon_t action)
{
    switch (action) {
    case ICON_SAVE:   return "save";
    case ICON_IMPORT: return "import";
    default:          return "load";
    }
}

static void open_slot_ring(menu_icon_t action)
{
    s_pending = action;
    /* Importing reads a different set of slots from saving and loading - the
     * PNGs on the drive, not the scenes in flash - so the ring has to be told
     * which of the two it is showing. */
    menu_set_slot_usage(action == ICON_IMPORT ? usb_png_mask() : scene_used_mask());
    menu_open(MENU_SET_SLOTS, FB_W / 2, FB_H / 2);
    /* The middle shows which operation you are committing to - the pips alone
     * cannot say whether a pick would load or overwrite. */
    menu_set_centre(action);
    menu_repaint();
    ESP_LOGI(TAG, "%s: pick a slot", action_name(action));
}

/* One buffer serves every path that hands a whole grid to the simulation. */
static uint8_t *grid_scratch(void)
{
    static uint8_t *scratch;
    if (!scratch) {
        scratch = heap_caps_malloc(GRID_CELLS_MAX, MALLOC_CAP_SPIRAM);
        if (!scratch) {
            ESP_LOGE(TAG, "no PSRAM for the load buffer");
        }
    }
    return scratch;
}

/* Both sources of a whole scene - a saved slot and an imported image - carry
 * the grain size they were built at, so adopting one switches to it. The
 * cells only mean anything at their own resolution. */
static bool install_grid(uint8_t *cells, int cell_px)
{
    if (cell_px != g_cell_px) {
        sand_init(cell_px);
    }
    sand_load_grid(cells);
    return true;
}

static bool load_scene(int slot)
{
    uint8_t *scratch = grid_scratch();
    if (!scratch) {
        return false;
    }

    int cell_px = g_cell_px;
    if (scene_load(slot, scratch, &cell_px) != ESP_OK) {
        return false;
    }
    return install_grid(scratch, cell_px);
}

static bool import_png(int slot)
{
    uint8_t *scratch = grid_scratch();
    if (!scratch) {
        return false;
    }

    int cell_px = g_cell_px;
    if (usb_png_load(slot, scratch, &cell_px) != ESP_OK) {
        return false;
    }
    return install_grid(scratch, cell_px);
}

/* The action is passed in rather than read back from s_pending: the caller
 * clears that as soon as the ring closes, and reading it here quietly turned
 * every save into a load. */
static void use_slot(int slot, menu_icon_t action)
{
    switch (action) {
    case ICON_SAVE:
        if (scene_save(slot, sand_grid()) == ESP_OK) {
            s_slot = slot;
        }
        break;
    case ICON_IMPORT:
        if (!import_png(slot)) {
            ESP_LOGW(TAG, "no slot%d.png to import", slot + 1);
        }
        break;
    default:
        if (load_scene(slot)) {
            s_slot = slot;
        } else {
            ESP_LOGW(TAG, "slot %d has nothing to load", slot + 1);
        }
        break;
    }
}

/* The menu speaks screen pixels, the dirty tracking speaks grid cells. They
 * used to be the same unit; they stopped being so when the grain size became
 * a setting. Rounding outwards so a partly covered cell still repaints. */
static void force_dirty_px(int x0, int y0, int x1, int y1)
{
    sand_force_dirty((x0 - g_origin_x) / g_cell_px,
                     (y0 - g_origin_y) / g_cell_px,
                     (x1 - g_origin_x) / g_cell_px + 1,
                     (y1 - g_origin_y) / g_cell_px + 1);
}

static void menu_repaint(void)
{
    int x0, y0, x1, y1;
    menu_bounds(&x0, &y0, &x1, &y1);
    force_dirty_px(x0, y0, x1, y1);
}

/* Repaint one segment's pixel box, if the icon is on the open ring. */
static void selection_repaint(menu_icon_t icon)
{
    int x0, y0, x1, y1;
    menu_selection_bounds(icon, &x0, &y0, &x1, &y1);
    if (x1 >= x0 && y1 >= y0) {
        force_dirty_px(x0, y0, x1, y1);
    }
}

static void apply_tool(int from_px, int from_py, int to_px, int to_py)
{
    if (s_inert) {
        return;
    }

    int fx, fy, tx, ty;
    screen_to_cell(from_px, from_py, &fx, &fy);
    screen_to_cell(to_px, to_py, &tx, &ty);

    const int material = s_erasing ? MAT_EMPTY : s_material;

    if (s_filling) {
        /* One shot per press, not per frame of a drag. */
        sand_fill_region(tx, ty, material);
        return;
    }
    sand_paint(fx, fy, tx, ty, brush_cells(), material);
}

static void handle_touch(int x, int y, int *last_x, int *last_y, bool *was_touching)
{
    if (menu_is_open()) {
        /* Only the wedges that changed state repaint. The menu covers the
         * whole panel now, and repainting all of it on every touch sample
         * would mean pushing a full frame per tracked finger position. */
        const menu_icon_t before = menu_selection();
        if (menu_track(x, y) != before) {
            selection_repaint(before);
            selection_repaint(menu_selection());
        }
    } else if (!s_filling || !*was_touching) {
        /* Straight to painting - the menus live on the button, so a press has
         * nothing to disambiguate and never has to be held back. The bucket
         * is the exception: it acts once, on the press. */
        const int from_x = *was_touching ? *last_x : x;
        const int from_y = *was_touching ? *last_y : y;
        apply_tool(from_x, from_y, x, y);
    }

    *last_x = x;
    *last_y = y;
    *was_touching = true;
}

static const char *material_name(int material)
{
    switch (material) {
    case MAT_SAND:  return "sand";
    case MAT_WATER: return "water";
    case MAT_ACID:  return "acid";
    case MAT_ACID_GAS: return "acid gas";
    case MAT_LAVA:  return "lava";
    case MAT_FIRE:  return "fire";
    case MAT_STEAM: return "steam";
    case MAT_WOOD:  return "wood";
    case MAT_DIRT:  return "dirt";
    case MAT_ROCK:  return "rock";
    default:        return "?";
    }
}

/* Brush radii on the glass, so they cover the same area whatever the grain. */
static const int BRUSH_PX[4] = { 4, 12, 24, 40 };

static menu_icon_t material_icon(int material)
{
    switch (material) {
    case MAT_SAND:  return ICON_SAND;
    case MAT_WATER: return ICON_WATER;
    case MAT_ACID:  return ICON_ACID;
    case MAT_ACID_GAS: return ICON_ACID_GAS;
    case MAT_LAVA:  return ICON_LAVA;
    case MAT_FIRE:  return ICON_FIRE;
    case MAT_STEAM: return ICON_STEAM;
    case MAT_WOOD:  return ICON_WOOD;
    case MAT_DIRT:  return ICON_DIRT;
    case MAT_ROCK:  return ICON_ROCK;
    default:        return ICON_NONE;
    }
}

/* Light whatever each ring is currently set to, so opening one answers "what
 * am I painting with, how big, how coarse?" instead of asking you to
 * remember. Driven off the state itself rather than updated at every place
 * that changes it - that way the two cannot drift apart. */
static void refresh_lights(void)
{
    const menu_icon_t mat = material_icon(s_material);
    /* Every material icon, which is why they are kept contiguous in the enum:
     * ICON_SAND first, ICON_DIRT last. Adding one past the end of that run
     * silently leaves it unable to show as selected. */
    for (menu_icon_t i = ICON_SAND; i <= ICON_DIRT; i++) {
        /* Fill uses the current material, so the material stays lit; erase
         * and the null tool replace it, so it does not. */
        menu_set_lit(i, !s_erasing && !s_inert && i == mat);
    }
    menu_set_lit(ICON_ERASE,  s_erasing);
    menu_set_lit(ICON_FILL,   s_filling);
    menu_set_lit(ICON_NODRAW, s_inert);
    menu_set_lit(ICON_PAUSE,  s_paused);
    menu_set_lit(ICON_USB,    usb_storage_shared());

    for (int i = 0; i < 4; i++) {
        menu_set_lit(ICON_BRUSH1 + i, s_brush_px == BRUSH_PX[i]);
        menu_set_lit(ICON_BRIGHT1 + i, s_brightness == BRIGHT_LEVELS[i]);
    }
    for (int i = 0; i <= CELL_PX_MAX - 2; i++) {
        menu_set_lit(ICON_SIZE2 + i, g_cell_px == i + 2);
    }

    for (int i = 0; i < SCENE_SLOTS; i++) {
        /* An import has no current slot: that ring is showing the PNGs on the
         * drive, which have nothing to do with which scene you are in. */
        menu_set_lit(ICON_SLOT1 + i, s_pending != ICON_IMPORT && i == s_slot);
    }
}

static void pick_material(int material)
{
    s_material = material;
    /* Choosing something to lay down implies laying it down, so it also
     * leaves erase and fill - there is no separate "draw" tool any more. */
    s_erasing = false;
    s_filling = false;
    s_inert = false;
    ESP_LOGI(TAG, "material: %s", material_name(material));
}

/* Show the pen when the bowl is empty, and the pause bars while stopped, so
 * the panel always says something about what the board is doing. */
static void update_hint(void)
{
    /* The hint arc reaches above the square a menu repaints, so opening a
     * ring over it left the slice outside that square on the panel. Repaint
     * the hint's own area whenever a ring opens or closes: the overlay skips
     * the hint while a ring is up, so this clears it on open and restores it
     * on close. */
    static bool menu_was_open;
    if (menu_is_open() != menu_was_open) {
        menu_was_open = menu_is_open();
        if (menu_hint() != HINT_NONE) {
            int x0, y0, x1, y1;
            menu_hint_bounds(&x0, &y0, &x1, &y1);
            force_dirty_px(x0, y0, x1, y1);
        }
    }

    const menu_hint_t want = s_paused            ? HINT_PAUSED
                           : (sand_count() == 0) ? HINT_DRAW
                                                 : HINT_NONE;
    if (want == menu_hint()) {
        return;
    }

    /* The prompt wraps the rim and the pause mark fills the middle, so the
     * two cover quite different ground. Repaint the outgoing one's area as
     * well as the incoming one's, or whichever was larger leaves a ghost. */
    int x0, y0, x1, y1;
    menu_hint_bounds(&x0, &y0, &x1, &y1);
    force_dirty_px(x0, y0, x1, y1);

    menu_set_hint(want);

    menu_hint_bounds(&x0, &y0, &x1, &y1);
    force_dirty_px(x0, y0, x1, y1);
}

static void handle_release(void)
{
    if (!menu_is_open()) {
        return;
    }

    const menu_icon_t picked = menu_selection();
    menu_close();
    menu_repaint();

    const menu_icon_t pending = s_pending;
    s_pending = ICON_NONE;

    switch (picked) {
    /* Centre buttons that lead somewhere rather than doing something. */
    case ICON_TOOLS:
        menu_open(MENU_SET_TOOLS, FB_W / 2, FB_H / 2);
        menu_repaint();
        return;
    case ICON_PALETTE:
        menu_open(MENU_SET_PAINT, FB_W / 2, FB_H / 2);
        menu_repaint();
        return;

    case ICON_ERASE:
        s_erasing = true;  s_filling = false; s_inert = false;
        ESP_LOGI(TAG, "tool: erase");
        break;
    case ICON_FILL:
        s_erasing = false; s_filling = true;  s_inert = false;
        ESP_LOGI(TAG, "tool: fill");
        break;
    case ICON_NODRAW:
        s_inert = true;
        ESP_LOGI(TAG, "tool: none - touch will not draw");
        break;

    case ICON_SETTINGS:
        menu_open(MENU_SET_GRAIN, FB_W / 2, FB_H / 2);
        menu_repaint();
        return;

    case ICON_BRIGHT:
        menu_open(MENU_SET_BRIGHT, FB_W / 2, FB_H / 2);
        menu_repaint();
        return;

    case ICON_BRIGHT1: case ICON_BRIGHT2:
    case ICON_BRIGHT3: case ICON_BRIGHT4: {
        s_brightness = BRIGHT_LEVELS[picked - ICON_BRIGHT1];
        board_brightness_set(s_brightness);
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, NVS_KEY_BRIGHT, (uint8_t)s_brightness);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "brightness: %d%%", s_brightness);
        /* Stay in the ring: the change is on the panel in front of you, so
         * being able to try the next one straight away is the whole point. */
        menu_open(MENU_SET_BRIGHT, FB_W / 2, FB_H / 2);
        menu_repaint();
        return;
    }

    case ICON_SIZE2: case ICON_SIZE3: case ICON_SIZE4:
    case ICON_SIZE5: case ICON_SIZE6:
        /* Rebuilding the grid cannot preserve the contents - a cell at one
         * grain is not a cell at another - so it starts fresh, and fresh
         * means ramp colours even after an original-colours import. */
        sand_init(picked - ICON_SIZE2 + 2);
        sand_render_palette_reset();
        break;

    case ICON_BRUSH1: case ICON_BRUSH2: case ICON_BRUSH3: case ICON_BRUSH4:
        s_brush_px = BRUSH_PX[picked - ICON_BRUSH1];
        ESP_LOGI(TAG, "brush: %d px", s_brush_px);
        break;

    case ICON_PAUSE:
        s_paused = !s_paused;
        ESP_LOGI(TAG, "%s", s_paused ? "paused" : "running");
        break;

    /* These three do not act yet - they open the slot ring and wait. */
    case ICON_SAVE:
    case ICON_LOAD:
    case ICON_IMPORT:
        open_slot_ring(picked);
        return;

    case ICON_SLOT1: case ICON_SLOT2: case ICON_SLOT3:
    case ICON_SLOT4: case ICON_SLOT5: case ICON_SLOT6:
        use_slot(picked - ICON_SLOT1, pending);
        break;

    case ICON_USB: {
        /* Stay in the settings ring so the switch is seen to change - it is
         * the only button here that has a state rather than an effect. */
        const bool want = !usb_storage_shared();
        if (usb_storage_share(want) != ESP_OK) {
            ESP_LOGW(TAG, "could not %s sharing", want ? "start" : "stop");
        }
        menu_open(MENU_SET_GRAIN, FB_W / 2, FB_H / 2);
        menu_repaint();
        return;
    }

    case ICON_CLEAR:
        sand_clear_all();
        sand_render_palette_reset();
        ESP_LOGI(TAG, "cleared");
        break;

    case ICON_POWER:
        ESP_LOGI(TAG, "powering down");
        board_brightness_set(0);
        sand_input_power_off();
        break;

    case ICON_SAND:  pick_material(MAT_SAND);  break;
    case ICON_WATER: pick_material(MAT_WATER); break;
    case ICON_ACID:  pick_material(MAT_ACID);  break;
    case ICON_ACID_GAS: pick_material(MAT_ACID_GAS); break;
    case ICON_LAVA:  pick_material(MAT_LAVA);  break;
    case ICON_FIRE:  pick_material(MAT_FIRE);  break;
    case ICON_STEAM: pick_material(MAT_STEAM); break;
    case ICON_WOOD:  pick_material(MAT_WOOD);  break;
    case ICON_DIRT:  pick_material(MAT_DIRT);  break;
    case ICON_ROCK:  pick_material(MAT_ROCK);  break;

    default:
        break;      /* lifted off in the dead zone: cancelled */
    }
}

static void sand_task(void *arg)
{
    TickType_t next_frame = xTaskGetTickCount();
    int64_t physics_us = 0;
    int64_t render_us = 0;
    uint32_t frames = 0;

    /* Previous fingertip position, so a drag can shove grains along. */
    int last_x = 0, last_y = 0;
    bool was_touching = false;

    /* If a driver call swallows this task, the watchdog dumps a backtrace
     * naming the exact call instead of the board just going quiet. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        esp_task_wdt_reset();
        g_sand_frame++;

        SAND_STAGE("button");
        switch (sand_input_button()) {
        case BUTTON_CLICK:
            if (menu_is_open()) {
                menu_close();
                menu_repaint();
            } else {
                /* The one changed most often, so it gets the quicker gesture. */
                menu_open(MENU_SET_PAINT, FB_W / 2, FB_H / 2);
                menu_repaint();
            }
            break;
        case BUTTON_HOLD:
            /* Freshen the gauge before the ring draws it. One I2C read per
             * open is plenty - the number cannot drift meaningfully while a
             * menu is on screen. */
            menu_set_battery(sand_input_battery());
            menu_open(MENU_SET_SYSTEM, FB_W / 2, FB_H / 2);
            menu_repaint();
            break;
        default:
            break;
        }

        SAND_STAGE("imu-read");
        float gx, gy;
        sand_input_gravity(&gx, &gy);

        SAND_STAGE("touch-read");
        int touch_x, touch_y;
        if (sand_input_touch(&touch_x, &touch_y)) {
            handle_touch(touch_x, touch_y, &last_x, &last_y, &was_touching);
        } else if (was_touching) {
            handle_release();
            was_touching = false;
        }

        /* An empty bowl looks identical to a dead board, so put up a dim
         * prompt whenever there is nothing to watch. */
        update_hint();

        /* Recomputed here rather than at each place a setting changes: it is
         * a handful of bit operations, it only runs while a ring is actually
         * on screen, and it cannot fall out of step with the state. */
        if (menu_is_open()) {
            refresh_lights();
        }

        SAND_STAGE("physics");
        const int64_t t0 = esp_timer_get_time();
        /* An open ring holds the simulation still. Picking from it takes a
         * moment, and having the scene run out from under the choice - sand
         * draining away while you decide - is worse than a brief freeze. It
         * also leaves the frame budget to the menu, which repaints its whole
         * area every frame while it tracks a finger. */
        if (!s_paused && !menu_is_open()) {
            sand_step(gx, gy);
        }
        const int64_t t1 = esp_timer_get_time();
        sand_render_frame(sand_grid());
        const int64_t t2 = esp_timer_get_time();
        SAND_STAGE("pacing");

        physics_us += t1 - t0;
        render_us += t2 - t1;

        if (++frames == STATS_INTERVAL) {
            ESP_LOGI(TAG, "physics %.1f ms | render %.1f ms | busy %.0f%% | "
                          "gravity %+.2f,%+.2f | %lu grains",
                     (double)physics_us / (STATS_INTERVAL * 1000.0),
                     (double)render_us / (STATS_INTERVAL * 1000.0),
                     (double)(physics_us + render_us) /
                         (STATS_INTERVAL * FRAME_PERIOD_MS * 10.0),
                     gx, gy, (unsigned long)sand_count());
            physics_us = render_us = 0;
            frames = 0;
        }

        /* vTaskDelayUntil returns immediately once the deadline has already
         * passed, so an overrunning frame would spin without ever yielding
         * and starve the idle task. Re-base and give up a tick instead. */
        if ((TickType_t)(xTaskGetTickCount() - next_frame) >= pdMS_TO_TICKS(FRAME_PERIOD_MS)) {
            next_frame = xTaskGetTickCount();
            vTaskDelay(1);
        } else {
            vTaskDelayUntil(&next_frame, pdMS_TO_TICKS(FRAME_PERIOD_MS));
        }
    }
}


/* ------------------------------------------------------------------------
 * Touch calibration: two dots, two taps, instructions on the glass.
 *
 * Two markers, well separated along BOTH axes and off both diagonals - a
 * point near a diagonal is useless because several orientations map it to
 * the same place, and poor separation on one axis leaves the fit almost
 * blind to a flip of that axis.
 * ---------------------------------------------------------------------- */
#define CAL_A_X         28
#define CAL_A_Y         82
#define CAL_B_X         88
#define CAL_B_Y         34
#define CAL_MARKER_R    8
#define CAL_TOLERANCE   60      /* per-tap miss, in screen pixels */
#define CAL_ATTEMPTS    3
#define CAL_TAP_WAIT_US (30 * 1000000LL)    /* per prompt, then give up */

static int cal_screen_x(int cell_x) { return g_origin_x + cell_x * g_cell_px + g_cell_px / 2; }
static int cal_screen_y(int cell_y) { return g_origin_y + cell_y * g_cell_px + g_cell_px / 2; }

/* One dot and the banner, on an otherwise empty bowl. */
static void cal_show(int cell_x, int cell_y, const char *text)
{
    menu_set_banner(text);
    sand_clear_all();
    if (cell_x >= 0) {
        sand_paint(cell_x, cell_y, cell_x, cell_y, CAL_MARKER_R, MAT_ROCK);
    }
    sand_render_frame(sand_grid());
}

/* Just words, held on screen long enough to read. */
static void cal_message(const char *text)
{
    cal_show(-1, 0, text);
    vTaskDelay(pdMS_TO_TICKS(1200));
}

/* Wait for the finger to come off, then for a fresh press - otherwise one
 * long touch would answer both prompts. Bounded, so a dead controller (or a
 * walked-away user) cannot park boot in calibration forever. */
static bool cal_wait_tap(int *raw_x, int *raw_y)
{
    const int64_t deadline = esp_timer_get_time() + CAL_TAP_WAIT_US;
    int ignored_x, ignored_y;
    while (sand_input_touch_raw(&ignored_x, &ignored_y)) {
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    while (!sand_input_touch_raw(raw_x, raw_y)) {
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

static void calibrate_touch(void)
{
    const int fallback = sand_input_touch_orientation();
    bool fits = false;
    int best = fallback;

    for (int attempt = 1; attempt <= CAL_ATTEMPTS; attempt++) {
        int ax, ay, bx, by, error = 0;

        cal_show(CAL_A_X, CAL_A_Y, "TAP THE DOT");
        ESP_LOGI(TAG, "calibration %d/%d: tap the first dot", attempt, CAL_ATTEMPTS);
        if (!cal_wait_tap(&ax, &ay)) {
            break;
        }

        cal_show(CAL_B_X, CAL_B_Y, "NOW THIS ONE");
        ESP_LOGI(TAG, "calibration %d/%d: tap the second dot", attempt, CAL_ATTEMPTS);
        if (!cal_wait_tap(&bx, &by)) {
            break;
        }

        best = sand_input_touch_solve(ax, ay, cal_screen_x(CAL_A_X), cal_screen_y(CAL_A_Y),
                                      bx, by, cal_screen_x(CAL_B_X), cal_screen_y(CAL_B_Y),
                                      &error);

        ESP_LOGI(TAG, "A raw=(%d,%d) want (%d,%d), B raw=(%d,%d) want (%d,%d)",
                 ax, ay, cal_screen_x(CAL_A_X), cal_screen_y(CAL_A_Y),
                 bx, by, cal_screen_x(CAL_B_X), cal_screen_y(CAL_B_Y));

        if (error <= CAL_TOLERANCE) {
            ESP_LOGI(TAG, "calibrated: orientation %d, %d px out", best, error);
            fits = true;
            break;
        }

        ESP_LOGW(TAG, "best fit is orientation %d but still %d px out", best, error);
        cal_message("TRY AGAIN");
    }

    if (!fits) {
        /* Taps that fit no rotation or mirror are noise - a missed dot, a
         * knuckle on the glass. Storing them anyway is how a mirrored touch
         * map once got burned into flash, so a calibration that never
         * converged changes nothing. */
        ESP_LOGE(TAG, "calibration never fit; keeping orientation %d", fallback);
        sand_input_touch_set_orientation(fallback);
        cal_message("SKIPPED");
        menu_set_banner(NULL);
        sand_init(g_cell_px);
        return;
    }

    sand_input_touch_set_orientation(best);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_TOUCH, (uint8_t)best);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    cal_message("TOUCH OK");
    menu_set_banner(NULL);
    /* Undo the markers and whatever they displaced. */
    sand_init(g_cell_px);
}

/* Forcing a redo takes a full second of holding Key1, not an instant of the
 * pin reading low: Key1 is also the boot strap and the menu button, so a
 * bounce - or a reboot that happens mid menu gesture - must never be enough
 * to re-enter calibration by accident. */
static bool button_held_for_calibration(void)
{
    for (int i = 0; i < 20; i++) {
        if (gpio_get_level(BUTTON_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static void setup_touch_orientation(void)
{
    uint8_t stored = 0;
    bool have_stored = false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        have_stored = (nvs_get_u8(nvs, NVS_KEY_TOUCH, &stored) == ESP_OK);
        nvs_close(nvs);
    }

    if (have_stored && !button_held_for_calibration()) {
        sand_input_touch_set_orientation(stored);
        ESP_LOGI(TAG, "touch orientation %u (stored; hold the button at boot to redo)",
                 (unsigned)stored);
        return;
    }

    /* First boot, or asked for: calibration announces itself on the glass
     * now, so running it here is no longer a trap for stray fingers - and a
     * failed or ignored run falls back to the compile-time guess and boots
     * on regardless. */
    calibrate_touch();
}

void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* Before anything claims the I2C pins, so the readings mean something. */
    sand_input_bus_check();

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    /* One band per transaction is all the transfer size ever needs. */
    ESP_ERROR_CHECK(board_display_new(BAND_BYTES, &panel, &panel_io));
    /* Dark until the first frame is up, so the panel's power-on contents
     * never show. */
    board_brightness_set(0);

    ESP_ERROR_CHECK(sand_render_init(panel, panel_io));
    sand_input_init();

    ESP_ERROR_CHECK(sand_init(CELL_PX_DEFAULT));
    scene_init();
    /* Mounts the FAT partition for our own use. Nothing appears on a host
     * until the settings ring asks for it, so the serial console is intact
     * for anyone who is watching this boot. */
    usb_storage_init();
    /* Pick up where the last session left off. The scene carries its own
     * grain size, so this also restores that setting. */
    if (load_scene(0)) {
        ESP_LOGI(TAG, "restored slot 1");
    }
    sand_render_frame(sand_grid());
    {
        nvs_handle_t nvs;
        uint8_t stored;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            if (nvs_get_u8(nvs, NVS_KEY_BRIGHT, &stored) == ESP_OK && stored > 0) {
                s_brightness = stored;
            }
            nvs_close(nvs);
        }
    }
    board_brightness_set(s_brightness);

    setup_touch_orientation();
    sand_render_frame(sand_grid());

    /* Roomier than the frame loop needs: a PNG import runs on this task, and
     * libpng is by far the deepest thing that ever does. */
    xTaskCreatePinnedToCore(sand_task, "sand", 10240, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(watch_task, "watch", 3072, NULL, 1, NULL, 0);
}
