#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "lvgl.h"

/*
 * Day list and Event detail (spec 7.2, 8.1-8.3; board 02). The Day face sits left of Now: swipe right
 * from Now opens the Day list (today's remaining Moments as a vertical scrolling list of rows, overlap notes and "N MIN FREE" gap rows, 40 px fades where more lies beyond);
 * swipe left goes back to Now. Tap a row, or the Moment title curved along the bottom of Now, for that
 * Moment's Event detail card (tapping the Now numeral swaps time and countdown instead).
 * In detail, swipe left / right (or tap a chevron) steps through every Moment of today in time order,
 * past ones included and marked as ended; the card shows title, time range and duration, place, travel /
 * leave-by, overlap, note, weight / state and its buttons (tap, or hold 0.8 s with the screen fill).
 * Swipe down dismisses the card back to where it was opened from (Day list or Now). PWR, sleep or 60 s
 * untouched closes it. Sits above the face and below Earlier, Settings and takeovers.
 */
void dayview_init(void);                     /* once, after dial_create() and before earlier_init() */
void dayview_on_events(const cJSON *events); /* `events` resource, id -> Event (any task) */
void dayview_on_action_result(const char *key, const char *status, const char *reason);   /* any task */
void dayview_close(void);                    /* any task: PWR press, sleep */
bool dayview_is_open(void);

/* LVGL context (gesture router): a horizontal swipe. Returns true when the Day face or detail took it. */
bool dayview_swipe_lv(lv_dir_t dir);
void dayview_close_lv(void);
bool dayview_is_day_list(void);                /* the Day list is showing (not Now, not a detail card) */
/* LVGL context: swipe down on a detail card (swipe up is ignored). Back to the Day list or Now, whichever opened it; false
   when no detail card is showing. */
bool dayview_dismiss_lv(void);
bool dayview_debug_detail(const char *id);     /* /debug/ui "detail [id]": open that (default the first listed) Moment's card (any task) */
bool dayview_debug_scroll(int dy);             /* /debug/ui "day-scroll <dy>" (any task) */
