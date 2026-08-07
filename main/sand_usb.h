#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The FAT partition on the NOR flash, and the USB device that exposes it.
 *
 * Exactly one side may own a filesystem at a time. While the host has the
 * drive, the board must not touch it - two writers on one FAT volume corrupt
 * it in a way that survives a reboot - so sharing it and reading from it are
 * deliberately the same switch. */

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the partition for our own use, formatting it on first boot. Touches
 * nothing to do with USB, so the serial console survives. */
void usb_storage_init(void);
bool usb_storage_ready(void);

/* Hand the drive to the host, or take it back.
 *
 * The first hand-over installs the USB device stack, which takes the port
 * away from USB-Serial-JTAG for the rest of the session: logging over USB
 * stops and flashing needs the BOOT button or a reboot. Turning sharing back
 * off unmounts the drive from the host and returns the filesystem to us, but
 * does not give the port back - only a restart does that. */
esp_err_t usb_storage_share(bool on);
bool usb_storage_shared(void);

/* Bit per slot, set where a slotN.png is waiting to be imported. Reads as
 * empty while the host owns the drive, because it is not ours to look at. */
uint8_t usb_png_mask(void);

/* Decode slotN.png into a cell grid, picking the grain size that best fits
 * the image and reporting it - the caller has to rebuild the grid at that
 * size before the cells mean anything. */
esp_err_t usb_png_load(int slot, uint8_t *cells, int *out_cell_px);

#ifdef __cplusplus
}
#endif
