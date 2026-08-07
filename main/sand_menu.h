#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sand_config.h"

/* Every icon the menus can show. An icon means the same thing wherever it
 * appears, so it doubles as the identity of whatever was picked and the
 * caller can switch on it directly. */
typedef enum {
    ICON_TOOLS = 0,     /* centre of the paint ring */
    ICON_PALETTE,       /* centre of the tools ring, back to materials */
    ICON_ERASE,
    ICON_FILL,
    ICON_NODRAW,        /* touch does nothing, for tilting without painting */
    ICON_CLEAR,
    ICON_PAUSE,
    ICON_SAVE,
    ICON_LOAD,
    ICON_IMPORT,        /* bring a scene in from a PNG on the drive */
    ICON_USB,           /* share the drive with a host */
    ICON_POWER,
    ICON_SAND,
    ICON_WATER,
    ICON_ACID,
    ICON_ACID_GAS,
    ICON_LAVA,
    ICON_FIRE,
    ICON_STEAM,
    ICON_ROCK,
    ICON_WOOD,
    ICON_DIRT,
    /* Slots are consecutive so the index falls out of the icon. */
    ICON_SLOT1,
    ICON_SLOT2,
    ICON_SLOT3,
    ICON_SLOT4,
    ICON_SLOT5,
    ICON_SLOT6,
    ICON_SETTINGS,
    /* Grain sizes, consecutive so the pixel count falls out of the icon. */
    ICON_SIZE2,
    ICON_SIZE3,
    ICON_SIZE4,
    ICON_SIZE5,
    ICON_SIZE6,
    /* Brush radii, likewise consecutive. */
    ICON_BRUSH1,
    ICON_BRUSH2,
    ICON_BRUSH3,
    ICON_BRUSH4,
    ICON_BRIGHT,        /* opens the brightness ring */
    /* Brightness levels, consecutive so the level falls out of the icon. */
    ICON_BRIGHT1,
    ICON_BRIGHT2,
    ICON_BRIGHT3,
    ICON_BRIGHT4,
    ICON_BATTERY,       /* a readout, not a control: the battery gauge */
    ICON_COUNT,
    ICON_NONE = ICON_COUNT,
} menu_icon_t;

/* What to do, what to draw with, and which saved scene. */
typedef enum {
    MENU_SET_PAINT = 0, /* materials, with tools in the middle */
    MENU_SET_TOOLS,     /* eraser, fill, clear, brush sizes */
    MENU_SET_SYSTEM,    /* scenes and config, with play/pause in the middle */
    MENU_SET_GRAIN,
    MENU_SET_BRIGHT,
    MENU_SET_SLOTS,
} menu_set_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Open centred on a screen pixel. The centre is pulled inwards if it would
 * put part of the ring off the panel. */
void menu_open(menu_set_t set, int px, int py);

/* Override the centre button, for rings whose middle depends on context -
 * the slot ring shows whether it is saving or loading. */
void menu_set_centre(menu_icon_t icon);

/* Which slots already hold a scene. Empty ones are drawn faint, so it is
 * obvious what would be overwritten and what there is to load. */
void menu_set_slot_usage(uint8_t mask);

/* What the battery segment shows: 0-100, or negative for unknown. Set it
 * before opening the system ring - the glyph draws whatever was given last. */
void menu_set_battery(int percent);

/* Some buttons are switches rather than actions. Lighting one up is how the
 * ring says which way it is currently set, since the icon alone only names
 * the thing being switched. */
void menu_set_lit(menu_icon_t icon, bool lit);
void menu_close(void);
bool menu_is_open(void);

/* Follow the finger, in screen pixels; returns the icon under it. */
menu_icon_t menu_track(int px, int py);
menu_icon_t menu_selection(void);

/* Pixel rectangle the menu occupies, for forcing a repaint. The segments
 * reach the glass edge, so this is the whole panel. */
void menu_bounds(int *x0, int *y0, int *x1, int *y1);

/* Pixel box of one icon's segment (or the centre button) on the open ring,
 * so a selection change can repaint just the two segments that swapped state
 * instead of the whole screen. Empty (x1 < x0) if the icon is not on the
 * ring, including ICON_NONE. */
void menu_selection_bounds(menu_icon_t icon, int *x0, int *y0, int *x1, int *y1);

/* Paint over a band the renderer has already filled with simulation cells. */
void menu_overlay(uint16_t *buf, int row_first, int row_last, int col_first, int cols);

/* What the panel says when there is nothing else on it - otherwise a cleared
 * bowl is indistinguishable from a dead board. Bounds are in pixels, and
 * differ per hint, so switching between them has to repaint both. */
typedef enum {
    HINT_NONE = 0,
    HINT_DRAW,      /* empty bowl: a prompt curved around the rim */
    HINT_PAUSED,    /* stopped: a large pause mark in the middle */
} menu_hint_t;

void menu_set_hint(menu_hint_t hint);
menu_hint_t menu_hint(void);
void menu_hint_bounds(int *x0, int *y0, int *x1, int *y1);

/* One straight line of text across the middle of the glass - capitals and
 * spaces only. The string must stay alive while shown; NULL takes it down.
 * The caller repaints the area (calibration repaints everything anyway). */
void menu_set_banner(const char *text);

#ifdef __cplusplus
}
#endif
