#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sand_config.h"

/* Materials, ordered by density - and that ordering *is* the rule for what
 * displaces what. A cell may move into any neighbour holding something
 * strictly lighter, so sand sinks through water, water sinks through steam,
 * and nothing at all shifts rock. Empty being lightest falls out for free. */
#define MAT_EMPTY           0
#define MAT_SMOKE           1   /* what a fire leaves behind */
#define MAT_STEAM           2
#define MAT_ACID_GAS        3   /* what acid becomes when it eats something */
/* Heavier than every gas and lighter than every liquid, and both halves of
 * that matter. Nothing gaseous can displace a flame, and a flame is never
 * capped by the smoke it just produced - it climbs straight through it.
 *
 * Fire started out as the lightest material of all, on the reasoning that
 * flames belong under their own smoke. The cost was the exact opposite: the
 * mover let smoke swap places with fire while forbidding the reverse, so a
 * burning surface would blanket itself and its own flames would be shoved
 * off the fuel and skipped. Water still sinks into fire, which is what makes
 * pouring it on work. */
#define MAT_FIRE            4
#define MAT_WATER           5
#define MAT_ACID            6
#define MAT_LAVA            7
#define MAT_SAND            8
#define MAT_DIRT            9   /* heavier and slower than sand */
#define MAT_WOOD            10  /* static, but it burns */
#define MAT_ROCK            11
#define MAT_WALL            12  /* the bowl itself */
#define MAT_COUNT           13

/* Each cell is one byte: material in the high nibble, a shade in the low one
 * so a body of material has texture rather than reading as a flat blob. The
 * whole byte indexes the palette directly, which keeps the renderer's inner
 * loop to a single lookup. */
#define SHADE_BITS          4
#define SHADE_COUNT         (1 << SHADE_BITS)
#define SHADE_MASK          (SHADE_COUNT - 1)

#define CELL_MAKE(m, s)     ((uint8_t)(((m) << SHADE_BITS) | (s)))
#define CELL_MAT(c)         ((c) >> SHADE_BITS)
#define CELL_SHADE(c)       ((c) & SHADE_MASK)
#define CELL_EMPTY          CELL_MAKE(MAT_EMPTY, 0)
#define CELL_KINDS          (MAT_COUNT * SHADE_COUNT)

/* Geometry of the currently selected grain size. Plain globals rather than
 * accessors because the physics inner loop reads them constantly. */
extern int g_cell_px;       /* screen pixels per cell */
extern int g_grid_w;
extern int g_grid_h;
extern int g_grid_cells;
extern int g_origin_x;      /* where the grid sits on the panel, in pixels */
extern int g_origin_y;
extern int g_bowl_radius;   /* in cells */

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only view of the grid, GRID_W * GRID_H bytes, row major. */
const uint8_t *sand_grid(void);

/* Per-row span of cells that changed this frame, so the renderer can push a
 * tight rectangle. A row with nothing dirty reports hi < lo. */
const int16_t *sand_dirty_lo(void);
const int16_t *sand_dirty_hi(void);
void sand_clear_dirty(void);

/* Force a cell rectangle to repaint, for things drawn over the simulation
 * that the simulation itself knows nothing about. */
void sand_force_dirty(int x0, int y0, int x1, int y1);

/* Build the round bowl at the given grain size and drop a heap of sand in.
 * Safe to call again to change size; the contents are not carried over. */
esp_err_t sand_init(int cell_px);

/* The grid a given grain size produces. Exposed so that anything handing over
 * a whole grid - a saved scene, an imported image - can check it is the right
 * shape before it is copied in. */
void sand_grid_extent(int cell_px, int *w, int *h);

/* Advance the simulation. (gx, gy) is the gravity direction in screen space
 * and need not be normalised. */
void sand_step(float gx, float gy);

/* Paint `material` along the segment, so a dragged finger leaves a stroke.
 * MAT_EMPTY erases. Rock and wall are only displaced by an explicit erase. */
void sand_paint(int x0, int y0, int x1, int y1, int radius, int material);

/* Flood the connected run of whatever is at (x, y) with `material`. */
void sand_fill_region(int x, int y, int material);

/* Empty the bowl, leaving only the wall. */
void sand_clear_all(void);

/* Replace the whole grid, e.g. from a saved scene. The caller must have set
 * the matching grain size first. */
void sand_load_grid(const uint8_t *cells);

/* Cells currently holding something that moves. */
uint32_t sand_count(void);

#ifdef __cplusplus
}
#endif
