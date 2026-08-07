#include <math.h>
#include <string.h>

#include "esp_attr.h"

#include "sand_menu.h"
#include "sand_render.h"
#include "sand_sim.h"

/* All in screen pixels. The menu used to be laid out in grid cells, which
 * stopped making sense once the grain size became a setting - the ring would
 * have changed size with it. */
/* The menu is a full-screen pie: an annulus of wedge segments running from
 * the centre button's own rim right out to the edge of the glass,
 * each bounded by two concentric arcs and two radial edges. Segments rather
 * than floating discs because the panel is round - the outer border is a
 * circle concentric with the glass, so the menu reads as part of the device
 * instead of a gadget hovering over it.
 *
 * The glass curves away over roughly its last ten pixels, so the outer rim
 * is allowed to run under it - a filled segment still reads as reaching the
 * edge - and the glyphs ride as far out as they can without doing the same.
 *
 * The wedges tile the annulus completely: adjacent segments share their
 * radial border, each drawing its own half of the joint line, so the menu is
 * one connected lattice of arcs and spokes. The fills are translucent - the
 * paused scene shows through at half strength - with the borders and glyphs
 * solid on top for legibility. */
#define SEG_R_OUT       233     /* outer arc: the edge of the glass */
#define SEG_R_IN        74      /* inner arc, where the centre button begins */
#define SEG_BORDER_PX   2       /* each wedge's half of the shared border */
#define RING_PX         176     /* centre of the panel to centre of a glyph */
/* The middle is one big button filling everything inside the segments' inner
 * arc, holding whichever action the ring is most often opened for. On rings
 * whose middle holds nothing it is the cancel area - lifting off there picks
 * nothing - and everywhere the hardware button closes the ring. */
#define CENTRE_R        SEG_R_IN

/* ...but the menu is *drawn* in screen pixels, not cells.
 *
 * Everything else on the panel is quantised to CELL_PX because it comes from
 * the simulation grid. The menu does not: it is an overlay, so nothing stops
 * it using the panel's full resolution. That is the difference between circles
 * with 4-pixel stair-stepping and clean ones, and it is what makes room for
 * icons drawn as real shapes rather than as blown-up 7x7 bitmaps. */
#define RIM_PX          3

/* The radius the glyph shapes were authored inside. Both the segments and
 * the centre button sample them through a coordinate scale of GLYPH_R over
 * their own size, so the same authored shape serves every context. */
#define GLYPH_R         32

/* The virtual button radius a segment's glyph is scaled to. Sized so the
 * largest glyphs stay inside the narrowest wedge - eleven segments at
 * RING_PX leave about 100 px of arc between the borders. */
#define SEG_GLYPH_R     46

/* Likewise for the centre button's glyph. Exactly twice GLYPH_R, so the
 * sampling is a clean pixel doubling with no uneven rows. */
#define CENTRE_GLYPH_R  64

/* The farthest any authored glyph reaches from its centre (the outer ring
 * of the biggest brush dot). Sampling is only worth doing inside this box
 * at the drawing scale - every pixel outside is fill without asking, and
 * the asking is the expensive part for the busier glyphs. */
#define GLYPH_SPAN          25
#define SEG_GLYPH_BOX       (GLYPH_SPAN * SEG_GLYPH_R / GLYPH_R + 1)
#define CENTRE_GLYPH_BOX    (GLYPH_SPAN * CENTRE_GLYPH_R / GLYPH_R + 1)

/* Same packing the renderer uses. */
#if LCD_SWAP_BYTES
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) | (((g) & 0xE0) >> 5) |            \
                                    (((g) & 0x1C) << 11) | (((b) & 0xF8) << 5)))
#else
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#endif

#define COLOUR_BUTTON   RGB565(0x18, 0x1E, 0x28)
#define COLOUR_ACTIVE   RGB565(0x2E, 0x7C, 0xF0)
#define COLOUR_GLYPH    RGB565(0xFF, 0xFF, 0xFF)
#define COLOUR_RIM      RGB565(0x58, 0x62, 0x72)
#define COLOUR_RIM_ON   RGB565(0xBF, 0xD8, 0xFF)
/* The setting a ring is already on. Clearly lifted off the plain button, but
 * well short of the blue that means "you are touching this". */
#define COLOUR_SELECTED RGB565(0x22, 0x3C, 0x5E)

/* Plain (unswapped) RGB565, for colours that are blended rather than stored:
 * channel maths only works on this layout, whatever order the panel wants
 * the bytes in afterwards. */
#define RGB565_RAW(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Tints for the translucent wedge fills, mixed half and half with the scene.
 * Black tints to a plain darkening - smoked glass - which is what a resting
 * wedge uses; the touched and already-set tints run brighter than their
 * solid cousins above so they survive the halving. */
#define TINT_REST       RGB565_RAW(0x00, 0x00, 0x00)
#define TINT_ACTIVE     RGB565_RAW(0x2E, 0x7C, 0xF0)
#define TINT_SELECTED   RGB565_RAW(0x2A, 0x50, 0x88)

/* Fire is the one material whose icon does not take its colour from the
 * simulation. Shade is remaining life there, so the brightest shade - what
 * every other icon uses - is the near-white of a flame's hottest core. That
 * is right on the panel, where the whole ramp down to a dull ember is on show
 * at once, but as a single flat colour it reads as a pale blob. */
#define COLOUR_FIRE     RGB565(0xF4, 0x2C, 0x14)

/* Each ring is one concern, with its most-used action in the middle. */
static const menu_icon_t SET_PAINT[] = {
    ICON_SAND, ICON_WATER, ICON_ACID, ICON_ACID_GAS, ICON_LAVA, ICON_FIRE,
    ICON_STEAM, ICON_WOOD, ICON_DIRT, ICON_ROCK, ICON_NODRAW,
};
static const menu_icon_t SET_TOOLS[] = {
    ICON_ERASE, ICON_FILL, ICON_CLEAR,
    ICON_BRUSH1, ICON_BRUSH2, ICON_BRUSH3, ICON_BRUSH4,
};
static const menu_icon_t SET_SYSTEM[] = {
    ICON_SAVE, ICON_LOAD, ICON_IMPORT, ICON_SETTINGS, ICON_BATTERY, ICON_POWER,
};
static const menu_icon_t SET_GRAIN[] = {
    ICON_SIZE2, ICON_SIZE3, ICON_SIZE4, ICON_SIZE5, ICON_SIZE6,
    ICON_BRIGHT, ICON_USB,
};
static const menu_icon_t SET_BRIGHT[] = {
    ICON_BRIGHT1, ICON_BRIGHT2, ICON_BRIGHT3, ICON_BRIGHT4,
};
static const menu_icon_t SET_SLOTS[] = {
    ICON_SLOT1, ICON_SLOT2, ICON_SLOT3, ICON_SLOT4, ICON_SLOT5, ICON_SLOT6,
};

static uint8_t s_slot_usage;

void menu_set_slot_usage(uint8_t mask) { s_slot_usage = mask; }

/* What the battery glyph reads out; negative draws as dashes. */
static int s_battery_pct = -1;

void menu_set_battery(int percent) { s_battery_pct = percent; }

/* The idle prompt, set around the top of the glass. Following the rim keeps
 * it out of the middle, which is where the sand goes the moment the prompt
 * has done its job. */
#define HINT_TEXT       "DRAW TO START"
#define TEXT_SCALE      3
#define TEXT_COLS       5
#define TEXT_ROWS       7
#define TEXT_ADVANCE    ((TEXT_COLS + 1) * TEXT_SCALE)
#define TEXT_R_OUT      222                                 /* just inside the glass */
#define TEXT_R_IN       (TEXT_R_OUT - TEXT_ROWS * TEXT_SCALE)
#define TEXT_R_MID      ((TEXT_R_OUT + TEXT_R_IN) / 2)
#define TEXT_LEN        ((int)(sizeof(HINT_TEXT) - 1))
#define TEXT_ARC        (TEXT_LEN * TEXT_ADVANCE)           /* pixels along the baseline */

/* Big enough to read across a room, which is the point of it. */
#define PAUSE_HINT_GAP  14      /* half the space between the bars */
#define PAUSE_HINT_W    32
#define PAUSE_HINT_H    56      /* half height */
#define PAUSE_HINT_PX   (PAUSE_HINT_GAP + PAUSE_HINT_W + 4)

static menu_hint_t s_hint = HINT_NONE;

void menu_set_hint(menu_hint_t hint) { s_hint = hint; }
menu_hint_t menu_hint(void)          { return s_hint; }

/* A line of plain text across the middle of the glass, for moments that
 * need words - touch calibration, mainly. Capitals and spaces only, since
 * that is all the font has. The caller owns keeping the string alive and
 * forcing a repaint of the area; NULL takes it down. */
#define BANNER_SCALE    3
#define BANNER_ADV      ((TEXT_COLS + 1) * BANNER_SCALE)

static const char *s_banner;

void menu_set_banner(const char *text) { s_banner = text; }

/* Half the angle the text subtends, and where it starts. Screen y is down, so
 * angle increases clockwise and -90 degrees is the top: running from
 * -90 - half to -90 + half reads left to right over the top of the panel. */
static inline float text_half_angle(void)
{
    return (float)TEXT_ARC / (float)TEXT_R_MID * 0.5f;
}

void menu_hint_bounds(int *x0, int *y0, int *x1, int *y1)
{
    const int cx = FB_W / 2, cy = FB_H / 2;

    if (s_hint == HINT_DRAW) {
        const float h = text_half_angle();
        /* Widest at the ends of the arc, at the outer radius; lowest at those
         * same ends but at the inner one. */
        const int ex = (int)(sinf(h) * (float)TEXT_R_OUT) + 2;
        *x0 = cx - ex;
        *x1 = cx + ex;
        *y0 = cy - TEXT_R_OUT - 1;
        *y1 = cy - (int)(cosf(h) * (float)TEXT_R_IN) + 2;
        return;
    }

    *x0 = cx - PAUSE_HINT_PX;
    *y0 = cy - PAUSE_HINT_H - 4;
    *x1 = cx + PAUSE_HINT_PX;
    *y1 = cy + PAUSE_HINT_H + 4;
}

/* Switches that are currently on. ICON_COUNT is past 32, so this has to be
 * the wider type. */
static uint64_t s_lit;

void menu_set_lit(menu_icon_t icon, bool lit)
{
    if (icon >= ICON_COUNT) {
        return;
    }
    const uint64_t bit = 1ull << icon;
    s_lit = lit ? (s_lit | bit) : (s_lit & ~bit);
}

/* Materials draw in their own colour, so the ring reads at a glance without
 * needing to recognise every silhouette. */
static uint16_t icon_colour(menu_icon_t icon, uint16_t fallback)
{
    switch (icon) {
    case ICON_SAND:  return sand_render_material_colour(MAT_SAND);
    case ICON_WATER: return sand_render_material_colour(MAT_WATER);
    case ICON_ACID:  return sand_render_material_colour(MAT_ACID);
    case ICON_ACID_GAS: return sand_render_material_colour(MAT_ACID_GAS);
    case ICON_LAVA:  return sand_render_material_colour(MAT_LAVA);
    case ICON_FIRE:  return COLOUR_FIRE;
    case ICON_STEAM: return sand_render_material_colour(MAT_STEAM);
    case ICON_WOOD:  return sand_render_material_colour(MAT_WOOD);
    case ICON_DIRT:  return sand_render_material_colour(MAT_DIRT);
    case ICON_ROCK:  return sand_render_material_colour(MAT_ROCK);
    default:         return fallback;
    }
}

static bool s_open;
static int s_cx, s_cy;
static const menu_icon_t *s_items = SET_PAINT;
static int s_count = (int)(sizeof(SET_PAINT) / sizeof(SET_PAINT[0]));
static menu_icon_t s_centre = ICON_NONE;
static menu_icon_t s_selection = ICON_NONE;

/* Unit vectors (x256) along each segment's leading radial edge, set when a
 * ring opens. Segment k owns the wedge from boundary k to boundary k+1. */
#define MENU_ITEMS_MAX  11
static int16_t s_bound_x[MENU_ITEMS_MAX];
static int16_t s_bound_y[MENU_ITEMS_MAX];

void menu_set_centre(menu_icon_t icon) { s_centre = icon; }

/* Buttons start at the top and go clockwise. Spacing follows the item count,
 * so both rings stay evenly filled. */
static void button_centre(int item, int *bx, int *by)
{
    const float angle = -1.5707963f + (float)item * (6.2831853f / (float)s_count);
    *bx = s_cx + (int)lroundf(cosf(angle) * RING_PX);
    *by = s_cy + (int)lroundf(sinf(angle) * RING_PX);
}

void menu_open(menu_set_t set, int px, int py)
{
    switch (set) {
    case MENU_SET_TOOLS:
        s_items = SET_TOOLS;
        s_count = (int)(sizeof(SET_TOOLS) / sizeof(SET_TOOLS[0]));
        s_centre = ICON_PALETTE;    /* straight back to the materials */
        break;
    case MENU_SET_SYSTEM:
        s_items = SET_SYSTEM;
        s_count = (int)(sizeof(SET_SYSTEM) / sizeof(SET_SYSTEM[0]));
        s_centre = ICON_PAUSE;
        break;
    case MENU_SET_GRAIN:
        s_items = SET_GRAIN;
        s_count = (int)(sizeof(SET_GRAIN) / sizeof(SET_GRAIN[0]));
        s_centre = ICON_NONE;
        break;
    case MENU_SET_BRIGHT:
        s_items = SET_BRIGHT;
        s_count = (int)(sizeof(SET_BRIGHT) / sizeof(SET_BRIGHT[0]));
        s_centre = ICON_NONE;
        break;
    case MENU_SET_SLOTS:
        s_items = SET_SLOTS;
        s_count = (int)(sizeof(SET_SLOTS) / sizeof(SET_SLOTS[0]));
        s_centre = ICON_NONE;       /* caller fills in save or load */
        break;
    default:
        s_items = SET_PAINT;
        s_count = (int)(sizeof(SET_PAINT) / sizeof(SET_PAINT[0]));
        s_centre = ICON_TOOLS;
        break;
    }

    /* The segments reach the glass edge, so the menu can only ever sit dead
     * centre; where the caller asked for it is accepted and ignored. */
    (void)px;
    (void)py;
    s_cx = FB_W / 2;
    s_cy = FB_H / 2;

    /* Item 0 is centred at the top and the rest run clockwise, so boundary k
     * sits half a step anticlockwise of item k's spoke. */
    const float step = 6.2831853f / (float)s_count;
    for (int k = 0; k < s_count; k++) {
        const float a = -1.5707963f + ((float)k - 0.5f) * step;
        s_bound_x[k] = (int16_t)lroundf(cosf(a) * 256.0f);
        s_bound_y[k] = (int16_t)lroundf(sinf(a) * 256.0f);
    }

    s_selection = ICON_NONE;
    s_open = true;
}

void menu_close(void)
{
    s_open = false;
    s_selection = ICON_NONE;
}

bool menu_is_open(void)          { return s_open; }
menu_icon_t menu_selection(void) { return s_selection; }

menu_icon_t menu_track(int px, int py)
{
    if (!s_open) {
        return ICON_NONE;
    }

    const int dx = px - s_cx;
    const int dy = py - s_cy;
    const int d2 = dx * dx + dy * dy;

    if (d2 < CENTRE_R * CENTRE_R) {
        /* The centre fills the whole middle. On rings whose middle holds no
         * action s_centre is ICON_NONE, so lifting off here cancels. */
        s_selection = s_centre;
        return s_selection;
    }

    /* Nearest glyph centre. They all sit at the same radius, so nearest is
     * exactly "whose wedge", without caring about the gaps between them. */
    int best = -1;
    long best_d2 = -1;
    for (int i = 0; i < s_count; i++) {
        int bx, by;
        button_centre(i, &bx, &by);
        const long ex = px - bx;
        const long ey = py - by;
        const long d2 = ex * ex + ey * ey;
        if (best_d2 < 0 || d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }

    s_selection = (best >= 0) ? s_items[best] : ICON_NONE;
    return s_selection;
}

void menu_bounds(int *x0, int *y0, int *x1, int *y1)
{
    /* The segments run to the glass edge, so the menu owns the whole panel. */
    *x0 = 0;
    *y0 = 0;
    *x1 = FB_W - 1;
    *y1 = FB_H - 1;
}

/* Pixel box of one icon's wedge (or the centre button) on the open ring, so
 * a selection change repaints just the two wedges that swapped state rather
 * than the full panel - which, now the menu covers the glass, would mean
 * pushing a whole frame per touch sample. Empty (x1 < x0) when the icon is
 * not on the ring. Rarely called, so it can afford real trigonometry. */
void menu_selection_bounds(menu_icon_t icon, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0; *y0 = 0;
    *x1 = -1; *y1 = -1;
    if (!s_open || icon == ICON_NONE) {
        return;
    }

    if (icon == s_centre) {
        const int r = CENTRE_R + 1;
        *x0 = s_cx - r; *y0 = s_cy - r;
        *x1 = s_cx + r; *y1 = s_cy + r;
        return;
    }

    int item = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_items[i] == icon) {
            item = i;
            break;
        }
    }
    if (item < 0) {
        return;
    }

    const float step = 6.2831853f / (float)s_count;
    const float a0 = -1.5707963f + ((float)item - 0.5f) * step;
    const float a1 = a0 + step;

    /* The box is set by the four wedge corners, plus wherever the outer arc
     * crosses an axis inside the wedge - that is where an arc bulges past
     * its endpoints. */
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (int e = 0; e < 4; e++) {
        const float a = (e & 1) ? a1 : a0;
        const float r = (e & 2) ? (float)SEG_R_OUT : (float)SEG_R_IN;
        const float x = cosf(a) * r;
        const float y = sinf(a) * r;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    for (int axis = 0; axis < 4; axis++) {
        for (int wrap = 0; wrap < 2; wrap++) {
            const float a = ((float)axis - 1.0f) * 1.5707963f +
                            (float)wrap * 6.2831853f;
            if (a < a0 || a > a1) {
                continue;
            }
            const float x = cosf(a) * (float)SEG_R_OUT;
            const float y = sinf(a) * (float)SEG_R_OUT;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }

    *x0 = s_cx + (int)floorf(min_x) - 2;
    *y0 = s_cy + (int)floorf(min_y) - 2;
    *x1 = s_cx + (int)ceilf(max_x) + 2;
    *y1 = s_cy + (int)ceilf(max_y) + 2;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > FB_W - 1) *x1 = FB_W - 1;
    if (*y1 > FB_H - 1) *y1 = FB_H - 1;
}

/* ------------------------------------------------------------------------
 * Icons.
 *
 * Drawn as shapes rather than stored as bitmaps: a thick stroke is "every
 * pixel within w of this line segment", which comes out with rounded caps and
 * clean edges for free, and stays sharp whatever size the buttons are set to.
 * Coordinates are pixels from the centre of the button.
 * ---------------------------------------------------------------------- */

/* Squared distance from a point to a line segment. */
static inline IRAM_ATTR int seg_dist2(int px, int py, int x0, int y0, int x1, int y1)
{
    const int vx = x1 - x0, vy = y1 - y0;
    const int wx = px - x0, wy = py - y0;
    const int vv = vx * vx + vy * vy;

    int t = (vv > 0) ? ((wx * vx + wy * vy) * 256 / vv) : 0;
    if (t < 0) t = 0; else if (t > 256) t = 256;

    const int dx = px - (x0 + vx * t / 256);
    const int dy = py - (y0 + vy * t / 256);
    return dx * dx + dy * dy;
}

static inline IRAM_ATTR bool stroke(int px, int py, int x0, int y0, int x1, int y1, int half)
{
    return seg_dist2(px, py, x0, y0, x1, y1) <= half * half;
}

static inline IRAM_ATTR bool disc(int px, int py, int cx, int cy, int r)
{
    const int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

/* Put a point into a tilted shape's own frame, so something at an angle can
 * still be described as a plain box. The angle is given as its cosine and
 * sine scaled by 256. */
static inline IRAM_ATTR void rot(int x, int y, int cs, int sn, int *u, int *v)
{
    *u = (x * cs + y * sn) >> 8;
    *v = (-x * sn + y * cs) >> 8;
}

/* Just enough of a font to label the grain sizes and count the battery,
 * which are the things in the menus a picture cannot say precisely.
 * Three by five cells, one bit per pixel, most significant bit leftmost. */
static const uint8_t DIGIT_ROWS[10][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 },    /* 0 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 },    /* 1 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 },    /* 2 */
    { 0x7, 0x1, 0x7, 0x1, 0x7 },    /* 3 */
    { 0x5, 0x5, 0x7, 0x1, 0x1 },    /* 4 */
    { 0x7, 0x4, 0x7, 0x1, 0x7 },    /* 5 */
    { 0x7, 0x4, 0x7, 0x5, 0x7 },    /* 6 */
    { 0x7, 0x1, 0x2, 0x2, 0x2 },    /* 7 */
    { 0x7, 0x5, 0x7, 0x5, 0x7 },    /* 8 */
    { 0x7, 0x5, 0x7, 0x1, 0x7 },    /* 9 */
};
static const uint8_t CROSS_ROWS[5]   = { 0x0, 0x5, 0x2, 0x5, 0x0 };
static const uint8_t PERCENT_ROWS[5] = { 0x5, 0x1, 0x2, 0x4, 0x5 };
static const uint8_t DASH_ROWS[5]    = { 0x0, 0x0, 0x7, 0x0, 0x0 };

/* One 3x5 cell blown up by `scale`, with its top-left at (ox, oy). */
static inline IRAM_ATTR bool cell_hit(const uint8_t rows[5], int x, int y,
                                      int ox, int oy, int scale)
{
    const int cx = (x - ox) / scale;
    const int cy = (y - oy) / scale;
    if (x < ox || y < oy || cx > 2 || cy > 4) {
        return false;
    }
    return (rows[cy] >> (2 - cx)) & 1;
}

/* "NxN", centred horizontally on the button and sitting at `top`. */
static IRAM_ATTR bool label_nxn(int n, int x, int y, int top, int scale)
{
    const int cw = 3 * scale;
    const int gap = scale;
    const int total = 3 * cw + 2 * gap;
    int ox = -total / 2;

    if (cell_hit(DIGIT_ROWS[n], x, y, ox, top, scale)) return true;
    ox += cw + gap;
    if (cell_hit(CROSS_ROWS, x, y, ox, top, scale)) return true;
    ox += cw + gap;
    return cell_hit(DIGIT_ROWS[n], x, y, ox, top, scale);
}

/* Point inside a convex polygon whose vertices are wound clockwise on screen
 * (remembering that y runs down). Straight edges and corners are what make a
 * shape read as cut or faceted; circles never will. */
static inline IRAM_ATTR bool poly_hit(int x, int y, const int8_t v[][2], int n)
{
    for (int i = 0; i < n; i++) {
        const int j = (i + 1 == n) ? 0 : i + 1;
        const int ex = v[j][0] - v[i][0];
        const int ey = v[j][1] - v[i][1];
        if ((x - v[i][0]) * ey - (y - v[i][1]) * ex > 0) {
            return false;
        }
    }
    return true;
}

/* A teardrop: a disc with a taper drawn up to a point above it. */
static inline IRAM_ATTR bool droplet(int x, int y)
{
    if (disc(x, y, 0, 6, 12)) {
        return true;
    }
    if (y >= -18 && y <= 6) {
        const int half = (y + 18) / 2;
        return x >= -half && x <= half;
    }
    return false;
}

/* The flame is traced, not constructed.
 *
 * Every other icon here is built from discs and strokes, which suits shapes
 * that can be described in words - a bin, a cog, a floppy disc. A flame
 * cannot be. What makes one read is the precise sweep of the outline, the
 * small tongue curling up behind its slit, and the teardrop cut out of the
 * body; four attempts at approximating that with primitives each came out as
 * a droplet leaning over, which is the one thing it must not look like with a
 * water drop two buttons round the same ring.
 *
 * So it is a bitmap taken off the reference artwork: 27 by 35, one bit per
 * pixel, 140 bytes. Reading it is a shift and a mask, rather cheaper than the
 * dozen distance calculations the constructed version was costing. */
#define FIRE_W      27
#define FIRE_H      35
#define FIRE_X0     (-13)
#define FIRE_Y0     (-17)

static const uint32_t FIRE_BITS[FIRE_H] = {
    0x0000000, 0x0010000, 0x003C000, 0x003E000, 0x007F000, 0x007F800, 0x007FC00,
    0x007FC00, 0x00FFE00, 0x00FFE00, 0x01FFF00, 0x01FFF00, 0x03FFF00, 0x07FFF18,
    0x07FFF88, 0x0FFFF8C, 0x1FF7F8C, 0x1FE7F8E, 0x1FE7F0E, 0x3FC7F1E, 0x3F87F1E,
    0x3F83FBE, 0x3F83FFE, 0x3F03FFE, 0x3F01FFE, 0x3F00FFE, 0x1F80FFC, 0x1F80FFC,
    0x0FE3FF8, 0x07FFFF0, 0x03FFFE0, 0x01FFFC0, 0x00FFF80, 0x001FC00, 0x0000000,
};

static IRAM_ATTR bool glyph_hit(int icon, int x, int y)
{
    switch (icon) {
    case ICON_TOOLS: {
        /* A combination spanner on the diagonal: ring end at the bottom
         * left, shaft, and an open jaw at the top right. The old one bit
         * the jaw out with an offset disc, which left a crescent that read
         * as a moon on a stick. The jaw is now a straight slot cut along
         * the shaft's own axis, which is the shape a spanner actually is. */
        if (disc(x, y, -14, 14, 9) && !disc(x, y, -14, 14, 4)) return true;
        if (stroke(x, y, -11, 11, 9, -9, 5)) return true;
        if (!disc(x, y, 13, -13, 11)) return false;
        int u, v;
        rot(x - 13, y + 13, 181, -181, &u, &v);     /* -45 degrees */
        return !(u >= 2 && v >= -5 && v <= 5);      /* the jaw slot */
    }

    case ICON_PALETTE:
        /* A painter's palette - a blob with thumb hole and paint wells. */
        if (!disc(x, y, 0, 0, 19)) return false;
        return !(disc(x, y, 7, 8, 6) || disc(x, y, -9, -6, 4) ||
                 disc(x, y, 2, -10, 4) || disc(x, y, -8, 5, 4));

    case ICON_ERASE: {
        /* A rubber held at an angle, with the band between the rubber and the
         * sleeve, and the line it is taking off underneath. A cross meant
         * "delete this", which is a different promise from rubbing pixels off
         * what is already there. */
        if (y >= 15 && y <= 18 && x >= -16 && x <= 11) return true;

        int u, v;
        rot(x, y + 4, 210, 147, &u, &v);        /* 35 degrees */
        if (u < -9 || u > 9 || v < -12 || v > 10) return false;
        return !(v >= -3 && v <= 0);            /* the band, left unpainted */
    }

    case ICON_NODRAW:
        /* Just the prohibition sign. Every other button in this ring puts a
         * material down, so "none of them" needs no further explanation - and
         * a pen behind the bar only turned it into a blob at this size. */
        if (disc(x, y, 0, 0, 18) && !disc(x, y, 0, 0, 13)) return true;
        return stroke(x, y, -10, 10, 10, -10, 3);

    case ICON_CLEAR: {
        /* A bin: handle, lid, tapered body. */
        if (x >= -5 && x <= 5 && y >= -20 && y <= -16) return true;
        if (x >= -16 && x <= 16 && y >= -16 && y <= -12) return true;
        if (y >= -10 && y <= 16) {
            const int half = 13 - (y + 10) / 6;
            return x >= -half && x <= half;
        }
        return false;
    }

    case ICON_POWER: {
        /* Broken ring plus the upright bar. */
        const int d2 = x * x + y * y;
        if (d2 >= 13 * 13 && d2 <= 18 * 18) {
            const bool in_gap = (y < 0) && (x >= -6 && x <= 6);
            if (!in_gap) {
                return true;
            }
        }
        return stroke(x, y, 0, -21, 0, -4, 3);
    }

    case ICON_SAND:
        /* A heap with grains falling into it. */
        if (y >= 2 && y <= 18) {
            const int half = (y - 2) * 5 / 4 + 3;
            if (x >= -half && x <= half) {
                return true;
            }
        }
        return disc(x, y, -9, -13, 4) || disc(x, y, 3, -19, 4) || disc(x, y, 11, -6, 4);

    case ICON_WATER:
        return droplet(x, y);

    case ICON_ACID:
        /* The same drop, pitted - it is the one that eats things. */
        if (!droplet(x, y)) {
            return false;
        }
        return !(disc(x, y, -4, 4, 4) || disc(x, y, 5, 10, 3) || disc(x, y, 2, -3, 3));

    case ICON_FIRE: {
        const int gx = x - FIRE_X0;
        const int gy = y - FIRE_Y0;
        if (gx < 0 || gx >= FIRE_W || gy < 0 || gy >= FIRE_H) {
            return false;
        }
        return (FIRE_BITS[gy] >> gx) & 1;
    }

    case ICON_ACID_GAS:
        /* A plume of bubbles climbing and thinning out. Steam is already
         * three wisps in this same ring, so fumes had to be something else
         * entirely rather than a variation on it. */
        return disc(x, y, -6, 12, 6) || disc(x, y, 6, 6, 5) ||
               disc(x, y, -5, 0, 4) || disc(x, y, 7, -6, 4) ||
               disc(x, y, -3, -11, 3) || disc(x, y, 8, -15, 2);

    case ICON_STEAM: {
        /* Three wisps curling upward. */
        for (int w = -1; w <= 1; w++) {
            const int ox = w * 11;
            if (stroke(x, y, ox - 4, 16, ox + 4, 6, 3) ||
                stroke(x, y, ox + 4, 6, ox - 4, -4, 3) ||
                stroke(x, y, ox - 4, -4, ox + 3, -14, 3)) {
                return true;
            }
        }
        return false;
    }

    case ICON_PAUSE:
        /* Two bars for running, since tapping it is what stops things. */
        return (x >= -13 && x <= -4 && y >= -16 && y <= 16) ||
               (x >=   4 && x <= 13 && y >= -16 && y <= 16);

    case ICON_LAVA: {
        /* A molten slab with drips running off it. */
        if (x >= -18 && x <= 18 && y >= -10 && y <= -2) return true;
        return disc(x, y, -10, 6, 6) || stroke(x, y, -10, -2, -10, 6, 3) ||
               disc(x, y,   1, 13, 7) || stroke(x, y,   1, -2,   1, 13, 3) ||
               disc(x, y,  12, 3, 5)  || stroke(x, y,  12, -2,  12, 3, 3);
    }

    case ICON_SAVE: {
        /* A floppy disk: shell, the sprung shutter across the top with its
         * window, and a lined label below.
         *
         * Drawn as a frame with those parts sitting inside it. The previous
         * version punched two full-width slots out of a filled square, which
         * leaves the side rails and the bar between them - an H. */
        if (x < -14 || x > 14 || y < -14 || y > 14) return false;
        if (x - y > 21) return false;                       /* keyed corner */
        if (x < -11 || x > 11 || y < -11 || y > 11) return true;    /* shell */

        /* Shutter. Runs right up to the top edge, the way the real one does. */
        if (x >= -6 && x <= 6 && y >= -11 && y <= -3) {
            return !(x >= 2 && x <= 4 && y >= -10 && y <= -6);
        }
        /* Label, with two lines to write on. */
        if (x >= -8 && x <= 8 && y >= 2 && y <= 10) {
            return !(x >= -6 && x <= 6 && ((y >= 4 && y <= 5) || (y >= 7 && y <= 8)));
        }
        return false;
    }

    case ICON_LOAD: {
        /* A folder: back panel, tab, and the front flap. */
        if (y >= -14 && y <= -9 && x >= -15 && x <= -2) return true;  /* tab   */
        return (x >= -15 && x <= 15 && y >= -9 && y <= 14);           /* body  */
    }

    case ICON_IMPORT: {
        /* A framed picture - sun over hills. Deliberately not a folder: this
         * is the one that reads an image, and the folder already means
         * loading a saved scene. */
        if (x < -16 || x > 16 || y < -13 || y > 13) return false;
        if (x < -12 || x > 12 || y < -9 || y > 9) return true;     /* frame */
        if (disc(x, y, -6, -4, 3)) return true;                    /* sun   */
        {
            const int d1 = (x - 4 < 0) ? (4 - x) : (x - 4);
            const int d2 = (x + 7 < 0) ? (-7 - x) : (x + 7);
            if (y >= 0 + d1 || y >= 3 + d2) return true;           /* hills */
        }
        return false;
    }

    case ICON_USB: {
        /* The USB trident: round base, stem to an arrow tip, and the two
         * prongs - one ending in a disc, one in a square. */
        if (disc(x, y, 0, 17, 6)) return true;                     /* base  */
        if (stroke(x, y, 0, 17, 0, -11, 3)) return true;           /* stem  */
        if (y >= -19 && y <= -10) {                                /* tip   */
            const int half = (y + 19) / 2 + 1;
            if (x >= -half && x <= half) return true;
        }
        if (stroke(x, y, 0, 3, -11, -5, 3)) return true;           /* left  */
        if (disc(x, y, -13, -6, 5)) return true;
        if (stroke(x, y, 0, 9, 11, 3, 3)) return true;             /* right */
        return (x >= 7 && x <= 16 && y >= -1 && y <= 8);
    }

    case ICON_SLOT1: case ICON_SLOT2: case ICON_SLOT3:
    case ICON_SLOT4: case ICON_SLOT5: case ICON_SLOT6: {
        /* Dice pips - instantly countable at a glance, and no font needed. */
        static const uint8_t LAYOUT[6][9] = {
            { 0,0,0, 0,1,0, 0,0,0 },
            { 1,0,0, 0,0,0, 0,0,1 },
            { 1,0,0, 0,1,0, 0,0,1 },
            { 1,0,1, 0,0,0, 1,0,1 },
            { 1,0,1, 0,1,0, 1,0,1 },
            { 1,0,1, 1,0,1, 1,0,1 },
        };
        const int n = icon - ICON_SLOT1 + 1;
        const int step = 12;
        for (int py = -1; py <= 1; py++) {
            for (int px = -1; px <= 1; px++) {
                const int bit = (py + 1) * 3 + (px + 1);
                if (!LAYOUT[n - 1][bit]) {
                    continue;
                }
                if (disc(x, y, px * step, py * step, 5)) {
                    return true;
                }
            }
        }
        return false;
    }

    case ICON_BATTERY: {
        /* A battery lying on its side, charge shown twice over: the body
         * fills from the left the way the real thing does, and the number
         * underneath spells it out exactly. */
        const int pct = s_battery_pct;

        if (x >= 15 && x <= 19 && y >= -16 && y <= -8) return true;     /* terminal */
        if (x >= -21 && x <= 15 && y >= -21 && y <= -3) {               /* body */
            if (x <= -19 || x >= 13 || y <= -19 || y >= -5) return true;  /* shell */
            /* The charge level. What the bar leaves empty stays the fill
             * colour, so a low battery reads as a mostly hollow shell. */
            return pct >= 0 && x < -18 + (31 * pct) / 100;
        }

        /* The readout. Small enough that it is rebuilt per pixel rather
         * than carried in state - three compares and a table walk. */
        if (y < 3 || y > 13) return false;
        const uint8_t *cells[4];
        int len;
        if (pct < 0) {
            cells[0] = DASH_ROWS; cells[1] = DASH_ROWS;
            len = 2;
        } else if (pct >= 100) {
            cells[0] = DIGIT_ROWS[1]; cells[1] = DIGIT_ROWS[0];
            cells[2] = DIGIT_ROWS[0]; cells[3] = PERCENT_ROWS;
            len = 4;
        } else if (pct >= 10) {
            cells[0] = DIGIT_ROWS[pct / 10]; cells[1] = DIGIT_ROWS[pct % 10];
            cells[2] = PERCENT_ROWS;
            len = 3;
        } else {
            cells[0] = DIGIT_ROWS[pct]; cells[1] = PERCENT_ROWS;
            len = 2;
        }
        const int adv = 3 * 2 + 2;              /* cell width plus a gap */
        int ox = -(adv * len - 2) / 2;
        for (int c = 0; c < len; c++) {
            if (cell_hit(cells[c], x, y, ox, 3, 2)) return true;
            ox += adv;
        }
        return false;
    }

    case ICON_SETTINGS: {
        /* A cog: hub, bore, and teeth around the rim. The teeth sit at
         * fixed angles, so their centres are a table - glyphs are asked
         * about every pixel they might cover, and cosf here is a software
         * library call costing hundreds of cycles per pixel. Trigonometry
         * in a glyph is what made opening a ring visibly slow. */
        static const int8_t TEETH[8][2] = {
            { 15, 0 }, { 11, 11 }, { 0, 15 }, { -11, 11 },
            { -15, 0 }, { -11, -11 }, { 0, -15 }, { 11, -11 },
        };
        const int d2 = x * x + y * y;
        if (d2 <= 6 * 6) return false;              /* bore */
        if (d2 <= 12 * 12) return true;             /* hub  */
        for (int t = 0; t < 8; t++) {
            if (disc(x, y, TEETH[t][0], TEETH[t][1], 5)) return true;
        }
        return false;
    }

    case ICON_BRIGHT:
    case ICON_BRIGHT1: case ICON_BRIGHT2:
    case ICON_BRIGHT3: case ICON_BRIGHT4: {
        /* A sun. On the level buttons the disc fills from the bottom, so the
         * four of them read as a gauge instead of as four identical suns.
         * Rays from a table for the same reason as the cog's teeth. */
        static const int8_t RAYS[8][2] = {
            { 16, 0 }, { 11, 11 }, { 0, 16 }, { -11, 11 },
            { -16, 0 }, { -11, -11 }, { 0, -16 }, { 11, -11 },
        };
        for (int t = 0; t < 8; t++) {
            if (disc(x, y, RAYS[t][0], RAYS[t][1], 3)) return true;
        }
        if (!disc(x, y, 0, 0, 10)) return false;
        if (icon == ICON_BRIGHT) return true;
        return y >= 10 - 20 * (icon - ICON_BRIGHT1 + 1) / 4;
    }

    case ICON_SIZE2: case ICON_SIZE3: case ICON_SIZE4:
    case ICON_SIZE5: case ICON_SIZE6: {
        /* A block of squares showing how coarse the grain would be, with the
         * number spelled out underneath. Four squares against nine is not a
         * difference you can count at a glance on a ring you are dragging a
         * thumb across, and the pictures alone never said which was which. */
        const int n = icon - ICON_SIZE2 + 2;
        if (label_nxn(n, x, y, 5, 2)) {
            return true;
        }

        const int span = 20;                    /* the grid part, pixels */
        const int step = span / n;
        const int gap = (step >= 6) ? 2 : 1;
        const int gx = x + span / 2;
        const int gy = y + 18;                  /* sits above the label */
        if (gx < 0 || gx >= step * n || gy < 0 || gy >= step * n) {
            return false;
        }
        return (gx % step) < (step - gap) && (gy % step) < (step - gap);
    }

    case ICON_FILL: {
        /* A paint bucket tipped over the puddle it is making. The puddle is
         * what says "fill an area" rather than "pour a drop"; the old icon
         * was a taper and a single droplet, which said neither.
         *
         * There is no handle. One belongs on a bucket, but at twenty-one
         * pixels of radius the arc merged with the body and turned the whole
         * thing into a blob - the tip and the puddle carry the meaning. */
        {
            const int dy = y - 14;              /* puddle, a=9 b=3 */
            const int dx = x + 7;
            if (dy * dy * 81 + dx * dx * 9 <= 729) return true;
        }
        if (disc(x, y, -9, 7, 2) || disc(x, y, -7, 1, 2)) return true;  /* drips */

        /* Body: tipped so the mouth faces down and to the left, over the
         * puddle. */
        int u, v;
        rot(x - 7, y + 7, -196, -165, &u, &v);  /* 220 degrees */
        if (v >= -14 && v < -10) {
            return (u >= -10 && u <= 10);       /* the rim */
        }
        if (v >= -10 && v <= 8) {
            const int half = 9 - (v + 10) * 4 / 18;
            return (u >= -half && u <= half);
        }
        return false;
    }

    case ICON_BRUSH1: case ICON_BRUSH2: case ICON_BRUSH3: case ICON_BRUSH4: {
        /* Just a dot the size of the brush - nothing else says it as
         * directly. Ringed so the smallest is still visible. */
        const int n = icon - ICON_BRUSH1;
        static const int R[4] = { 4, 9, 15, 21 };
        return disc(x, y, 0, 0, R[n]) ||
               (x * x + y * y >= 23 * 23 && x * x + y * y <= 25 * 25);
    }

    case ICON_WOOD:
        /* Two stacked planks, with a gap so it does not read as a plain
         * block the way rock does. */
        if (x < -18 || x > 18) return false;
        return (y >= -17 && y <= -2) || (y >= 2 && y <= 17);

    case ICON_DIRT:
        /* A lumpy mound with clods broken out of it. Sand next door is a neat
         * cone with grains dropping into it; earth is none of those things. */
        if (!(disc(x, y, -6, 8, 11) || disc(x, y, 6, 10, 10) ||
              disc(x, y, 0, 4, 8)) || y > 18) {
            return false;
        }
        return !(disc(x, y, -9, 2, 3) || disc(x, y, 3, 3, 2) ||
                 disc(x, y, 9, 7, 3) || disc(x, y, -2, 12, 2) ||
                 disc(x, y, 10, 14, 2));

    case ICON_ROCK: {
        /* A cleaved stone: an angular block with facet lines cut into it.
         * Overlapping discs read as a cloud however many bites are taken out
         * of them - stone is straight edges and corners. */
        static const int8_t FACES[6][2] = {
            { -15, 1 }, { -9, -11 }, { 2, -15 }, { 13, -6 }, { 11, 9 }, { -3, 15 },
        };
        if (!poly_hit(x, y, FACES, 6)) {
            return false;
        }
        return !(stroke(x, y, -9, -11, -1, -2, 2) ||
                 stroke(x, y, -1, -2, 11, 9, 2) ||
                 stroke(x, y, -1, -2, 1, 15, 2));
    }

    default:
        return false;
    }
}

/* Dim, so it reads as a prompt rather than as part of the simulation. */
#define COLOUR_HINT     RGB565(0x38, 0x40, 0x50)

/* What the pause bars are made of. Light, because it is blended half and
 * half with the scene: over black it settles near the old solid grey, and
 * over sand it reads as a pale veil with the grains still visible. */
#define COLOUR_PAUSE_RAW    RGB565_RAW(0xA8, 0xB0, 0xC0)

/* Average one pixel with a plain-layout colour: a 50% translucent overlay.
 * The bit shuffling either side is the panel's byte order - channel maths
 * only works on the unswapped layout - and 0x7BEF is the classic mask that
 * stops each channel's low bit falling into its neighbour when the word
 * shifts. Mixing with black is a plain halving. */
static inline IRAM_ATTR uint16_t mix_px(uint16_t px, uint16_t raw)
{
#if LCD_SWAP_BYTES
    px = (uint16_t)((px >> 8) | (px << 8));
#endif
    px = (uint16_t)((px & raw) + (((px ^ raw) >> 1) & 0x7BEF));
#if LCD_SWAP_BYTES
    px = (uint16_t)((px >> 8) | (px << 8));
#endif
    return px;
}

/* Capitals only, five by seven. The prompt is three short words; lower case
 * at this size is a smear, and descenders would break the baseline the text
 * is bent around. */
static const uint8_t FONT5X7[26][TEXT_ROWS] = {
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   /* A */
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },   /* B */
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },   /* C */
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },   /* D */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },   /* E */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },   /* F */
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },   /* G */
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   /* H */
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* I */
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },   /* J */
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },   /* K */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },   /* L */
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },   /* M */
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },   /* N */
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   /* O */
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },   /* P */
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },   /* Q */
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },   /* R */
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },   /* S */
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },   /* T */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   /* U */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },   /* V */
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },   /* W */
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },   /* X */
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },   /* Y */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },   /* Z */
};

static inline IRAM_ATTR bool font_hit(char ch, int col, int row)
{
    if (ch < 'A' || ch > 'Z' || col < 0 || col >= TEXT_COLS) {
        return false;
    }
    return (FONT5X7[ch - 'A'][row] >> (TEXT_COLS - 1 - col)) & 1;
}

/* The prompt, bent around the rim.
 *
 * Worked backwards - from each pixel to the character it lands in - rather
 * than forwards from each character. Placing rotated glyphs forwards leaves
 * seams where they meet and doubles up where they overlap; going the other
 * way every pixel is asked exactly once and the answer is exact.
 *
 * The trigonometry looks expensive for a per-pixel loop, and would be if this
 * ran every frame. It does not: the prompt is only redrawn when a band it
 * covers is dirty, which happens once when it appears and once when it goes. */
static IRAM_ATTR void draw_curved_text(uint16_t *buf, int pitch, int height,
                                       int base_x, int base_y)
{
    const int cx = FB_W / 2, cy = FB_H / 2;
    const float a0 = -1.5707963f - text_half_angle();

    int x0, y0, x1, y1;
    menu_hint_bounds(&x0, &y0, &x1, &y1);

    for (int sy = y0; sy <= y1; sy++) {
        const int v = sy - base_y;
        if (v < 0 || v >= height) {
            continue;
        }
        uint16_t *row = buf + v * pitch;
        const int dy = sy - cy;

        for (int sx = x0; sx <= x1; sx++) {
            const int u = sx - base_x;
            if (u < 0 || u >= pitch) {
                continue;
            }
            const int dx = sx - cx;
            const int r2 = dx * dx + dy * dy;
            if (r2 < TEXT_R_IN * TEXT_R_IN || r2 > TEXT_R_OUT * TEXT_R_OUT) {
                continue;
            }

            /* How far along the baseline this pixel sits, and how far in from
             * the outer edge - the glyph's x and y, in other words. */
            const float s = (atan2f((float)dy, (float)dx) - a0) * (float)TEXT_R_MID;
            if (s < 0.0f || s >= (float)TEXT_ARC) {
                continue;
            }
            const int si = (int)s;
            const int ci = si / TEXT_ADVANCE;
            const int col = (si - ci * TEXT_ADVANCE) / TEXT_SCALE;
            const int gr = (int)(((float)TEXT_R_OUT - sqrtf((float)r2)) / (float)TEXT_SCALE);
            if (gr >= TEXT_ROWS) {
                continue;
            }
            if (font_hit(HINT_TEXT[ci], col, gr)) {
                row[u] = COLOUR_HINT;
            }
        }
    }
}

static IRAM_ATTR void draw_hint(uint16_t *buf, int row_first, int row_last,
                                int col_first, int cols)
{
    const int pitch = cols * g_cell_px;
    const int height = (row_last - row_first) * g_cell_px;
    const int base_x = g_origin_x + col_first * g_cell_px;
    const int base_y = g_origin_y + row_first * g_cell_px;

    if (s_hint == HINT_DRAW) {
        draw_curved_text(buf, pitch, height, base_x, base_y);
        return;
    }

    /* Paused: two plain bars, drawn straight rather than through the icon
     * table so they can be sized for the whole panel instead of for a button.
     * Translucent - the scene is frozen underneath, not gone, and the bars
     * saying so should not hide it. */
    const int cx = FB_W / 2, cy = FB_H / 2;

    for (int sy = cy - PAUSE_HINT_H; sy <= cy + PAUSE_HINT_H; sy++) {
        const int v = sy - base_y;
        if (v < 0 || v >= height) {
            continue;
        }
        uint16_t *row = buf + v * pitch;

        for (int sx = cx - PAUSE_HINT_PX; sx <= cx + PAUSE_HINT_PX; sx++) {
            const int u = sx - base_x;
            if (u < 0 || u >= pitch) {
                continue;
            }
            const int dx = sx - cx;
            const int ax = (dx < 0) ? -dx : dx;
            if (ax >= PAUSE_HINT_GAP && ax < PAUSE_HINT_GAP + PAUSE_HINT_W) {
                row[u] = mix_px(row[u], COLOUR_PAUSE_RAW);
            }
        }
    }
}

/* The wedge segments. Each pixel of the annulus is classified into its wedge
 * with two cross products against the radial boundary directions, walking
 * from the previous pixel's wedge - along a row the angle is monotonic, so
 * the walk is a step at most. No per-pixel trigonometry: the only roots are
 * two chord half-widths per row.
 *
 * The cross products double as the border test. With unit boundary vectors
 * scaled by 256, the cross product IS 256 times the distance to that radial
 * edge, so each wedge's half of the shared border comes out at constant
 * width for free, and the joint line joins up across neighbours. */
static IRAM_ATTR void draw_segments(uint16_t *buf, int pitch, int height,
                                    int base_x, int base_y)
{
    const int n = s_count;

    /* Per-segment looks, settled once rather than per pixel. */
    uint16_t tint[MENU_ITEMS_MAX], rims[MENU_ITEMS_MAX], glyphs[MENU_ITEMS_MAX];
    int16_t gx[MENU_ITEMS_MAX], gy[MENU_ITEMS_MAX];
    for (int i = 0; i < n; i++) {
        const menu_icon_t icon = s_items[i];
        const bool active = (s_selection == icon);
        const bool lit = (icon < ICON_COUNT) && (s_lit & (1ull << icon));

        /* An empty slot still has to be pickable - you save into it - so it
         * is drawn faint rather than left out. */
        bool occupied = true;
        if (icon >= ICON_SLOT1 && icon <= ICON_SLOT6) {
            occupied = (s_slot_usage >> (icon - ICON_SLOT1)) & 1u;
        }

        tint[i] = active ? TINT_ACTIVE : lit ? TINT_SELECTED : TINT_REST;
        rims[i] = (active || lit) ? COLOUR_RIM_ON : COLOUR_RIM;
        glyphs[i] = icon_colour(icon, occupied ? COLOUR_GLYPH : COLOUR_RIM);

        int bx, by;
        button_centre(i, &bx, &by);
        gx[i] = (int16_t)bx;
        gy[i] = (int16_t)by;
    }

    const int out2 = SEG_R_OUT * SEG_R_OUT;
    const int out_rim2 = (SEG_R_OUT - RIM_PX) * (SEG_R_OUT - RIM_PX);
    const int in2 = SEG_R_IN * SEG_R_IN;
    const int in_rim2 = (SEG_R_IN + RIM_PX) * (SEG_R_IN + RIM_PX);
    const int edge = SEG_BORDER_PX * 256;

    int k = 0;
    for (int sy = s_cy - SEG_R_OUT; sy <= s_cy + SEG_R_OUT; sy++) {
        const int v = sy - base_y;
        if (v < 0 || v >= height) {
            continue;
        }
        uint16_t *row = buf + v * pitch;
        const int dy = sy - s_cy;
        const int dy2 = dy * dy;
        const int half_out = (int)sqrtf((float)(out2 - dy2));

        /* One span across the annulus, or two either side of the hole. */
        int sx0[2], sx1[2];
        int spans;
        if (dy2 < in2) {
            const int half_in = (int)sqrtf((float)(in2 - dy2));
            sx0[0] = s_cx - half_out; sx1[0] = s_cx - half_in - 1;
            sx0[1] = s_cx + half_in + 1; sx1[1] = s_cx + half_out;
            spans = 2;
        } else {
            sx0[0] = s_cx - half_out; sx1[0] = s_cx + half_out;
            spans = 1;
        }

        for (int s = 0; s < spans; s++) {
            int from = sx0[s], to = sx1[s];
            if (from < base_x) from = base_x;
            if (to > base_x + pitch - 1) to = base_x + pitch - 1;

            for (int sx = from; sx <= to; sx++) {
                const int dx = sx - s_cx;
                const int d2 = dx * dx + dy2;
                if (d2 > out2 || d2 < in2) {
                    continue;   /* the chord roots round outwards */
                }

                /* Which wedge. A positive cross product means the pixel is
                 * clockwise of that boundary. */
                int c0 = s_bound_x[k] * dy - s_bound_y[k] * dx;
                while (c0 < 0) {
                    k = (k == 0) ? n - 1 : k - 1;
                    c0 = s_bound_x[k] * dy - s_bound_y[k] * dx;
                }
                int k1 = (k + 1 == n) ? 0 : k + 1;
                int c1 = s_bound_x[k1] * dy - s_bound_y[k1] * dx;
                while (c1 >= 0) {
                    k = k1;
                    c0 = c1;
                    k1 = (k + 1 == n) ? 0 : k + 1;
                    c1 = s_bound_x[k1] * dy - s_bound_y[k1] * dx;
                }

                const int u = sx - base_x;
                uint16_t colour;
                if (d2 > out_rim2 || d2 < in_rim2 || c0 < edge || c1 > -edge) {
                    colour = rims[k];
                } else {
                    const int gdx = sx - gx[k];
                    const int gdy = sy - gy[k];
                    if (gdx >= -SEG_GLYPH_BOX && gdx <= SEG_GLYPH_BOX &&
                        gdy >= -SEG_GLYPH_BOX && gdy <= SEG_GLYPH_BOX &&
                        glyph_hit(s_items[k], gdx * GLYPH_R / SEG_GLYPH_R,
                                              gdy * GLYPH_R / SEG_GLYPH_R)) {
                        colour = glyphs[k];
                    } else {
                        /* Translucent: the paused scene shows through the
                         * fill at half strength, tinted by the state. */
                        colour = mix_px(row[u], tint[k]);
                    }
                }
                row[u] = colour;
            }
        }
    }
}

/* The banner: one straight line of text, centred on the panel. */
static IRAM_ATTR void draw_banner(uint16_t *buf, int pitch, int height,
                                  int base_x, int base_y)
{
    const int len = (int)strlen(s_banner);
    const int wpx = len * BANNER_ADV - BANNER_SCALE;
    const int x0 = (FB_W - wpx) / 2;
    const int y0 = (FB_H - TEXT_ROWS * BANNER_SCALE) / 2;

    for (int sy = y0; sy < y0 + TEXT_ROWS * BANNER_SCALE; sy++) {
        const int v = sy - base_y;
        if (v < 0 || v >= height) {
            continue;
        }
        uint16_t *row = buf + v * pitch;
        const int gr = (sy - y0) / BANNER_SCALE;

        for (int sx = x0; sx < x0 + wpx; sx++) {
            const int u = sx - base_x;
            if (u < 0 || u >= pitch) {
                continue;
            }
            const int ci = (sx - x0) / BANNER_ADV;
            const int col = ((sx - x0) - ci * BANNER_ADV) / BANNER_SCALE;
            if (font_hit(s_banner[ci], col, gr)) {
                row[u] = COLOUR_GLYPH;
            }
        }
    }
}

/* The centre button: one disc filling everything inside the segments' inner
 * arc. Its rim is a wedge's half of a shared border, so where it meets the
 * segments' own inner rim the two read as one joint ring, the same weight
 * as the spokes. The fill is translucent like the wedges'.
 *
 * Three states, kept apart in the fill rather than the glyph: `active` is
 * the finger on it right now, `lit` is what the setting is already, and
 * plain is neither. Recolouring the glyph instead was tried and wrong - it
 * painted over the material colours, so selecting sand turned it green,
 * which is the colour of acid. */
static IRAM_ATTR void draw_button(uint16_t *buf, int pitch, int height,
                                  int base_x, int base_y,
                                  int bx, int by, int radius,
                                  menu_icon_t icon, bool active, uint16_t glyph)
{
    const bool lit = (icon < ICON_COUNT) && (s_lit & (1ull << icon));
    const uint16_t tint = active ? TINT_ACTIVE : lit ? TINT_SELECTED : TINT_REST;
    const uint16_t rim = (active || lit) ? COLOUR_RIM_ON : COLOUR_RIM;
    const int outer2 = radius * radius;
    const int inner2 = (radius - SEG_BORDER_PX) * (radius - SEG_BORDER_PX);

    for (int sy = by - radius; sy <= by + radius; sy++) {
        const int v = sy - base_y;
        if (v < 0 || v >= height) {
            continue;
        }
        uint16_t *row = buf + v * pitch;
        const int dy = sy - by;

        for (int sx = bx - radius; sx <= bx + radius; sx++) {
            const int u = sx - base_x;
            if (u < 0 || u >= pitch) {
                continue;
            }
            const int dx = sx - bx;
            const int d2 = dx * dx + dy * dy;
            if (d2 > outer2) {
                continue;
            }
            /* Plain division for the glyph scale, not a shift: it truncates
             * toward zero, which keeps the sampling symmetric about the
             * centre. */
            row[u] = (d2 > inner2) ? rim
                   : (dx >= -CENTRE_GLYPH_BOX && dx <= CENTRE_GLYPH_BOX &&
                      dy >= -CENTRE_GLYPH_BOX && dy <= CENTRE_GLYPH_BOX &&
                      glyph_hit(icon, dx * GLYPH_R / CENTRE_GLYPH_R,
                                      dy * GLYPH_R / CENTRE_GLYPH_R)) ? glyph
                   : mix_px(row[u], tint);
        }
    }
}

IRAM_ATTR void menu_overlay(uint16_t *buf, int row_first, int row_last, int col_first, int cols)
{
    if (!s_open) {
        if (s_hint != HINT_NONE) {
            draw_hint(buf, row_first, row_last, col_first, cols);
        }
        if (s_banner) {
            draw_banner(buf, cols * g_cell_px, (row_last - row_first) * g_cell_px,
                        g_origin_x + col_first * g_cell_px,
                        g_origin_y + row_first * g_cell_px);
        }
        return;
    }

    const int pitch = cols * g_cell_px;
    const int height = (row_last - row_first) * g_cell_px;
    const int base_x = g_origin_x + col_first * g_cell_px;
    const int base_y = g_origin_y + row_first * g_cell_px;

    draw_segments(buf, pitch, height, base_x, base_y);

    if (s_centre != ICON_NONE) {
        draw_button(buf, pitch, height, base_x, base_y, s_cx, s_cy, CENTRE_R,
                    s_centre, s_selection == s_centre,
                    icon_colour(s_centre, COLOUR_GLYPH));
    }
}
