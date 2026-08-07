# esp_sim

A falling-sand simulation for the Waveshare **ESP32-S3-Touch-AMOLED-1.75C** —
a 466x466 round AMOLED (CO5300, QSPI) with CST9217 touch, a QMI8658 IMU, and
an AXP2101 PMIC. Tilt the board and the sand pours toward the low edge; touch
the glass to paint materials in; set fire to things.

Video: https://youtu.be/Gdmk3TZFWq0

Rendering goes straight to the panel over QSPI — no LVGL, no framebuffer in
PSRAM — painted and pushed a band at a time from internal DMA memory, with
per-row dirty tracking so only what changed gets sent.

## Materials

Twelve materials, ordered by density — and that ordering *is* the physics:
a cell may move into any neighbour holding something strictly lighter.

| | |
|---|---|
| **sand, dirt** | granular; heaps hold a 45-degree angle of repose |
| **water, acid, lava** | liquids; spread until level. Acid dissolves sand, dirt, rock and wood; lava ignites wood and turns to rock in water |
| **fire** | burns wood, needs air, climbs against gravity, dies into smoke |
| **smoke, steam, acid gas** | rise and thin out; steam condenses when trapped |
| **wood, rock** | static — but wood burns and both dissolve in acid |
| **wall** | the bowl itself |

## Controls

| Gesture | Action |
|---|---|
| touch / drag | paint the selected material |
| Key1 click | materials menu (centre: tools) |
| Key1 hold (0.5 s) | system menu — save/load/import, settings, battery, power |
| Key1 click while a menu is open | close it |
| Key1 held ~1 s at power-on | redo touch calibration |

Menus are full-screen pie rings: wedge segments running to the edge of the
glass, translucent over the paused scene, with a big centre button. Drag to a
wedge and lift to pick; lift on the centre of a ring with no centre action to
cancel.

First boot runs a guided touch calibration — **TAP THE DOT**, twice — and
stores the measured orientation in NVS. A run that never fits stores nothing
and falls back to the compile-time guess in `main/sand_config.h`.

## Scenes and PNG import

Six scene slots live in a raw flash partition (cells plus colour palette;
slot 1 is restored at boot). The 4 MB FAT partition can be shared to a host
as a USB drive (settings ring → USB): drop in `slot1.png` … `slot6.png` and
import them from the system menu.

Images are fitted to the screen and each pixel becomes the material with the
nearest colour; the grain size is picked from the image's own resolution
(233x233 down to 77x77 for 1:1 pixels). Two modes, chosen by the image
itself:

- **default** — recoloured and re-grained, as if painted on by hand
- **top-right pixel pure white** — the scene keeps the image's own colours,
  via per-material palette overrides that travel with the saved scene

A `README.txt` with the resolution table and the exact material colour ramps
is generated onto the drive from the renderer's own palette data.

## Building

ESP-IDF **v5.5.x**, target `esp32s3`:

```sh
idf.py build flash monitor
```

`sdkconfig.defaults` and `partitions.csv` (8 MB app, 512 K raw scene
partition, 4 MB FAT) are checked in; the first build regenerates `sdkconfig`.
Managed components (BSP, TinyUSB, libpng…) are fetched by the component
manager into `managed_components/`.

## Source layout

| File | What it owns |
|---|---|
| `main/sand_board.c` | the board itself: CO5300 panel over QSPI, CST9217 touch, shared I2C, brightness — the thin slice of hardware init a vendor BSP would otherwise wrap in LVGL |
| `main/sand_sim.c` | the cellular physics: movement, reactions, per-row activity spans so settled cells cost nothing |
| `main/sand_render.c` | palette, band painting, dirty-rect transfers to the panel |
| `main/sand_menu.c` | pie menus, icons, hints, banner text, overlay drawing |
| `main/sand_input.c` | IMU gravity, touch mapping + orientation solver, button, PMIC |
| `main/sand_scene.c` | scene slots in raw flash |
| `main/sand_usb.c` | FAT/USB mass storage, PNG import, drive README |
| `main/sand_config.h` | every tuning knob: grain sizes, frame period, orientations, brush |
| `main/sand_trace.h` | frame-loop stage markers read by a watchdog task |

## Performance notes

The whole visible pipeline is dirty-driven: physics keeps per-row activity
spans and proves settled cells immobile (all eight neighbours at least as
dense — true whichever way gravity turns), the renderer trims every transfer
to the changed rows and columns, and menu tracking repaints only the wedges
whose highlight changed. The stats line in the log reports physics/render
cost every 200 frames.
