#include "pomodoro.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"
#include "src/core/lv_obj_private.h"
#include "cJSON.h"
#include "fonts.h"
#include "dial.h"
#include "display.h"
#include "power.h"
#include "audio_out.h"
#include "decision.h"
#include "thread.h"
#include "agents.h"
#include "dayview.h"
#include "earlier.h"
#include "setscreen.h"
#include "capture.h"
#include "pomo_logic.h"

static const char *TAG = "pomodoro";

#define C 233
#define RING_OUT 232
#define RING_W 40
#define RING_MID 212                  /* centre line of the ring: the head dot and the moon ride it */
#define DEF_FOCUS_MIN 25
#define DEF_BREAK_MIN 5
#define DEF_LONG_MIN 15
#define PULSE_US 1500000
#define HOLD_SHOW_MS 300              /* a tap never shows the fill; a hold shows it from here ... */
#define HOLD_CANCEL_MS 800            /* ... and cancels here */
#define COL_PEACH  0xF6B08C           /* focus */
#define COL_VIOLET 0xB5A0F5           /* break */
#define COL_SHADE_ 0x0F0614
#define COL_PAUSE  0xF7C95C
#define COL_HAIR_  0x8B7098
#define COL_INK_   0xF8ECE8
#define COL_INK2_  0xCDB5C6
#define COL_ROSE_  0xF5A99B

/* ---- the timer (any task; s_mu) ---- */
static SemaphoreHandle_t s_mu;
static pomo_t s_p;
static int64_t s_pulse_until_us;
static double s_focus_min = DEF_FOCUS_MIN, s_break_min = DEF_BREAK_MIN, s_long_min = DEF_LONG_MIN;
static esp_timer_handle_t s_esp_timer;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

typedef struct {
    bool active, paused;
    pomo_phase_t phase;
    int done;
    int64_t elapsed_ms, len_ms;
    int64_t pulse_until_us;
} snap_t;

static void take_snap(snap_t *s)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int64_t t = now_ms();
    s->active = s_p.active; s->paused = s_p.paused; s->phase = s_p.phase; s->done = s_p.done;
    s->elapsed_ms = pomo_elapsed_ms(&s_p, t); s->len_ms = pomo_len_ms(&s_p);
    s->pulse_until_us = s_pulse_until_us;
    xSemaphoreGive(s_mu);
}

bool pomodoro_active(void) { return s_p.active; }

/* A phase changed: pulse the ring; a natural end also chimes (not in quiet hours) and wakes a panel that slept. */
static void phase_cue(pomo_ev_t ev, bool natural)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_pulse_until_us = esp_timer_get_time() + PULSE_US;
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "%s", ev == POMO_FINISHED ? "finished" : "next phase");
    if (!natural) return;
    if (power_quiet()) return;
    audio_out_chime();
    if (power_state() == PWR_SLEEP) power_wake("pomodoro phase end");
}

static void esp_tick(void *arg)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    pomo_ev_t ev = s_p.active ? pomo_update(&s_p, now_ms()) : POMO_NONE;
    xSemaphoreGive(s_mu);
    if (ev != POMO_NONE) phase_cue(ev, true);
}

/* ---- the screen (LVGL context) ---- */
static lv_obj_t *s_root, *s_covered_scr;
static lv_timer_t *s_timer;
static bool s_root_hidden;                       /* a decision or Thread has the glass; the face is shown for them */
static uint32_t s_hold_from;
static int s_fill_h;

typedef struct {
    bool valid, paused, last_min, hot;
    pomo_phase_t phase;
    int done, rem_s, head_s, total_s, pulse;     /* pulse: 0 none, 1 bright, 2 dim */
    float head_deg;
} view_t;
static view_t s_v;

static void set_covered_screen(bool hide)
{
    lv_obj_t *scr = lv_scr_act();
    if (!scr) return;
    if (hide && !lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN)) { lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN); s_covered_scr = scr; }
    if (!hide && s_covered_scr) {
        /* not lv_obj_remove_flag: on a screen it marks the NULL parent's layout dirty (LVGL 9.5, LoadProhibited) */
        s_covered_scr->flags &= ~LV_OBJ_FLAG_HIDDEN;
        lv_obj_invalidate(s_covered_scr);
        s_covered_scr = NULL;
    }
}

static void close_lv(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_root) { lv_obj_delete(s_root); s_root = NULL; }
    set_covered_screen(false);
    s_root_hidden = false;
    s_hold_from = 0; s_fill_h = 0;
    s_v.valid = false;
}

static uint32_t mix_white(uint32_t c, int pct)
{
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    r += (255 - r) * pct / 100; g += (255 - g) * pct / 100; b += (255 - b) * pct / 100;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static inline bool on_clip(lv_layer_t *l, const lv_area_t *a) { return lv_area_is_on(a, &l->_clip_area); }

/* Flat-ended arc in dial degrees (clockwise from 12), outer edge at RING_OUT. A closed ring when it spans the circle. */
static void ring_arc(lv_layer_t *l, float from, float to, uint32_t col, lv_opa_t opa)
{
    if (to - from < 0.3f) return;
    if (to - from >= 359.5f) { dial_draw_ring_opa(l, C, C, RING_OUT, RING_W, col, opa); return; }
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.center.x = C; d.center.y = C; d.radius = RING_OUT; d.width = RING_W; d.rounded = 0;
    d.start_angle = fmodf(from - 90 + 720, 360); d.end_angle = fmodf(to - 90 + 720, 360);
    d.color = lv_color_hex(col); d.opa = opa;
    lv_area_t a;
    lv_draw_arc_get_area(d.center.x, d.center.y, d.radius, d.start_angle, d.end_angle, d.width, d.rounded, &a);
    if (on_clip(l, &a)) lv_draw_arc(l, &d);
}

/* Tabular MM:SS: every digit in a cell as wide as the widest. */
typedef struct { int dw, cw; int16_t gw[11]; } cells_t;
static cells_t s_cells;
static const lv_font_t *s_num_font;
static const cells_t *cells(const lv_font_t *f)
{
    if (s_num_font != f) {
        s_cells.dw = 0;
        for (int k = 0; k < 10; k++) { s_cells.gw[k] = (int16_t)lv_font_get_glyph_width(f, (uint32_t)('0' + k), 0); if (s_cells.gw[k] > s_cells.dw) s_cells.dw = s_cells.gw[k]; }
        s_cells.gw[10] = (int16_t)lv_font_get_glyph_width(f, ':', 0);
        s_cells.cw = s_cells.gw[10];
        s_num_font = f;
    }
    return &s_cells;
}

static int num_total_w(const char *s, const lv_font_t *f)
{
    const cells_t *c = cells(f);
    int w = 0;
    for (const char *p = s; *p; p++) w += *p == ':' ? c->cw : c->dw;
    return w;
}

static int num_top(const lv_font_t *f) { return C - lv_font_get_line_height(f) / 2 - 2; }

static void num_draw(lv_layer_t *l, const char *s, const lv_font_t *f, uint32_t col, lv_opa_t opa)
{
    int lh = lv_font_get_line_height(f), y = num_top(f);
    if (y > l->_clip_area.y2 || y + lh < l->_clip_area.y1) return;
    const cells_t *c = cells(f);
    int x = C - num_total_w(s, f) / 2;
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = f; d.color = lv_color_hex(col); d.opa = opa; d.text_local = 1;
    char one[2] = { 0, 0 };
    for (const char *p = s; *p; p++) {
        int w = *p == ':' ? c->cw : c->dw, gw = *p == ':' ? c->gw[10] : c->gw[*p - '0'];
        one[0] = *p; d.text = one;
        lv_area_t a = { x + (w - gw) / 2, y, x + (w - gw) / 2 + gw + 2, y + lh };
        if (on_clip(l, &a)) lv_draw_label(l, &d, &a);
        x += w;
    }
}

static void text_c(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, int y)
{
    int lh = lv_font_get_line_height(f);
    if (y > l->_clip_area.y2 || y + lh < l->_clip_area.y1) return;
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_area_t a = { C - sz.x / 2, y, C - sz.x / 2 + sz.x + 2, y + lh };
    if (!on_clip(l, &a)) return;
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = s; d.font = f; d.color = lv_color_hex(col); d.letter_space = space; d.text_local = 1;
    lv_draw_label(l, &d, &a);
}

static void head_pos(float deg, float *x, float *y)
{
    float a = deg * 3.14159265f / 180;
    *x = C + RING_MID * sinf(a); *y = C - RING_MID * cosf(a);
}

static void fmt_time(char *b, size_t n, int s) { if (s > 5999) s = 5999; snprintf(b, n, "%02d:%02d", (s / 60) % 100, s % 60); }

#define DOTS_Y 340
#define CAP_Y 120

static void draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    const view_t *v = &s_v;
    if (!v->valid) return;
    bool brk = v->phase != POMO_FOCUS;
    uint32_t acc = brk ? COL_VIOLET : COL_PEACH;
    uint32_t col = v->last_min && !v->paused ? mix_white(acc, 30) : acc;
    lv_opa_t opa = v->paused ? 90 : v->pulse == 2 ? 120 : v->last_min ? LV_OPA_COVER : 225;

    dial_draw_ring_opa(l, C, C, RING_OUT, RING_W, COL_HAIR_, 46);                 /* the empty track */
    float th = v->head_deg;
    if (!brk) ring_arc(l, th, 360, col, opa);                                    /* drains clockwise from 12 */
    else ring_arc(l, 0, th, col, opa);                                           /* fills clockwise from 12 */

    float hx, hy; head_pos(th, &hx, &hy);                                        /* head dot and the second-hand moon */
    float ma = (v->head_s % 60) * 6 * 3.14159265f / 180;
    dial_draw_disc_opa(l, hx, hy, 7, COL_SHADE_, LV_OPA_COVER);
    dial_draw_ring_opa(l, hx, hy, 8, 1.5f, COL_INK_, LV_OPA_COVER);
    dial_draw_disc_opa(l, hx + 13 * sinf(ma), hy - 13 * cosf(ma), 3.5f, COL_INK_, LV_OPA_COVER);

    const char *cap = v->paused ? "PAUSED" : v->phase == POMO_FOCUS ? "FOCUS" : v->phase == POMO_BREAK ? "BREAK" : "LONG BREAK";
    text_c(l, cap, f_label, 4, v->paused ? COL_PAUSE : acc, CAP_Y);

    char b[16]; fmt_time(b, sizeof b, v->rem_s);
    uint32_t nc = v->paused ? COL_INK2_ : v->hot ? acc : COL_INK_;
    num_draw(l, b, f_num_l, nc, v->paused ? 150 : LV_OPA_COVER);

    for (int i = 0; i < POMO_ROUNDS; i++) {                                      /* rounds done */
        float x = C + (i - 1.5f) * 30;
        if (i < v->done) dial_draw_disc_opa(l, x, DOTS_Y, 7, COL_PEACH, LV_OPA_COVER);
        else if (i == v->done && v->phase == POMO_FOCUS) dial_draw_ring_opa(l, x, DOTS_Y, 7, 2, COL_PEACH, LV_OPA_COVER);
        else dial_draw_disc_opa(l, x, DOTS_Y, 4, COL_HAIR_, 150);
    }
    if (v->paused) {
        text_c(l, "TAP TO RESUME", f_whisper, 3, COL_INK2_, 372);
        text_c(l, "HOLD TO CANCEL", f_whisper, 3, COL_INK2_, 398);
    }
    if (s_fill_h > 0) dial_draw_hold_fill(l, s_fill_h, COL_ROSE_, 90);
}

static void inval_box(float cx, float cy, int r)
{
    lv_area_t a = { (int)cx - r, (int)cy - r, (int)cx + r, (int)cy + r };
    lv_obj_invalidate_area(s_root, &a);
}

static bool covered_by_face(void)
{
    return decision_showing() || thread_is_open() || agents_is_open() || dayview_is_open() || earlier_is_open() ||
           setscreen_is_open() || capture_busy();
}

static void tick(lv_timer_t *t)
{
    if (!s_root) return;
    snap_t s; take_snap(&s);
    if (!s.active) { close_lv(); return; }

    bool cov = covered_by_face();                                                /* a decision, Thread ...: they own the glass */
    if (cov != s_root_hidden) {
        s_root_hidden = cov;
        if (cov) { lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN); set_covered_screen(false); }
        else { lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN); set_covered_screen(true); lv_obj_invalidate(s_root); s_v.valid = false; }
    }

    if (s_hold_from) {                                                           /* hold to cancel: the fill, then the cancel */
        uint32_t ms = lv_tick_elaps(s_hold_from);
        int h = ms < HOLD_SHOW_MS ? 0 : ms >= HOLD_CANCEL_MS ? 466 : (int)((ms - HOLD_SHOW_MS) * 466 / (HOLD_CANCEL_MS - HOLD_SHOW_MS));
        dial_hold_fill_step(s_root, &s_fill_h, h);
        if (ms >= HOLD_CANCEL_MS) { s_hold_from = 0; ESP_LOGI(TAG, "hold: cancel"); pomodoro_stop(); return; }
    }

    int64_t rem = s.len_ms - s.elapsed_ms;
    view_t n = { .valid = true, .paused = s.paused, .phase = s.phase, .done = s.done };
    n.total_s = (int)((s.len_ms + 500) / 1000);
    n.rem_s = (int)((rem + 999) / 1000);
    n.head_s = (int)(s.elapsed_ms / 1000);
    /* LVGL arc angles are whole degrees (no LV_USE_FLOAT), so the ring and its head both move in 1 degree steps */
    n.head_deg = s.len_ms ? roundf(360.0f * (n.head_s * 1000.0f) / s.len_ms) : 0;
    if (n.head_deg > 359) n.head_deg = 359;
    n.last_min = n.rem_s <= 60 && s.len_ms >= 120000;                            /* a phase under 2 min has no "last minute" */
    n.hot = n.last_min && !s.paused && (s.elapsed_ms % 1000) < 300;
    int64_t left = s.pulse_until_us - esp_timer_get_time();
    n.pulse = left > 0 ? (((left / 250000) & 1) ? 1 : 2) : 0;

    view_t o = s_v;
    bool full = !o.valid || o.paused != n.paused || o.phase != n.phase || o.done != n.done || o.last_min != n.last_min ||
                o.pulse != n.pulse || o.total_s != n.total_s;
    s_v = n;
    if (s_root_hidden) return;
    if (full) { lv_obj_invalidate(s_root); return; }
    if (o.rem_s != n.rem_s || o.hot != n.hot) {
        lv_area_t a = { C - 170, num_top(f_num_l) - 2, C + 170, num_top(f_num_l) + lv_font_get_line_height(f_num_l) + 2 };
        lv_obj_invalidate_area(s_root, &a);
    }
    if (o.head_s != n.head_s || o.head_deg != n.head_deg) {
        float x, y; head_pos(o.head_deg, &x, &y); inval_box(x, y, 30);
        head_pos(n.head_deg, &x, &y); inval_box(x, y, 30);
    }
}

static void on_input(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_hold_from = lv_tick_get() ? lv_tick_get() : 1;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_hold_from = 0;
        if (s_root && s_fill_h) dial_hold_fill_step(s_root, &s_fill_h, 0);
    } else if (code == LV_EVENT_GESTURE) {
        s_hold_from = 0;
        ESP_LOGI(TAG, "swipe: skip");
        pomodoro_skip();
    } else if (code == LV_EVENT_CLICKED) {
        if (compact_press_was_drag()) return;                                    /* LVGL clicks after a swipe too */
        ESP_LOGI(TAG, "tap: pause/resume");
        pomodoro_pause(2);
    }
}

static void open_lv(void)
{
    if (s_root) { lv_obj_invalidate(s_root); s_v.valid = false; return; }
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    dial_use_ground(s_root);                                                     /* plum glass by day, black at night */
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(s_root, on_input, LV_EVENT_ALL, NULL);
    s_timer = lv_timer_create(tick, 100, NULL);
    s_v.valid = false;
    s_root_hidden = false;
    set_covered_screen(true);                                                    /* the face underneath is not redrawn meanwhile */
    lv_display_trigger_activity(NULL);
    tick(NULL);
    lv_obj_invalidate(s_root);
}

/* ---- public ---- */

bool pomodoro_showing(void) { return s_p.active && s_root && !s_root_hidden; }

void pomodoro_wake_lv(void)
{
    if (!s_root) return;
    s_v.valid = false;
    lv_obj_invalidate(s_root);
}

bool pomodoro_start(double focus_min, double break_min, double long_min)
{
    if (focus_min <= 0) focus_min = DEF_FOCUS_MIN;
    if (break_min <= 0) break_min = DEF_BREAK_MIN;
    if (long_min <= 0) long_min = DEF_LONG_MIN;
    if (focus_min < 0.1 || break_min < 0.1 || long_min < 0.1 || focus_min > 99 || break_min > 99 || long_min > 99) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_focus_min = focus_min; s_break_min = break_min; s_long_min = long_min;
    pomo_start(&s_p, (int64_t)(focus_min * 60000), (int64_t)(break_min * 60000), (int64_t)(long_min * 60000), now_ms());
    s_pulse_until_us = 0;
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "start: focus %g, break %g, long %g min", focus_min, break_min, long_min);
    bsp_display_lock(-1);
    open_lv();
    bsp_display_unlock();
    if (power_state() != PWR_AWAKE) power_wake("pomodoro start");
    return true;
}

void pomodoro_stop(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool was = s_p.active;
    pomo_stop(&s_p);
    s_pulse_until_us = 0;
    xSemaphoreGive(s_mu);
    if (was) ESP_LOGI(TAG, "stopped");
    bsp_display_lock(-1);
    close_lv();
    lv_display_trigger_activity(NULL);
    bsp_display_unlock();
}

bool pomodoro_pause(int mode)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool ok = s_p.active;
    if (ok) {
        bool pause = mode == 2 ? !s_p.paused : mode == 1;
        if (pause) pomo_pause(&s_p, now_ms()); else pomo_resume(&s_p, now_ms());
    }
    xSemaphoreGive(s_mu);
    return ok;
}

bool pomodoro_skip(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    pomo_ev_t ev = pomo_skip(&s_p, now_ms());
    xSemaphoreGive(s_mu);
    if (ev == POMO_NONE) return false;
    phase_cue(ev, false);
    return true;
}

static const char *state_word(const snap_t *s) { return !s->active ? "idle" : s->paused ? "paused" : "running"; }

size_t pomodoro_info_json(char *out, size_t n)
{
    snap_t s; take_snap(&s);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    double f = s_focus_min, b = s_break_min, lg = s_long_min;
    xSemaphoreGive(s_mu);
    if (!s.active)
        return (size_t)snprintf(out, n, "{\"state\":\"idle\",\"focus_min\":%g,\"break_min\":%g,\"long_min\":%g}", f, b, lg);
    int rem = (int)((s.len_ms - s.elapsed_ms + 999) / 1000);
    return (size_t)snprintf(out, n,
        "{\"state\":\"%s\",\"phase\":\"%s\",\"remaining_s\":%d,\"elapsed_s\":%d,\"phase_s\":%d,\"rounds_done\":%d,"
        "\"focus_min\":%g,\"break_min\":%g,\"long_min\":%g,\"showing\":%s}",
        state_word(&s), pomo_phase_name(s.phase), rem, (int)(s.elapsed_ms / 1000), (int)(s.len_ms / 1000), s.done, f, b, lg,
        pomodoro_showing() ? "true" : "false");
}

bool pomodoro_command(const char *body, char *out, size_t n)
{
    char cmd[16] = "";
    double a[3] = { 0, 0, 0 };
    int na = 0;
    while (*body == ' ' || *body == '\n' || *body == '\r') body++;
    if (*body == '{') {
        cJSON *j = cJSON_Parse(body);
        if (!j) { snprintf(out, n, "{\"error\":\"bad JSON\"}"); return false; }
        const cJSON *c = cJSON_GetObjectItem(j, "action");
        if (cJSON_IsString(c)) strlcpy(cmd, c->valuestring, sizeof cmd);
        const char *keys[3] = { "focus", "break", "long" };
        for (int i = 0; i < 3; i++) { const cJSON *v = cJSON_GetObjectItem(j, keys[i]); if (cJSON_IsNumber(v)) a[i] = v->valuedouble; }
        cJSON_Delete(j);
    } else {
        char rest[80] = "";
        sscanf(body, "%15s %79[^\n]", cmd, rest);
        char *p = rest, *e;
        while (na < 3) { double v = strtod(p, &e); if (e == p) break; a[na++] = v; p = e; }
    }
    bool ok = true;
    if (!strcmp(cmd, "start")) {
        if (!pomodoro_start(a[0], a[1], a[2])) { snprintf(out, n, "{\"error\":\"minutes must be 0.1 to 99\"}"); return false; }
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "cancel")) pomodoro_stop();
    else if (!strcmp(cmd, "pause")) ok = pomodoro_pause(1);
    else if (!strcmp(cmd, "resume")) ok = pomodoro_pause(0);
    else if (!strcmp(cmd, "toggle")) ok = pomodoro_pause(2);
    else if (!strcmp(cmd, "skip")) ok = pomodoro_skip();
    else if (strcmp(cmd, "status") && cmd[0]) { snprintf(out, n, "{\"error\":\"body: start [focus_min [break_min [long_min]]] | stop | pause | resume | toggle | skip | status\"}"); return false; }
    else if (!cmd[0]) { snprintf(out, n, "{\"error\":\"body: start [focus_min [break_min [long_min]]] | stop | pause | resume | toggle | skip | status\"}"); return false; }
    if (!ok) { snprintf(out, n, "{\"error\":\"no timer running\"}"); return false; }
    pomodoro_info_json(out, n);
    return true;
}

void pomodoro_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    const esp_timer_create_args_t a = { .callback = esp_tick, .name = "pomodoro" };
    esp_timer_create(&a, &s_esp_timer);
    esp_timer_start_periodic(s_esp_timer, 200 * 1000);
}
