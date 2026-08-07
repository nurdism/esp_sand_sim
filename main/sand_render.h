#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the band buffers and hook the panel's transfer-done callback. */
esp_err_t sand_render_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io);

/* Brightest shade of a material, for anything outside the simulation that
 * wants to show what it is - menu icons, mainly. */
uint16_t sand_render_material_colour(int material);

/* The two ends of a material's colour ramp, as plain RGB. Importing an image
 * works the other way round from drawing one, and needs the ramp itself
 * rather than a single colour so a pixel can pick its shade as well as its
 * material. */
void sand_render_material_ramp(int material, uint8_t lo[3], uint8_t hi[3]);

/* Repaint and push every band containing a row the simulation touched. */
void sand_render_frame(const uint8_t *grid);

/* The live palette, CELL_KINDS RGB565 entries in panel byte order - an
 * opaque blob to everyone but the renderer, exposed so a saved scene can
 * carry its colours (an import may have replaced them; see below). */
const uint16_t *sand_render_palette(void);
void sand_render_palette_set(const uint16_t *palette);

/* Put the ramp-built palette back. Anything that starts a fresh scene calls
 * this, since an import with original colours leaves its own behind. */
void sand_render_palette_reset(void);

/* Replace one material's SHADE_COUNT shades with arbitrary RGB triples, for
 * scenes that keep an imported image's own colours. */
void sand_render_palette_override(int material, const uint8_t rgb[][3]);

#ifdef __cplusplus
}
#endif
