#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * A model of panel memory built from the bytes actually sent to the glass (screenshot.c). Pure C, no ESP-IDF or LVGL,
 * host tested in test/host/ring_px_test.c.
 *
 * The image is w x h pixels of two bytes each in PANEL order: the bytes exactly as handed to the driver's draw_bitmap
 * (the CO5300 takes RGB565 high byte first). Coordinates are the ones handed to draw_bitmap (x_end, y_end exclusive);
 * the driver adds the panel's column gap itself, the same for every write, so the model needs no gap. Every loop here
 * is a bounded for loop over the given rectangle, clipped to the image.
 */

/* Copy a packed write (x_end - x_start pixels per row) into the image. rowpx (h entries, may be NULL) counts the
   pixels written per row. Returns false (and writes nothing) for a rectangle not inside the image. */
bool pimg_write(uint8_t *img, int w, int h, int xs, int ys, int xe, int ye, const uint8_t *src, uint16_t *rowpx);

/* How many pixels of a packed write differ from what the image holds there (0 for a rectangle not inside it). */
uint32_t pimg_diff(const uint8_t *img, int w, int h, int xs, int ys, int xe, int ye, const uint8_t *src);

/* One image row decoded as the panel shows it: RGB888, 5/6-bit channels widened as LVGL and PNG viewers do. */
void pimg_row_rgb(const uint8_t *row, int w, uint8_t *rgb);
