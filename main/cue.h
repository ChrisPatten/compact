#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

/*
 * Breathing ring (proposal section 2): a thin coloured ring on the rim that breathes while something
 * waits for him that does not own the glass. Colour is the weight of the top open item: faint plum for a notice,
 * amber for a decision, rose for an urgent. Two or more at the top weight breathe twice (a heartbeat).
 *
 * It is derived from decision.c (decision_cue_query), so no bus fields. This file decides colour, opacity and shape in
 * a 40 ms LVGL timer that exists only while a ring is pending; display.c's compositor draws it (compact_ring_show):
 * blended into every band LVGL sends and, for a breath step, sent straight to the panel without an LVGL redraw
 * (proposals/2026-11-22-stage2-ring-compositor.md). Hidden while Capture, push-to-talk, Settings, the power menu,
 * Charging or the flashlight is up, and while the panel is off. Quiet hours: notice off, decision at 60% of its range.
 */
void cue_kick(void);                                  /* LVGL context (display lock held): pending set may have changed */
void cue_debug(const char *args);                     /* any task but LVGL's: "plum|amber|rose [phase 0-100] [count] [quiet]" pins, "off" releases, "tune ...", "bench" */
size_t cue_info_json(char *out, size_t n);            /* {"level":N,"count":N,"shown":bool,...} */
