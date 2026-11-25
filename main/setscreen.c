#include "setscreen.h"
#include "restart.h"
#include "display.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "fonts.h"
#include "dial.h"
#include "power.h"
#include "prefs.h"
#include "overlay.h"
#include "audio_out.h"
#include "intercom_ws.h"
#include "settings.h"
#include "pill_list.h"
#include "esp_system.h"

#define COL_RAISE 0x4A3752
#define COL_LINE  0x6B5575
#define COL_HAIR  0x8B7098
#define COL_INK   0xF8ECE8
#define COL_INK2  0xCDB5C6
#define COL_ROSE  0xF5A99B

#define SL_FROM   230.0f         /* brightness slider on r 214: 8 o'clock (bottom) ... */
#define SL_TO     310.0f         /* ... to 10 o'clock (top); fills upward */
#define VL_FROM   130.0f         /* volume slider on r 214: 4 o'clock (bottom) ... */
#define VL_TO     50.0f          /* ... to 2 o'clock (top); fills upward, mirroring brightness */
#define IDLE_MS   60000          /* spec 8.3 */
#define HINT_MS   1500           /* the brightness readout lingers this long after a drag */

static const char *TAG = "settings";
static const int SLEEP_STEPS[] = { 1, 2, 5, 10, 30, 0 };   /* minutes; 0 = never */
static const int GAP_STEPS[] = { 30, 60, 120, 180 };       /* RIM GAP, minutes (12-hour rim, Nov 22) */

static lv_obj_t *s_scr, *s_hint;
static pill_list_t *s_pl;     /* the six setting rows, a scrolling pill list (spec 7.8) */
static bool s_open;
static uint32_t s_hint_at;
#define HOLD_MS 800
static uint32_t s_hold_from;  /* a row hold in progress: FLASHLIGHT or WI-FI */
static int s_hold_what;       /* 1 flashlight, 2 switch network, 3 phone link */
static uint32_t s_restart_at; /* LINK was held: restart once the hint has shown */
static int s_fill_h;
static uint32_t s_net_at, s_hint_ms = 1500;
static int s_net_seen;        /* the switch_state last shown */

static float slider_deg(void) { return SL_FROM + (SL_TO - SL_FROM) * (prefs_brightness() - 10) / 90.0f; }
static float volume_deg(void) { return VL_FROM + (VL_TO - VL_FROM) * prefs_volume() / 100.0f; }
static int s_volume_at_open;

static void pt(float deg, float r, float *x, float *y)
{
    float a = deg * 3.14159265f / 180;
    *x = 233 + r * sinf(a);
    *y = 233 - r * cosf(a);
}

/* ---- drawing: crescent, the slider (track hair 60%, rose fill upward, ink knob), the sun ---- */

static void sun(lv_layer_t *l, float cx, float cy)
{
    dial_draw_ring(l, cx, cy, 5.5f, 2, COL_INK2);
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(COL_INK2); d.width = 2; d.round_start = 1; d.round_end = 1;
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4, s = sinf(a), c = cosf(a);
        d.p1 = (lv_point_precise_t){ cx + 7.5f * s, cy - 7.5f * c };
        d.p2 = (lv_point_precise_t){ cx + 9.5f * s, cy - 9.5f * c };
        lv_draw_line(l, &d);
    }
}

/* Speaker mark (spec 5.10 `sound`, 24 px grid) at (cx, cy); waves only when not muted. */
static void speaker(lv_layer_t *l, float cx, float cy, bool waves)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(COL_INK2); d.width = 2; d.round_start = 1; d.round_end = 1;
    static const float body[] = { 4, 9.5f, 7.5f, 9.5f, 12, 6, 12, 18, 7.5f, 14.5f, 4, 14.5f };
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        d.p1 = (lv_point_precise_t){ cx - 12 + body[2 * i], cy - 12 + body[2 * i + 1] };
        d.p2 = (lv_point_precise_t){ cx - 12 + body[2 * j], cy - 12 + body[2 * j + 1] };
        lv_draw_line(l, &d);
    }
    if (!waves) {                                              /* muted: a cross instead of the waves */
        d.p1 = (lv_point_precise_t){ cx + 3, cy - 3 }; d.p2 = (lv_point_precise_t){ cx + 9, cy + 3 }; lv_draw_line(l, &d);
        d.p1 = (lv_point_precise_t){ cx + 9, cy - 3 }; d.p2 = (lv_point_precise_t){ cx + 3, cy + 3 }; lv_draw_line(l, &d);
        return;
    }
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.color = lv_color_hex(COL_INK2); a.width = 2; a.rounded = 1;
    a.center.x = (int)cx; a.center.y = (int)cy;
    a.radius = 5; a.start_angle = 315; a.end_angle = 45; lv_draw_arc(l, &a);
    a.radius = 9; lv_draw_arc(l, &a);
}

static void draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    dial_draw_crescent(l);                                     /* no rim dots, now dot or moon here */
    float k = slider_deg(), kx, ky;
    dial_draw_arc_opa(l, 214, 10, SL_FROM, SL_TO, COL_HAIR, LV_OPA_60);
    dial_draw_arc(l, 214, 10, SL_FROM, k, COL_ROSE);
    pt(k, 214, &kx, &ky);
    dial_draw_disc(l, kx, ky, 10, COL_INK);
    sun(l, 90, 358);
    float v = volume_deg(), vx, vy;                           /* volume: the right of the rim */
    dial_draw_arc_opa(l, 214, 10, VL_TO, VL_FROM, COL_HAIR, LV_OPA_60);
    if (prefs_volume() > 0) dial_draw_arc(l, 214, 10, v, VL_FROM, COL_ROSE);
    pt(v, 214, &vx, &vy);
    dial_draw_disc(l, vx, vy, 10, COL_INK);
    speaker(l, 376, 358, prefs_volume() > 0);
    dial_draw_hold_fill(l, s_fill_h, COL_ROSE, 90);
}

/* ---- the slider: drag along the left of the rim ---- */

static void slider_area(lv_area_t *a) { *a = (lv_area_t){ 0, 70, 120, 400 }; }

static void slider_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING) {
        lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
        float deg = atan2f(p.x - 233.0f, -(p.y - 233.0f)) * 180 / 3.14159265f;
        if (deg < 0) deg += 360;
        if (deg < 180) deg = p.y > 233 ? SL_FROM : SL_TO;        /* dragged past the bottom or top */
        deg = deg < SL_FROM ? SL_FROM : deg > SL_TO ? SL_TO : deg;
        int pct = 10 + (int)lroundf((deg - SL_FROM) / (SL_TO - SL_FROM) * 90);
        if (pct != prefs_brightness()) {
            prefs_set_brightness(pct);
            power_apply_brightness();
            lv_area_t a; slider_area(&a);
            lv_obj_invalidate_area(s_scr, &a);
        }
        char b[24]; snprintf(b, sizeof b, "BRIGHTNESS %d%%", prefs_brightness());
        lv_label_set_text(s_hint, b);
        s_hint_at = lv_tick_get();
    }
}

/* ---- the volume slider: drag along the right of the rim; a short tone on release at the new level ---- */

static void volume_area(lv_area_t *a) { *a = (lv_area_t){ 346, 70, 465, 400 }; }

static void volume_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED || c == LV_EVENT_PRESSING) {
        lv_point_t p; lv_indev_get_point(lv_indev_active(), &p);
        float deg = atan2f(p.x - 233.0f, -(p.y - 233.0f)) * 180 / 3.14159265f;
        if (deg < 0) deg += 360;
        if (deg > 180) deg = p.y > 233 ? VL_FROM : VL_TO;        /* dragged past the bottom or top */
        deg = deg > VL_FROM ? VL_FROM : deg < VL_TO ? VL_TO : deg;
        int pct = (int)lroundf((VL_FROM - deg) / (VL_FROM - VL_TO) * 20) * 5;   /* steps of 5; 0 = muted */
        if (pct != prefs_volume()) {
            prefs_set_volume(pct);
            audio_out_set_volume(pct);
            lv_area_t a; volume_area(&a);
            lv_obj_invalidate_area(s_scr, &a);
        }
        char b[24];
        if (prefs_volume()) snprintf(b, sizeof b, "VOLUME %d%%", prefs_volume()); else snprintf(b, sizeof b, "MUTED");
        lv_label_set_text(s_hint, b);
        s_hint_at = lv_tick_get();
    } else if (c == LV_EVENT_RELEASED) {
        audio_out_tone(660, 120);                              /* hear the new level */
    }
}

/* ---- rows: spec 5.8 pills in a scrolling list (pill_list.c): label as the title, the current value right-aligned.
   Every row is 270 x 52 at a 60 px pitch in a viewport x 98..368, y 92..364 (inside r 190 at every point, clear of the
   slider bands, the sun and speaker marks and the hint line at y 404). Four rows show; FLASHLIGHT, WI-FI and LINK sit
   below the bottom fade until the list is dragged up. ---- */
#define LIST_X    98
#define LIST_Y    92
#define LIST_W    270
#define LIST_H    272
#define ROW_H     52
enum { R_SLEEP, R_QUIET, R_GAP, R_FLASH, R_NET, R_LINK, R_N };
static char s_rt[R_N][16], s_rv[R_N][20];     /* what each row shows now, so refreshes only touch changed rows */

static void set_row(int i, const char *title, const char *val)
{
    if (i < pill_list_count(s_pl) && !strcmp(s_rt[i], title) && !strcmp(s_rv[i], val)) return;
    snprintf(s_rt[i], sizeof s_rt[i], "%s", title);
    snprintf(s_rv[i], sizeof s_rv[i], "%s", val);
    pill_row_t r = { .title = s_rt[i], .value = s_rv[i], .tappable = i <= R_GAP };   /* the hold rows take raw press events */
    pill_list_set(s_pl, i, &r);
    if (i >= R_FLASH) lv_obj_add_flag(pill_list_row_obj(s_pl, i), LV_OBJ_FLAG_CLICKABLE);   /* pill_list_set clears it on an update */
}

static void sleep_text(char *b, size_t n)
{
    int m = prefs_sleep_min();
    if (m) snprintf(b, n, "%d MIN", m); else snprintf(b, n, "NEVER");
}

static void sleep_tap(void)
{
    int m = prefs_sleep_min(), n = sizeof SLEEP_STEPS / sizeof SLEEP_STEPS[0], i = 0;
    while (i < n && SLEEP_STEPS[i] != m) i++;
    prefs_set_sleep_min(SLEEP_STEPS[(i + 1) % n]);             /* an odd saved value restarts at 1 MIN */
    char b[16]; sleep_text(b, sizeof b);
    set_row(R_SLEEP, "SLEEP AFTER", b);
}

/* RIM GAP: how much of the rim stays empty behind the planet (0:30, 1:00, 2:00, 3:00). A tap steps to the next value;
   the face redraws when Settings closes, and the value is saved then (prefs_save). The bus widget dial_config can set
   it too; the last writer wins. */
static void gap_text(char *b, size_t n)
{
    int m = dial_gap_min();
    snprintf(b, n, "%d:%02d", m / 60, m % 60);
}

static void gap_tap(void)
{
    int m = dial_gap_min(), n = sizeof GAP_STEPS / sizeof GAP_STEPS[0], i = 0;
    while (i < n && GAP_STEPS[i] != m) i++;
    prefs_set_gap_min(GAP_STEPS[(i + 1) % n]);
    char b[16]; gap_text(b, sizeof b);
    set_row(R_GAP, "RIM GAP", b);
}

/* QUIET HOURS: ON / OFF from the schedule (22:00-06:30) or a manual flip, which holds until the next change. */
static void quiet_tap(void)
{
    power_quiet_toggle();
    set_row(R_QUIET, "QUIET HOURS", power_quiet() ? "ON" : "OFF");
}

/* The list reports taps only (never a drag or a swipe: pill_list.c checks compact_press_was_drag). */
static void row_tap(int i, void *ud)
{
    (void)ud;
    if (i == R_SLEEP) sleep_tap();
    else if (i == R_QUIET) quiet_tap();
    else if (i == R_GAP) gap_tap();
}

/* WI-FI shows the network in use (no password entry on the device). The label reads WI-FI 2 on the secondary.
   The name is shown in capitals, ASCII only (an iPhone's curly apostrophe becomes '), at most 13 characters.
   Held 0.8 s it switches to the other configured network, if a scan sees it (the hint says how it went). */

static void net_text(char *label, size_t ln, char *val, size_t n)
{
    intercom_net_t net;
    intercom_net_info(&net);
    snprintf(label, ln, net.slot == 2 ? "WI-FI 2" : "WI-FI");
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)net.ssid;
    for (; *p && o < 13 && o + 1 < n; p++) {
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) { val[o++] = '\''; p += 2; }
        else if (*p >= 0x20 && *p < 0x7F) val[o++] = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
    }
    val[o] = 0;
    if (intercom_ble_mode()) snprintf(val, n, "OFF");          /* PHONE-mode boot: no Wi-Fi */
    else if (!o) snprintf(val, n, "NONE");
    else if (!net.online) snprintf(val + (o > 9 ? 9 : o), n - (o > 9 ? 9 : o), " ...");   /* searching */
}

static void hint(const char *text, uint32_t ms)
{
    lv_label_set_text(s_hint, text);
    s_hint_at = lv_tick_get() ? lv_tick_get() : 1;
    s_hint_ms = ms;
}

static void net_refresh(void)
{
    if (!s_pl) return;
    char nl[12], nv[20];
    net_text(nl, sizeof nl, nv, sizeof nv);
    set_row(R_NET, nl, nv);
}

static void net_switch(void)
{
    if (!intercom_net_switch()) { hint("NO OTHER NETWORK SET", 2500); return; }
    s_net_seen = 1;
    hint("LOOKING FOR IT", 8000);
}

/* LINK: AUTO normally (Wi-Fi first, the phone as the fallback), PHONE on a PHONE-mode boot (Bluetooth to the
   relay only; no Wi-Fi). Held 0.8 s it restarts into the other one. PHONE lasts one boot: any later restart
   comes back in AUTO. */
static void link_switch(void)
{
    if (!intercom_ble_mode() && !settings_link_ble_next_boot()) { hint("COULD NOT SAVE", 2500); return; }
    hint(intercom_ble_mode() ? "RESTARTING IN AUTO" : "RESTARTING ON PHONE", 3000);
    s_restart_at = lv_tick_get() ? lv_tick_get() : 1;
    ESP_LOGW(TAG, "link switch: restarting");
}

/* FLASHLIGHT is held 0.8 s (the screen fills from the bottom, like every hold); PWR or BOOT ends it.
   WI-FI is held the same way. */

static void flash_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        s_hold_from = lv_tick_get() ? lv_tick_get() : 1;
        s_hold_what = (int)(intptr_t)lv_event_get_user_data(e);
    } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && s_hold_from) {
        s_hold_from = 0;                                       /* let go early: nothing happens */
        dial_hold_fill_step(s_scr, &s_fill_h, 0);
    }
}

/* A hold row (FLASHLIGHT, WI-FI, LINK): the list's row object takes the raw press and release events. A drag that
   becomes a scroll sends PRESS_LOST, which cancels the hold. */
static void hold_row(int i, int what)
{
    lv_obj_t *r = pill_list_row_obj(s_pl, i);
    if (!r) return;
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(r, 2);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_HAIR), LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, flash_cb, LV_EVENT_ALL, (void *)(intptr_t)what);
}

static void draw_post(lv_event_t *e)
{
    pill_list_draw_post(s_pl, lv_event_get_layer(e));          /* the 40 px fades, over the rows */
}

/* ---- open / close ---- */

static void build(void)
{
    lv_obj_clean(s_scr);
    pill_list_cfg_t pc = { .x = LIST_X, .y = LIST_Y, .w = LIST_W, .h = LIST_H, .row_w = LIST_W, .row_h = ROW_H, .gap = 8,
                           .icon = 0, .width = PILL_W_FIXED, .tap = row_tap };
    s_pl = pill_list_create(s_scr, &pc);
    for (int i = 0; i < R_N; i++) s_rt[i][0] = 0;
    char b[16]; sleep_text(b, sizeof b);
    set_row(R_SLEEP, "SLEEP AFTER", b);
    set_row(R_QUIET, "QUIET HOURS", power_quiet() ? "ON" : "OFF");
    char g[16]; gap_text(g, sizeof g);
    set_row(R_GAP, "RIM GAP", g);
    set_row(R_FLASH, "FLASHLIGHT", "HOLD");
    char nl[12], nv[20];
    net_text(nl, sizeof nl, nv, sizeof nv);
    set_row(R_NET, nl, nv);
    set_row(R_LINK, "LINK", intercom_ble_mode() ? "PHONE" : "AUTO");
    hold_row(R_FLASH, 1);
    hold_row(R_NET, 2);
    hold_row(R_LINK, 3);
    s_net_at = lv_tick_get();
    intercom_net_t net;
    intercom_net_info(&net);
    s_net_seen = net.switch_state;
    s_hold_from = 0;
    s_hold_what = 0;
    s_fill_h = 0;
    s_hint = lv_label_create(s_scr);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_font(s_hint, f_meta13, 0);
    lv_obj_set_style_text_letter_space(s_hint, 2, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(COL_INK2), 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 404);
    s_hint_at = 0;
    /* the slider's touch band: its drags must not become screen swipes */
    lv_obj_t *t = lv_obj_create(s_scr);
    lv_obj_remove_style_all(t);
    lv_area_t a; slider_area(&a);
    lv_obj_set_pos(t, a.x1, a.y1);
    lv_obj_set_size(t, lv_area_get_width(&a) - 24, lv_area_get_height(&a));
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(t, slider_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *v = lv_obj_create(s_scr);
    lv_obj_remove_style_all(v);
    volume_area(&a);
    lv_obj_set_pos(v, a.x1 + 24, a.y1);
    lv_obj_set_size(v, lv_area_get_width(&a) - 24, lv_area_get_height(&a));
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(v, volume_cb, LV_EVENT_ALL, NULL);
    s_volume_at_open = prefs_volume();
}

void setscreen_open_lv(void)
{
    if (s_open) return;
    s_open = true;
    build();
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "open");
}

void setscreen_close_lv(void)
{
    if (!s_open) return;
    s_open = false;
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_scr);
    s_pl = NULL;
    lv_obj_invalidate(lv_scr_act());
    prefs_save();
    if (prefs_volume() != s_volume_at_open)                    /* the bus skips speech at volume 0 */
        intercom_queue_state(power_state() == PWR_SLEEP, power_battery_pct());
}

static void tick_cb(lv_timer_t *t)
{
    if (!s_open) return;
    if (s_restart_at && lv_tick_elaps(s_restart_at) >= 700) { s_restart_at = 0; compact_restart("settings: link switch"); return; }
    if (s_hold_from) {
        uint32_t ms = lv_tick_elaps(s_hold_from);
        dial_hold_fill_step(s_scr, &s_fill_h, ms >= HOLD_MS ? 466 : (int)(ms * 466 / HOLD_MS));
        if (ms >= HOLD_MS) {
            s_hold_from = 0;
            dial_hold_fill_step(s_scr, &s_fill_h, 0);
            if (s_hold_what == 2) net_switch();
            else if (s_hold_what == 3) link_switch();
            else overlay_flashlight_lv();
        }
        return;
    }
    if (lv_display_get_inactive_time(NULL) >= IDLE_MS) { setscreen_close_lv(); return; }
    if (s_pl) set_row(R_QUIET, "QUIET HOURS", power_quiet() ? "ON" : "OFF");   /* the schedule may flip it while open */
    if (lv_tick_elaps(s_net_at) >= 500) {                      /* the network row follows roaming and switches */
        s_net_at = lv_tick_get();
        net_refresh();
        intercom_net_t net;
        intercom_net_info(&net);
        if (net.switch_state != s_net_seen) {
            s_net_seen = net.switch_state;
            if (net.switch_state == 2) hint(net.slot == 2 ? "JOINING WI-FI 2" : "JOINING WI-FI", 4000);
            else if (net.switch_state == 3) hint("NOT IN RANGE", 3000);
        }
    }
    if (s_hint_at && lv_tick_elaps(s_hint_at) >= s_hint_ms) {
        s_hint_at = 0;
        s_hint_ms = HINT_MS;
        lv_label_set_text(s_hint, "");
    }
}

void setscreen_init(void)
{
    bsp_display_lock(-1);
    s_scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    dial_use_ground(s_scr);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_scr, draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(s_scr, draw_post, LV_EVENT_DRAW_POST_END, NULL);
    lv_timer_create(tick_cb, 20, NULL);                       /* returns at once while closed */
    bsp_display_unlock();
}

void setscreen_close(void)
{
    bsp_display_lock(-1);
    setscreen_close_lv();
    bsp_display_unlock();
}

bool setscreen_is_open(void) { return s_open; }

/* The swipe-up close (earlier.c's router). With rows to scroll to, a vertical drag that starts on the list is a scroll,
   so only a swipe up that starts outside it (above or below the viewport) closes; a short list closes from anywhere.
   The 60 s idle timeout and a press of PWR close it too. */
void setscreen_swipe_up_lv(void)
{
    if (!s_open) return;
    if (s_pl && pill_list_overflows(s_pl) && pill_list_contains(s_pl, lv_indev_get_active_obj())) return;
    setscreen_close_lv();
}

/* POST /debug/ui "settings-scroll <dy>": scroll the list by dy px (animated, clamped). */
bool setscreen_debug_scroll(int dy)
{
    bsp_display_lock(-1);
    bool ok = s_open && s_pl;
    if (ok) pill_list_scroll_by(s_pl, dy);
    bsp_display_unlock();
    return ok;
}
