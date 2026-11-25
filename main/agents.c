#include "agents.h"
#include "display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"
#include "fonts.h"
#include "dial.h"
#include "intercom_ws.h"
#include "thread.h"
#include "dayview.h"
#include "power.h"
#include "pill_list.h"

/* Plum Dusk tokens (spec 3.1) */
#define COL_RAISE    0x4A3752
#define COL_LINE     0x6B5575
#define COL_HAIR     0x8B7098
#define COL_INK      0xF8ECE8
#define COL_INK2     0xCDB5C6
#define COL_ROSE     0xF5A99B
#define COL_AMBER    0xF7C95C
#define COL_PERI     0xA9B8FF
#define COL_PERI_INK 0x1D1A33

#define MAX_JOBS   24                  /* what the bus sends at most */
#define ROW_H      66
#define LIST_X     83                  /* viewport: x 83..383, y 132..360 (see build_list) */
#define LIST_Y     132
#define LIST_W     300
#define LIST_H     228
#define LIST_ROW_W 270
#define GLYPH      28
#define IDLE_MS    60000               /* spec 8.3: Agents untouched 60 s -> Now */
#define CAP_Y      318
#define Q_FETCH_N  12                  /* messages fetched to find the question */
#define Q_RETRY_MS 4000

static const char *TAG = "agents";

/* ---- the jobs (the `sessions` resource, mapped like the web client's sessionState) ---- */

typedef enum { J_WAIT, J_FAIL, J_WORK, J_DONE } jstate_t;     /* also the sort order (spec 6.3) */
typedef struct {
    char id[66], conv[66], name[72], qmsg[66];
    jstate_t st;
    bool needs;                        /* needs_you: an answer-needed question is open */
    time_t since, last;
} job_t;

static job_t *s_job;                   /* PSRAM, sorted waiting, failed, working, done */
static int s_nj, s_working;

/* The question of the top waiting job: text and options from its answer-needed message. */
typedef struct { int index; char label[28]; } opt_t;
static struct {
    char sid[66], qmsg[66];            /* which job / message it belongs to */
    bool have;
    char text[400], mid[66];
    time_t at;
    int nopt;
    opt_t opt[3];
    int64_t tried_ms;                  /* last fetch start, for the retry */
} s_q;

static int64_t now_ms(void) { return lv_tick_get(); }

static int by_state(const void *a, const void *b)
{
    const job_t *x = a, *y = b;
    if (x->st != y->st) return (int)x->st - (int)y->st;
    if (x->needs != y->needs) return y->needs - x->needs;                                  /* a question outranks a plain wait */
    return x->since < y->since ? 1 : x->since > y->since ? -1 : strcmp(x->id, y->id);   /* newest first, like the bus */
}

static const char *str_of(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* sessionState() in model.js: needs_you or waiting -> waiting; stuck -> failed; working; finished -> done;
   idle (and anything else) is hidden. Returns -1 for hidden. */
static int job_state(const cJSON *s)
{
    const char *st = str_of(s, "state");
    if (cJSON_IsTrue(cJSON_GetObjectItem(s, "needs_you")) || !strcmp(st, "waiting")) return J_WAIT;
    if (!strcmp(st, "stuck")) return J_FAIL;
    if (!strcmp(st, "working")) return J_WORK;
    if (!strcmp(st, "finished")) return J_DONE;
    return -1;
}

/* ---- the screen ---- */

typedef enum { V_NONE, V_LIST, V_WAIT } view_t;
static lv_obj_t *s_scr, *s_cap;
static bool s_open, s_dirty;
static view_t s_view;
static int s_keep_y;
static bool s_test;                    /* debug: fake jobs, the bus `sessions` resource is ignored */
static pill_list_t *s_pl;              /* the list view: rows scroll when more than fit */
static char s_top[64];
static time_t s_built_min;
static char s_act_key[40]; static int s_act_seg = -1;
static enum { A_NONE, A_SENDING, A_SENT, A_FAILED } s_act;
static char s_act_sid[66];             /* the job the action state belongs to */

static const job_t *top_job(void) { return s_nj ? &s_job[0] : NULL; }
static bool wait_view(void) { const job_t *t = top_job(); return t && t->st == J_WAIT && t->needs; }
static bool question_ready(const job_t *t) { return s_q.have && !strcmp(s_q.sid, t->id) && !strcmp(s_q.qmsg, t->qmsg); }

static void minutes_text(char *b, size_t n, long sec, bool ago)
{
    long m = sec < 0 ? 0 : (sec + 30) / 60;
    if (ago && m < 1) { snprintf(b, n, "JUST NOW"); return; }
    if (m < 120) snprintf(b, n, "%ld MIN%s", m < 1 ? 1 : m, ago ? " AGO" : "");
    else if (m < 48 * 60) snprintf(b, n, "%ld H%s", m / 60, ago ? " AGO" : "");
    else snprintf(b, n, "%ld D%s", m / 1440, ago ? " AGO" : "");
}

/* "12m", "2h", "3d": the compact duration on rows. */
static void short_dur(char *b, size_t n, long sec)
{
    long m = sec < 0 ? 0 : (sec + 30) / 60;
    if (m < 60) snprintf(b, n, "%ldm", m);
    else if (m < 24 * 60) snprintf(b, n, "%ldh", m / 60);
    else snprintf(b, n, "%ldd", m / 1440);
}

static void hm_ampm(char *b, size_t n, time_t t)
{
    struct tm lt; localtime_r(&t, &lt);
    snprintf(b, n, "%d:%02d %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
}

/* ---- drawing ---- */

static bool on_strip(lv_layer_t *l, const lv_area_t *a) { return lv_area_is_on(a, &l->_clip_area); }

static void stroke(lv_layer_t *l, float x1, float y1, float x2, float y2, uint32_t col)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col); d.width = 2; d.opa = LV_OPA_COVER; d.round_start = 1; d.round_end = 1;
    d.p1 = (lv_point_precise_t){ x1, y1 }; d.p2 = (lv_point_precise_t){ x2, y2 };
    lv_draw_line(l, &d);
}

/* A status glyph (spec 5.9) on the 24 px grid scaled to 28 px, top-left at (x, y). Working spins 1.6 s a turn. */
static void glyph(lv_layer_t *l, int st, float x, float y)
{
    const float s = GLYPH / 24.0f;
    float cx = x + 12 * s, cy = y + 12 * s;
    if (st == J_WORK) {
        lv_area_t box = { (int)x, (int)y, (int)x + GLYPH, (int)y + GLYPH };
        if (!on_strip(l, &box)) return;
        int a0 = (int)(now_ms() % 1600 * 360 / 1600);
        lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
        d.color = lv_color_hex(COL_INK); d.width = 2; d.opa = LV_OPA_COVER; d.rounded = 1;
        d.center.x = (int)cx; d.center.y = (int)cy; d.radius = (int)(9 * s) + 1;
        d.start_angle = (a0 + 270) % 360; d.end_angle = (a0 + 180) % 360;      /* open 270 deg: from the top round to the left */
        lv_draw_arc(l, &d);
    } else if (st == J_WAIT) {
        dial_draw_ring(l, cx, cy, 9 * s, 2, COL_PERI);
        dial_draw_disc(l, cx, cy, 2.6f * s, COL_PERI);
    } else if (st == J_DONE) {
        stroke(l, x + 5 * s, y + 12.5f * s, x + 9.5f * s, y + 17 * s, COL_INK2);
        stroke(l, x + 9.5f * s, y + 17 * s, x + 19 * s, y + 7.5f * s, COL_INK2);
    } else {                                                       /* failed: diamond with a cross */
        static const float dm[4][2] = { { 12, 2.5f }, { 21.5f, 12 }, { 12, 21.5f }, { 2.5f, 12 } };
        for (int i = 0; i < 4; i++) {
            int j = (i + 1) % 4;
            stroke(l, x + dm[i][0] * s, y + dm[i][1] * s, x + dm[j][0] * s, y + dm[j][1] * s, COL_AMBER);
        }
        stroke(l, x + 9 * s, y + 9 * s, x + 15 * s, y + 15 * s, COL_AMBER);
        stroke(l, x + 15 * s, y + 9 * s, x + 9 * s, y + 15 * s, COL_AMBER);
    }
}

static void draw_main(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    dial_draw_base(l, s_view == V_WAIT ? COL_PERI : COL_ROSE);
    if (s_view == V_NONE) {
        dial_draw_ring_opa(l, 233, 164, 58, 1.5f, COL_HAIR, 153 /* 60% */);
        dial_draw_ring(l, 233, 164, 38, 2, COL_HAIR);
        dial_draw_disc(l, 233, 164, 7, COL_ROSE);
    } else if (s_view == V_WAIT) {                                 /* waiting glyph: ring + dot (spec 5.9), as on a decision */
        dial_draw_ring(l, 233, 78, 13.25f, 2.5f, COL_PERI);
        dial_draw_disc(l, 233, 78, 3.5f, COL_PERI);
    }
    if (s_top[0]) dial_draw_top_caption(l, s_top, COL_INK);
}

/* Glyphs go on top of the rows, so they are drawn after the children. */
static void draw_post(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    if (s_view == V_LIST) pill_list_draw_post(s_pl, l);            /* row glyphs, then the 40 px fades */
}

static void icon_cb(lv_layer_t *l, float x, float y, int st, void *ud) { (void)ud; glyph(l, st, x, y); }

/* ---- building ---- */

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t col, int x, int y, int w, int space)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, t);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(o, space, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(o, w, lv_font_get_line_height(f));             /* one line, cut with an ellipsis */
    lv_obj_set_pos(o, x, y);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

static int text_w(const char *t, const lv_font_t *f, int space)
{
    lv_point_t p; lv_text_get_size(&p, t, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

/* LVGL 9.5 sends CLICKED after a swipe too: a tap handler ignores a press that was a gesture. */
static bool was_swipe(void) { return compact_press_was_drag(); }

static void close_lv(void)
{
    if (!s_open) return;
    s_open = false;
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_scr);
    s_cap = NULL; s_pl = NULL; s_keep_y = 0;
    lv_obj_invalidate(lv_scr_act());
}

static void open_job_lv(const job_t *j)
{
    if (!j->conv[0]) return;
    char conv[66]; strlcpy(conv, j->conv, sizeof conv);            /* the thread call may rebuild our rows */
    close_lv();
    thread_open_conv_lv(conv);
}

static void open_cb(lv_event_t *e)                                 /* the question headline: a way into the thread */
{
    if (was_swipe()) return;
    const job_t *t = top_job();
    if (t && t->needs) open_job_lv(t);
}

static void opt_cb(lv_event_t *e)
{
    if (was_swipe()) return;
    int seg = (int)(intptr_t)lv_event_get_user_data(e);
    const job_t *t = top_job();
    if (!t || !t->needs) return;
    if (s_act == A_FAILED) { s_act = A_NONE; s_dirty = true; return; }       /* TRY AGAIN: the buttons come back */
    if (!question_ready(t) || seg >= s_q.nopt) { open_job_lv(t); return; }    /* OPEN */
    if (s_act == A_SENDING) return;
    snprintf(s_act_key, sizeof s_act_key, "agt-%08lx", (unsigned long)esp_random());
    strlcpy(s_act_sid, t->id, sizeof s_act_sid);
    s_act_seg = seg;
    s_act = A_SENDING;
    ESP_LOGI(TAG, "button %s on %s", s_q.opt[seg].label, s_q.mid);
    if (!intercom_send_conv_action(t->conv, s_q.opt[seg].index, s_q.mid, s_act_key)) s_act = A_FAILED;
    s_dirty = true;
}

/* The action capsule (spec 5.6) in the waiting tone: one capsule, segments fit their labels, the first one
   filled `peri` (peri-ink text), the rest ghost, 2 px `peri` border (`amber` when `warn`). One segment is a
   220 x 56 pill. */
static lv_obj_t *capsule(const char *const *labels, int n, bool ghost, bool warn)
{
    int w[3], total = 0;
    for (int i = 0; i < n; i++) { w[i] = text_w(labels[i], f_label15, 1) + 48; if (w[i] < 94) w[i] = 94; total += w[i]; }
    if (n == 1 && total < 220) { w[0] = 220; total = 220; }
    if (total > 296) { for (int i = 0; i < n; i++) w[i] = w[i] * 296 / total; total = 296; }
    lv_obj_t *c = lv_obj_create(s_scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, total, n == 1 ? 56 : 60);
    lv_obj_set_pos(c, 233 - total / 2, n == 1 ? CAP_Y + 2 : CAP_Y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(warn ? COL_AMBER : COL_PERI), 0);
    lv_obj_set_style_border_post(c, true, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    int x = 0;
    for (int i = 0; i < n; i++) {
        bool primary = i == 0 && !ghost;
        lv_obj_t *s = lv_obj_create(c);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, w[i], LV_PCT(100));
        lv_obj_set_pos(s, x, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(primary ? COL_PERI : COL_INK), 0);
        lv_obj_set_style_bg_opa(s, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(COL_INK), LV_STATE_PRESSED);   /* press inverts within 100 ms */
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_STATE_PRESSED);
        if (i) { lv_obj_set_style_border_side(s, LV_BORDER_SIDE_LEFT, 0); lv_obj_set_style_border_width(s, 2, 0); lv_obj_set_style_border_color(s, lv_color_hex(COL_PERI), 0); }
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s, 8);                                      /* hit target >= 76 px (spec 11.3) */
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s, opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *t = label(s, labels[i], f_label15, primary ? COL_PERI_INK : COL_INK, 0, 0, w[i], 1);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(COL_PERI_INK), LV_STATE_PRESSED);
        lv_obj_center(t);
        x += w[i];
    }
    return c;
}

/* Cut `name` (caps, UTF-8) so that "name" + suffix fits max_w; the cut ends in "...". */
static void fit_name(char *name, size_t n, const char *suffix, const lv_font_t *f, int space, int max_w)
{
    char both[160];
    snprintf(both, sizeof both, "%s%s", name, suffix);
    if (text_w(both, f, space) <= max_w) return;
    size_t len = strlen(name);
    while (len > 0) {
        do { len--; } while (len > 0 && ((unsigned char)name[len] & 0xC0) == 0x80);
        while (len > 0 && name[len - 1] == ' ') len--;
        if (len + 4 > n) continue;
        memcpy(name + len, "...", 4);
        snprintf(both, sizeof both, "%s%s", name, suffix);
        if (text_w(both, f, space) <= max_w) return;
    }
}

static void build_wait(const job_t *t, time_t now)
{
    s_view = V_WAIT;
    bool ready = question_ready(t);
    char name[72]; strlcpy(name, t->name, sizeof name);
    fit_name(name, sizeof name, " \xC2\xB7 NEEDS YOU", f_whisper, 2, 314);
    char who[100]; snprintf(who, sizeof who, "%s \xC2\xB7 NEEDS YOU", name);
    lv_obj_t *o = label(s_scr, who, f_whisper, COL_PERI, 76, 100, 314, 2);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);

    /* the question as the headline: Hanken 600 26 / 32, two lines; a tap on it opens the thread */
    o = lv_label_create(s_scr);
    lv_label_set_text(o, ready ? s_q.text : "Waiting for your answer");
    lv_obj_set_style_text_font(o, f_headline, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(o, 32 - lv_font_get_line_height(f_headline), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(o, 300, 64);
    lv_obj_set_pos(o, 83, 130);
    if (t->conv[0]) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(o, open_cb, LV_EVENT_CLICKED, NULL);
    }

    char l1[40] = "", l2[40] = "", ago[24], run[24], buf[96];
    if (ready && s_q.at && now >= 1700000000) { minutes_text(ago, sizeof ago, (long)(now - s_q.at), true); snprintf(l1, sizeof l1, "ASKED %s", ago); }
    if (t->since && now >= 1700000000) { minutes_text(run, sizeof run, (long)(now - t->since), false); snprintf(l2, sizeof l2, "RUNNING %s", run); }
    snprintf(buf, sizeof buf, "%s%s%s", l1, l1[0] && l2[0] ? "\n" : "", l2);
    o = lv_label_create(s_scr);
    lv_label_set_text(o, buf);
    lv_obj_set_style_text_font(o, f_meta13, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(COL_INK2), 0);
    lv_obj_set_style_text_letter_space(o, 1, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(o, 18 - lv_font_get_line_height(f_meta13), 0);
    lv_obj_set_size(o, 300, 36);
    lv_obj_set_pos(o, 83, 226);

    /* the capsule: up to 2 options + OPEN; a question with 3 options shows all three (the headline opens the thread) */
    if (s_act == A_SENT && !strcmp(s_act_sid, t->id)) {
        lv_obj_t *w = label(s_scr, "SENT", f_label15, COL_PERI, 83, CAP_Y + 20, 300, 3);
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    if (s_act == A_FAILED && !strcmp(s_act_sid, t->id)) {
        const char *again[1] = { "TRY AGAIN" };
        s_cap = capsule(again, 1, true, true);
        return;
    }
    const char *labs[3];
    int n = 0;
    if (ready) {
        int keep = s_q.nopt > 2 ? 3 : s_q.nopt;
        for (int i = 0; i < keep; i++) labs[n++] = s_q.opt[i].label;
    }
    if (n < 3) labs[n++] = "OPEN";
    if (n == 1 && !t->conv[0]) return;                                       /* nothing to press: no OPEN without a thread */
    s_cap = capsule(labs, n, false, false);
    if (s_act == A_SENDING && s_act_seg >= 0 && s_act_seg < n && !strcmp(s_act_sid, t->id)) {
        lv_obj_t *seg = lv_obj_get_child(s_cap, s_act_seg);
        lv_label_set_text(lv_obj_get_child(seg, 0), "\xE2\x80\xA6");
    }
}

/* The list lives in a viewport inside the circle's safe area. The top caption is a curved band on r 151..196 that
   sweeps down the sides to about y 140 at the widest letters, so the viewport starts at LIST_Y (below it) and every
   row has one width (LIST_ROW_W), centred. Rows beyond the viewport are reached by a vertical drag. */
static void row_tap(int i, void *ud)
{
    (void)ud;
    if (i < s_nj) open_job_lv(&s_job[i]);                          /* rows are the jobs in sort order */
}

static void build_list(time_t now)
{
    s_view = V_LIST;
    pill_list_cfg_t c = { .x = LIST_X, .y = LIST_Y, .w = LIST_W, .h = LIST_H, .row_w = LIST_ROW_W, .row_h = ROW_H, .gap = 8,
                          .icon = GLYPH, .width = PILL_W_FIXED, .icon_fn = icon_cb, .tap = row_tap };
    s_pl = pill_list_create(s_scr, &c);
    for (int i = 0; i < s_nj; i++) {
        const job_t *j = &s_job[i];
        char det[40], dur[12];
        uint32_t dcol = COL_INK2;
        if (j->st == J_WAIT) { snprintf(det, sizeof det, "%s", j->needs ? "needs you" : "waiting"); dcol = COL_PERI; }
        else if (j->st == J_FAIL) { snprintf(det, sizeof det, "failed"); dcol = COL_AMBER; }
        else if (j->st == J_WORK) { short_dur(dur, sizeof dur, j->since ? (long)(now - j->since) : 0); snprintf(det, sizeof det, "working \xC2\xB7 %s", dur); }
        else { short_dur(dur, sizeof dur, j->last ? (long)(now - j->last) : 0); snprintf(det, sizeof det, "done \xC2\xB7 %s ago", dur); }
        pill_row_t r = { .title = j->name, .sub = det, .title_col = j->st == J_DONE ? COL_INK2 : COL_INK, .sub_col = dcol,
                         .style = j->st, .tappable = j->conv[0] != 0,   /* no conversation: not tappable */
                         .animated = j->st == J_WORK };
        pill_list_set(s_pl, i, &r);
    }
    if (s_keep_y) pill_list_scroll_to(s_pl, s_keep_y);
}

static void build_none(time_t now)
{
    s_view = V_NONE;
    snprintf(s_top, sizeof s_top, "All quiet");
    lv_obj_t *o = label(s_scr, "NOTHING RUNNING", f_lab17, COL_INK, 83, 250, 300, 2);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    const job_t *last = NULL;
    for (int i = 0; i < s_nj; i++) if (s_job[i].st == J_DONE && (!last || s_job[i].last > last->last)) last = &s_job[i];
    if (last && last->last) {
        char t[16], line[120], nm[72];
        hm_ampm(t, sizeof t, last->last);
        strlcpy(nm, last->name, sizeof nm);
        char tail[24]; snprintf(tail, sizeof tail, ", %s", t);
        fit_name(nm, sizeof nm, tail, f_whisper, 1, 330);
        snprintf(line, sizeof line, "LAST: %s%s", nm, tail);
        o = label(s_scr, line, f_whisper, COL_INK2, 68, 282, 330, 1);
        lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    }
    o = label(s_scr, "Hold BOOT and ask", f_hint18, COL_INK, 103, 324 + (24 - lv_font_get_line_height(f_hint18)) / 2, 260, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    (void)now;
}

static void build(void)
{
    s_dirty = false;
    s_keep_y = s_pl ? pill_list_scroll_y(s_pl) : 0;               /* a refresh keeps the scroll position */
    lv_obj_clean(s_scr);
    s_cap = NULL;
    s_pl = NULL;
    s_top[0] = 0;
    time_t now = time(NULL);
    s_built_min = now / 60;
    const job_t *t = top_job();
    if (wait_view()) {
        build_wait(t, now);
        int others = 0;
        for (int i = 1; i < s_nj; i++) if (s_job[i].st == J_WAIT && s_job[i].needs) others++;
        if (others) snprintf(s_top, sizeof s_top, "%d more waiting", others);   /* the others wait behind this one */
    } else if (!t || t->st == J_DONE) {
        build_none(now);
    } else {
        int c[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < s_nj; i++) c[s_job[i].st]++;
        static const char *const word[3] = { "waiting", "failed", "working" };
        size_t o = 0;
        s_top[0] = 0;
        for (int k = 0; k < 3; k++) {
            if (!c[k]) continue;
            o += snprintf(s_top + o, sizeof s_top - o, "%s%d %s", o ? " \xC2\xB7 " : "", c[k], word[k]);
        }
        build_list(now);
    }
    lv_obj_invalidate(s_scr);
}

/* ---- the question ---- */

static void ask_question(const job_t *t)
{
    int64_t now = now_ms();
    if (!t->conv[0] || (s_q.tried_ms && now - s_q.tried_ms < Q_RETRY_MS)) return;
    s_q.tried_ms = now;
    if (!intercom_fetch_conversation_id(t->conv, Q_FETCH_N)) s_q.tried_ms = now - Q_RETRY_MS + 1000;   /* busy: soon again */
}

/* Called with the display lock held. Starts, or restarts, the lookup when the top waiting job or its message changed. */
static void ensure_question(void)
{
    const job_t *t = wait_view() ? top_job() : NULL;
    if (!t) return;
    if (strcmp(s_q.sid, t->id) || strcmp(s_q.qmsg, t->qmsg)) {
        memset(&s_q, 0, sizeof s_q);
        strlcpy(s_q.sid, t->id, sizeof s_q.sid);
        strlcpy(s_q.qmsg, t->qmsg, sizeof s_q.qmsg);
    }
    if (!s_q.have) ask_question(t);
}

void agents_on_conversation(const char *conv_id, const cJSON *page, int http_status)
{
    bsp_display_lock(-1);
    const job_t *t = wait_view() ? top_job() : NULL;
    if (t && !s_q.have && !strcmp(t->conv, conv_id) && !strcmp(s_q.sid, t->id)) {
        if (!page) {
            ESP_LOGW(TAG, "question fetch failed (HTTP %d)", http_status);
            s_q.tried_ms = now_ms() + 6000;                          /* try again in 10 s */
        } else {
            /* the answer-needed message by id, else the newest one flagged answer_needed (questionOf in model.js) */
            const cJSON *m, *pick = NULL;
            cJSON_ArrayForEach(m, cJSON_GetObjectItem(page, "messages")) {
                if (t->qmsg[0] && !strcmp(str_of(m, "id"), t->qmsg)) { pick = m; break; }
                if (cJSON_IsTrue(cJSON_GetObjectItem(m, "answer_needed"))) pick = m;
            }
            if (pick) {
                s_q.have = true;
                strlcpy(s_q.mid, str_of(pick, "id"), sizeof s_q.mid);
                thread_sanitize(s_q.text, sizeof s_q.text, str_of(pick, "text"));
                const char *at = str_of(pick, "created_at");
                s_q.at = at[0] ? dial_parse_iso(at) : 0;
                const cJSON *b;
                cJSON_ArrayForEach(b, cJSON_GetObjectItem(pick, "buttons")) {
                    if (s_q.nopt == 3) break;
                    const cJSON *ix = cJSON_GetObjectItem(b, "index"), *lab = cJSON_GetObjectItem(b, "label");
                    if (!cJSON_IsNumber(ix) || !cJSON_IsString(lab)) continue;
                    s_q.opt[s_q.nopt].index = ix->valueint;
                    char caps[64]; dial_caps(caps, sizeof caps, lab->valuestring);
                    if (strlen(caps) > 10) { ESP_LOGW(TAG, "button label cut: %s", caps); caps[10] = 0; }   /* spec 10: no ellipsis */
                    strlcpy(s_q.opt[s_q.nopt].label, caps, sizeof s_q.opt[0].label);
                    s_q.nopt++;
                }
                ESP_LOGI(TAG, "question for %s: %d options", t->id, s_q.nopt);
                if (!s_q.text[0]) s_q.have = false;                  /* only emoji or markup: keep the plain line */
            }
            if (!s_q.have) s_q.tried_ms = now_ms() + 56000;           /* no question in the page: look again in a minute */
            if (s_open) s_dirty = true;
        }
    }
    bsp_display_unlock();
}

void agents_on_action_result(const char *key, const char *status, const char *reason)
{
    bsp_display_lock(-1);
    if (s_act == A_SENDING && !strcmp(key, s_act_key)) {
        bool ok = !strcmp(status, "done") || !strcmp(status, "already_done");
        s_act = ok ? A_SENT : A_FAILED;
        if (!ok) ESP_LOGW(TAG, "button failed: %s %s", status, reason ? reason : "?");
        if (s_open) s_dirty = true;
    }
    bsp_display_unlock();
}

/* ---- the sessions resource ---- */

void agents_on_sessions(const cJSON *items)
{
    if (!s_job || s_test) return;
    bsp_display_lock(-1);
    int n = 0, working = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (n == MAX_JOBS || !cJSON_IsObject(it)) break;
        int st = job_state(it);
        if (st < 0) continue;
        job_t *j = &s_job[n++];
        memset(j, 0, sizeof *j);
        strlcpy(j->id, it->string ? it->string : "", sizeof j->id);
        strlcpy(j->conv, str_of(it, "conversation_id"), sizeof j->conv);
        const char *nm = str_of(it, "subject");
        if (!nm[0]) nm = str_of(it, "title");
        dial_caps(j->name, sizeof j->name, nm[0] ? nm : "Session");
        if (!j->name[0]) strlcpy(j->name, "SESSION", sizeof j->name);
        j->st = (jstate_t)st;
        j->needs = cJSON_IsTrue(cJSON_GetObjectItem(it, "needs_you"));
        strlcpy(j->qmsg, str_of(it, "needs_you_message_id"), sizeof j->qmsg);
        const char *since = str_of(it, "since"), *last = str_of(it, "last_activity_at");
        j->since = since[0] ? dial_parse_iso(since) : 0;
        j->last = last[0] ? dial_parse_iso(last) : j->since;
        if (st == J_WORK) working++;
    }
    qsort(s_job, n, sizeof s_job[0], by_state);
    s_nj = n;
    s_working = working;
    const job_t *t = wait_view() ? top_job() : NULL;
    if (!t || strcmp(s_act_sid, t->id)) s_act = A_NONE;            /* answered, or another job on top: the capsule resets */
    ensure_question();                                             /* fetch the question now, so opening it is instant */
    if (s_open) s_dirty = true;
    bsp_display_unlock();
    dial_set_agent_count(working);
}

/* ---- open / close (LVGL context) ---- */

void agents_open_lv(void)
{
    if (s_open) return;
    s_open = true;
    ensure_question();
    build();
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    lv_display_trigger_activity(NULL);
    ESP_LOGI(TAG, "open (%d jobs, %d working)", s_nj, s_working);
}

void agents_close_lv(void) { close_lv(); }

bool agents_swipe_lv(lv_dir_t dir)
{
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return false;
    if (s_open) {                                                   /* Day · Now · Agents · Thread */
        if (dir == LV_DIR_LEFT) { close_lv(); thread_open_lv(); }   /* on to Thread (the followed conversation) */
        else close_lv();                                            /* back to Now */
        return true;
    }
    power_state_t ps = power_state();
    if (dir != LV_DIR_LEFT || thread_is_open() || dayview_is_open() || ps == PWR_REST || ps == PWR_SLEEP) return false;
    agents_open_lv();                                               /* spec 8.1: swipe left from Now = Agents */
    return true;
}

/* ---- timers ---- */

static void tick_cb(lv_timer_t *t)
{
    if (!s_open) return;
    if (lv_display_get_inactive_time(NULL) >= IDLE_MS) { close_lv(); return; }   /* spec 8.3: back to Now */
    time_t now = time(NULL);
    if (now / 60 != s_built_min && now >= 1700000000) s_dirty = true;            /* durations refresh each minute */
    ensure_question();
    if (s_dirty) { build(); return; }
    if (s_view == V_LIST) pill_list_tick(s_pl);                    /* the working spinner turns */
}

/* ---- public ---- */

void agents_init(void)
{
    s_job = heap_caps_calloc(MAX_JOBS, sizeof(job_t), MALLOC_CAP_SPIRAM);
    bsp_display_lock(-1);
    s_scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    dial_use_ground(s_scr);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_scr, draw_main, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(s_scr, draw_post, LV_EVENT_DRAW_POST_END, NULL);
    lv_timer_create(tick_cb, 100, NULL);
    bsp_display_unlock();
}

void agents_close(void)
{
    bsp_display_lock(-1);
    close_lv();
    bsp_display_unlock();
}

bool agents_is_open(void) { return s_open; }

/* Testing (POST /debug/ui): "agents-test <n>" shows n fake jobs (n 0 or "off": back to the bus), "agents-scroll <dy>"
   scrolls the list by dy px (animated). */
bool agents_debug(const char *what, int dy)
{
    if (!s_job) return false;
    if (!strcmp(what, "agents-scroll")) {
        bsp_display_lock(-1);
        if (s_pl) pill_list_scroll_by(s_pl, dy);
        bsp_display_unlock();
        return true;
    }
    if (strcmp(what, "agents-test")) return false;
    bsp_display_lock(-1);
    if (dy <= 0) { s_test = false; s_nj = 0; s_working = 0; s_dirty = true; bsp_display_unlock(); return true; }
    s_test = true;
    static const char *const nm[] = { "COMPACT", "PEGGY", "COMPACT DEVELOPMENT", "TRIP BOOKING", "WEEKLY REPORT", "MORTGAGE REFI",
                                      "KINDLE LAYOUT", "TAX FILING", "AGENTBUS EPIC" };
    static const jstate_t sts[] = { J_WAIT, J_FAIL, J_WORK, J_WORK, J_DONE, J_DONE, J_WORK, J_DONE, J_DONE };
    time_t now = time(NULL);
    s_nj = dy > 9 ? 9 : dy;
    for (int i = 0; i < s_nj; i++) {
        job_t *j = &s_job[i];
        memset(j, 0, sizeof *j);
        snprintf(j->id, sizeof j->id, "test-%d", i);
        strlcpy(j->name, nm[i], sizeof j->name);
        j->st = sts[i];
        j->since = j->last = now - 300 * (i + 1);
    }
    s_working = 0;
    for (int i = 0; i < s_nj; i++) if (s_job[i].st == J_WORK) s_working++;
    s_dirty = true;
    bsp_display_unlock();
    return true;
}
