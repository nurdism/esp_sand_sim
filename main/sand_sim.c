#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "sand_sim.h"

static const char *TAG = "sand_sim";

#define SAND_PI  3.14159265358979f

/* The eight neighbour directions, ordered by angle. Screen space, so +y is
 * down and index k sits at k * 45 degrees. */
static const int8_t DIR_X[8] = {  1,  1,  0, -1, -1, -1,  0,  1 };
static const int8_t DIR_Y[8] = {  0,  1,  1,  1,  0, -1, -1, -1 };

/* Same directions as a flat index offset, which is what the inner loop wants. */
static int16_t s_delta[8];

int g_cell_px = CELL_PX_DEFAULT;
int g_grid_w, g_grid_h, g_grid_cells;
int g_origin_x, g_origin_y;
int g_bowl_radius;

/* Sized for the finest grain and reused at every setting. Only the grid is
 * big enough to be worth allocating; sand_init keeps it in internal RAM,
 * where at the finest supported grain it is 54 KB. */
static uint8_t *s_grid;

/* Inclusive span of non-wall cells per row. The bowl is a circle, so most
 * rows are a good deal narrower than the grid and the corners never get
 * looked at once. */
static int16_t s_row_x0[GRID_H_MAX];
static int16_t s_row_x1[GRID_H_MAX];

/* Cells that already moved this substep, so nothing travels twice in one pass
 * no matter which way the scan runs. */
#define MOVED_WORDS  ((GRID_CELLS_MAX + 31) / 32)
static uint32_t s_moved[MOVED_WORDS];
#define MOVED_GET(i) (s_moved[(uint32_t)(i) >> 5] &   (1u << ((i) & 31)))
#define MOVED_SET(i) (s_moved[(uint32_t)(i) >> 5] |=  (1u << ((i) & 31)))

static int16_t s_dirty_lo[GRID_H_MAX];
static int16_t s_dirty_hi[GRID_H_MAX];

static inline IRAM_ATTR void mark_dirty(int x, int y)
{
    if (x < s_dirty_lo[y]) {
        s_dirty_lo[y] = (int16_t)x;
    }
    if (x > s_dirty_hi[y]) {
        s_dirty_hi[y] = (int16_t)x;
    }
}

/* Physics activity spans: the same shape as the dirty spans, but for the
 * stepper rather than the renderer. A pass only walks each row between its
 * span ends, so the settled bulk of a heap - which is most of the bowl, most
 * of the time - is never even looked at.
 *
 * Double buffered: one page is consumed by the pass that is running while the
 * other collects what still needs looking at next pass. Wakes are written to
 * BOTH pages so nothing is lost to the swap, whichever page a caller lands
 * on; the stale copy is cleared when its page next becomes the collector. */
static int16_t s_act_lo[2][GRID_H_MAX];
static int16_t s_act_hi[2][GRID_H_MAX];
static int s_act_page;

static inline IRAM_ATTR void span_grow(int16_t *lo, int16_t *hi, int x0, int x1)
{
    if (x0 < *lo) {
        *lo = (int16_t)x0;
    }
    if (x1 > *hi) {
        *hi = (int16_t)x1;
    }
}

/* Keep (x, y) itself under consideration next pass. */
static inline IRAM_ATTR void wake_cell(int x, int y)
{
    span_grow(&s_act_lo[0][y], &s_act_hi[0][y], x, x);
    span_grow(&s_act_lo[1][y], &s_act_hi[1][y], x, x);
}

/* A cell changed, so anything beside it may have become able to move. Safe
 * unclamped: writes only ever land on non-wall cells, and the bowl circle
 * keeps those at least one cell inside the grid on every side. */
static inline IRAM_ATTR void wake_around(int x, int y)
{
    for (int dy = -1; dy <= 1; dy++) {
        const int ny = y + dy;
        span_grow(&s_act_lo[0][ny], &s_act_hi[0][ny], x - 1, x + 1);
        span_grow(&s_act_lo[1][ny], &s_act_hi[1][ny], x - 1, x + 1);
    }
}

/* Every simulation write goes through this: the renderer needs to repaint the
 * cell, and the stepper needs to reconsider its neighbourhood. mark_dirty
 * alone stays for overlays - the menu repaints pixels without touching the
 * grid, and must not wake anything. */
static inline IRAM_ATTR void touch_cell(int x, int y)
{
    mark_dirty(x, y);
    wake_around(x, y);
}

static void wake_all(void)
{
    for (int page = 0; page < 2; page++) {
        for (int y = 0; y < g_grid_h; y++) {
            s_act_lo[page][y] = 0;
            s_act_hi[page][y] = (int16_t)(g_grid_w - 1);
        }
    }
}

static uint32_t s_active_count;
static uint32_t s_rng = 0x2545F491u;

static inline uint32_t rng_next(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static inline uint8_t make_cell(int material)
{
    return CELL_MAKE(material, rng_next() & SHADE_MASK);
}

/* Where a cell will try to go, as offsets from the direction it is being
 * pulled, in order of preference.
 *
 * Grains stop at +/-45 degrees, which is what gives a heap its angle of
 * repose. Liquids and gases carry on to +/-90 - straight sideways - so they
 * spread until level instead of stacking. */
static const int8_t ORDER_GRAIN_A[3] = { 0, +1, -1 };
static const int8_t ORDER_GRAIN_B[3] = { 0, -1, +1 };
static const int8_t ORDER_FLOW_A[5]  = { 0, +1, -1, +2, -2 };
static const int8_t ORDER_FLOW_B[5]  = { 0, -1, +1, -2, +2 };

typedef struct {
    const int8_t *order_a;
    const int8_t *order_b;
    uint8_t options;    /* zero means it never moves */
    bool rises;         /* against gravity rather than with it */
    uint8_t sluggish;   /* out of 256, how often it declines to move at all */
    /* Destroys whatever it displaces instead of swapping places with it.
     * Everything else trades: the heavier cell sinks and the lighter one is
     * carried up in its place. A flame climbing through its own smoke has no
     * business depositing that smoke back where it just came from. */
    bool devours;
} mat_rule_t;

static const mat_rule_t RULES[MAT_COUNT] = {
    [MAT_EMPTY]    = { NULL, NULL, 0, false, 0 },
    /* Fire climbs, but reluctantly: left to move every step it would race off
     * the top of the fuel instead of sitting in it and burning. It burns off
     * the smoke it rises through rather than shuffling it back underneath. */
    [MAT_FIRE]     = { ORDER_FLOW_A,  ORDER_FLOW_B,  5, true,  150, true },
    [MAT_SMOKE]    = { ORDER_FLOW_A,  ORDER_FLOW_B,  5, true,  0 },
    [MAT_STEAM]    = { ORDER_FLOW_A,  ORDER_FLOW_B,  5, true,  0 },
    /* Heavier and lazier than the other gases: it creeps rather than billows,
     * which is what keeps it looking corrosive instead of like steam. */
    [MAT_ACID_GAS] = { ORDER_FLOW_A,  ORDER_FLOW_B,  5, true,  170 },
    [MAT_WATER]    = { ORDER_FLOW_A,  ORDER_FLOW_B,  5, false, 0 },
    [MAT_ACID]     = { ORDER_FLOW_A,  ORDER_FLOW_B,  5, false, 0 },
    /* Lava spreads like a liquid but mostly sits still, which is what makes
     * it read as molten rock rather than orange water. */
    [MAT_LAVA]     = { ORDER_FLOW_A,  ORDER_FLOW_B,  5, false, 190 },
    [MAT_SAND]     = { ORDER_GRAIN_A, ORDER_GRAIN_B, 3, false, 0 },
    /* Soil: the same grain rules as sand but heavier and slower to shift, so
     * it slumps rather than pours and sand floats on top of it. */
    [MAT_DIRT]     = { ORDER_GRAIN_A, ORDER_GRAIN_B, 3, false, 90 },
    [MAT_WOOD]     = { NULL, NULL, 0, false, 0 },
    [MAT_ROCK]     = { NULL, NULL, 0, false, 0 },
    [MAT_WALL]     = { NULL, NULL, 0, false, 0 },
};

/* The same facts as RULES, but as bitmasks the inner loop can test with one
 * shift instead of a table load. MOVABLE is every material with options != 0;
 * SETTLES is the subset with no behaviour beyond falling - no reactions, no
 * decay - which is what makes them safe to put to sleep once they are
 * provably stuck. Everything else acts on its own schedule (fire cools, gas
 * thins, acid bites, lava reacts) and stays awake for as long as it exists. */
#define MAT_BIT(m)      (1u << (m))
#define MOVABLE_MASK    (MAT_BIT(MAT_FIRE) | MAT_BIT(MAT_SMOKE) | MAT_BIT(MAT_STEAM) | \
                         MAT_BIT(MAT_ACID_GAS) | MAT_BIT(MAT_WATER) | MAT_BIT(MAT_ACID) | \
                         MAT_BIT(MAT_LAVA) | MAT_BIT(MAT_SAND) | MAT_BIT(MAT_DIRT))
#define SETTLES_MASK    (MAT_BIT(MAT_SAND) | MAT_BIT(MAT_WATER) | MAT_BIT(MAT_DIRT))

const uint8_t *sand_grid(void)          { return s_grid; }
const int16_t *sand_dirty_lo(void)      { return s_dirty_lo; }
const int16_t *sand_dirty_hi(void)      { return s_dirty_hi; }
uint32_t sand_count(void)               { return s_active_count; }

void sand_clear_dirty(void)
{
    for (int y = 0; y < g_grid_h; y++) {
        s_dirty_lo[y] = g_grid_w;
        s_dirty_hi[y] = -1;
    }
}

void sand_force_dirty(int x0, int y0, int x1, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > g_grid_h - 1) y1 = g_grid_h - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > g_grid_w - 1) x1 = g_grid_w - 1;

    for (int y = y0; y <= y1; y++) {
        mark_dirty(x0, y);
        mark_dirty(x1, y);
    }
}

static void mark_all_dirty(void)
{
    for (int y = 0; y < g_grid_h; y++) {
        s_dirty_lo[y] = 0;
        s_dirty_hi[y] = g_grid_w - 1;
    }
}

void sand_clear_all(void)
{
    for (int y = 0; y < g_grid_h; y++) {
        for (int x = s_row_x0[y]; x <= s_row_x1[y]; x++) {
            s_grid[y * g_grid_w + x] = CELL_EMPTY;
        }
    }
    s_active_count = 0;
    mark_all_dirty();
    wake_all();
}

/* Put the wall back around whatever was just dropped into the grid.
 *
 * The inner loop skips bounds checks because the outermost ring is always
 * wall, so a grid that arrives without one is not merely wrong on screen - it
 * lets grains walk off the end of the array. Cells coming from a file or an
 * imported image are not ours to trust, so the wall is re-stamped rather than
 * assumed. The row extents were worked out when the grid was built and still
 * hold; only the contents changed. */
static void reseal_bowl(void)
{
    for (int y = 0; y < g_grid_h; y++) {
        uint8_t *row = s_grid + y * g_grid_w;
        for (int x = 0; x < s_row_x0[y]; x++) {
            row[x] = CELL_MAKE(MAT_WALL, 0);
        }
        for (int x = s_row_x1[y] + 1; x < g_grid_w; x++) {
            row[x] = CELL_MAKE(MAT_WALL, 0);
        }
    }
}

void sand_load_grid(const uint8_t *cells)
{
    memcpy(s_grid, cells, g_grid_cells);
    reseal_bowl();

    /* Recount rather than trusting the file: the tally is only a statistic,
     * and a stale one would be confusing in the log. */
    s_active_count = 0;
    for (int i = 0; i < g_grid_cells; i++) {
        const int material = CELL_MAT(s_grid[i]);
        if (material != MAT_EMPTY && material != MAT_WALL) {
            s_active_count++;
        }
    }
    mark_all_dirty();
    wake_all();
}

static int s_substeps = 2;

void sand_grid_extent(int cell_px, int *w, int *h)
{
    if (cell_px < CELL_PX_MIN) cell_px = CELL_PX_MIN;
    if (cell_px > CELL_PX_MAX) cell_px = CELL_PX_MAX;

    int gw = FB_W / cell_px;
    int gh = FB_H / cell_px;

    /* The panel addresses columns and rows in pairs: every rectangle sent to
     * it has to start on an even coordinate and cover an even number of them.
     * (The BSP does this for LVGL in a rounder callback; drawing straight to
     * the panel means doing it here instead.)
     *
     * Keeping the grid's own extent even in pixels is what makes that always
     * possible - any sub-rectangle of it can then be widened by a cell to
     * suit. Without this an odd grain silently drew rectangles a pixel wider
     * than the window it had just opened, and the picture sheared. */
    if ((gw * cell_px) & 1) gw--;
    if ((gh * cell_px) & 1) gh--;

    *w = gw;
    *h = gh;
}

esp_err_t sand_init(int cell_px)
{
    if (cell_px < CELL_PX_MIN) cell_px = CELL_PX_MIN;
    if (cell_px > CELL_PX_MAX) cell_px = CELL_PX_MAX;

    g_cell_px = cell_px;
    sand_grid_extent(cell_px, &g_grid_w, &g_grid_h);

    g_grid_cells = g_grid_w * g_grid_h;
    g_origin_x = ((FB_W - g_grid_w * cell_px) / 2) & ~1;
    g_origin_y = ((FB_H - g_grid_h * cell_px) / 2) & ~1;
    g_bowl_radius = ((g_grid_w < g_grid_h ? g_grid_w : g_grid_h) / 2) - 1;

    /* Two everywhere. Finer grains want more substeps to fall at the same
     * rate on the glass, but across the 2-6 px range the difference is small
     * enough that a constant reads better than a varying frame cost. */
    s_substeps = 2;

    if (!s_grid) {
        /* Internal: it is the hottest array in the program, and at the
         * coarsest supported grain it is only 54 KB. */
        s_grid = heap_caps_malloc(GRID_CELLS_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_grid) {
            ESP_LOGE(TAG, "no room for a %d byte grid", GRID_CELLS_MAX);
            return ESP_ERR_NO_MEM;
        }
    }

    uint32_t seed = esp_random();
    s_rng = seed ? seed : 0x2545F491u;
    s_active_count = 0;

    for (int k = 0; k < 8; k++) {
        s_delta[k] = (int16_t)(DIR_Y[k] * g_grid_w + DIR_X[k]);
    }

    /* Carve the round bowl. The outermost ring always lands outside the
     * circle, which is what lets the inner loop skip bounds checks: nothing
     * that moves can ever sit against the edge of the array. */
    const float cx = (g_grid_w - 1) * 0.5f;
    const float cy = (g_grid_h - 1) * 0.5f;
    const float r2 = (float)g_bowl_radius * (float)g_bowl_radius;

    for (int y = 0; y < g_grid_h; y++) {
        int first = -1, last = -2;
        for (int x = 0; x < g_grid_w; x++) {
            const float dx = (float)x - cx;
            const float dy = (float)y - cy;
            const bool inside = (dx * dx + dy * dy <= r2);
            s_grid[y * g_grid_w + x] = inside ? CELL_EMPTY : CELL_MAKE(MAT_WALL, 0);
            if (inside) {
                if (first < 0) {
                    first = x;
                }
                last = x;
            }
        }
        s_row_x0[y] = (int16_t)(first < 0 ? 1 : first);
        s_row_x1[y] = (int16_t)(first < 0 ? 0 : last);
    }

    /* A heap of sand to start with, so there is something to tilt. */
    const int fill_from = (g_grid_h * 60) / 100;
    for (int y = fill_from; y < g_grid_h; y++) {
        for (int x = s_row_x0[y]; x <= s_row_x1[y]; x++) {
            const int i = y * g_grid_w + x;
            if (s_grid[i] == CELL_EMPTY) {
                s_grid[i] = make_cell(MAT_SAND);
                s_active_count++;
            }
        }
    }

    mark_all_dirty();
    wake_all();
    ESP_LOGI(TAG, "%dx%d cells at %d px, %d substeps",
             g_grid_w, g_grid_h, g_cell_px, s_substeps);
    return ESP_OK;
}

/* Try to shift the cell one step along direction `k`, taking the first
 * neighbour holding something lighter.
 *
 * No bounds checking: everything that moves is at least one cell inside the
 * array because the bowl circle never reaches the border, so i + delta is
 * always valid. */
static inline IRAM_ATTR bool try_move(uint8_t *const grid, int i, int x, int y,
                                      int k, uint8_t cell, uint32_t r)
{
    const int material = CELL_MAT(cell);
    const mat_rule_t *rule = &RULES[material];

    const int base = rule->rises ? ((k + 4) & 7) : k;
    const int8_t *order = (r & 0x10000) ? rule->order_a : rule->order_b;

    for (int c = 0; c < rule->options; c++) {
        const int dir = (base + order[c]) & 7;
        const int j = i + s_delta[dir];
        const uint8_t target = grid[j];

        /* Density is the material index, so this one compare covers empty,
         * lighter fluids, and the immovable rock and wall alike. */
        if (CELL_MAT(target) >= material) {
            continue;
        }

        grid[j] = cell;
        if (rule->devours && CELL_MAT(target) != MAT_EMPTY) {
            grid[i] = CELL_EMPTY;       /* consumed, not carried back */
            s_active_count--;
        } else {
            grid[i] = target;           /* whatever was there swaps back */
        }
        MOVED_SET(j);
        MOVED_SET(i);
        touch_cell(x, y);
        touch_cell(x + DIR_X[dir], y + DIR_Y[dir]);
        return true;
    }

    return false;
}

/* Chemistry, run before movement. Returns true when the cell no longer holds
 * what it did, so the mover leaves it alone this step. */

/* Acid eats sand and rock, and boils off into gas doing it. Left alone it
 * gradually loses its bite and becomes water. */
static inline IRAM_ATTR bool acid_react(uint8_t *const grid, int i, int x, int y, uint32_t r)
{
    /* Look at every neighbour for something to eat, from a random start so it
     * does not always bite the same way.
     *
     * This used to test one random direction, which meant acid sitting in a
     * bath of sand did nothing seven times in eight - and since a splash of
     * acid is consumed by what it dissolves, it usually vanished before it
     * happened to look the right way. Wood was simply missing from the list. */
    static const int8_t AROUND[8] = { 0, +1, -1, +2, -2, +3, -3, +4 };
    const int start = (int)(r & 7);

    int dir = -1;
    for (int c = 0; c < 8; c++) {
        const int d = (start + AROUND[c]) & 7;
        const int m = CELL_MAT(grid[i + s_delta[d]]);
        /* Never the bowl wall: that is what keeps the inner loop from running
         * off the end of the grid. */
        if (m == MAT_SAND || m == MAT_DIRT || m == MAT_ROCK || m == MAT_WOOD) {
            dir = d;
            break;
        }
    }

    if (dir >= 0 && (r & 0x30) == 0) {
        grid[i + s_delta[dir]] = CELL_EMPTY;
        s_active_count--;
        touch_cell(x + DIR_X[dir], y + DIR_Y[dir]);

        /* The acid is spent either way, but only a quarter of it comes off as
         * fumes. A puff for every single cell dissolved filled the bowl with
         * gas faster than the acid could eat its way through anything, and
         * hid the reaction that was producing it. Full shade is full life; it
         * counts down from there. */
        if ((r & 0xC0) == 0) {
            grid[i] = CELL_MAKE(MAT_ACID_GAS, SHADE_MASK);
        } else {
            grid[i] = CELL_EMPTY;
            s_active_count--;
        }
        touch_cell(x, y);
        return true;
    }

    /* Nothing to attack, so it slowly goes flat. */
    if ((r & 0x3FF) == 0) {
        grid[i] = CELL_MAKE(MAT_WATER, rng_next() & SHADE_MASK);
        touch_cell(x, y);
    }
    return false;
}

/* Both gases are inert - they just drift up and thin out. */
static inline IRAM_ATTR bool gas_decay(uint8_t *const grid, int i, int x, int y,
                                       uint8_t cell, uint32_t r)
{
    const int material = CELL_MAT(cell);

    if ((r & 0x0F) != 0) {
        return false;
    }

    const int life = CELL_SHADE(cell);
    if (life <= 1) {
        grid[i] = CELL_EMPTY;
        s_active_count--;
        touch_cell(x, y);
        return true;
    }

    grid[i] = CELL_MAKE(material, life - 1);
    touch_cell(x, y);
    return false;
}

/* How often a flame loses a shade of life: one step in (this + 1). */
#define FIRE_COOLING    3

/* Fire. It stands in the wood it is consuming, reaches for more, and dies
 * down into smoke.
 *
 * Spread is the whole point: lava only ever has to light the first cell, and
 * the flame front carries itself through the rest. Ignition favours the
 * up-gravity neighbour, so a fire climbs a wall of wood quickly and creeps
 * down through it slowly, which is what fire actually does. Turn the board
 * over and it climbs the other way, because "up" is wherever gravity is not. */
static inline IRAM_ATTR bool fire_react(uint8_t *const grid, int i, int x, int y,
                                        uint8_t cell, uint32_t r, int up)
{
    /* Look at every neighbour for fuel, working outward from up-gravity so
     * ignition climbs. Occasionally start somewhere else instead, which is
     * what keeps the burn front ragged rather than a flat advancing line. */
    static const int8_t AROUND[8] = { 0, +1, -1, +2, -2, +3, -3, +4 };
    const int start = ((r & 3) != 0) ? up : (int)((r >> 2) & 7);

    int fuel = -1, wet = -1;
    bool air = false;
    for (int c = 0; c < 8; c++) {
        const int dir = (start + AROUND[c]) & 7;
        const int m = CELL_MAT(grid[i + s_delta[dir]]);
        if (m == MAT_WATER) {
            wet = dir;
            break;              /* dousing beats anything else in reach */
        }
        if (m == MAT_WOOD && fuel < 0) {
            fuel = dir;
        }
        if (m == MAT_EMPTY) {
            air = true;
        }
    }

    if (wet >= 0) {
        /* Doused. The water is spent putting it out, so a splash costs
         * something and a thin film cannot hold back a blaze. */
        grid[i + s_delta[wet]] = CELL_MAKE(MAT_STEAM, SHADE_MASK);
        touch_cell(x + DIR_X[wet], y + DIR_Y[wet]);
        grid[i] = CELL_MAKE(MAT_SMOKE, SHADE_MASK);
        touch_cell(x, y);
        return true;
    }

    /* How readily wood catches - now purely a speed dial.
     *
     * It used to have a floor. On the old timer a flame had to light a
     * neighbour before its own life ran out, so halving this rate did not
     * halve the burn, it made fires sputter out having eaten a fifth of the
     * wood. With flames holding their heat while they have fuel and air, the
     * relationship is simply proportional: one step in sixty-four takes about
     * fifteen seconds to work through a block where one in thirty-two took
     * seven, and both finish. */
    if (fuel >= 0 && (r & 0xFC0) == 0) {
        grid[i + s_delta[fuel]] = CELL_MAKE(MAT_FIRE, SHADE_MASK);
        touch_cell(x + DIR_X[fuel], y + DIR_Y[fuel]);
    }

    /* Oxygen, without an oxygen field.
     *
     * A flame with fuel beside it and somewhere to breathe holds at full
     * heat; anything else is starving and cools. Nothing tracks oxygen
     * concentration - having an empty neighbour *is* the test, because a cell
     * with none is by definition walled in. (Lifted from how Noita does it,
     * which is far cleaner than the timer this replaces.)
     *
     * The behaviour that falls out costs no special cases: fire sealed inside
     * a solid block suffocates, fire at the surface keeps going, and a fire
     * front burns as long as there is wood in front of it and burnt-out space
     * behind. That last one is what matters here - a flame is no longer
     * racing its own lifetime to ignite a neighbour, so the spread rate can
     * be set for how it looks rather than kept above the rate at which fires
     * die out. */
    const int life = CELL_SHADE(cell);

    if (fuel >= 0 && air) {
        if (life != SHADE_MASK) {
            grid[i] = CELL_MAKE(MAT_FIRE, SHADE_MASK);
            touch_cell(x, y);
        }
    } else {
        /* Enclosed fire goes out twice as fast as fire that has simply run
         * out of things to burn. Shade is life, so either way the flame cools
         * from pale yellow down to a dull ember as it dies. */
        const uint32_t gate = air ? FIRE_COOLING : 1;
        if ((r >> 12 & gate) == 0) {
            if (life > 1) {
                grid[i] = CELL_MAKE(MAT_FIRE, life - 1);
                touch_cell(x, y);
            } else {
                /* Mostly goes up as smoke: a fire that simply vanished would
                 * leave no sign that anything had burned. */
                if ((r >> 16 & 3) != 0) {
                    grid[i] = CELL_MAKE(MAT_SMOKE, SHADE_MASK);
                } else {
                    grid[i] = CELL_EMPTY;
                    s_active_count--;
                }
                touch_cell(x, y);
                return true;
            }
        }
    }

    /* Held in place while there is anything left to burn beside it.
     *
     * This is what was missing: a flame on the surface of a plank used to
     * drift off into the air on the step after it lit, so the wood charred on
     * top and never caught. Fire stays in its fuel, and only lifts off once
     * the fuel around it is gone. Returning true here only stops the mover
     * for this step - the cell is still fire. */
    return fuel >= 0;
}

/* Lava quenched by water: the classic trade, one becomes stone and the other
 * flashes to steam. Wood in reach simply burns, and the lava carries on -
 * molten rock is not used up by setting fire to something. */
static inline IRAM_ATTR bool lava_react(uint8_t *const grid, int i, int x, int y, uint32_t r)
{
    const int dir = r & 7;
    const int j = i + s_delta[dir];
    const int target = CELL_MAT(grid[j]);

    if (target == MAT_WOOD) {
        if ((r & 0x38) != 0) {
            return false;   /* takes a moment to catch, rather than instantly */
        }
        /* Lights it rather than consuming it. Everything after this is the
         * fire's business, and it will carry itself through the rest of the
         * wood without the lava going anywhere near. */
        grid[j] = CELL_MAKE(MAT_FIRE, SHADE_MASK);
        touch_cell(x + DIR_X[dir], y + DIR_Y[dir]);
        return false;       /* the lava itself is unchanged */
    }

    if (target != MAT_WATER) {
        return false;
    }

    grid[j] = CELL_MAKE(MAT_STEAM, rng_next() & SHADE_MASK);
    touch_cell(x + DIR_X[dir], y + DIR_Y[dir]);
    grid[i] = CELL_MAKE(MAT_ROCK, rng_next() & SHADE_MASK);
    touch_cell(x, y);
    return true;
}

IRAM_ATTR void sand_step(float gx, float gy)
{
    /* In free fall (or if the IMU hiccups) just keep pulling down. */
    if (gx == 0.0f && gy == 0.0f) {
        gy = 1.0f;
    }

    /* Split the continuous gravity angle across the two nearest of the eight
     * grid directions. Choosing between them per cell, weighted by how close
     * the angle is to each, averages out to the true direction - so a heap
     * tilts smoothly instead of snapping to 45 degree steps. */
    float sector = atan2f(gy, gx) * (4.0f / SAND_PI);
    if (sector < 0.0f) {
        sector += 8.0f;
    }
    const int dir_lo = ((int)sector) & 7;
    const int dir_hi = (dir_lo + 1) & 7;
    /* Whichever way is not down, for flames to climb towards. */
    const int up_dir = (dir_lo + 4) & 7;
    const uint32_t hi_threshold = (uint32_t)((sector - (float)((int)sector)) * 65536.0f);

    /* Scan the cells furthest along gravity first, so a falling cell finds
     * the space ahead of it already vacated. */
    int y_first, y_last, y_step;
    if (gy >= 0.0f) {
        y_first = g_grid_h - 1; y_last = -1;     y_step = -1;
    } else {
        y_first = 0;          y_last = g_grid_h; y_step = 1;
    }
    const bool x_descending = (gx >= 0.0f);
    uint8_t *const grid = s_grid;

    for (int pass = 0; pass < s_substeps; pass++) {
        /* Whole words, or the tail of the clear runs past the bitmap: the
         * old byte count of (cells + 31) / 8 overshot the array by two bytes
         * at the finest grain and quietly corrupted whatever came next. */
        memset(s_moved, 0, (((size_t)g_grid_cells + 31) / 32) * sizeof(uint32_t));

        /* Consume one activity page while the other collects what still
         * needs looking at next pass. */
        const int16_t *act_lo = s_act_lo[s_act_page];
        const int16_t *act_hi = s_act_hi[s_act_page];
        int16_t *next_lo = s_act_lo[s_act_page ^ 1];
        int16_t *next_hi = s_act_hi[s_act_page ^ 1];
        for (int y = 0; y < g_grid_h; y++) {
            next_lo[y] = (int16_t)g_grid_w;
            next_hi[y] = -1;
        }

        for (int y = y_first; y != y_last; y += y_step) {
            int x_lo = s_row_x0[y];
            int x_hi = s_row_x1[y];
            if (act_lo[y] > x_lo) x_lo = act_lo[y];
            if (act_hi[y] < x_hi) x_hi = act_hi[y];
            if (x_lo > x_hi) {
                continue;       /* the whole row is asleep */
            }
            const int row = y * g_grid_w;

            int x = x_descending ? x_hi : x_lo;
            const int x_adv = x_descending ? -1 : 1;
            for (int n = x_lo; n <= x_hi; n++, x += x_adv) {
                const int i = row + x;
                const uint8_t cell = grid[i];
                const int material = CELL_MAT(cell);

                if (!((MOVABLE_MASK >> material) & 1u)) {
                    continue;
                }

                /* Anything with behaviour of its own - burning, decaying,
                 * biting, reacting - stays awake for as long as it exists.
                 * Only the plain fallers can earn sleep below. */
                const bool settles = (SETTLES_MASK >> material) & 1u;
                if (!settles) {
                    wake_cell(x, y);
                }

                const uint32_t r = rng_next();

                bool spent = false;
                switch (material) {
                case MAT_ACID:     spent = acid_react(grid, i, x, y, r);          break;
                case MAT_ACID_GAS:
                case MAT_SMOKE:    spent = gas_decay(grid, i, x, y, cell, r);     break;
                case MAT_LAVA:     spent = lava_react(grid, i, x, y, r);          break;
                case MAT_FIRE:     spent = fire_react(grid, i, x, y, cell, r, up_dir); break;
                default: break;
                }
                if (spent) {
                    continue;   /* it is something else now */
                }

                /* The once-per-pass rule is about movement, not chemistry.
                 * Testing it before the reaction meant a cell that had been
                 * shoved along by something else got no turn at all - and
                 * acid with sand pouring through it is displaced constantly,
                 * so it was skipped nearly every pass and never ate anything.
                 *
                 * Safe to react here: the scan runs furthest-along-gravity
                 * first, so a cell that fell moved into ground already
                 * covered, and the cell now sitting at i is the one it
                 * swapped with, which has not had a turn this pass. */
                if (MOVED_GET(i)) {
                    continue;   /* displaced; the write that did it woke it */
                }

                const mat_rule_t *rule = &RULES[material];
                if (rule->sluggish && (r >> 20 & 0xFF) < rule->sluggish) {
                    if (settles) {
                        wake_cell(x, y);    /* declined, but not proven stuck */
                    }
                    continue;   /* too thick to move this step */
                }

                const int primary = ((r & 0xFFFF) < hi_threshold) ? dir_hi : dir_lo;
                if (!try_move(grid, i, x, y, primary, cell, r)) {
                    if (material == MAT_STEAM) {
                        /* Boxed in at the top, so it gives up and condenses -
                         * otherwise steam would just accumulate forever. */
                        if ((r & 0x1FF) == 0) {
                            grid[i] = make_cell(MAT_WATER);
                            touch_cell(x, y);
                        }
                    } else if (settles) {
                        /* Blocked. A cell may only ever move into something
                         * strictly lighter, so with every neighbour at least
                         * as dense it cannot move whichever way gravity
                         * turns, and sleeps until a write beside it says
                         * otherwise. Anything less than all eight would tie
                         * the proof to the current gravity direction and
                         * need every sleeper re-checked on every tilt. */
                        for (int d = 0; d < 8; d++) {
                            if (CELL_MAT(grid[i + s_delta[d]]) < material) {
                                wake_cell(x, y);
                                break;
                            }
                        }
                    }
                }
            }
        }

        s_act_page ^= 1;
    }
}

/* Stamp one disc of material. */
static void paint_dot(int cx, int cy, int radius, int material)
{
    const int r2 = radius * radius;

    for (int dy = -radius; dy <= radius; dy++) {
        const int y = cy + dy;
        if (y < 0 || y >= g_grid_h) {
            continue;
        }
        const int row = y * g_grid_w;

        for (int dx = -radius; dx <= radius; dx++) {
            const int x = cx + dx;
            if (x < 0 || x >= g_grid_w || dx * dx + dy * dy > r2) {
                continue;
            }
            const int i = row + x;
            if (CELL_MAT(s_grid[i]) == MAT_WALL) {
                continue;       /* the bowl is not paintable */
            }

            const uint8_t was = s_grid[i];
            const uint8_t now = (material == MAT_EMPTY) ? CELL_EMPTY : make_cell(material);
            if (was == now) {
                continue;
            }

            if (CELL_MAT(was) != MAT_EMPTY) s_active_count--;
            if (material != MAT_EMPTY)      s_active_count++;

            s_grid[i] = now;
            touch_cell(x, y);
        }
    }
}

/* Scanline flood fill. The span-based form is used rather than pushing every
 * cell because a bowl-sized region at the finest grain is 200k cells, and a
 * per-cell stack for that would dwarf everything else in the program. */
#define FILL_STACK  2048
static uint32_t s_fill_stack[FILL_STACK];

void sand_fill_region(int x, int y, int material)
{
    if (x < 0 || x >= g_grid_w || y < 0 || y >= g_grid_h) {
        return;
    }

    const int from = CELL_MAT(s_grid[y * g_grid_w + x]);
    if (from == MAT_WALL || from == material) {
        return;     /* nothing to do, and filling the bowl itself is out */
    }

    int top = 0;
    s_fill_stack[top++] = ((uint32_t)y << 16) | (uint32_t)x;

    while (top > 0) {
        const uint32_t seed = s_fill_stack[--top];
        const int sy = (int)(seed >> 16);
        int sx = (int)(seed & 0xFFFF);

        const int row = sy * g_grid_w;
        if (CELL_MAT(s_grid[row + sx]) != from) {
            continue;
        }

        int left = sx;
        while (left > 0 && CELL_MAT(s_grid[row + left - 1]) == from) {
            left--;
        }
        int right = sx;
        while (right < g_grid_w - 1 && CELL_MAT(s_grid[row + right + 1]) == from) {
            right++;
        }

        for (int i = left; i <= right; i++) {
            if (material == MAT_EMPTY) {
                s_active_count--;
            } else if (from == MAT_EMPTY) {
                s_active_count++;
            }
            s_grid[row + i] = (material == MAT_EMPTY) ? CELL_EMPTY : make_cell(material);
        }
        /* Spans are intervals, so touching the two ends covers the whole run
         * for the repaint and the wake alike. */
        touch_cell(left, sy);
        touch_cell(right, sy);

        /* Seed the rows either side, one entry per run rather than per cell. */
        for (int dy = -1; dy <= 1; dy += 2) {
            const int ny = sy + dy;
            if (ny < 0 || ny >= g_grid_h) {
                continue;
            }
            const int nrow = ny * g_grid_w;
            bool in_run = false;
            for (int i = left; i <= right; i++) {
                const bool match = (CELL_MAT(s_grid[nrow + i]) == from);
                if (match && !in_run) {
                    if (top < FILL_STACK) {
                        s_fill_stack[top++] = ((uint32_t)ny << 16) | (uint32_t)i;
                    }
                    in_run = true;
                } else if (!match) {
                    in_run = false;
                }
            }
        }
    }
}

void sand_paint(int x0, int y0, int x1, int y1, int radius, int material)
{
    /* Step along the drag so a fast swipe leaves a stroke, not a dotted line -
     * at 50 Hz a finger easily crosses several brush widths a frame. */
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int span = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
    const int steps = (span + radius - 1) / (radius > 0 ? radius : 1);

    for (int n = 0; n <= steps; n++) {
        const int x = (steps > 0) ? x0 + dx * n / steps : x0;
        const int y = (steps > 0) ? y0 + dy * n / steps : y0;
        paint_dot(x, y, radius, material);
    }
}
