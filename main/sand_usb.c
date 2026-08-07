/*
 * The NOR flash as a USB drive, and PNGs on it as scenes.
 *
 * The board keeps the FAT partition mounted for itself from boot. Turning on
 * sharing unmounts it here and hands the blocks to the host; turning it off
 * takes them back. Ownership is never shared, because FAT has no way to
 * arbitrate two writers and the damage outlives a power cycle.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "wear_levelling.h"

#include "png.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"

#include "sand_config.h"
#include "sand_render.h"
#include "sand_scene.h"
#include "sand_sim.h"
#include "sand_usb.h"

static const char *TAG = "usb";

#define MOUNT_PATH  "/scenes"

/* An image far larger than the panel buys nothing - it is about to be
 * squeezed into at most 233 cells across - and decoding it costs three bytes
 * per pixel. Refuse rather than quietly exhaust PSRAM. */
#define PNG_MAX_DIM 1024

/* Colours are matched through a lookup keyed on the top four bits of each
 * channel. Comparing every pixel against every shade of every material would
 * be ten million distance calculations for one import; this is four thousand,
 * done once, and 4-bit channels are far finer than the difference between any
 * two materials. */
#define LUT_BITS    4
#define LUT_SIDE    (1 << LUT_BITS)
#define LUT_SIZE    (LUT_SIDE * LUT_SIDE * LUT_SIDE)

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_ready;
static bool s_shared;
static bool s_driver_up;
static uint8_t *s_lut;

/* ------------------------------------------------------------------------
 * Mounting
 * ---------------------------------------------------------------------- */

/* Explains itself on the drive, since an empty volume gives no clue what the
 * board expects to find there. The first line carries a version tag: the file
 * is rewritten when the tag is out of date and left alone otherwise, so the
 * text can improve across firmware updates without clobbering the host's
 * copy at every boot. The colour table is generated from the renderer's own
 * ramps rather than written out by hand, so it cannot drift from what the
 * import will actually match against. */
#define README_TAG  "Sand simulation - scene images (v4)"

static void write_readme(void)
{
    const char *path = MOUNT_PATH "/README.txt";

    char first[64] = { 0 };
    FILE *f = fopen(path, "r");
    if (f) {
        const char *line = fgets(first, sizeof(first), f);
        fclose(f);
        if (line && strncmp(line, README_TAG, strlen(README_TAG)) == 0) {
            return;
        }
    }

    f = fopen(path, "w");
    if (!f) {
        return;
    }
    fputs(README_TAG "\r\n"
          "\r\n"
          "Put PNG files here named slot1.png through slot6.png, then pick\r\n"
          "Import from the system menu (hold the button) and choose a slot.\r\n"
          "\r\n"
          "The image is fitted to the screen and each pixel becomes the\r\n"
          "material whose colour is closest to it. Near-black (R+G+B < 60)\r\n"
          "is empty space. The scene comes out grained, as if the materials\r\n"
          "had been painted on by hand.\r\n"
          "\r\n"
          "Best sizes, one image pixel per grain at each grain setting:\r\n"
          "  233 x 233   (2 px grains)\r\n"
          "  154 x 154   (3 px)\r\n"
          "  116 x 116   (4 px)\r\n"
          "   92 x 92    (5 px)\r\n"
          "   77 x 77    (6 px)\r\n"
          "The grain size is picked from the image's own resolution, and\r\n"
          "other sizes are scaled to fit. 1024 x 1024 is the most either\r\n"
          "side may be.\r\n"
          "\r\n"
          "Make the TOP-RIGHT pixel pure white and the scene keeps the\r\n"
          "image's own colours instead of being recoloured to the palette\r\n"
          "below. Materials are still chosen by nearest colour either way.\r\n"
          "\r\n"
          "Material colours. Each material shades along a ramp between the\r\n"
          "two ends below; any colour on or near that line becomes that\r\n"
          "material:\r\n", f);

    static const struct { int mat; const char *name; } MATS[] = {
        { MAT_SAND,     "sand"     }, { MAT_WATER, "water" },
        { MAT_ACID,     "acid"     }, { MAT_ACID_GAS, "acid gas" },
        { MAT_LAVA,     "lava"     }, { MAT_FIRE,  "fire"  },
        { MAT_STEAM,    "steam"    }, { MAT_SMOKE, "smoke" },
        { MAT_WOOD,     "wood"     }, { MAT_DIRT,  "dirt"  },
        { MAT_ROCK,     "rock"     },
    };
    for (size_t i = 0; i < sizeof(MATS) / sizeof(MATS[0]); i++) {
        uint8_t lo[3], hi[3];
        sand_render_material_ramp(MATS[i].mat, lo, hi);
        fprintf(f, "  %-9s #%02X%02X%02X - #%02X%02X%02X\r\n", MATS[i].name,
                lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    }

    fputs("\r\n"
          "Turn sharing off on the board before importing - only one of us\r\n"
          "can have the drive at a time.\r\n", f);
    fclose(f);
}

void usb_storage_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!part) {
        ESP_LOGW(TAG, "no 'storage' partition; USB sharing unavailable");
        return;
    }

    esp_err_t err = wl_mount(part, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount failed: %s", esp_err_to_name(err));
        return;
    }

    const tinyusb_msc_spiflash_config_t cfg = {
        .wl_handle = s_wl,
        .mount_config = { .max_files = 4 },
    };
    err = tinyusb_msc_storage_init_spiflash(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Formats the volume if it has never been used. */
    err = tinyusb_msc_storage_mount(MOUNT_PATH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return;
    }

    s_ready = true;
    write_readme();
    ESP_LOGI(TAG, "scene drive ready, %lu KB",
             (unsigned long)((uint64_t)tinyusb_msc_storage_get_sector_count() *
                             tinyusb_msc_storage_get_sector_size() / 1024));
}

bool usb_storage_ready(void)  { return s_ready; }
bool usb_storage_shared(void) { return s_shared; }

esp_err_t usb_storage_share(bool on)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on == s_shared) {
        return ESP_OK;
    }

    if (on) {
        if (!s_driver_up) {
            /* Say this before it happens, because saying it afterwards may
             * not reach anyone: the same port carries the log. */
            ESP_LOGW(TAG, "claiming the USB port - serial over USB stops here, "
                          "and stays stopped until the next restart");
            const tinyusb_config_t tcfg = { 0 };   /* default descriptors */
            const esp_err_t err = tinyusb_driver_install(&tcfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "USB install failed: %s", esp_err_to_name(err));
                return err;
            }
            s_driver_up = true;
        }
        /* Let go before the host is allowed to look. */
        tinyusb_msc_storage_unmount();
        tud_connect();
        s_shared = true;
        ESP_LOGI(TAG, "drive handed to the host");
    } else {
        tud_disconnect();
        const esp_err_t err = tinyusb_msc_storage_mount(MOUNT_PATH);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not take the drive back: %s", esp_err_to_name(err));
            return err;
        }
        s_shared = false;
        ESP_LOGI(TAG, "drive is ours again");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Colour matching
 * ---------------------------------------------------------------------- */

/* Nearest shade of any material to one colour, as a ready-made cell. */
static uint8_t nearest_cell(int r, int g, int b)
{
    int best = CELL_EMPTY;
    int best_d = INT_MAX;

    /* From 1, so MAT_EMPTY is never a candidate; MAT_WALL is skipped for the
     * same reason - both are black, and neither is something to paint. */
    for (int m = 1; m < MAT_COUNT; m++) {
        if (m == MAT_WALL) {
            continue;
        }
        uint8_t lo[3], hi[3];
        sand_render_material_ramp(m, lo, hi);

        for (int s = 0; s < SHADE_COUNT; s++) {
            const int den = SHADE_COUNT - 1;
            const int dr = r - (lo[0] + (hi[0] - lo[0]) * s / den);
            const int dg = g - (lo[1] + (hi[1] - lo[1]) * s / den);
            const int db = b - (lo[2] + (hi[2] - lo[2]) * s / den);
            const int d = dr * dr + dg * dg + db * db;
            if (d < best_d) {
                best_d = d;
                best = CELL_MAKE(m, s);
            }
        }
    }
    return (uint8_t)best;
}

static bool build_lut(void)
{
    if (s_lut) {
        return true;
    }
    s_lut = heap_caps_malloc(LUT_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_lut) {
        ESP_LOGE(TAG, "no PSRAM for the colour table");
        return false;
    }

    /* Each entry stands for a 16-wide cube of colours, so match its middle. */
    const int step = 256 / LUT_SIDE;
    int i = 0;
    for (int r = 0; r < LUT_SIDE; r++) {
        for (int g = 0; g < LUT_SIDE; g++) {
            for (int b = 0; b < LUT_SIDE; b++) {
                s_lut[i++] = nearest_cell(r * step + step / 2,
                                          g * step + step / 2,
                                          b * step + step / 2);
            }
        }
    }
    return true;
}

static inline uint8_t classify(int r, int g, int b)
{
    /* Near-black is background. Empty space is genuinely off on this panel,
     * so what looks like nothing in the image becomes nothing here. */
    if (r + g + b < 60) {
        return CELL_EMPTY;
    }
    const int idx = ((r >> (8 - LUT_BITS)) << (2 * LUT_BITS)) |
                    ((g >> (8 - LUT_BITS)) << LUT_BITS) |
                     (b >> (8 - LUT_BITS));
    return s_lut[idx];
}

/* ------------------------------------------------------------------------
 * Keeping an image's own colours.
 *
 * A cell is material plus a four-bit shade, and the shade indexes the
 * renderer's palette - so a scene can wear the image's colours by turning
 * each material's sixteen shades into a sixteen-colour palette drawn from
 * the image itself. Pixels are binned by brightness within their material,
 * each bin's average colour becomes that shade's entry, and every cell is
 * re-shaded to its bin. Physics is untouched - the material was already
 * chosen the ordinary way - and colour travels with a falling grain
 * because it lives in the cell byte.
 *
 * Fire, smoke and acid gas stay on their ramps: their shade is remaining
 * life, so rebinning would scramble their lifetimes, and repainting them
 * would dress every later flame in the image's colours. */
static bool shade_is_life(int material)
{
    return material == MAT_FIRE || material == MAT_SMOKE || material == MAT_ACID_GAS;
}

static void install_true_colours(uint8_t *cells, const uint8_t *pix,
                                 int gw, int gh, int dw, int dh,
                                 int ox, int oy, int nx, int ny)
{
    /* Static rather than stacked: three KB is a lot to ask of the frame
     * task's stack, and imports are serialised anyway. */
    static uint32_t s_sum[MAT_COUNT][SHADE_COUNT][3];
    static uint16_t s_bins[MAT_COUNT][SHADE_COUNT];
    static int16_t s_lum_lo[MAT_COUNT], s_lum_hi[MAT_COUNT];

    memset(s_sum, 0, sizeof(s_sum));
    memset(s_bins, 0, sizeof(s_bins));
    for (int m = 0; m < MAT_COUNT; m++) {
        s_lum_lo[m] = INT16_MAX;
        s_lum_hi[m] = -1;
    }

    /* Materials the image never uses go back to their ramps, in case an
     * earlier import left its own colours on them. */
    sand_render_palette_reset();

    /* Pass one: the brightness range each material spans in this image. */
    for (int cy = 0; cy < gh; cy++) {
        const int v = cy - oy;
        if (v < 0 || v >= dh) {
            continue;
        }
        const uint8_t *src = pix + (size_t)(v * ny / dh) * nx * 3;
        const uint8_t *row = cells + (size_t)cy * gw;
        for (int cx = 0; cx < gw; cx++) {
            const int u = cx - ox;
            if (u < 0 || u >= dw) {
                continue;
            }
            const int m = CELL_MAT(row[cx]);
            if (m == MAT_EMPTY || shade_is_life(m)) {
                continue;
            }
            const uint8_t *p = src + (size_t)(u * nx / dw) * 3;
            const int luma = p[0] + p[1] + p[2];
            if (luma < s_lum_lo[m]) s_lum_lo[m] = (int16_t)luma;
            if (luma > s_lum_hi[m]) s_lum_hi[m] = (int16_t)luma;
        }
    }

    /* Pass two: bin every cell by brightness within its material, sum each
     * bin's colour, and point the cell's shade at its bin. */
    for (int cy = 0; cy < gh; cy++) {
        const int v = cy - oy;
        if (v < 0 || v >= dh) {
            continue;
        }
        const uint8_t *src = pix + (size_t)(v * ny / dh) * nx * 3;
        uint8_t *row = cells + (size_t)cy * gw;
        for (int cx = 0; cx < gw; cx++) {
            const int u = cx - ox;
            if (u < 0 || u >= dw) {
                continue;
            }
            const int m = CELL_MAT(row[cx]);
            if (m == MAT_EMPTY || shade_is_life(m)) {
                continue;
            }
            const uint8_t *p = src + (size_t)(u * nx / dw) * 3;
            const int luma = p[0] + p[1] + p[2];
            const int range = s_lum_hi[m] - s_lum_lo[m];
            const int shade = (range > 0) ? (luma - s_lum_lo[m]) * SHADE_MASK / range : 0;
            s_sum[m][shade][0] += p[0];
            s_sum[m][shade][1] += p[1];
            s_sum[m][shade][2] += p[2];
            s_bins[m][shade]++;
            row[cx] = CELL_MAKE(m, shade);
        }
    }

    /* Install. Bins nothing landed in borrow the nearest filled one, so
     * material the user pours in later still comes out in the image's own
     * colours rather than a stray ramp entry. */
    for (int m = 1; m < MAT_COUNT; m++) {
        if (shade_is_life(m) || s_lum_hi[m] < 0) {
            continue;
        }
        uint8_t rgb[SHADE_COUNT][3];
        int first = -1, prev = -1;
        for (int s = 0; s < SHADE_COUNT; s++) {
            if (s_bins[m][s]) {
                rgb[s][0] = (uint8_t)(s_sum[m][s][0] / s_bins[m][s]);
                rgb[s][1] = (uint8_t)(s_sum[m][s][1] / s_bins[m][s]);
                rgb[s][2] = (uint8_t)(s_sum[m][s][2] / s_bins[m][s]);
                if (first < 0) first = s;
                prev = s;
            } else if (prev >= 0) {
                rgb[s][0] = rgb[prev][0];
                rgb[s][1] = rgb[prev][1];
                rgb[s][2] = rgb[prev][2];
            }
        }
        for (int s = 0; s < first; s++) {
            rgb[s][0] = rgb[first][0];
            rgb[s][1] = rgb[first][1];
            rgb[s][2] = rgb[first][2];
        }
        sand_render_palette_override(m, rgb);
    }
}

/* ------------------------------------------------------------------------
 * Import
 * ---------------------------------------------------------------------- */

static void slot_path(int slot, char *out, size_t len)
{
    snprintf(out, len, MOUNT_PATH "/slot%d.png", slot + 1);
}

uint8_t usb_png_mask(void)
{
    if (!s_ready || s_shared) {
        return 0;
    }

    uint8_t mask = 0;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        char path[48];
        struct stat st;
        slot_path(i, path, sizeof(path));
        if (stat(path, &st) == 0 && st.st_size > 0) {
            mask |= (uint8_t)(1u << i);
        }
    }
    return mask;
}

esp_err_t usb_png_load(int slot, uint8_t *cells, int *out_cell_px)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_shared) {
        ESP_LOGW(TAG, "the host has the drive; turn sharing off to import");
        return ESP_ERR_INVALID_STATE;
    }
    if (!build_lut()) {
        return ESP_ERR_NO_MEM;
    }

    char path[48];
    slot_path(slot, path, sizeof(path));

    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_file(&img, path)) {
        ESP_LOGW(TAG, "%s: %s", path, img.message);
        return ESP_ERR_NOT_FOUND;
    }
    if (img.width > PNG_MAX_DIM || img.height > PNG_MAX_DIM) {
        ESP_LOGW(TAG, "%s is %lux%lu; %d is the most either side may be",
                 path, (unsigned long)img.width, (unsigned long)img.height, PNG_MAX_DIM);
        png_image_free(&img);
        return ESP_ERR_INVALID_SIZE;
    }

    img.format = PNG_FORMAT_RGB;
    const size_t bytes = PNG_IMAGE_SIZE(img);
    uint8_t *pix = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!pix) {
        ESP_LOGE(TAG, "no PSRAM for a %ux%u image", (unsigned)img.width, (unsigned)img.height);
        png_image_free(&img);
        return ESP_ERR_NO_MEM;
    }
    /* Composite any alpha onto black explicitly. Transparent regions then
     * classify as empty space, and - just as important - the white-corner
     * test below stays deterministic: left to guess a background, libpng
     * could hand back a corner that looked white in the editor but is not
     * white here. */
    const png_color on_black = { 0, 0, 0 };
    if (!png_image_finish_read(&img, &on_black, pix, 0, NULL)) {
        ESP_LOGW(TAG, "%s: %s", path, img.message);
        heap_caps_free(pix);
        png_image_free(&img);
        return ESP_FAIL;
    }

    const int nx = (int)img.width;
    const int ny = (int)img.height;
    png_image_free(&img);

    /* The image itself says how it wants to arrive: a white top-right pixel
     * asks for its own colours to be kept, anything else gets recoloured to
     * the material palette. In-band, so no menu setting to remember - and
     * the marker pixel itself lands in the bowl wall's corner, where nothing
     * is ever shown. */
    const uint8_t *marker = pix + (size_t)(nx - 1) * 3;
    const bool original_colours = (marker[0] >= 0xF0 && marker[1] >= 0xF0 &&
                                   marker[2] >= 0xF0);

    /* Pick the grain from the image's own resolution, so a picture drawn at
     * one cell per pixel arrives at exactly that. */
    const int span = (nx > ny) ? nx : ny;
    int cell = (span > 0) ? (FB_W + span / 2) / span : CELL_PX_DEFAULT;
    if (cell < CELL_PX_MIN) cell = CELL_PX_MIN;
    if (cell > CELL_PX_MAX) cell = CELL_PX_MAX;

    /* Whatever grid that grain will actually build - not FB_W/cell, which is
     * a cell too wide at the odd grain sizes. */
    int gw, gh;
    sand_grid_extent(cell, &gw, &gh);

    /* Fit the whole image inside the grid, keeping its shape; whatever is
     * left over at the sides stays empty. */
    int dw, dh;
    if (nx * gh >= ny * gw) {
        dw = gw;
        dh = ny * gw / nx;
    } else {
        dh = gh;
        dw = nx * gh / ny;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    const int ox = (gw - dw) / 2;
    const int oy = (gh - dh) / 2;

    /* Flat fills straight off the LUT come out flat - one shade for the
     * whole region - which reads as pasted clip-art next to anything drawn
     * on the glass. Recolouring means making the image native, so each cell
     * rolls a random shade exactly the way the brush does. The image's own
     * shading only survives in original-colours mode, where it is the
     * point. */
    uint32_t rng = esp_random() | 1;

    for (int cy = 0; cy < gh; cy++) {
        uint8_t *row = cells + (size_t)cy * gw;
        const int v = cy - oy;
        if (v < 0 || v >= dh) {
            memset(row, CELL_EMPTY, gw);
            continue;
        }
        const uint8_t *src = pix + (size_t)(v * ny / dh) * nx * 3;
        for (int cx = 0; cx < gw; cx++) {
            const int u = cx - ox;
            if (u < 0 || u >= dw) {
                row[cx] = CELL_EMPTY;
                continue;
            }
            const uint8_t *p = src + (size_t)(u * nx / dw) * 3;
            uint8_t c = classify(p[0], p[1], p[2]);
            if (!original_colours && c != CELL_EMPTY) {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                c = CELL_MAKE(CELL_MAT(c), rng & SHADE_MASK);
            }
            row[cx] = c;
        }
    }

    if (original_colours) {
        install_true_colours(cells, pix, gw, gh, dw, dh, ox, oy, nx, ny);
    } else {
        /* A previous import may have left its own colours installed. */
        sand_render_palette_reset();
    }

    heap_caps_free(pix);
    *out_cell_px = cell;
    ESP_LOGI(TAG, "imported %s: %dx%d image into a %dx%d grid at %d px, %s "
                  "(top-right pixel %02X%02X%02X)",
             path, nx, ny, gw, gh, cell,
             original_colours ? "keeping its colours" : "recoloured",
             marker[0], marker[1], marker[2]);
    return ESP_OK;
}
