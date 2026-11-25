#pragma once
/*
 * The 12-hour fixed rim (proposal 2026-11-22 section 1; spec 4.2 and 5.1): pure maths, no LVGL, no ESP-IDF, so it is
 * host tested (test/host/rim12_test.c). dial.c caches one rim_t per minute and draws it; dayview.c uses the lane
 * rule and the angle for the selected Moment.
 *
 * Angles are dial degrees, clockwise from 12 o'clock. The rim is a clock face: theta = (local wall-clock hours mod 12)
 * / 12 x 360, so 12:00 and 0:00 are both at the top and one hour is 30 degrees.
 *
 * The window runs from now to the leading edge, (12 h - gap) of WALL-CLOCK time ahead. Measuring the window on the
 * wall clock (not in elapsed seconds) keeps the gap behind the planet exactly gap x 30 degrees on a daylight-saving
 * change day too, so a Moment tomorrow morning can never be drawn on top of the planet; the planet itself jumps
 * 30 degrees at the change (accepted, Nov 22). Everything is a function of `now`: no timers, no tweens.
 */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define RIM_HALF_DAY_S   43200          /* 12 h: one turn of the rim */
#define RIM_REVEAL_S     (15 * 60)      /* a Moment fades in over the first 15 min after the edge reaches it */
#define RIM_TIP_DEG      7.5f           /* a span still cut by the edge: its last 15 min at 50% */
#define RIM_LOOKBACK_S   (12 * 3600)    /* lanes are assigned over Moments ending after now - 12 h ... */
#define RIM_LOOKAHEAD_S  (24 * 3600)    /* ... and starting before now + 24 h */
#define RIM_GAP_DEFAULT  60             /* minutes; the allowed set is 30, 60, 120, 180 */
#define RIM_MAX          64             /* inputs and marks */

/* sorted by start, ties by id (the caller). free: a FREE Moment (bus `free`: true): still on the rim, but not busy, so it
   takes no lane, is never an overlap and is drawn quieter (spec 5.1 "Free Moment"). */
typedef struct { time_t start, end; bool has_end, free; } rim_in_t;

enum { RIM_POINT = 1, RIM_SPAN = 2 };

typedef struct {
    uint8_t kind, lane;        /* lane 1 or 2 for a busy span, 0 for a point or a free span */
    bool free;                 /* a FREE Moment: drawn as an outline, off the lanes */
    uint8_t opa, tip_opa;      /* main part and tip opacity 0-255 (tip_opa 0: no tip) */
    float a0, a1;              /* POINT: a0 is the place. SPAN: the main arc a0..a1 (equal: no main part) */
    float t0, t1;              /* SPAN still cut by the edge: the tip arc t0..t1 */
    float rad;                 /* POINT: disc radius, 2 + 2p px */
    int16_t src;               /* index of the input */
} rim_mark_t;

typedef struct {
    time_t now;
    int gap_min;
    float planet;              /* theta(now), 0..360 */
    float edge;                /* planet + window degrees: unwrapped, so planet <= every mark angle <= edge */
    float gap_deg;             /* gap_min / 2 */
    bool midnight;             /* local midnight is inside the window ... */
    float midnight_deg;        /* ... here (unwrapped; always 0 mod 360) */
    int hidden;                /* spans in the window with no lane (a third overlap; not drawn) */
    int n;
    rim_mark_t m[RIM_MAX];
} rim_t;

int rim_gap_valid(int gap_min);                       /* 30, 60, 120 or 180 unchanged; anything else: 60 */
int64_t rim_wall(time_t t);                           /* local wall-clock seconds since 1970-01-01 00:00 local */
float rim_deg(time_t t);                              /* theta(t), 0..360 */
int32_t rim_window_s(int gap_min);                    /* 12 h - gap, seconds */
int64_t rim_wall_offset(time_t now, time_t t);        /* wall-clock seconds from now to t */
bool rim_in_window(time_t now, int gap_min, time_t t);   /* now <= t and t is no further than the edge */
float rim_window_deg(time_t now, int gap_min, time_t t); /* t's unwrapped angle, clamped to planet .. edge */
void rim_lanes(const rim_in_t *in, int n, time_t now, uint8_t *lane);   /* lane per input: 1, 2 or 0 */
void rim_build(rim_t *r, const rim_in_t *in, int n, time_t now, int gap_min);
bool rim_dot_in_gap(const rim_t *r, float deg);       /* a rim dot at deg lies inside the empty gap zone */
