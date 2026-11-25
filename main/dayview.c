/*
 * Day list and Event detail (spec 7.2; board 02 a-d). See dayview.h for the behaviour.
 *
 * One full-screen container on the active screen, created before Earlier and Settings so they (and
 * the decision takeover) cover it. Its draw callback paints the plum base, the rim marks, the curved
 * captions, the chevrons and the hold fill; the text and buttons are LVGL children. The Moments come
 * from the same `events` resource as the face, parsed here into a PSRAM list sorted by start.
 */
#include "dayview.h"
#include "display.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "fonts.h"
#include "dial.h"
#include "decision.h"
#include "power.h"
#include "overlay.h"
#include "intercom_ws.h"
#include "rim12.h"
#include "pill_list.h"
#include "gap_text.h"
#include "mom_logic.h"

/* Plum Dusk tokens (spec 3.1) */
#define COL_RAISE    0x4A3752
#define COL_HAIR     0x8B7098
#define COL_INK      0xF8ECE8
#define COL_INK2     0xCDB5C6
#define COL_ROSE     0xF5A99B
#define COL_ROSE_INK 0x2A1A2C
#define COL_AMBER    0xF7C95C
#define COL_PERI     0xA9B8FF

#define MAX_M       RIM_MAX      /* 64, as the face */
#define IDLE_MS     60000        /* spec 8.3: Day untouched 60 s -> Now */
#define HOLD_MS     800          /* spec 8.1 */
#define WAITING_MS  3000         /* spec 5.6 / 7.3: no ack in 3 s -> WAITING */
#define GIVE_UP_MS  15000        /* then TRY AGAIN (as decision.c) */
#define STEP_MS     240          /* spec 9: Step Moment */
#define STEP_DX     200          /* how far the cards travel during a step */
#define OPA_PAST    115          /* 45%: the selected Moment once it has ended (Event detail) */

static const char *TAG = "dayview";

typedef struct { char id[33], label[25]; bool hold, draft; } mact_t;

typedef struct {
    char id[66], title[96], place[72], note[240], state[12], other[48];
    time_t start, end, expires, leave_by;
    bool has_end;
    bool free;                  /* a FREE Moment (bus `free`: true): shown, but not busy (mom_logic.h) */
    int weight;                 /* decision_weight(): 0 ambient or ended, 1 notice, 2 decision, 3 urgent */
    int travel_min;
    char travel_word[12];
    int nact;
    mact_t act[3];
} mom_t;

enum { V_NONE, V_DAY, V_DETAIL };

static mom_t *s_m;              /* PSRAM: every timed Moment, sorted by start */
static int s_nm;
static mom_t s_sel;             /* the Moment on the detail card (a copy, so it survives a refresh) */
static bool s_sel_gone;         /* it left the resource while shown: marked WITHDRAWN, not in the steps */

static lv_obj_t *s_root, *s_card, *s_out;   /* the screen, the card, the card sliding out */
static int s_view;
static int s_detail_from;           /* V_NONE or V_DAY: where the open detail card came from */
static bool s_dirty;            /* new data while a hold was running: rebuild after */
static int s_last_min = -1, s_card_phase;
static char s_row_id[MAX_M + 1][66];        /* ids behind the Day list's tappable rows (slot MAX_M: "N EARLIER TODAY") */
static lv_obj_t *s_list;                    /* the Day list's scroll viewport (NULL when it is not built) */
static int s_day_scroll;                    /* its scroll position, kept across rebuilds and a visit to a detail card */

/* The Day list viewport (screen px): x 83..383 clear of the rim marks (the fades span its width), y 110 where the first
   row has always sat, to 380: the chord at 380 is 52..414, so the rows (x 93..373) stay inside the circle at rest and
   scrolled, and the Sunset caption (curved, below about y 396) stays clear. */
#define DAYL_X 83
#define DAYL_Y 110
#define DAYL_W 300
#define DAYL_H 270

/* a hold in progress, and the screen fill that shows it */
static struct { uint32_t from; int seg; } s_hold;
static int s_fill_h;

/* the button on the card: capsule (or pill) and its segments */
static lv_obj_t *s_cap, *s_seg[3], *s_seg_lab[3];
static int s_seg_act[3], s_nseg;

/* the tap in flight (one at a time) */
static struct {
    bool active, failed, done;
    char key[40], ev[66], aid[33], label[25];
    int64_t at;
    char word[16];
} s_act;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---- text ---- */

/* Keep case; drop what the fonts cannot draw (emoji, symbols from U+2190 up), as decision.c does. */
static void clean(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && o + 4 < n) {
        int len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
        uint32_t cp = len == 1 ? *p : len == 2 ? ((p[0] & 0x1F) << 6) | (p[1] & 0x3F)
                    : len == 3 ? ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F) : 0x1F000;
        for (int i = 1; i < len; i++) if (!p[i]) { len = i; cp = 0x1F000; break; }
        if (cp == '\n' || cp == '\r' || cp == '\t') { if (o && dst[o - 1] != ' ') dst[o++] = ' '; p += len; continue; }
        if (cp < 0x2190 && cp != 0x200D && !(cp >= 0xFE00 && cp <= 0xFE0F)) { memcpy(dst + o, p, len); o += len; }
        p += len;
    }
    dst[o] = 0;
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void hm(char *b, size_t n, time_t t)
{
    struct tm lt; localtime_r(&t, &lt);
    snprintf(b, n, "%d:%02d", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min);
}

static bool is_pm(time_t t) { struct tm lt; localtime_r(&t, &lt); return lt.tm_hour >= 12; }

static void hm_ampm(char *b, size_t n, time_t t)
{
    char x[12]; hm(x, sizeof x, t);
    snprintf(b, n, "%s %s", x, is_pm(t) ? "PM" : "AM");
}

static int text_w(const char *t, const lv_font_t *f, int space)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* A label clamped to `lines` lines of `line_h` px, cut with an ellipsis (spec 10). */
static lv_obj_t *text(lv_obj_t *par, const char *t, const lv_font_t *f, uint32_t col, int x, int y, int w,
                      int line_h, int lines, int space, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(par);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(l, space, 0);
    lv_obj_set_style_text_align(l, align, 0);
    if (line_h) lv_obj_set_style_text_line_space(l, line_h - lv_font_get_line_height(f), 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, (line_h ? line_h : lv_font_get_line_height(f)) * lines);
    lv_obj_set_pos(l, x, y);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

/* How many lines `t` wraps to in `w` px. */
static int lines_of(const char *t, const lv_font_t *f, int space, int w)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, space, 0, w, LV_TEXT_FLAG_NONE);
    int lh = lv_font_get_line_height(f);
    return lh ? (sz.y + lh / 2) / lh : 1;
}

/* ---- small drawn glyphs the fonts lack: the diamond (spec 3.1 amber always travels with one) and the check ---- */

enum { IC_DIAMOND, IC_CHECK };

static void icon_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *l = lv_event_get_layer(e);
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_color_t col = lv_obj_get_style_bg_color(o, 0);
    int w = lv_area_get_width(&a), cx = a.x1 + w / 2, cy = a.y1 + lv_area_get_height(&a) / 2, h = w / 2;
    if ((int)(intptr_t)lv_event_get_user_data(e) == IC_DIAMOND) {
        lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
        d.color = col; d.opa = LV_OPA_COVER;
        d.p[0] = (lv_point_precise_t){ cx, cy - h }; d.p[1] = (lv_point_precise_t){ cx + h, cy }; d.p[2] = (lv_point_precise_t){ cx - h, cy };
        lv_draw_triangle(l, &d);
        d.p[0] = (lv_point_precise_t){ cx - h, cy }; d.p[1] = (lv_point_precise_t){ cx + h, cy }; d.p[2] = (lv_point_precise_t){ cx, cy + h };
        lv_draw_triangle(l, &d);
    } else {                                                    /* check  M5 12.5l4.5 4.5L19 7.5 on 24 px */
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
        d.color = col; d.width = 2; d.round_start = 1; d.round_end = 1;
        float s = w / 24.0f;
        d.p1 = (lv_point_precise_t){ a.x1 + 5 * s, a.y1 + 12.5f * s }; d.p2 = (lv_point_precise_t){ a.x1 + 9.5f * s, a.y1 + 17 * s };
        lv_draw_line(l, &d);
        d.p1 = d.p2; d.p2 = (lv_point_precise_t){ a.x1 + 19 * s, a.y1 + 7.5f * s };
        lv_draw_line(l, &d);
    }
}

static lv_obj_t *icon(lv_obj_t *par, int kind, uint32_t col, int x, int y, int size)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);        /* carries the colour; bg itself stays transparent */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(o, icon_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)kind);
    return o;
}

/* ---- the Moment list ---- */

static void day_bounds(time_t now, time_t *d0, time_t *d1)
{
    struct tm lt; localtime_r(&now, &lt);
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    *d0 = mktime(&lt);
    lt.tm_mday += 1; lt.tm_isdst = -1;
    *d1 = mktime(&lt);                                          /* a DST day is 23 or 25 h */
}

static time_t end_of(const mom_t *m) { return m->has_end ? m->end : m->start; }

/* Today's Moments (start within today), in start order, as indices into s_m. */
static int today(int *idx)
{
    time_t now = dial_now(), d0, d1;
    day_bounds(now, &d0, &d1);
    int n = 0;
    for (int i = 0; i < s_nm; i++) if (s_m[i].start >= d0 && s_m[i].start < d1) idx[n++] = i;
    return n;
}

/* The Moments Event detail steps through: today's, plus any later one the rim window already shows (tomorrow
   morning, late in the evening), in start order. */
static int listed(int *idx)
{
    time_t now = dial_now(), d0, d1;
    day_bounds(now, &d0, &d1);
    int n = 0;
    for (int i = 0; i < s_nm; i++)
        if ((s_m[i].start >= d0 && s_m[i].start < d1) || (s_m[i].start > now && dial_in_window(s_m[i].start))) idx[n++] = i;
    return n;
}

static rim_in_t rin(const mom_t *m) { return (rim_in_t){ m->start, m->end, m->has_end, m->free }; }

/* Minutes two Moments overlap; 0 when either is free (a free Moment is no conflict). */
static int overlap_min(const mom_t *a, const mom_t *b)
{
    rim_in_t x = rin(a), y = rin(b);
    return mom_overlap_min(&x, &y);
}

static int by_start(const void *a, const void *b)
{
    const mom_t *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return strcmp(x->id, y->id);
}

static void parse_one(mom_t *m, const cJSON *e, const char *id)
{
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "%s", id);
    dial_caps(m->title, sizeof m->title, str(e, "title") ? str(e, "title") : "");
    m->start = dial_parse_iso(str(e, "at"));
    m->end = str(e, "end") ? dial_parse_iso(str(e, "end")) : 0;
    m->has_end = m->end > m->start;
    if (!m->has_end) m->end = m->start;
    m->free = dial_moment_is_free(e);
    m->expires = str(e, "expires_at") ? dial_parse_iso(str(e, "expires_at")) : 0;
    snprintf(m->state, sizeof m->state, "%s", str(e, "state") ? str(e, "state") : "open");
    m->weight = decision_weight(e, false);
    const cJSON *det = cJSON_GetObjectItem(e, "detail");
    const char *place = det ? str(det, "location") : NULL;
    const char *note = det && str(det, "description") ? str(det, "description") : str(e, "body");
    if (place) dial_caps(m->place, sizeof m->place, place);
    if (note) clean(m->note, sizeof m->note, note);
    const cJSON *tr = cJSON_GetObjectItem(e, "travel"), *mins = tr ? cJSON_GetObjectItem(tr, "minutes") : NULL;
    if (cJSON_IsNumber(mins) && mins->valueint > 0) {
        m->travel_min = mins->valueint;
        const char *mode = str(tr, "mode");
        snprintf(m->travel_word, sizeof m->travel_word, "%s", !mode ? "TRAVEL" : !strcmp(mode, "walk") ? "WALK" :
                 !strcmp(mode, "drive") ? "DRIVE" : !strcmp(mode, "bike") ? "BIKE" : !strcmp(mode, "train") ? "TRAIN" :
                 !strcmp(mode, "transit") ? "TRANSIT" : "TRAVEL");
        m->leave_by = str(tr, "leave_by") ? dial_parse_iso(str(tr, "leave_by")) : m->start - m->travel_min * 60;
    }
    const cJSON *acts = cJSON_GetObjectItem(e, "actions"), *a;
    cJSON_ArrayForEach(a, acts) {
        if (m->nact >= 3) break;
        const char *aid = str(a, "id"), *lab = str(a, "label"), *k = str(a, "kind");
        if (!aid || !lab) continue;
        mact_t *x = &m->act[m->nact];
        snprintf(x->id, sizeof x->id, "%s", aid);
        dial_caps(x->label, sizeof x->label, lab);
        if (strlen(x->label) > 10) { ESP_LOGW(TAG, "label cut: %s", x->label); x->label[10] = 0; }   /* spec 10: no ellipsis */
        const cJSON *cf = cJSON_GetObjectItem(a, "confirm");
        x->draft = cf && str(cf, "draft") && str(cf, "draft")[0];
        x->hold = k ? !strcmp(k, "hold") : x->draft;
        m->nact++;
    }
}

/* The first other Moment it overlaps: "VENDOR CALL, 3:00" (spec 7.2 conflict row). */
static void find_overlap(mom_t *m)
{
    m->other[0] = 0;
    for (int i = 0; i < s_nm; i++) {
        const mom_t *o = &s_m[i];
        if (!strcmp(o->id, m->id) || !overlap_min(m, o)) continue;
        char t[12], name[30];
        hm(t, sizeof t, o->start);
        snprintf(name, sizeof name, "%.29s", o->title);
        if (strlen(o->title) > 22) { name[22] = 0; while (strlen(name) && name[strlen(name) - 1] == ' ') name[strlen(name) - 1] = 0; strcat(name, "..."); }
        snprintf(m->other, sizeof m->other, "%s, %s", name, t);
        return;
    }
}

static const mom_t *find_id(const char *id)
{
    for (int i = 0; i < s_nm; i++) if (!strcmp(s_m[i].id, id)) return &s_m[i];
    return NULL;
}

/* ---- drawing (the container's draw callback) ---- */

/* Event detail: only the selected Moment, unwindowed, at its own place on the 12-hour rim (spec 5.1 "Selected
   Moment"): its arc 4 px on its lane (rose), or a diamond for a point; 45% once it has ended. Its lane comes from the
   same rule as the face (rim12.c rim_lanes over this list). */
static void draw_selected(lv_layer_t *l, const mom_t *sel)
{
    time_t now = dial_now();
    static rim_in_t in[MAX_M];                                  /* LVGL task only; statics live in PSRAM */
    static uint8_t lane[MAX_M];
    int at = -1;
    for (int i = 0; i < s_nm; i++) {
        in[i] = rin(&s_m[i]);
        if (!strcmp(s_m[i].id, sel->id)) at = i;
    }
    rim_lanes(in, s_nm, now, lane);
    bool past = end_of(sel) <= now && sel->start <= now;
    float a0 = dial_clock_deg(sel->start);
    if (sel->has_end) {
        float len = (float)(rim_wall(sel->end) - rim_wall(sel->start)) / 120.0f;   /* wall clock, as the rim */
        if (len > 359) len = 359;
        if (len < 1) len = 1;
        float r = sel->free ? DIAL_FREE_R : at >= 0 && lane[at] == 2 ? DIAL_LANE2_R : DIAL_LANE1_R;
        float w = sel->free ? 3 : 4;                            /* a free one keeps the outline's place, a touch thicker */
        if (past) dial_draw_arc_opa(l, r, w, a0, a0 + len, COL_ROSE, OPA_PAST);
        else dial_draw_arc(l, r, w, a0, a0 + len, COL_ROSE);
    } else if (sel->free) {                                     /* a free point: the diamond's outline */
        float s = sinf(a0 * 3.14159265f / 180), c = cosf(a0 * 3.14159265f / 180);
        int cx = (int)lroundf(233 + 214 * s), cy = (int)lroundf(233 - 214 * c), h = 8;
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
        d.color = lv_color_hex(COL_ROSE); d.opa = past ? OPA_PAST : LV_OPA_COVER; d.width = 2; d.round_start = 1; d.round_end = 1;
        const int px[5] = { cx, cx + h, cx, cx - h, cx }, py[5] = { cy - h, cy, cy + h, cy, cy - h };
        for (int k = 0; k < 4; k++) {
            d.p1 = (lv_point_precise_t){ px[k], py[k] }; d.p2 = (lv_point_precise_t){ px[k + 1], py[k + 1] };
            lv_draw_line(l, &d);
        }
    } else {                                                    /* no end, in detail: a diamond (spec 5.1) */
        float s = sinf(a0 * 3.14159265f / 180), c = cosf(a0 * 3.14159265f / 180);
        int cx = (int)lroundf(233 + 214 * s), cy = (int)lroundf(233 - 214 * c), h = 8;
        lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
        d.color = lv_color_hex(COL_ROSE); d.opa = past ? OPA_PAST : LV_OPA_COVER;
        d.p[0] = (lv_point_precise_t){ cx, cy - h }; d.p[1] = (lv_point_precise_t){ cx + h, cy }; d.p[2] = (lv_point_precise_t){ cx - h, cy };
        lv_draw_triangle(l, &d);
        d.p[0] = (lv_point_precise_t){ cx - h, cy }; d.p[1] = (lv_point_precise_t){ cx + h, cy }; d.p[2] = (lv_point_precise_t){ cx, cy + h };
        lv_draw_triangle(l, &d);
    }
}

static void chevron(lv_layer_t *l, bool left)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(COL_INK2); d.width = 3; d.round_start = 1; d.round_end = 1;
    int x0 = left ? 42 : 424, x1 = left ? 33 : 433;             /* M42 222 L33 233 L42 244 and its mirror */
    d.p1 = (lv_point_precise_t){ x0, 222 }; d.p2 = (lv_point_precise_t){ x1, 233 }; lv_draw_line(l, &d);
    d.p1 = d.p2; d.p2 = (lv_point_precise_t){ x0, 244 }; lv_draw_line(l, &d);
}

/* Position of the shown Moment among today's: its index, or where it would sit if it is gone. */
static void neighbours(bool *has_prev, bool *has_next)
{
    int idx[MAX_M], n = listed(idx), before = 0, after = 0;
    for (int i = 0; i < n; i++) {
        const mom_t *m = &s_m[idx[i]];
        if (!strcmp(m->id, s_sel.id)) continue;
        if (by_start(m, &s_sel) < 0) before++; else after++;
    }
    *has_prev = before > 0; *has_next = after > 0;
}

/* The top caption of a card: when it is, its weight while open, its state once ended. */
static void card_caption(char *b, size_t n, uint32_t *col)
{
    time_t now = dial_now();
    const mom_t *m = &s_sel;
    char when[40], t[16];
    *col = COL_ROSE;
    if (s_sel_gone) { snprintf(b, n, "WITHDRAWN"); *col = COL_INK2; return; }
    if (m->has_end && m->start <= now && m->end > now) {
        int left = (int)((m->end - now + 59) / 60);
        if (left < 120) snprintf(when, sizeof when, "NOW \xC2\xB7 %d MIN LEFT", left);
        else snprintf(when, sizeof when, "NOW \xC2\xB7 UNTIL %s", (hm_ampm(t, sizeof t, m->end), t));
    } else if (m->start > now) {
        int min = (int)((m->start - now + 59) / 60);
        if (min < 120) snprintf(when, sizeof when, "IN %d MIN", min);
        else snprintf(when, sizeof when, "IN %d H %d MIN", min / 60, min % 60);
    } else {
        *col = COL_INK2;
        if (m->has_end) { hm_ampm(t, sizeof t, m->end); snprintf(when, sizeof when, "ENDED %s", t); }
        else snprintf(when, sizeof when, "EARLIER TODAY");
    }
    bool free_up = false;
    if (m->free && *col != COL_INK2) {                          /* free: no countdown, "FREE \xC2\xB7 7:28\xE2\x80\x93" the span instead */
        char a[16], z[16];
        hm(a, sizeof a, m->start);
        if (m->has_end) { hm(z, sizeof z, m->end); snprintf(when, sizeof when, "FREE \xC2\xB7 %.10s\xE2\x80\x93%.10s", a, z); }
        else snprintf(when, sizeof when, "FREE \xC2\xB7 %.10s", a);
        free_up = true;
    } else if (m->free) {
        char w2[40]; snprintf(w2, sizeof w2, "FREE \xC2\xB7 %.30s", when); snprintf(when, sizeof when, "%s", w2);
    }
    bool open = !strcmp(m->state, "open");
    if (!open) { char st[16]; dial_caps(st, sizeof st, m->state); snprintf(b, n, "%s \xC2\xB7 %s", st, when); *col = COL_INK2; }
    else if (m->weight == 3 && *col != COL_INK2) snprintf(b, n, "URGENT \xC2\xB7 %s", when);
    else if (m->weight == 2 && *col != COL_INK2) { snprintf(b, n, "NEEDS YOU \xC2\xB7 %s", when); *col = COL_PERI; }
    else snprintf(b, n, "%s", when);
    if (free_up && *col == COL_ROSE) *col = COL_INK2;           /* a free caption is quieter than a busy one's rose */
}

static void day_caption(char *top, size_t n1, char *bot, size_t n2)
{
    static const char *wd[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
    static const char *mo[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    time_t now = dial_now();
    struct tm lt; localtime_r(&now, &lt);
    snprintf(top, n1, "%s \xC2\xB7 %d %s", wd[lt.tm_wday], lt.tm_mday, mo[lt.tm_mon]);
    float rise, set, h = lt.tm_hour + lt.tm_min / 60.0f;
    bot[0] = 0;
    if (dial_sun_hours(now, &rise, &set)) {
        float x = h < set ? set : rise;
        if (h >= set) dial_sun_hours(now + 86400, &rise, &set), x = rise;
        int hh = (int)x, mm = (int)((x - hh) * 60 + 0.5f);
        if (mm == 60) { hh++; mm = 0; }
        snprintf(bot, n2, "%s %d:%02d %s", h < set ? "SUNSET" : "SUNRISE", hh % 12 ? hh % 12 : 12, mm, hh >= 12 ? "PM" : "AM");
    }
}

static void root_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    dial_draw_base(l, COL_ROSE);                                /* crescent, rim, now dot and moon */
    if (dial_now() < 1700000000) return;
    char top[64], bot[64];
    if (s_view == V_DAY) {
        dial_draw_rim_marks(l);                                 /* the rim window, exactly as Now draws it (spec 7.2) */
        day_caption(top, sizeof top, bot, sizeof bot);
        dial_draw_top_caption(l, top, COL_INK);
        if (bot[0]) dial_draw_bottom_caption(l, bot, COL_INK2);
    } else if (s_view == V_DETAIL) {
        if (!s_sel_gone) draw_selected(l, &s_sel);              /* only the selected Moment, 4 px */
        bool p, n; neighbours(&p, &n);
        if (p) chevron(l, true);
        if (n) chevron(l, false);
        uint32_t col;
        card_caption(top, sizeof top, &col);
        dial_draw_top_caption(l, top, col);
    }
    dial_draw_hold_fill(l, s_fill_h, COL_ROSE, 90);             /* rose 35% on plum (spec 8.1) */
}

/* The Day list's 40 px fades, over the rows and only on a side with more beyond (spec 7.5; as the pill lists). */
static void root_draw_post(lv_event_t *e)
{
    if (s_view != V_DAY || !s_list) return;
    lv_area_t vp; lv_obj_get_coords(s_list, &vp);
    pill_fades_draw(lv_event_get_layer(e), &vp, lv_obj_get_scroll_top(s_list) > 0, lv_obj_get_scroll_bottom(s_list) > 0);
}

/* ---- building: the Day list ---- */

static void open_detail(const char *id, int dir);

static bool was_swipe(void) { return compact_press_was_drag(); }

static void day_tap_cb(lv_event_t *e)
{
    if (was_swipe()) return;                                    /* LVGL clicks after a gesture too */
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i <= MAX_M && s_row_id[i][0]) open_detail(s_row_id[i], 0);
}

static lv_obj_t *tappable(lv_obj_t *par, int slot, int x, int y, int w, int h, const char *id)
{
    lv_obj_t *r = lv_obj_create(par);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_RAISE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a swipe on a row still reaches the router */
    lv_obj_set_ext_click_area(r, 6);
    snprintf(s_row_id[slot], sizeof s_row_id[slot], "%s", id);
    lv_obj_add_event_cb(r, day_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
    return r;
}

/* The dotted gap row: a Whisper "45 MIN FREE" with a 2 px dotted `hair` rule each side, drawn by the row itself so it
   scrolls and clips with the list. user data is the text width. */
static void gap_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *l = lv_event_get_layer(e);
    lv_area_t a; lv_obj_get_coords(o, &a);
    int w = (int)(intptr_t)lv_event_get_user_data(e), y = a.y1 + 8;
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(COL_HAIR); d.width = 2; d.dash_width = 2; d.dash_gap = 2;
    d.p1 = (lv_point_precise_t){ 93, y }; d.p2 = (lv_point_precise_t){ 233 - w / 2 - 12, y }; lv_draw_line(l, &d);
    d.p1 = (lv_point_precise_t){ 233 + w / 2 + 12, y }; d.p2 = (lv_point_precise_t){ 373, y }; lv_draw_line(l, &d);
}

static void list_scroll_cb(lv_event_t *e)
{
    s_day_scroll = (int)lv_obj_get_scroll_top(s_list);
    lv_area_t a = { DAYL_X, DAYL_Y, DAYL_X + DAYL_W, DAYL_Y + DAYL_H };        /* the fades follow the scroll */
    lv_obj_invalidate_area(s_root, &a);
}

/* The Day list scrolls: every remaining Moment, with its overlap note and the free gaps, in one
   vertical list. Rows, gap rows and notes keep the old look; a rebuild keeps the scroll position. */
static void build_day(void)
{
    lv_obj_clean(s_root);
    s_card = s_out = s_list = NULL;
    s_nseg = 0; s_cap = NULL;
    memset(s_row_id, 0, sizeof s_row_id);
    time_t now = dial_now();
    int idx[MAX_M], n = today(idx), rem[MAX_M], nr = 0, past = 0;
    for (int i = 0; i < n; i++) { if (end_of(&s_m[idx[i]]) >= now) rem[nr++] = idx[i]; else past++; }
    if (!nr) {                                                  /* spec 10: nothing (more) today */
        text(s_root, n ? "NOTHING MORE TODAY" : "NOTHING ON TODAY", f_label15, COL_INK2, 33, 220, 400, 0, 1, 2, LV_TEXT_ALIGN_CENTER);
        if (past) {                                             /* the day so far stays reachable */
            char b[32]; snprintf(b, sizeof b, "%d EARLIER TODAY", past);
            lv_obj_t *r = tappable(s_root, MAX_M, 113, 248, 240, 32, s_m[idx[n - 1]].id);
            text(r, b, f_whisper, COL_INK2, 0, 8, 240, 0, 1, 2, LV_TEXT_ALIGN_CENTER);
        }
        lv_obj_invalidate(s_root);
        return;
    }
    lv_obj_t *L = s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(L);
    lv_obj_set_pos(L, DAYL_X, DAYL_Y);
    lv_obj_set_size(L, DAYL_W, DAYL_H);
    lv_obj_set_style_pad_bottom(L, 8, 0);
    lv_obj_set_scroll_dir(L, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(L, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(L, LV_OBJ_FLAG_SCROLL_ELASTIC);          /* no bounce past the ends */
    lv_obj_add_flag(L, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    const mom_t *next = NULL;
    for (int i = 0; i < nr && !next; i++) if (s_m[rem[i]].start > now && !s_m[rem[i]].free) next = &s_m[rem[i]];   /* rose: the next busy Moment */
    /* rows and spacing as the Phase prototype (board 02a: rows at 110, 200, 272); y is on the screen, the list is at (DAYL_X, DAYL_Y) */
    int y = DAYL_Y, prev = -1;                                  /* prev kind: 0 row, 1 note, 2 gap */
    time_t max_end = 0;
    static const int after_row[] = { 16, 18, 14 }, after_note[] = { 12, 0, 12 }, after_gap[] = { 10, 0, 0 };
    for (int i = 0; i < nr; i++) {
        const mom_t *m = &s_m[rem[i]];
        int lines = lines_of(m->title, f_label, 1, 174) > 1 ? 2 : 1, h = lines > 1 ? 44 : 32;
        y += prev < 0 ? 0 : prev == 0 ? after_row[0] : prev == 1 ? after_note[0] : after_gap[0];
        uint32_t col = m == next ? COL_ROSE : m->free ? COL_INK2 : COL_INK;   /* a free row is quieter */
        lv_obj_t *r = tappable(L, i, 93 - DAYL_X, y - DAYL_Y, 280, h, m->id);
        char t[12]; hm(t, sizeof t, m->start);
        text(r, t, f_num30, col, 0, 0, 92, 0, 1, 0, LV_TEXT_ALIGN_RIGHT);
        text(r, m->title, f_label, col, 106, 6, 174, 19, 2, 1, LV_TEXT_ALIGN_LEFT);
        y += h; prev = 0;
        if (!m->free && end_of(m) > max_end) max_end = end_of(m);   /* a free Moment does not interrupt free time */
        int ov = 0;                                             /* "◆ OVERLAPS 15 MIN" under its row */
        for (int k = 0; k < nr && !ov; k++) {
            const mom_t *o = &s_m[rem[k]];
            if (o != m && o->start >= m->start) ov = overlap_min(m, o);
        }
        if (ov) {
            y += after_row[1];
            char b[32]; snprintf(b, sizeof b, "OVERLAPS %d MIN", ov);
            icon(L, IC_DIAMOND, COL_AMBER, 199 - DAYL_X, y - DAYL_Y + 4, 8);
            text(L, b, f_whisper, COL_AMBER, 213 - DAYL_X, y - DAYL_Y, 170, 0, 1, 1, LV_TEXT_ALIGN_LEFT);
            y += 16; prev = 1;
        } else if (m->free) {                                   /* the FREE tag, on the overlap note's line, under the title */
            y += after_row[1];
            text(L, "FREE", f_whisper, COL_INK2, 199 - DAYL_X, y - DAYL_Y, 170, 0, 1, 1, LV_TEXT_ALIGN_LEFT);
            y += 16; prev = 1;
        }
        if (i + 1 < nr) {                                       /* a free gap of 30 min or more */
            rim_in_t nx = rin(&s_m[rem[i + 1]]);
            int free = mom_gap_min(&nx, max_end);               /* 0 before a free row: the gap row sits before the next busy one */
            if (free >= 30) {
                y += prev == 0 ? after_row[2] : after_note[2];
                char b[24];
                gap_free_text(b, sizeof b, free);
                int w = text_w(b, f_whisper, 1);
                lv_obj_t *g = lv_obj_create(L);
                lv_obj_remove_style_all(g);
                lv_obj_set_pos(g, 0, y - DAYL_Y);
                lv_obj_set_size(g, DAYL_W, 16);
                lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(g, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
                lv_obj_add_event_cb(g, gap_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)w);
                text(g, b, f_whisper, COL_INK2, 0, 0, DAYL_W, 0, 1, 1, LV_TEXT_ALIGN_CENTER);
                y += 16; prev = 2;
            }
        }
    }
    lv_obj_add_event_cb(L, list_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_update_layout(L);
    lv_obj_scroll_to_y(L, s_day_scroll, LV_ANIM_OFF);          /* clamped to the new length */
    s_day_scroll = (int)lv_obj_get_scroll_top(L);
    lv_obj_invalidate(s_root);
}

/* POST /debug/ui "detail [id]": open Event detail for the Moment `id` (default: the first listed), as a row tap would (no touch involved). */
bool dayview_debug_detail(const char *id)
{
    bsp_display_lock(-1);
    int idx[MAX_M], n = listed(idx);
    bool ok = n > 0 && s_view != V_DETAIL && (!id || !*id || find_id(id));
    if (ok) open_detail(id && *id ? id : s_m[idx[0]].id, 0);
    bsp_display_unlock();
    return ok;
}

/* POST /debug/ui "day-scroll <dy>": scroll the Day list by dy px (animated, clamped; dy > 0 reveals lower rows). */
bool dayview_debug_scroll(int dy)
{
    bsp_display_lock(-1);
    bool ok = s_view == V_DAY && s_list;
    if (ok) {
        int top = (int)lv_obj_get_scroll_top(s_list), bot = (int)lv_obj_get_scroll_bottom(s_list);
        if (dy > bot) dy = bot;
        if (dy < -top) dy = -top;
        lv_obj_scroll_by(s_list, 0, -dy, LV_ANIM_ON);
    }
    bsp_display_unlock();
    return ok;
}

/* ---- building: the Event detail card ---- */

static void seg_cb(lv_event_t *e);

/* Remove buttons that can no longer do anything: the Moment ended, expired or was withdrawn (spec 5.6). */
static bool actionable(const mom_t *m)
{
    if (s_sel_gone || strcmp(m->state, "open")) return false;
    return !(m->expires && m->expires <= time(NULL));
}

/* Spec 5.6: one action is a 220 x 56 rose pill; two or three share a 296 x 60 capsule, first segment rose. */
static void build_actions(lv_obj_t *card, const mom_t *m, int y_capsule, int y_pill)
{
    s_nseg = 0; s_cap = NULL;
    if (!actionable(m)) return;
    int ids[3], n = 0;
    for (int i = 0; i < m->nact; i++) {
        if (m->act[i].draft && m->weight < 2) continue;         /* a draft needs the decision's confirm screen */
        ids[n++] = i;
    }
    if (!n) return;
    bool pill = n == 1;
    int W = pill ? 220 : 296, H = pill ? 56 : 60;
    lv_obj_t *cap = lv_obj_create(card);
    lv_obj_remove_style_all(cap);
    lv_obj_set_pos(cap, 233 - W / 2, pill ? y_pill : y_capsule);
    lv_obj_set_size(cap, W, H);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(cap, true, 0);
    lv_obj_set_style_border_width(cap, pill ? 0 : 2, 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(COL_HAIR), 0);
    lv_obj_set_style_border_post(cap, true, 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_EVENT_BUBBLE);
    s_cap = cap;
    const lv_font_t *f = pill ? f_label15 : f_whisper;
    int lw[3], sum = 0;
    for (int i = 0; i < n; i++) { lw[i] = text_w(m->act[ids[i]].label, f, 1) + 28; sum += lw[i]; }
    int x = 0;
    for (int i = 0; i < n; i++) {
        int w = i < n - 1 ? lw[i] * W / sum : W - x;
        lv_obj_t *seg = lv_obj_create(cap);
        lv_obj_remove_style_all(seg);
        lv_obj_set_pos(seg, x, 0);
        lv_obj_set_size(seg, w, H);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_EVENT_BUBBLE);         /* swipes on a button still step */
        if (pill) lv_obj_set_ext_click_area(seg, 8);
        bool primary = i == 0;
        if (primary) {
            lv_obj_set_style_bg_color(seg, lv_color_hex(COL_ROSE), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        }
        if (!m->act[ids[i]].hold) {                             /* pressed: invert to ink (spec 5.6) */
            lv_obj_set_style_bg_color(seg, lv_color_hex(COL_INK), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_STATE_PRESSED);
        }                                                       /* a hold stays still: the screen fills */
        if (i > 0) {
            lv_obj_set_style_border_side(seg, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(seg, 2, 0);
            lv_obj_set_style_border_color(seg, lv_color_hex(COL_HAIR), 0);
        }
        lv_obj_t *l = lv_label_create(seg);
        lv_label_set_text(l, m->act[ids[i]].label);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(primary ? COL_ROSE_INK : COL_INK), 0);
        if (!m->act[ids[i]].hold) lv_obj_set_style_text_color(l, lv_color_hex(COL_ROSE_INK), LV_STATE_PRESSED);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_width(l, w - 8);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_center(l);
        lv_obj_add_event_cb(seg, seg_cb, LV_EVENT_ALL, (void *)(intptr_t)s_nseg);
        s_seg[s_nseg] = seg; s_seg_lab[s_nseg] = l; s_seg_act[s_nseg] = ids[i]; s_nseg++;
        x += w;
    }
}

/* A centred 14 px caps row (place, travel, conflict); a diamond leads the conflict row. */
static void info_row(lv_obj_t *card, const char *t, uint32_t col, int y, int space, bool diamond)
{
    int max = 346 - (diamond ? 14 : 0), w = text_w(t, f_whisper, space);
    if (w > max) w = max;
    int x = 233 - (w + (diamond ? 14 : 0)) / 2;
    if (diamond) { icon(card, IC_DIAMOND, col, x, y + 4, 8); x += 14; }
    text(card, t, f_whisper, col, x, y, w + 2, 0, 1, space, LV_TEXT_ALIGN_LEFT);
}

/* 0 future, 1 running, 2 past: the card is rebuilt when this changes. */
static int phase_of(const mom_t *m, time_t now)
{
    if (m->start > now) return 0;
    return m->has_end && m->end > now ? 1 : 2;
}

static lv_obj_t *s_applied_lab;
static lv_obj_t *s_title;                                       /* the detail card's title label, placed by place_title() */
static char s_title_cap[64];                                    /* the caption it was placed against */
static int s_title_floor;                                       /* y of the top of the time digits below it */

/* Ink height above the baseline of a digit or capital in `f`. */
static int ink_above(const lv_font_t *f, uint32_t ch)
{
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(f, &g, ch, 0) ? g.box_h + g.ofs_y : lv_font_get_line_height(f) * 2 / 3;
}

/* Spec 7.2: the title block (1 to 3 lines) sits centred, ink to ink, between the lowest point of the curved caption
   (over the title's own width) and the top of the time digits. A caption that dips low (long state text) can leave
   room for fewer lines; the title then clamps to what fits, at least one, with an ellipsis. Run again whenever the
   caption text changes (its countdown). */
static void place_title(void)
{
    if (!s_title) return;
    char cap[64]; uint32_t col;
    card_caption(cap, sizeof cap, &col);
    snprintf(s_title_cap, sizeof s_title_cap, "%s", cap);
    const lv_font_t *f = f_lab17;
    const int pitch = 21, gapmin = 6;
    int lh = lv_font_get_line_height(f), capH = ink_above(f, 'H');
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(s_title), f, 1, 0, 300, LV_TEXT_FLAG_NONE);
    int want = (sz.y + lh / 2) / lh, half_w = sz.x / 2 + 4;
    if (want < 1) want = 1;
    if (want > 3) want = 3;
    int bottom = dial_top_caption_bottom(cap, half_w), n = want, top = 0;
    for (; n >= 1; n--) {
        int room = s_title_floor - bottom - (capH + (n - 1) * pitch);
        top = bottom + room / 2;
        if (room / 2 >= gapmin || n == 1) break;
    }
    lv_obj_set_height(s_title, pitch * n);
    lv_obj_set_y(s_title, top + capH - lh + f->base_line);     /* first baseline = y + lh - base_line */
    lv_obj_invalidate(s_root);
}

/* Per minute: the caption's countdown can change its length, so re-place the title under it. */
static void title_refresh(void)
{
    if (!s_title || s_view != V_DETAIL) return;
    char cap[64]; uint32_t col;
    card_caption(cap, sizeof cap, &col);
    if (strcmp(cap, s_title_cap)) place_title();
}

static lv_obj_t *build_card(void)
{
    s_applied_lab = NULL;
    s_title = NULL;
    const mom_t *m = &s_sel;
    time_t now = dial_now();
    s_card_phase = phase_of(m, now);
    bool past = s_card_phase == 2 || s_sel_gone;
    lv_obj_t *card = lv_obj_create(s_root);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 466, 466);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_move_to_index(card, 0);                              /* under the step zones */

    char range[32], sub[40], t1[12], t2[12];
    hm(t1, sizeof t1, m->start);
    const char *ap = is_pm(m->start) ? "PM" : "AM";
    if (m->has_end) {
        hm(t2, sizeof t2, m->end);
        snprintf(range, sizeof range, "%s\xE2\x80\x93%s", t1, t2);   /* en dash */
        int dur = (int)((m->end - m->start + 30) / 60);
        char d[16];
        if (dur >= 60 && dur % 60 == 0) snprintf(d, sizeof d, "%d H", dur / 60); else snprintf(d, sizeof d, "%d MIN", dur);
        if (is_pm(m->start) != is_pm(m->end)) snprintf(sub, sizeof sub, "%s\xE2\x80\x93%s \xC2\xB7 %s", ap, is_pm(m->end) ? "PM" : "AM", d);
        else snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", ap, d);
    } else {
        snprintf(range, sizeof range, "%s", t1);
        snprintf(sub, sizeof sub, "%s", ap);
    }
    char travel[64] = "", conflict[80] = "";
    if (m->travel_min > 0) {
        if (m->leave_by && m->leave_by > m->start - 6 * 3600) {
            char lb[12]; hm(lb, sizeof lb, m->leave_by);
            snprintf(travel, sizeof travel, "LEAVE %.11s \xC2\xB7 %d MIN %.11s", lb, m->travel_min, m->travel_word);
        } else snprintf(travel, sizeof travel, "%d MIN %.11s", m->travel_min, m->travel_word);
    }
    if (m->other[0]) snprintf(conflict, sizeof conflict, "OVERLAPS %.47s", m->other);
    uint32_t tcol = past ? COL_INK2 : COL_INK, rcol = past || m->free ? COL_INK2 : COL_ROSE;   /* a free time is ink-2, not the busy rose */

    bool sparse = !m->has_end && !m->place[0] && !travel[0] && !conflict[0];
    if (sparse) {                                               /* spec 7.2 "Sparse event", board 02d */
        s_title = text(card, m->title, f_lab17, tcol, 83, 96, 300, 21, 3, 1, LV_TEXT_ALIGN_CENTER);
        s_title_floor = 176 + lv_font_get_line_height(f_num64) - f_num64->base_line - ink_above(f_num64, '0');
        place_title();
        text(card, range, f_num64, rcol, 33, 176, 400, 68, 1, -1, LV_TEXT_ALIGN_CENTER);
        text(card, sub, f_whisper, COL_INK2, 33, 246, 400, 0, 1, 2, LV_TEXT_ALIGN_CENTER);
        if (m->note[0]) text(card, m->note, f_note17, past ? COL_INK2 : COL_INK, 96, 272, 274, 23, 2, 0, LV_TEXT_ALIGN_CENTER);
        build_actions(card, m, 322, 334);
    } else {                                                    /* spec 7.2 Event detail table, board 02b */
        s_title = text(card, m->title, f_lab17, tcol, 83, 92, 300, 21, 3, 1, LV_TEXT_ALIGN_CENTER);
        s_title_floor = 166 + lv_font_get_line_height(f_num36) - f_num36->base_line - ink_above(f_num36, '0');
        place_title();
        text(card, range, f_num36, rcol, 33, 166, 400, 40, 1, -1, LV_TEXT_ALIGN_CENTER);
        text(card, sub, f_whisper, COL_INK2, 33, 208, 400, 0, 1, 2, LV_TEXT_ALIGN_CENTER);
        int y = 238;                                            /* missing rows collapse upward */
        if (m->place[0]) { info_row(card, m->place, past ? COL_INK2 : COL_INK, y, 1, false); y += 22; }
        if (travel[0]) { info_row(card, travel, COL_INK2, y, 1, false); y += 22; }
        if (conflict[0]) { info_row(card, conflict, COL_AMBER, y, 1, true); y += 22; }
        if (m->note[0] && y <= 282)                             /* the note fills what is left: 2 lines, else 1 */
            text(card, m->note, f_detail15, COL_INK2, 96, y + 2, 274, 20, y <= 238 ? 2 : 1, 0, LV_TEXT_ALIGN_CENTER);
        build_actions(card, m, 322, 324);
    }
    return card;
}

static void apply_act_state(void);

static void step_zone_cb(lv_event_t *e)
{
    if (was_swipe()) return;
    bool prev = (int)(intptr_t)lv_event_get_user_data(e) == 0;
    dayview_swipe_lv(prev ? LV_DIR_RIGHT : LV_DIR_LEFT);
}

static void step_zone(int x, int which)
{
    lv_obj_t *z = lv_obj_create(s_root);                        /* spec 5.8: 56 x 120 at 9 and 3 o'clock */
    lv_obj_remove_style_all(z);
    lv_obj_set_pos(z, x, 173);
    lv_obj_set_size(z, 56, 120);
    lv_obj_remove_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(z, step_zone_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
}

static void kill_anims(void)
{
    if (s_out) { lv_anim_delete(s_out, NULL); lv_obj_delete(s_out); s_out = NULL; }
    if (s_card) { lv_anim_delete(s_card, NULL); lv_obj_set_style_translate_x(s_card, 0, 0); }
}

/* Build the detail screen for s_sel with no transition. */
static void build_detail(void)
{
    lv_obj_clean(s_root);
    s_card = s_out = s_list = NULL;
    memset(&s_hold, 0, sizeof s_hold);
    s_fill_h = 0;
    s_card = build_card();
    step_zone(8, 0);
    step_zone(402, 1);
    apply_act_state();
    lv_obj_invalidate(s_root);
}

static void anim_x(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void anim_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void out_done(lv_anim_t *a)
{
    if (a->var == s_out) s_out = NULL;
    lv_obj_delete(a->var);                                      /* the anim is already off LVGL's list */
}
static void anim_inval(lv_anim_t *a) { (void)a; lv_obj_invalidate(s_root); }

static void start_anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, STEP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);
    lv_anim_set_bezier3_param(&a, LV_BEZIER_VAL_FLOAT(0.2), 0, 0, LV_BEZIER_VAL_FLOAT(1));   /* cubic-bezier(.2, 0, 0, 1) */
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

/* Show `id` on the card. dir: 0 no transition, +1 came from the right (next), -1 from the left. */
static void open_detail(const char *id, int dir)
{
    const mom_t *m = find_id(id);
    if (!m) return;
    if (s_hold.from) { s_hold.from = 0; dial_hold_fill_step(s_root, &s_fill_h, 0); }   /* a swipe, not a hold */
    s_sel = *m;
    s_sel_gone = false;
    find_overlap(&s_sel);
    bool was_detail = s_view == V_DETAIL && s_card;
    if (s_view != V_DETAIL) s_detail_from = s_view;             /* a swipe down returns here */
    s_view = V_DETAIL;
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    if (!dir || !was_detail) { build_detail(); ESP_LOGI(TAG, "detail %s", id); return; }
    /* spec 7.2 stepping: the incoming card slides along the swipe axis, the outgoing one fades to 35% */
    kill_anims();
    lv_obj_t *old = s_card;
    lv_obj_remove_flag(old, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < s_nseg; i++) lv_obj_remove_flag(s_seg[i], LV_OBJ_FLAG_CLICKABLE);
    memset(&s_hold, 0, sizeof s_hold);
    if (s_fill_h) dial_hold_fill_step(s_root, &s_fill_h, 0);
    s_card = build_card();
    apply_act_state();
    s_out = old;
    lv_obj_move_to_index(old, 1);
    start_anim(s_card, anim_x, dir * STEP_DX, 0, anim_inval);
    start_anim(old, anim_x, 0, -dir * STEP_DX, NULL);
    start_anim(old, anim_opa, LV_OPA_COVER, 89 /* 35% */, out_done);
    lv_obj_invalidate(s_root);                                  /* the rim hands its thick arc over */
}

/* ---- the button on the card ---- */

static void fire(int a)
{
    const mact_t *x = &s_sel.act[a];
    if (x->draft) {                                             /* "sends exactly this" lives on the decision screen */
        char id[66]; snprintf(id, sizeof id, "%s", s_sel.id);
        dayview_close_lv();
        decision_reopen(id);
        return;
    }
    bool retry = s_act.failed && !strcmp(s_act.ev, s_sel.id) && !strcmp(s_act.aid, x->id) && s_act.key[0];
    if (!retry) {
        uint32_t r1 = esp_random(), r2 = esp_random();
        snprintf(s_act.key, sizeof s_act.key, "compact-%08lx%08lx", (unsigned long)r1, (unsigned long)r2);
    }
    s_act.active = true; s_act.failed = false; s_act.done = false; s_act.word[0] = 0;
    s_act.at = now_ms();
    snprintf(s_act.ev, sizeof s_act.ev, "%s", s_sel.id);
    snprintf(s_act.aid, sizeof s_act.aid, "%s", x->id);
    snprintf(s_act.label, sizeof s_act.label, "%s", x->label);
    char object[80]; snprintf(object, sizeof object, "events:%s", s_sel.id);
    if (!intercom_send_action(object, x->id, NULL, s_act.key)) { s_act.failed = true; ESP_LOGW(TAG, "action not sent"); }
    apply_act_state();
}

/* Spec 5.6 states on the button that was used: WAITING, SENT ✓ (rose), TRY AGAIN (amber). */
static const char *s_applied_word;

static void apply_act_state(void)
{
    if (!s_act.ev[0] || strcmp(s_act.ev, s_sel.id)) return;
    int seg = -1;
    for (int i = 0; i < s_nseg; i++) if (!strcmp(s_sel.act[s_seg_act[i]].id, s_act.aid)) seg = i;
    if (seg < 0) return;
    const char *word = s_act.done ? "SENT" : s_act.failed ? (s_act.word[0] ? s_act.word : "TRY AGAIN")
                     : now_ms() - s_act.at >= WAITING_MS ? "WAITING" : NULL;
    lv_obj_t *l = s_seg_lab[seg], *b = s_seg[seg];
    if (l == s_applied_lab && word == s_applied_word) return;   /* nothing new: no restyle, no redraw */
    s_applied_lab = l; s_applied_word = word;
    bool primary = seg == 0, pill = s_nseg == 1;
    if (!word) {                                                /* fired again: back to the button as built */
        lv_label_set_text(l, s_sel.act[s_seg_act[seg]].label);
        lv_obj_set_style_text_font(l, pill ? f_label15 : f_whisper, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(primary ? COL_ROSE_INK : COL_INK), 0);
        lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_cap, pill ? 0 : 2, 0);
        lv_obj_set_style_border_color(s_cap, lv_color_hex(COL_HAIR), 0);
        return;
    }
    uint32_t col = s_act.done ? COL_ROSE : s_act.failed ? COL_AMBER : COL_INK;
    lv_label_set_text(l, word);
    const lv_font_t *f = pill ? f_label15 : f_whisper;
    if (text_w(word, f, 1) + (s_act.done ? 22 : 0) > lv_obj_get_width(b) - 10) f = f_meta13;   /* a narrow segment */
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);               /* the state reads on plum, not on the rose fill */
    lv_obj_set_style_border_width(s_cap, 2, 0);
    lv_obj_set_style_border_color(s_cap, lv_color_hex(s_act.done ? COL_ROSE : s_act.failed ? COL_AMBER : COL_HAIR), 0);
    if (s_act.done && !lv_obj_get_child(b, 1)) {                /* SENT + a drawn check (the font has no ✓) */
        int w = text_w(word, f, 1);
        lv_obj_align(l, LV_ALIGN_CENTER, -9, 0);
        lv_obj_t *c = icon(b, IC_CHECK, COL_ROSE, 0, 0, 16);
        lv_obj_align(c, LV_ALIGN_CENTER, w / 2 + 3, 0);
    }
}

static void seg_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    int seg = (int)(intptr_t)lv_event_get_user_data(e);
    if (seg >= s_nseg || s_out) return;
    int a = s_seg_act[seg];
    bool busy = s_act.active && !s_act.failed && !s_act.done && !strcmp(s_act.ev, s_sel.id);
    bool sent = (s_act.done || !strcmp(s_act.word, "EXPIRED")) && !strcmp(s_act.ev, s_sel.id) && !strcmp(s_act.aid, s_sel.act[a].id);
    if (s_sel.act[a].hold && !s_sel.act[a].draft) {
        if (c == LV_EVENT_PRESSED && !busy && !sent) { s_hold.seg = seg; s_hold.from = lv_tick_get() ? lv_tick_get() : 1; }
        else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && s_hold.from) {
            s_hold.from = 0;                                    /* released early: nothing sent */
            dial_hold_fill_step(s_root, &s_fill_h, 0);
        }
        return;
    }
    if (c != LV_EVENT_CLICKED || was_swipe() || busy || sent) return;
    fire(a);
}

/* ---- open / close / refresh ---- */

static void open_day(bool keep_scroll)
{
    s_view = V_DAY;
    if (!keep_scroll) s_day_scroll = 0;                         /* from Now: start at the next Moment */
    build_day();
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "day list");
}

void dayview_close_lv(void)
{
    if (s_view == V_NONE) return;
    s_view = V_NONE;
    if (s_out) { lv_anim_delete(s_out, NULL); s_out = NULL; }
    if (s_card) lv_anim_delete(s_card, NULL);
    lv_obj_clean(s_root);
    s_card = NULL; s_list = NULL; s_nseg = 0; s_cap = NULL;
    memset(&s_hold, 0, sizeof s_hold);
    s_fill_h = 0;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_scr_act());
}

void dayview_close(void)
{
    bsp_display_lock(-1);
    dayview_close_lv();
    bsp_display_unlock();
}

bool dayview_is_open(void) { return s_view != V_NONE; }
bool dayview_is_day_list(void) { return s_view == V_DAY; }

bool dayview_dismiss_lv(void)
{
    if (s_view != V_DETAIL) return false;
    int from = s_detail_from;
    dayview_close_lv();
    if (from == V_DAY) open_day(true);                          /* the list as it was left */
    ESP_LOGI(TAG, "detail dismissed to %s", from == V_DAY ? "the Day list" : "Now");
    return true;
}

bool dayview_swipe_lv(lv_dir_t dir)
{
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return false;
    if (s_view == V_NONE) {                                     /* spec 8.1: swipe right from Now = Day */
        if (dir != LV_DIR_RIGHT || power_state() == PWR_REST || power_state() == PWR_SLEEP) return false;
        open_day(false);
        return true;
    }
    if (s_view == V_DAY) {                                      /* the face to the right of Day is Now */
        if (dir == LV_DIR_LEFT) dayview_close_lv();
        return true;
    }
    /* detail: swipe left = next Moment, right = previous (spec 8.1); the ends stay put */
    int idx[MAX_M], n = listed(idx), pick = -1;
    if (dir == LV_DIR_LEFT) { for (int i = 0; i < n && pick < 0; i++) if (by_start(&s_m[idx[i]], &s_sel) > 0) pick = idx[i]; }
    else for (int i = n - 1; i >= 0 && pick < 0; i--) if (by_start(&s_m[idx[i]], &s_sel) < 0 && strcmp(s_m[idx[i]].id, s_sel.id)) pick = idx[i];
    if (pick >= 0) open_detail(s_m[pick].id, dir == LV_DIR_LEFT ? 1 : -1);
    return true;
}

/* Taps on Now (replaces spec 8.1's "Now numeral -> detail"):
   - the numeral band swaps the big time and the smaller countdown (no-op without a next Moment);
   - the Moment title curved along the bottom (r 182, under the lower arc) opens that Moment's detail.
     It is the one the face names there: the next Moment, else the running one with the soonest end. */
static void face_tap_cb(lv_event_t *e)
{
    if (was_swipe() || s_view != V_NONE || decision_showing()) return;
    if (power_state() != PWR_AWAKE && power_state() != PWR_DIM) return;
    lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
    if (dial_bolt_hit(p.x, p.y)) { overlay_charging(CHG_TAP); return; }   /* the bolt: the charging screen */
    int dx = p.x - 233, dy = p.y - 233, d2 = dx * dx + dy * dy;
    if (now_crown_needs_shown() && dy < -110 && d2 >= 150 * 150 && d2 <= 228 * 228) {   /* the top caption "N NEEDS YOU": open the oldest */
        decision_open_needs();
        return;
    }
    if (dial_lower_arc_tap(p.x, p.y)) return;                   /* the lower arc: next gauge; before the numeral band, which it overlaps at the ends */
    if (p.x >= 83 && p.x <= 383 && p.y >= 140 && p.y <= 310) { dial_toggle_numeral(); return; }
    if (dy < 110 || d2 < 150 * 150 || d2 > 228 * 228) return;   /* not the bottom caption band */
    time_t now = dial_now();
    int idx[MAX_M], n = listed(idx), pick = -1;                 /* the face names the window's next Moment */
    for (int i = 0; i < n && pick < 0; i++) if (s_m[idx[i]].start > now && dial_in_window(s_m[idx[i]].start)) pick = idx[i];
    for (int i = 0; i < n && pick < 0; i++) {                  /* running: started, not ended; soonest end */
        const mom_t *m = &s_m[idx[i]];
        if (m->start <= now && end_of(m) > now) {
            pick = idx[i];
            for (int j = i + 1; j < n; j++)
                if (s_m[idx[j]].start <= now && end_of(&s_m[idx[j]]) > now && end_of(&s_m[idx[j]]) < end_of(&s_m[pick])) pick = idx[j];
        }
    }
    if (pick >= 0) open_detail(s_m[pick].id, 0);
}

/* New data: rebuild what is showing; a card whose Moment left the resource stays, marked WITHDRAWN. */
static void refresh(void)
{
    if (s_view == V_DAY) { build_day(); return; }
    if (s_view != V_DETAIL) return;
    const mom_t *m = find_id(s_sel.id);
    if (m) { s_sel = *m; s_sel_gone = false; find_overlap(&s_sel); }
    else s_sel_gone = true;
    kill_anims();
    build_detail();
}

static void tick_cb(lv_timer_t *t)
{
    if (s_view == V_NONE) return;
    if (lv_display_get_inactive_time(NULL) >= IDLE_MS || power_state() == PWR_REST || power_state() == PWR_SLEEP) {
        dayview_close_lv();                                     /* spec 8.3: back to Now */
        return;
    }
    if (s_hold.from) {
        if (was_swipe()) { s_hold.from = 0; dial_hold_fill_step(s_root, &s_fill_h, 0); return; }
        uint32_t ms = lv_tick_elaps(s_hold.from);
        dial_hold_fill_step(s_root, &s_fill_h, ms >= HOLD_MS ? 466 : (int)(ms * 466 / HOLD_MS));
        if (ms >= HOLD_MS) {
            int seg = s_hold.seg;
            s_hold.from = 0;
            dial_hold_fill_step(s_root, &s_fill_h, 0);
            if (seg < s_nseg) fire(s_seg_act[seg]);
        }
        return;
    }
    if (s_out) return;                                          /* mid-step */
    time_t now = dial_now();
    struct tm lt; localtime_r(&now, &lt);
    bool minute = lt.tm_min != s_last_min;
    s_last_min = lt.tm_min;
    bool touching = s_view == V_DAY && (lv_display_get_inactive_time(NULL) < 400 || lv_indev_get_state(lv_indev_get_next(NULL)) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(lv_indev_get_next(NULL)));
    if (touching) { if (minute) s_last_min = -1; return; }      /* never rebuild the list under a finger or its fling: retry next tick */
    if (s_dirty) { s_dirty = false; refresh(); return; }
    if (s_view == V_DAY && minute) { build_day(); return; }     /* the next row and "free" gap move on */
    if (s_view == V_DETAIL) {
        if (minute && !s_sel_gone && phase_of(&s_sel, now) != s_card_phase) { refresh(); return; }
        if (minute) title_refresh();
        if (s_act.active && !s_act.done && !s_act.failed && !strcmp(s_act.ev, s_sel.id)) {
            if (now_ms() - s_act.at >= GIVE_UP_MS) s_act.failed = true;
            apply_act_state();
        }
    }
}

void dayview_on_events(const cJSON *events)
{
    if (!s_m) return;
    bsp_display_lock(-1);
    s_nm = 0;
    time_t now = dial_now(), d0, d1, keep_from = now - RIM_LOOKBACK_S;
    day_bounds(now, &d0, &d1);
    if (d0 < keep_from) keep_from = d0;                         /* today's list and the lane lookback; older is dropped */
    const cJSON *e;
    cJSON_ArrayForEach(e, events) {
        if (s_nm >= MAX_M || !e->string) continue;
        const char *kind = str(e, "kind"), *st = str(e, "state");
        if (kind && strcmp(kind, "event")) continue;            /* approvals have no time */
        if (st && !strcmp(st, "withdrawn")) continue;           /* not happening: off the rim and the steps */
        if (!str(e, "at")) continue;
        mom_t *m = &s_m[s_nm];
        parse_one(m, e, e->string);
        if (!m->start || (m->has_end && m->end - m->start >= 86400)) continue;   /* as the face: all-day holds */
        if (end_of(m) < keep_from) continue;
        s_nm++;
    }
    qsort(s_m, s_nm, sizeof s_m[0], by_start);
    if (s_view != V_NONE) {
        if (s_hold.from || s_out) s_dirty = true;               /* never pull a card from under a finger */
        else if (s_view == V_DAY) s_dirty = true;               /* the list is rebuilt by the tick, never mid-drag */
        else refresh();
    }
    bsp_display_unlock();
}

void dayview_on_action_result(const char *key, const char *status, const char *reason)
{
    bsp_display_lock(-1);
    if (!s_act.active || strcmp(key, s_act.key)) { bsp_display_unlock(); return; }
    if (!strcmp(status, "done") || !strcmp(status, "already_done")) {
        s_act.done = true; s_act.active = false; s_act.key[0] = 0;
    } else if (!strcmp(status, "expired")) {
        s_act.failed = true; s_act.active = false; s_act.key[0] = 0;
        snprintf(s_act.word, sizeof s_act.word, "EXPIRED");
    } else {
        s_act.failed = true;
        if (reason && strcmp(reason, "internal_error")) s_act.key[0] = 0;   /* only internal_error may reuse the key */
    }
    ESP_LOGI(TAG, "action %s: %s %s", s_act.aid, status, reason ? reason : "");
    if (s_view == V_DETAIL) apply_act_state();
    bsp_display_unlock();
}

void dayview_init(void)
{
    s_m = heap_caps_calloc(MAX_M, sizeof(mom_t), MALLOC_CAP_SPIRAM);
    bsp_display_lock(-1);
    s_root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    dial_use_ground(s_root);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_root, root_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(s_root, root_draw_post, LV_EVENT_DRAW_POST_END, NULL);
    lv_obj_add_event_cb(lv_obj_get_child(lv_scr_act(), 0), face_tap_cb, LV_EVENT_CLICKED, NULL);   /* the Now face */
    lv_timer_create(tick_cb, 30, NULL);                         /* returns at once while closed */
    bsp_display_unlock();
}
