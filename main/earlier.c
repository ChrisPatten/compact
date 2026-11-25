#include "earlier.h"
#include "display.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "fonts.h"
#include "dial.h"
#include "decision.h"
#include "setscreen.h"
#include "thread.h"
#include "agents.h"
#include "dayview.h"
#include "prefs.h"

#define COL_RAISE 0x4A3752
#define COL_LINE  0x6B5575
#define COL_HAIR  0x8B7098
#define COL_INK   0xF8ECE8
#define COL_INK2  0xCDB5C6
#define COL_ROSE  0xF5A99B
#define COL_PERI  0xA9B8FF

#define MAX_E     24
#define KEEP_S    (24 * 3600)
#define ROWS      3
#define HOLD_MS   800
#define IDLE_MS   60000          /* spec 8.3: back to Now after 60 s untouched */

static const char *TAG = "earlier";

#define BODY_MAX  160            /* the item's body / detail text, kept for the detail view of an ended row */
#define RES_MAX   96             /* the reply resolution ("Booked Fri 10:30"), 60 characters at most from the bus */

typedef struct {
    char id[66], title[84], state[12];
    char body[BODY_MAX], resolution[RES_MAX];
    int kind, src;
    time_t at, expires;
} entry_t;                       /* 24 of these are about 10 KB of PSRAM (.bss is mapped there, main/linker.lf) */

static struct { char id[66]; time_t at; } s_gone[MAX_E];   /* rows cleared one by one: a repeated tombstone stays gone */
static int s_ngone;
static char s_det_id[66];        /* the ended row whose detail view is up ("" = the list) */

static entry_t s_e[MAX_E];       /* newest first */
static int s_n;
static time_t s_cleared_at;      /* HOLD TO CLEAR: ended rows up to here stay gone */
static lv_obj_t *s_tray;
static int s_fill_h;              /* HOLD TO CLEAR progress: screen filled from the bottom */
static bool s_open, s_dirty;
static int64_t s_hold_from;
static int s_shown;              /* rows on the glass */
static struct { int x, y, w; } s_row[ROWS];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool is_open_row(const entry_t *e) { return !strcmp(e->state, "open"); }

/* ---- the list ---- */

static void prune(void)
{
    time_t cut = time(NULL) - KEEP_S;
    int w = 0;
    for (int i = 0; i < s_n; i++) if (s_e[i].at >= cut || is_open_row(&s_e[i])) s_e[w++] = s_e[i];
    s_n = w;
}

static entry_t *find(const char *id)
{
    for (int i = 0; i < s_n; i++) if (!strcmp(s_e[i].id, id)) return &s_e[i];
    return NULL;
}

static void remove_at(int i)
{
    memmove(&s_e[i], &s_e[i + 1], sizeof(entry_t) * (s_n - i - 1));
    s_n--;
}

void earlier_note(const char *id, const char *title, int kind, const char *state, time_t at, int src, time_t expires)
{
    bool open = !strcmp(state, "open");
    if (!open && s_cleared_at && at <= s_cleared_at) return;     /* cleared already; a tombstone repeats */
    if (!open) for (int i = 0; i < s_ngone; i++) if (!strcmp(s_gone[i].id, id) && at <= s_gone[i].at) return;   /* cleared from its detail */
    entry_t *e = find(id);
    if (e && !strcmp(e->state, state) && (open || e->at == at)) return;
    char keep_body[BODY_MAX] = "", keep_res[RES_MAX] = "";             /* an open row's text carries over to how it ended */
    if (e) {
        snprintf(keep_body, sizeof keep_body, "%s", e->body);
        snprintf(keep_res, sizeof keep_res, "%s", e->resolution);
        remove_at(e - s_e);
    }
    if (s_n == MAX_E) s_n--;                                      /* the oldest falls off */
    memmove(&s_e[1], &s_e[0], sizeof(entry_t) * s_n);
    e = &s_e[0];
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->title, sizeof e->title, "%s", title);
    snprintf(e->state, sizeof e->state, "%s", state);
    memcpy(e->body, keep_body, sizeof keep_body);
    memcpy(e->resolution, keep_res, sizeof keep_res);
    e->kind = kind; e->src = src; e->at = at; e->expires = expires;
    s_n++;
    prune();
    s_dirty = true;
    ESP_LOGI(TAG, "%s: %s", id, state);
}

/* A string copy that never cuts a UTF-8 character in half. */
static void copy_utf8(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);
    if (len >= n) {
        len = n - 1;
        while (len && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

void earlier_note_text(const char *id, const char *body, const char *resolution)
{
    entry_t *e = find(id);
    if (!e) return;
    if (body && body[0] && strcmp(e->body, body)) { copy_utf8(e->body, sizeof e->body, body); s_dirty = true; }
    if (resolution && resolution[0] && strcmp(e->resolution, resolution)) { copy_utf8(e->resolution, sizeof e->resolution, resolution); s_dirty = true; }
}

void earlier_drop_open(const char *id)
{
    entry_t *e = find(id);
    if (e && is_open_row(e)) { remove_at(e - s_e); s_dirty = true; }
}

void earlier_settle(const cJSON *items, int src)
{
    time_t now = time(NULL);
    for (int i = 0; i < s_n; i++) {
        entry_t *e = &s_e[i];
        if (e->src != src || !is_open_row(e)) continue;
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(items, e->id);
        if (it && decision_weight(it, src == 1)) continue;          /* still open: still waiting */
        if (it && src == 0) continue;                               /* ended: its tombstone says how */
        snprintf(e->state, sizeof e->state, "%s", e->expires && e->expires <= now ? "expired" : "withdrawn");
        e->at = now;
        s_dirty = true;
    }
}

/* ---- drawing ---- */

static void strokes(lv_layer_t *l, const float *xy, int n, bool closed, float ox, float oy, float s, uint32_t col)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col); d.width = 2; d.round_start = 1; d.round_end = 1;
    for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        int j = (i + 1) % n;
        d.p1 = (lv_point_precise_t){ ox + xy[2 * i] * s, oy + xy[2 * i + 1] * s };
        d.p2 = (lv_point_precise_t){ ox + xy[2 * j] * s, oy + xy[2 * j + 1] * s };
        lv_draw_line(l, &d);
    }
}

/* Row glyphs (spec 5.9, 5.10), 26 px: waiting (open), urgent triangle, package, else the agent star. */
static void glyph(lv_layer_t *l, const entry_t *e, float x, float y)
{
    const float s = 26.0f / 24;
    if (is_open_row(e)) {
        dial_draw_ring(l, x + 13, y + 13, 9.75f * s, 2, COL_PERI);
        dial_draw_disc(l, x + 13, y + 13, 2.8f, COL_PERI);
    } else if (e->kind == 3) {
        static const float tri[] = { 12, 3, 22, 20, 2, 20 }, bar[] = { 12, 10, 12, 15 };
        strokes(l, tri, 3, true, x, y, s, COL_ROSE);
        strokes(l, bar, 2, false, x, y, s, COL_ROSE);
        dial_draw_disc(l, x + 12 * s, y + 17.5f * s, 1.2f, COL_ROSE);
    } else if (strstr(e->title, "PACKAGE") || strstr(e->title, "DELIVER") || strstr(e->title, "PARCEL")) {
        static const float box[] = { 3, 8, 12, 3, 21, 8, 21, 16, 12, 21, 3, 16 }, lid[] = { 3, 8, 12, 13, 21, 8 }, mid[] = { 12, 13, 12, 21 };
        strokes(l, box, 6, true, x, y, s, COL_INK);
        strokes(l, lid, 3, false, x, y, s, COL_INK);
        strokes(l, mid, 2, false, x, y, s, COL_INK);
    } else {
        static const float star[] = { 12, 3, 13.8f, 9.2f, 20, 11, 13.8f, 12.8f, 12, 19, 10.2f, 12.8f, 4, 11, 10.2f, 9.2f };
        strokes(l, star, 8, true, x, y, s, COL_INK);
    }
}

static void tray_draw(lv_event_t *ev)
{
    lv_layer_t *l = lv_event_get_layer(ev);
    dial_draw_base(l, COL_ROSE);
    dial_draw_top_caption(l, "EARLIER", COL_INK);
    dial_draw_hold_fill(l, s_fill_h, COL_ROSE, 90);
}

/* Glyphs go on top of the rows, so they are drawn after the children. */
static void tray_draw_post(lv_event_t *ev)
{
    lv_layer_t *l = lv_event_get_layer(ev);
    for (int i = 0; i < s_shown; i++) glyph(l, &s_e[i], s_row[i].x + 22, s_row[i].y + 16);
    if (s_det_id[0]) {                                              /* the detail view: the row's glyph above the title */
        const entry_t *e = find(s_det_id);
        if (e) glyph(l, e, 220, 84);
    }
}

/* ---- building ---- */

static void row_cb(lv_event_t *ev);
static void clear_cb(lv_event_t *ev);
static void build(void);
static void build_detail(const entry_t *e);

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t col, int x, int y, int w, int space)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, t);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(o, space, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(o, w, lv_font_get_line_height(f));            /* one line, cut with an ellipsis */
    lv_obj_set_pos(o, x, y);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

static void detail(char *b, size_t n, const entry_t *e)
{
    if (is_open_row(e)) { snprintf(b, n, "open \xC2\xB7 tap to act"); return; }
    struct tm lt; localtime_r(&e->at, &lt);
    int h = lt.tm_hour % 12; if (!h) h = 12;
    snprintf(b, n, "%d:%02d %s \xC2\xB7 %s", h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM", e->state);
}

/* HOLD TO CLEAR: ghost pill, rose wipe (the whole list, or the one entry in the detail view). */
static void clear_pill(void)
{
    lv_obj_t *b = lv_obj_create(s_tray);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, 138, 318);
    lv_obj_set_size(b, 190, 56);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_HAIR), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_ROSE), LV_STATE_PRESSED);
    lv_obj_set_style_border_post(b, true, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *t = label(b, "HOLD TO CLEAR", f_meta13, COL_INK, 0, 0, 170, 2);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(t);
    lv_obj_add_event_cb(b, clear_cb, LV_EVENT_ALL, NULL);
}

static void build(void)
{
    s_det_id[0] = 0;
    lv_obj_clean(s_tray);
    s_hold_from = 0;
    s_fill_h = 0;
    prune();
    s_dirty = false;
    s_shown = s_n < ROWS ? s_n : ROWS;
    static const int ys[ROWS] = { 96, 166, 236 };
    for (int i = 0; i < s_shown; i++) {
        const entry_t *e = &s_e[i];
        int x = i == 0 ? 100 : 90, w = i == 0 ? 266 : 286;           /* the top row narrows with the glass */
        s_row[i].x = x; s_row[i].y = ys[i]; s_row[i].w = w;
        lv_obj_t *r = lv_obj_create(s_tray);
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, x, ys[i]);
        lv_obj_set_size(r, w, 58);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(COL_RAISE), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        bool open = is_open_row(e);
        lv_obj_set_style_border_width(r, open ? 2 : 1, 0);         /* 1.5 px `line`; open rows keep 2 px `peri` */
        lv_obj_set_style_border_color(r, lv_color_hex(open ? COL_PERI : COL_LINE), 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);                  /* open rows reopen their Moment, ended rows open their detail */
        lv_obj_set_style_bg_color(r, lv_color_hex(COL_HAIR), LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char d[48]; detail(d, sizeof d, e);
        label(r, e->title, f_whisper, COL_INK, 62, 10, w - 62 - 22, 1);
        label(r, d, f_detail15, open ? COL_PERI : COL_INK2, 62, 29, w - 62 - 22, 0);
    }
    if (s_n > ROWS) {
        char m[24]; snprintf(m, sizeof m, "+%d MORE", s_n - ROWS);
        lv_obj_t *o = label(s_tray, m, f_meta13, COL_INK2, 133, 298, 200, 2);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    }
    bool ended = false;
    for (int i = 0; i < s_n; i++) if (!is_open_row(&s_e[i])) ended = true;
    if (!s_n) {
        lv_obj_t *o = label(s_tray, "NOTHING EARLIER", f_whisper, COL_INK2, 83, 222, 300, 3);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    } else if (ended) {                                             /* HOLD TO CLEAR: ghost pill, rose wipe */
        clear_pill();
    }
    lv_obj_invalidate(s_tray);
}


/* ---- detail view of an ended row (Nov 23) ---- */

/* A centred, wrapped label of at most `lines` lines (cut with an ellipsis), placed by its flex parent. */
static lv_obj_t *wrap_label(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t col, int w, int lines, int space)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, t);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(o, space, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_point_t sz; lv_text_get_size(&sz, t, f, space, 0, w, LV_TEXT_FLAG_NONE);
    int lh = lv_font_get_line_height(f), n = (sz.y + lh - 1) / lh;
    if (n < 1) n = 1;
    if (n > lines) n = lines;
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(o, w, n * lh);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

/* "EXPIRED \xC2\xB7 6:42 PM": how it ended and when (the state word is the bus's or the device's, upper-cased). */
static void ended_line(char *b, size_t n, const entry_t *e)
{
    char st[16]; size_t k = 0;
    for (; e->state[k] && k + 1 < sizeof st; k++) st[k] = e->state[k] >= 'a' && e->state[k] <= 'z' ? e->state[k] - 32 : e->state[k];
    st[k] = 0;
    struct tm lt; localtime_r(&e->at, &lt);
    int h = lt.tm_hour % 12; if (!h) h = 12;
    snprintf(b, n, "%s \xC2\xB7 %d:%02d %s", st, h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
}

/* The notice card's layout (spec 7.3 Inform: icon, title, line) read-only: glyph, title (17 caps), "STATE · TIME", the
   body when the item had one (none = no empty area), the reply resolution, then HOLD TO CLEAR for this entry alone. */
static void build_detail(const entry_t *e)
{
    lv_obj_clean(s_tray);
    s_hold_from = 0;
    s_fill_h = 0;
    s_dirty = false;
    s_shown = 0;
    snprintf(s_det_id, sizeof s_det_id, "%s", e->id);
    const int W = 340;
    lv_obj_t *c = lv_obj_create(s_tray);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    char t[100];
    wrap_label(c, e->title[0] ? e->title : "NOTICE", f_lab17, COL_INK, W, 2, 3);
    ended_line(t, sizeof t, e);
    wrap_label(c, t, f_whisper, COL_ROSE, W, 1, 2);
    if (e->body[0]) wrap_label(c, e->body, f_body, COL_INK2, W, e->resolution[0] ? 2 : 3, 0);
    if (e->resolution[0]) wrap_label(c, e->resolution, f_whisper, COL_INK, W, 2, 0);
    lv_obj_update_layout(c);
    int h = lv_obj_get_height(c), top = 120, room = 306 - top;       /* between the glyph and the pill */
    lv_obj_set_pos(c, (466 - W) / 2, top + (h < room ? (room - h) / 2 : 0));
    clear_pill();
    lv_obj_invalidate(s_tray);
    ESP_LOGI(TAG, "detail %s (%s%s)", e->id, e->state, e->body[0] ? ", body" : "");
}

static void close_detail(void)
{
    if (!s_det_id[0]) return;
    build();                                                        /* back to the list */
}

/* Clear this one entry (the detail's hold completed, or the debug hook), remember it so a repeated tombstone stays gone. */
static void clear_one(const char *id)
{
    entry_t *e = find(id);
    if (e) {
        if (s_ngone == MAX_E) { memmove(&s_gone[0], &s_gone[1], sizeof s_gone[0] * (MAX_E - 1)); s_ngone--; }
        snprintf(s_gone[s_ngone].id, sizeof s_gone[0].id, "%s", e->id);
        s_gone[s_ngone].at = e->at;
        s_ngone++;
        remove_at(e - s_e);
    }
    ESP_LOGI(TAG, "cleared %s", id);
}

static void open_detail_at(int i)
{
    if (i < 0 || i >= s_n || is_open_row(&s_e[i])) return;
    build_detail(&s_e[i]);
}

/* ---- open / close (LVGL context) ---- */

static void open_tray(void)
{
    if (s_open) return;
    s_open = true;
    build();
    lv_obj_remove_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "open (%d rows)", s_n);
}

static void close_tray(void)
{
    if (!s_open) return;
    s_open = false;
    s_det_id[0] = 0;
    s_hold_from = 0;
    lv_obj_add_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_tray);
    s_fill_h = 0;
    lv_obj_invalidate(lv_scr_act());
}

static void row_cb(lv_event_t *ev)
{
    int i = (int)(intptr_t)lv_event_get_user_data(ev);
    if (i >= s_shown || s_det_id[0]) return;
    if (compact_press_was_drag()) return;   /* LVGL 9.5 still clicks after a swipe or a drag */
    if (!is_open_row(&s_e[i])) { open_detail_at(i); return; }       /* an ended row: its read-only detail */
    char id[66]; snprintf(id, sizeof id, "%s", s_e[i].id);
    close_tray();
    remove_at(i);
    decision_reopen(id);                                            /* it takes the glass again */
}

static void clear_cb(lv_event_t *ev)
{
    lv_event_code_t c = lv_event_get_code(ev);
    if (c == LV_EVENT_PRESSED) s_hold_from = now_ms();
    else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && s_hold_from) {
        s_hold_from = 0;                                            /* released early: nothing cleared */
        dial_hold_fill_step(s_tray, &s_fill_h, 0);
    }
}

/* Vertical swipes (spec 8.1). Swipe up: on Now, the pending notice's card, else Earlier; nowhere
   else opens Earlier, so on the Day list (and Agents, Thread, a detail card) a vertical drag is only a scroll or nothing.
   Swipe down: a detail card dismisses (checked before Settings, so a swipe down on a card never opens Settings behind
   it); Now opens Settings; Earlier closes; elsewhere nothing. Swipe up on a detail card is ignored. */
static void route_vertical(lv_dir_t dir)
{
    if (dir == LV_DIR_TOP) {
        if (setscreen_is_open()) setscreen_swipe_up_lv();
        else if (s_open) return;
        else if (dayview_is_open() || agents_is_open() || thread_is_open()) return;   /* Earlier is Now only */
        else if (!decision_open_notice()) open_tray();              /* Now: the pending notice's card, else Earlier */
    } else if (dir == LV_DIR_BOTTOM) {
        if (s_open) { if (s_det_id[0]) close_detail(); else close_tray(); }   /* a detail goes back to the list first */
        else if (dayview_dismiss_lv()) return;                      /* a detail card: back where it came from */
        else if (setscreen_is_open() || dayview_is_open() || agents_is_open() || thread_is_open()) return;
        else setscreen_open_lv();                                   /* Now only */
    }
}

static void gesture_cb(lv_event_t *ev)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (decision_showing()) {                                       /* an alert owns the glass: only a swipe down, which dismisses it */
        if (dir == LV_DIR_BOTTOM) decision_swipe_down_lv();
        return;
    }
    /* Faces left to right Day / Now / Agents / Thread (horizontal). Settings: swipe down from
       Now only, swipe up closes. Earlier: swipe up from Now only (Nov 23), swipe down closes. A detail card or
       takeover: swipe down dismisses (Nov 23), swipe up does nothing. Day scrolls its list; Agents and Thread take no vertical swipes here. */
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        if (s_open || setscreen_is_open()) return;                  /* overlays take vertical swipes only */
        /* The faces right of Now take their own swipes before dayview, which takes right-from-Now (opens
           Day), left-from-Day (back to Now) and every step inside Event detail. Agents opens on a swipe
           left from Now and hands on to Thread; Thread (the last face) goes back to Agents. */
        if (agents_swipe_lv(dir)) return;
        if (thread_swipe_lv(dir)) return;
        if (dayview_swipe_lv(dir)) return;
        return;
    }
    route_vertical(dir);
}

/* POST /debug/ui "swipe up|down": the router's vertical branch, as the gesture would call it (no touch involved). */
bool earlier_debug_swipe(bool up)
{
    if (bsp_display_lock(2000) != ESP_OK) return false;
    lv_display_trigger_activity(NULL);                               /* a debug swipe counts as a touch: no rest, no 60 s close */
    if (decision_showing()) { if (!up) decision_swipe_down_lv(); }
    else route_vertical(up ? LV_DIR_TOP : LV_DIR_BOTTOM);
    bsp_display_unlock();
    return true;
}

static void tick_cb(lv_timer_t *t)
{
    if (!s_open) return;
    if (s_hold_from && compact_press_was_drag()) {                  /* it moved past the dead zone: a swipe, not a hold */
        s_hold_from = 0;
        dial_hold_fill_step(s_tray, &s_fill_h, 0);
    }
    if (s_hold_from) {
        int64_t ms = now_ms() - s_hold_from;
        dial_hold_fill_step(s_tray, &s_fill_h, ms >= HOLD_MS ? 466 : (int)(ms * 466 / HOLD_MS));
        if (ms >= HOLD_MS && s_det_id[0]) {                         /* the detail's one entry */
            s_hold_from = 0;
            char id[66]; snprintf(id, sizeof id, "%s", s_det_id);
            clear_one(id);
            build();
            return;
        }
        if (ms >= HOLD_MS) {                                        /* cleared: only open rows stay */
            s_hold_from = 0;
            int w = 0;
            for (int i = 0; i < s_n; i++) if (is_open_row(&s_e[i])) s_e[w++] = s_e[i];
            s_n = w;
            s_cleared_at = time(NULL);
            prefs_set_earlier_cleared((uint32_t)s_cleared_at);     /* survives a restart */
            build();
            return;
        }
    }
    if (lv_display_get_inactive_time(NULL) >= IDLE_MS) { close_tray(); return; }
    if (s_dirty && !s_hold_from) {
        const entry_t *e = s_det_id[0] ? find(s_det_id) : NULL;
        if (!s_det_id[0]) build();
        else if (e && !is_open_row(e)) build_detail(e);             /* its text or state changed under the detail */
        else build();                                               /* it is gone, or open again: back to the list */
    }
}

/* POST /debug/ui "earlier-test [off]" (seeds ended rows: an EXPIRED notice with a body, an ANSWERED one with a body and a
   resolution, a bodiless CLEARED one; "off" removes them), "earlier-row <n>" (what a tap on row n does; 0 = newest),
   "earlier-clear" (what the held clear on the open detail does). Any task. */
const char *earlier_debug(const char *arg)
{
    if (bsp_display_lock(2000) != ESP_OK) return "display busy\n";
    lv_display_trigger_activity(NULL);                               /* counts as a touch: no rest, no 60 s close */
    const char *r = "ok\n";
    if (!strncmp(arg, "test", 4)) {
        if (strstr(arg, "off")) {
            for (int i = s_n - 1; i >= 0; i--) if (!strncmp(s_e[i].id, "etest-", 6)) remove_at(i);
            s_ngone = 0;
        } else {
            time_t now = time(NULL);
            s_ngone = 0;
            s_cleared_at = 0;
            earlier_note("etest-3", "NOTICE TEST", 1, "cleared", now - 5400, 1, 0);
            earlier_note("etest-2", "DAVE ASKED TO MOVE THE 1:1", 2, "answered", now - 1800, 1, 0);
            earlier_note_text("etest-2", "Thursday 2:00 or Friday 10:30? He is free both.", "Booked Fri 10:30");
            earlier_note("etest-1", "PACKAGE DELIVERED", 1, "expired", now - 600, 1, 0);
            earlier_note_text("etest-1", "Front porch. Left by the carrier at 2:14 PM, signature not required.", NULL);
        }
        s_dirty = true;
    } else if (!strncmp(arg, "row", 3)) {
        int n = atoi(arg + 3);
        if (!s_open || s_det_id[0] || n < 0 || n >= s_shown || is_open_row(&s_e[n])) r = "no such ended row on the open list\n";
        else open_detail_at(n);
    } else if (!strncmp(arg, "clear", 5)) {
        if (!s_det_id[0]) r = "no detail open\n";
        else { char id[66]; snprintf(id, sizeof id, "%s", s_det_id); clear_one(id); build(); }
    } else r = "body: earlier-test [off] | earlier-row <n> | earlier-clear\n";
    bsp_display_unlock();
    return r;
}

/* ---- public ---- */

void earlier_init(void)
{
    s_cleared_at = (time_t)prefs_earlier_cleared();
    bsp_display_lock(-1);
    s_tray = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_tray);
    lv_obj_set_size(s_tray, 466, 466);
    lv_obj_center(s_tray);
    dial_use_ground(s_tray);
    lv_obj_set_style_bg_opa(s_tray, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_tray, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_tray, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_tray, tray_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(s_tray, tray_draw_post, LV_EVENT_DRAW_POST_END, NULL);
    lv_obj_add_event_cb(lv_scr_act(), gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_timer_create(tick_cb, 40, NULL);
    bsp_display_unlock();
}

void earlier_close(void)
{
    bsp_display_lock(-1);
    close_tray();
    bsp_display_unlock();
}

bool earlier_is_open(void) { return s_open; }
