#pragma once
#include <stdbool.h>

/*
 * Settings (spec 7.8 "Settings", 8.1-8.3; board 08/c). Swipe down from Now (only Now) opens it; swipe up,
 * PWR, sleep or 60 s untouched closes it (and saves). The left of the rim is the brightness slider (8 to
 * 10 o'clock, drag along it); rows below hold the rest: SLEEP AFTER, QUIET HOURS (tap flips it, ON / OFF),
 * FLASHLIGHT, WI-FI. The right of the rim is the volume slider.
 */
void setscreen_init(void);       /* once, after earlier_init() and before decision_init() */
void setscreen_close(void);      /* any task */
bool setscreen_is_open(void);
void setscreen_open_lv(void);    /* LVGL context (gesture handler) */
void setscreen_close_lv(void);
void setscreen_swipe_up_lv(void);  /* the router's swipe up: closes unless it was a scroll of the list */
bool setscreen_debug_scroll(int dy);   /* /debug/ui settings-scroll <dy> */
