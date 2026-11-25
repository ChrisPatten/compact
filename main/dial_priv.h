#pragma once
/* Internals shared by dial.c (rim, Moments, drawing primitives) and now_layout.c (the Now face's slot table).
   Not for other screens: they use dial.h. */
#include <stdbool.h>
#include <time.h>
#include "cJSON.h"
#include "lvgl.h"

#define C 233
#define PI_F 3.14159265f
#define TRACK 3   /* letter spacing: 0.16 em at 15-16 px */

/* Plum Dusk tokens (spec 3.1) */
#define COL_GLASS 0x3A2A41
#define COL_SHADE 0x0F0614
#define COL_HAIR  0x8B7098
#define COL_RIM   0x9E8AA3
#define COL_INK   0xF8ECE8
#define COL_INK2  0xCDB5C6
#define COL_ROSE  0xF5A99B
#define COL_AMBER 0xF7C95C
#define COL_PERI  0xA9B8FF
#define COL_NIGHT 0xB07C3A   /* the only ink on true-black screens */
#define COL_BLACK 0x000000

/* Everything the Now face's components read, computed once per redraw strip by dial.c. Strings point into dial.c
   state and are valid only during the draw. */
typedef struct {
    time_t now, d0;                  /* d0: local midnight today */
    struct tm lt;
    float hrs;                       /* local hours since midnight, fractional */
    bool has_next, has_running, running_has_end;
    time_t next_start, running_start, running_end, gap_from;
    const char *next_title, *running_title;
    /* FREE Moments (bus `free`: true; mom_logic.h): has_next/next_* may be a free one (next_free), which gets no
       countdown; has_running is only ever a busy one. busy_next_start (the upper arc's target) skips free ones,
       gap_from ignores them, and a running free one is has_free_running (it never reads busy). */
    bool next_free, has_busy_next, has_free_running;
    time_t busy_next_start, free_running_end;
    const char *free_running_title;
    int upcoming;                    /* Moments still ahead today */
    const char *link;                /* link word: PAIRED, PHONE, CONNECTING ... */
    bool link_up, phone;
    bool agent_busy;
    int needs_you;                   /* notification replies awaiting an answer (E105; 0 = none or the flag is off) */
    const char *status;              /* agent status text, may be empty */
    bool swap;                       /* the user tapped the numerals: countdown big, time small */
    time_t upper_target;             /* the winning upper gauge's target time, 0 = none; filled by now_draw */
    char upper_label[24], lower_label[24];   /* filled by now_draw from the winning gauges */
} now_ctx_t;

/* now_layout.c */
void now_draw(lv_layer_t *l, const now_ctx_t *c);
bool now_lower_step(void);           /* LVGL context: a tap on the lower arc; true if the face changed */
bool now_lower_tick(void);           /* LVGL timer: true when the 30 s tap hold ran out (face needs a redraw) */
bool now_lower_arc_hit(int x, int y);
void now_gauges_set(const cJSON *widgets);              /* the `widgets` resource: leave_by and weather payloads; display lock held */
void now_gauge_debug(const char *args, char *out, size_t n);   /* POST /debug/ui "gauge ..."; display lock held */

/* dial.c drawing primitives (strip-clipped) */
void dv_pt(float deg, float r, float *x, float *y);
void dv_disc(lv_layer_t *l, float x, float y, float r, uint32_t col, lv_opa_t opa);
void dv_arc(lv_layer_t *l, float r, float w, float from, float to, uint32_t col, lv_opa_t opa);
void dv_text_at(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, lv_opa_t opa, int x, int y_top);
void dv_text_c(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, int y_top);
int dv_text_w(const char *s, const lv_font_t *f, int space);
void dv_fit(char *s, size_t n, const lv_font_t *f, int space, int max_w);
int dv_fit_track(char *s, size_t n, const lv_font_t *f, int space, int max_w);   /* tighten tracking, then "..."; returns tracking */
void dv_caption(lv_layer_t *l, const char *s, const lv_font_t *f, uint32_t col, lv_opa_t opa, float r, bool top, int track);
int dv_cap_height(const lv_font_t *f);
void dv_fmt_clock(char *b, size_t n, time_t t);
void dv_seg(lv_layer_t *l, float x1, float y1, float x2, float y2, uint32_t col);   /* 2 px line, strip-clipped */
void dv_draw_icons(lv_layer_t *l);
int dv_icon_count(void);
