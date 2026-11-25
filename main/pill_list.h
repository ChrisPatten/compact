#pragma once
#include <stdbool.h>
#include "lvgl.h"

/*
 * pill_list: a vertically scrollable list of list-row pills (spec 5.8) with 40 px fades at the top and bottom
 * (spec 7.5). Used by Agents; Settings can adopt it (see README "Pill list"). All functions run in LVGL
 * context (display lock held). The list owns its rows; it is freed with its LVGL object (lv_obj_clean of
 * the parent), so drop your pointer then.
 *
 * Scrolling: vertical drag only, and only when the rows overflow the viewport. Horizontal swipes and the
 * gesture router (earlier.c) are untouched: rows and the list bubble gestures, and Agents/Settings take no
 * vertical swipes there.
 *
 * Fades and icons are drawn by the owner's screen: call pill_list_draw_post() from the screen's
 * LV_EVENT_DRAW_POST_END handler (after the children, so fades sit over the rows).
 */
typedef struct pill_list pill_list_t;

/* Draw a row icon whose top-left is (x, y); `style` is the row's style value. Clipped to the viewport. */
typedef void (*pill_icon_fn)(lv_layer_t *l, float x, float y, int style, void *ud);
typedef void (*pill_tap_fn)(int index, void *ud);   /* a tap, never a drag or swipe */

typedef enum {
    PILL_W_FIXED,   /* every row is row_w wide (the default: a consistent look) */
    PILL_W_CHORD,   /* row_w capped by the circle's chord at the row's resting position, never narrower than a row below it
                       would be at its own position; computed once at rest, it does not change while scrolling */
} pill_width_t;

typedef struct {
    int x, y, w, h;          /* viewport on the parent; rows are centred in it */
    int row_w, row_h, gap;   /* spec 5.8: 286 x 66; gap 8 */
    int icon;                /* icon box size (0: no icon; text starts at 22 + icon + 14) */
    pill_width_t width;
    pill_icon_fn icon_fn;    /* optional */
    pill_tap_fn tap;         /* optional; rows with .tappable set call it */
    void *ud;
} pill_list_cfg_t;

typedef struct {
    const char *title, *sub;     /* title (caps style) and the detail line; sub may be NULL */
    const char *value;           /* optional right-aligned value (Settings style row), NULL for none */
    uint32_t title_col, sub_col; /* 0 = ink / ink-2 */
    int style;                   /* passed to icon_fn */
    bool tappable;
    bool animated;               /* its icon changes every frame (a spinner): pill_list_tick() invalidates it */
} pill_row_t;

pill_list_t *pill_list_create(lv_obj_t *parent, const pill_list_cfg_t *cfg);
/* Add row `index` (== count) or update an existing one in place (scroll position kept). */
void pill_list_set(pill_list_t *p, int index, const pill_row_t *row);
void pill_list_trim(pill_list_t *p, int count);        /* delete rows from `count` on */
int  pill_list_count(const pill_list_t *p);
int  pill_list_scroll_y(const pill_list_t *p);         /* pixels scrolled down from the top */
void pill_list_scroll_to(pill_list_t *p, int y);       /* no animation */
void pill_list_scroll_by(pill_list_t *p, int dy);      /* animated (debug hook) */
bool pill_list_overflows(const pill_list_t *p);
lv_obj_t *pill_list_row_obj(const pill_list_t *p, int index);   /* the row's LVGL object (add your own press/release handlers; NULL if absent) */
bool pill_list_contains(const pill_list_t *p, const lv_obj_t *obj);   /* obj is the list, a row or a row's child */
void pill_list_tick(pill_list_t *p);                   /* call from a timer: animated icons */
void pill_list_draw_post(pill_list_t *p, lv_layer_t *l);

/* The 40 px top/bottom fades alone, for any scroll viewport `vp` (screen coordinates; the fade spans vp's x range, so
   keep it clear of the rim). Call from the owner's LV_EVENT_DRAW_POST_END; pass whether more content lies beyond. */
void pill_fades_draw(lv_layer_t *l, const lv_area_t *vp, bool top, bool bottom);
