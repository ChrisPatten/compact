#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

/*
 * Screenshot: BOOT + PWR pressed together (power.c, chord_check) or POST /debug/ui screenshot.
 * The picture is built from what was actually sent to the panel: display.c's single pixel writer (panel_write) calls
 * screenshot_record() for every write, LVGL bands and ring breath steps alike, and a grab records into one 466 x 466
 * image in PSRAM (434 KB, allocated on first use) in panel byte order at the coordinates handed to the driver
 * (panel_img.c). A grab invalidates the whole screen so every row is sent at least once, and while a ring breathes it
 * then asks the compositor for one direct step at the blend the glass already shows, compared byte for byte with what
 * the flushes sent (/info screenshot.step_diff must be 0). Only the last shot is kept; it is never uploaded anywhere.
 * GET /screenshot (Bearer) streams it as a PNG (stored deflate blocks, no compression, ~650 KB).
 */
bool screenshot_take(void);                   /* any task but LVGL's; false when refused (asleep, Capture, push-to-talk, no memory, timeout) */
esp_err_t screenshot_send_png(httpd_req_t *req);
size_t screenshot_info_json(char *out, size_t n);   /* {"count":N,"age_s":S,"step_diff":D,"step_px":P} */

/* display.c only, display lock held: */
void screenshot_record(int xs, int ys, int xe, int ye, const void *px, bool lvgl);   /* every panel write (x/y end exclusive, packed, panel order) */
bool screenshot_verify_wanted(void);
uint32_t screenshot_diff(int xs, int ys, int xe, int ye, const void *px);           /* pixels of a write that differ from the image */
void screenshot_verify_done(int32_t diff, uint32_t px);                              /* diff -1: no direct step possible */
