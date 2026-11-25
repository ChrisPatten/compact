/* Touch classification: tap, hold or swipe. Pure logic, no LVGL, host tested (test/host/touch_logic_test.c).
 *
 * Rule: a tap or hold must not move. Up to TOUCH_DEADZONE_PX - 1 of travel in either axis, measured
 * from the press point, is touch jitter and is ignored. A press that ever reaches TOUCH_DEADZONE_PX of travel is a swipe
 * (a scroll or a face swipe) for the rest of that press: it can never become a tap or a hold again, even if the finger
 * comes back. Inside the dead zone nothing scrolls and no swipe fires. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TOUCH_DEADZONE_PX 10   /* the one constant: LVGL's scroll limit and every tap/hold cancel use it */
#define TOUCH_HOLD_MIN_MS 800  /* default hold-to-confirm time; callers pass their own */

typedef enum { TOUCH_TAP, TOUCH_HOLD, TOUCH_SWIPE } touch_class_t;

/* True when (x, y) is at or beyond the dead zone from the press point (x0, y0): either axis, Chebyshev distance. */
bool touch_beyond_deadzone(int x0, int y0, int x, int y);

/* Classify a press. `moved` latches an earlier excursion past the dead zone (pass the previous result's swipe state).
   SWIPE if moved or now beyond the dead zone; else HOLD once elapsed_ms >= hold_ms (hold_ms 0 = no hold on this
   target, so TAP); else TAP. */
touch_class_t touch_classify(int x0, int y0, int x, int y, uint32_t elapsed_ms, uint32_t hold_ms, bool moved);
