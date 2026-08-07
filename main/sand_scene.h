#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sand_config.h"

/* Scenes live in their own raw flash partition. A scene is one fixed-size
 * blob, so a slot is nothing more than an offset - no filesystem to mount,
 * nothing to fsck, and a torn write can only damage the slot being written. */
#define SCENE_SLOTS     6

#ifdef __cplusplus
extern "C" {
#endif

/* Locate the partition. Safe to call if it is missing; save and load will
 * then simply report an error. */
void scene_init(void);

esp_err_t scene_save(int slot, const uint8_t *cells);

/* Loads the cells and reports the grain size the scene was built at, so the
 * caller can switch to it - a scene is raw cells and only means anything at
 * its own resolution. */
esp_err_t scene_load(int slot, uint8_t *cells, int *out_cell_px);

/* Bit per slot, set where a valid scene is stored. */
uint8_t scene_used_mask(void);

#ifdef __cplusplus
}
#endif
