#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "sand_render.h"
#include "sand_scene.h"
#include "sand_sim.h"

static const char *TAG = "sand_scene";

#define SCENE_MAGIC     0x53414E44u     /* "SAND" */
#define SCENE_VERSION   6   /* bumped when the palette joined the format */

/* The palette travels with the cells: an import that kept its image's own
 * colours means shade bytes only mean anything alongside the colours they
 * index, and slot 1 is restored at boot. */
#define PALETTE_BYTES   (CELL_KINDS * sizeof(uint16_t))

/* Flash erases a sector at a time, so a slot has to be a whole number of
 * them or saving one scene would destroy its neighbour. */
#define SECTOR          4096
#define SLOT_BYTES      (((sizeof(scene_header_t) + PALETTE_BYTES + GRID_CELLS_MAX) \
                          + SECTOR - 1) / SECTOR * SECTOR)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t cell_px;   /* grain size the scene was built at */
    uint16_t reserved;
} scene_header_t;

static const esp_partition_t *s_part;

void scene_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "scenes");
    if (!s_part) {
        ESP_LOGE(TAG, "no 'scenes' partition; saving disabled");
        return;
    }
    if (s_part->size < SCENE_SLOTS * SLOT_BYTES) {
        ESP_LOGE(TAG, "'scenes' is %u bytes, need %u", (unsigned)s_part->size,
                 (unsigned)(SCENE_SLOTS * SLOT_BYTES));
        s_part = NULL;
        return;
    }
    ESP_LOGI(TAG, "%d slots of %u bytes in %u bytes of flash",
             SCENE_SLOTS, (unsigned)SLOT_BYTES, (unsigned)s_part->size);
}

static bool slot_valid(int slot)
{
    return s_part && slot >= 0 && slot < SCENE_SLOTS;
}

esp_err_t scene_save(int slot, const uint8_t *cells)
{
    if (!slot_valid(slot)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t offset = (size_t)slot * SLOT_BYTES;
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_part, offset, SLOT_BYTES), TAG,
                        "erase of slot %d failed", slot);

    const scene_header_t header = {
        .magic = SCENE_MAGIC,
        .version = SCENE_VERSION,
        .width = g_grid_w,
        .height = g_grid_h,
        .cell_px = g_cell_px,
    };

    /* Payload first, header last. The header is what makes a slot count as
     * valid, so writing it only once the payload is down means an interrupted
     * save leaves the slot empty rather than half-written. */
    ESP_RETURN_ON_ERROR(esp_partition_write(s_part, offset + sizeof(header),
                                            sand_render_palette(), PALETTE_BYTES), TAG,
                        "palette of slot %d failed", slot);
    ESP_RETURN_ON_ERROR(esp_partition_write(s_part, offset + sizeof(header) + PALETTE_BYTES,
                                            cells, g_grid_cells), TAG,
                        "write of slot %d failed", slot);
    ESP_RETURN_ON_ERROR(esp_partition_write(s_part, offset, &header, sizeof(header)), TAG,
                        "header of slot %d failed", slot);

    ESP_LOGI(TAG, "saved slot %d", slot + 1);
    return ESP_OK;
}

esp_err_t scene_load(int slot, uint8_t *cells, int *out_cell_px)
{
    if (!slot_valid(slot)) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t offset = (size_t)slot * SLOT_BYTES;
    scene_header_t header;
    ESP_RETURN_ON_ERROR(esp_partition_read(s_part, offset, &header, sizeof(header)), TAG,
                        "read of slot %d failed", slot);

    if (header.magic != SCENE_MAGIC) {
        return ESP_ERR_NOT_FOUND;
    }
    /* A scene is raw cells, so a build with a different grid or a different
     * material encoding cannot use it. */
    if (header.version != SCENE_VERSION) {
        ESP_LOGW(TAG, "slot %d was saved by a different build", slot + 1);
        return ESP_ERR_INVALID_VERSION;
    }
    if (header.width > GRID_W_MAX || header.height > GRID_H_MAX ||
        header.cell_px < CELL_PX_MIN || header.cell_px > CELL_PX_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* The cells are about to be copied into a grid built from cell_px alone,
     * so the stored shape has to be the shape that grain still produces. It
     * has changed once already - making the grid an even number of pixels
     * wide moved the 3 px grain from 155 columns to 154 - and a mismatch here
     * would not fail, it would quietly shear the scene by a cell per row. */
    int want_w, want_h;
    sand_grid_extent(header.cell_px, &want_w, &want_h);
    if (header.width != want_w || header.height != want_h) {
        ESP_LOGW(TAG, "slot %d holds a %ux%u grid; %u px cells now make %dx%d",
                 slot + 1, (unsigned)header.width, (unsigned)header.height,
                 (unsigned)header.cell_px, want_w, want_h);
        return ESP_ERR_INVALID_SIZE;
    }
    if (out_cell_px) {
        *out_cell_px = header.cell_px;
    }

    uint16_t palette[CELL_KINDS];
    ESP_RETURN_ON_ERROR(esp_partition_read(s_part, offset + sizeof(header),
                                           palette, PALETTE_BYTES), TAG,
                        "palette of slot %d failed", slot);
    ESP_RETURN_ON_ERROR(esp_partition_read(s_part, offset + sizeof(header) + PALETTE_BYTES,
                                           cells, (size_t)header.width * header.height), TAG,
                        "read of slot %d failed", slot);

    /* Only once everything is read, so a failed load cannot leave the old
     * scene wearing the new scene's colours. */
    sand_render_palette_set(palette);
    ESP_LOGI(TAG, "loaded slot %d", slot + 1);
    return ESP_OK;
}

uint8_t scene_used_mask(void)
{
    uint8_t mask = 0;
    if (!s_part) {
        return 0;
    }

    for (int slot = 0; slot < SCENE_SLOTS; slot++) {
        scene_header_t header;
        if (esp_partition_read(s_part, (size_t)slot * SLOT_BYTES,
                               &header, sizeof(header)) != ESP_OK) {
            continue;
        }
        if (header.magic == SCENE_MAGIC) {
            mask |= (uint8_t)(1u << slot);
        }
    }
    return mask;
}
