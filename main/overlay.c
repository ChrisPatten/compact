#include "overlay.h"
#include "restart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "fonts.h"
#include "power.h"
#include "display.h"
#include "dial.h"

/* Plum Dusk tokens (spec 3.1) */
#define COL_HAIR     0x8B7098
#define COL_INK      0xF8ECE8
#define COL_INK2     0xCDB5C6
#define COL_ROSE     0xF5A99B
#define COL_ROSE_INK 0x2A1A2C
#define COL_PERI     0xA9B8FF
#define COL_AMBER    0xF7C95C
#define TRACK 3
#define HOLD_MS 800             /* spec 8.1: hold 0.8 s confirms anything that cannot be undone */
#define MENU_IDLE_MS 15000      /* the power menu closes itself back to Now */
#define NOTICE_MS 2500

static const char *TAG = "overlay";
static lv_obj_t *s_root;        /* the one open overlay, or NULL */
static lv_timer_t *s_timer;     /* auto-close / hold progress */

/* ---- building blocks ---- */

/* Lexend Exa is very wide: size a pill to its label with 28 px either side, never under `min_w`. */
static int fit_w(const char *text, int min_w)
{
    lv_point_t sz;
    lv_text_get_size(&sz, text, f_label15, TRACK, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x + 56;
    return w > min_w ? w : min_w;
}

static int s_nholds;

static lv_obj_t *s_bat_num;
static lv_obj_t *s_chg_line;     /* the Charging screen's status line */
static void close_locked(void)
{
    s_nholds = 0;
    s_bat_num = NULL;
    s_chg_line = NULL;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_root) { lv_obj_delete(s_root); s_root = NULL; }
}

static lv_obj_t *ground(void)
{
    close_locked();
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 466, 466);
    dial_use_ground(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);    /* swallow taps meant for the face below */
    s_root = o;
    lv_display_trigger_activity(NULL);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col, int y, int space)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(l, space, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, int w, int h, int y, uint32_t border)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, fit_w(text, w), h);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 8);               /* hit target >= 76 px (spec 11.3) */
    /* pressed: the segment inverts to ink within 100 ms (spec 5.x capsule) */
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_INK), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f_label15, 0);
    lv_obj_set_style_text_letter_space(l, TRACK, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_ROSE_INK), LV_STATE_PRESSED);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(l);
    return b;
}

/* ---- power menu ---- */

/* A hold-to-confirm pill (spec 5.x Holding): rose border; a rose fill wipes left to right over
   0.8 s; release early = cancel, no side effects. */
typedef struct {
    lv_obj_t *label;
    int w;
    uint32_t from;              /* lv_tick when the hold began, 0 when not held */
    const char *done_text;      /* shown once confirmed */
    lv_timer_cb_t action;
} hold_pill_t;

static hold_pill_t s_holds[2];
static int s_fill_h;            /* hold progress: the menu screen fills from the bottom */

/* Battery on the power menu: the charge as an arc on the inner lane (r 197), clockwise from 12 o'clock
   over a faint track; rose, or amber when low on battery. The number and state sit under the caption. */
static int s_bat_pct = -2, s_bat_chg = -1;
static lv_obj_t *s_bat_unit, *s_bat_state;
static uint32_t s_bat_at;
static volatile bool s_bat_poke;   /* USB plugged or unplugged (power task): refresh at once */
#define BAT_POLL_MS 500            /* only while the power menu is open */

void overlay_battery_changed(void)
{
    s_bat_poke = true;
    bsp_display_lock(-1);
    lv_obj_invalidate(lv_scr_act());       /* Now redraws its status icons at once */
    bsp_display_unlock();
}

static bool bat_low(void) { return s_bat_chg == PWR_CHG_BATTERY && s_bat_pct >= 0 && s_bat_pct < 20; }

static void menu_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    dial_draw_base(l, COL_ROSE);
    dial_draw_top_caption(l, "POWER", COL_ROSE);
    if (s_bat_pct >= 0) {
        dial_draw_arc_opa(l, 197, 3, 0, 359.9f, COL_HAIR, LV_OPA_30);
        if (s_bat_pct > 0) dial_draw_arc(l, 197, 3, 0, s_bat_pct * 3.6f, bat_low() ? COL_AMBER : COL_ROSE);
    }
    dial_draw_hold_fill(l, s_fill_h, COL_ROSE, 90);
}

static void bat_show(void)
{
    power_battery_refresh();
    int pct = power_battery_pct(), chg = power_charge_state();
    s_bat_at = lv_tick_get();
    if (pct == s_bat_pct && chg == s_bat_chg) return;
    s_bat_pct = pct; s_bat_chg = chg;
    char n[12]; snprintf(n, sizeof n, "%d", pct < 0 ? 0 : pct);
    lv_label_set_text(s_bat_num, pct < 0 ? "" : n);
    lv_label_set_text(s_bat_unit, pct < 0 ? "" : "%");
    /* centre the numeral and its % together, baselines level */
    lv_point_t a, b;
    lv_text_get_size(&a, lv_label_get_text(s_bat_num), f_num_m, -1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&b, lv_label_get_text(s_bat_unit), f_label, TRACK, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int x0 = 233 - (a.x + 6 + b.x) / 2, base = 132;
    lv_obj_set_align(s_bat_num, LV_ALIGN_DEFAULT);
    lv_obj_set_align(s_bat_unit, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(s_bat_num, x0, base - (lv_font_get_line_height(f_num_m) - f_num_m->base_line));
    lv_obj_set_pos(s_bat_unit, x0 + a.x + 6, base - (lv_font_get_line_height(f_label) - f_label->base_line));
    const char *word = pct < 0 ? "USB POWER" : chg == PWR_CHG_CHARGING ? "CHARGING" : chg == PWR_CHG_FULL ? "CHARGED"
                     : chg == PWR_CHG_USB ? "PLUGGED IN" : bat_low() ? "LOW \xC2\xB7 PLUG IN" : "ON BATTERY";
    uint32_t col = bat_low() ? COL_AMBER : (chg == PWR_CHG_CHARGING || chg == PWR_CHG_FULL) ? COL_ROSE : COL_INK2;
    lv_label_set_text(s_bat_state, word);
    lv_obj_set_style_text_color(s_bat_state, lv_color_hex(col), 0);
    lv_obj_align(s_bat_state, LV_ALIGN_TOP_MID, 0, 142);
    lv_obj_set_style_text_color(s_bat_unit, lv_color_hex(bat_low() ? COL_AMBER : COL_ROSE), 0);
    if (s_root) lv_obj_invalidate(s_root);
}

static void do_restart(lv_timer_t *t) { (void)t; compact_restart("power menu"); }   /* the timer repeats until the reset: a no-op */
static void do_power_off(lv_timer_t *t) { power_off_now(); }

static void menu_tick_cb(lv_timer_t *t)
{
    for (int i = 0; i < s_nholds; i++) {
        hold_pill_t *h = &s_holds[i];
        if (!h->from) continue;
        uint32_t ms = lv_tick_elaps(h->from);
        if (s_root) dial_hold_fill_step(s_root, &s_fill_h, ms >= HOLD_MS ? 466 : (int)(ms * 466 / HOLD_MS));
        if (ms >= HOLD_MS) {
            s_bat_num = s_bat_unit = s_bat_state = NULL;
            const char *text = h->done_text;
            lv_timer_cb_t action = h->action;
            ESP_LOGW(TAG, "%s", text);
            s_nholds = 0;
            lv_obj_t *o = ground();                  /* deletes this timer and the pills */
            label(o, text, f_label, COL_INK2, 222, TRACK);
            s_timer = lv_timer_create(action, 600, NULL);
            lv_timer_set_repeat_count(s_timer, 1);
            return;
        }
        return;                                      /* while holding, the menu does not time out */
    }
    if (lv_display_get_inactive_time(NULL) >= MENU_IDLE_MS) { s_nholds = 0; close_locked(); return; }
    if (s_bat_num && (s_bat_poke || lv_tick_elaps(s_bat_at) >= BAT_POLL_MS)) { s_bat_poke = false; bat_show(); }
}

static void hold_cb(lv_event_t *e)
{
    hold_pill_t *h = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        h->from = lv_tick_get() ? lv_tick_get() : 1;
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (!h->from) return;
        h->from = 0;
        if (s_root) dial_hold_fill_step(s_root, &s_fill_h, 0);
    }
}

static void hold_pill(lv_obj_t *parent, const char *text, int w, int y, const char *done_text, lv_timer_cb_t action)
{
    hold_pill_t *h = &s_holds[s_nholds++];
    *h = (hold_pill_t){ .w = w, .done_text = done_text, .action = action };
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 60);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(COL_ROSE), 0);
    lv_obj_set_style_border_post(b, true, 0);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 8);
    h->label = lv_label_create(b);
    lv_label_set_text(h->label, text);
    lv_obj_set_style_text_font(h->label, f_label15, 0);
    lv_obj_set_style_text_letter_space(h->label, TRACK, 0);
    lv_obj_set_style_text_color(h->label, lv_color_hex(COL_INK), 0);
    lv_obj_center(h->label);
    lv_obj_add_event_cb(b, hold_cb, LV_EVENT_ALL, h);
}

static void cancel_cb(lv_event_t *e) { s_nholds = 0; close_locked(); }

void overlay_power_menu(void)
{
    bsp_display_lock(-1);
    lv_obj_t *o = ground();
    s_fill_h = 0;
    lv_obj_add_event_cb(o, menu_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    /* POWER is the curved top caption; under it the battery: "84 %" in the numeral face, then its state */
    s_bat_num = label(o, "", f_num_m, COL_INK, 0, -1);
    s_bat_unit = label(o, "", f_label, COL_ROSE, 0, TRACK);
    s_bat_state = label(o, "", f_whisper, COL_INK2, 142, TRACK);
    s_bat_pct = -2; s_bat_chg = -1;
    bat_show();
    s_nholds = 0;
    int w = fit_w("HOLD TO POWER OFF", 220);        /* both hold pills share the wider width */
    hold_pill(o, "HOLD TO RESTART", w, 172, "RESTARTING", do_restart);
    hold_pill(o, "HOLD TO POWER OFF", w, 246, "POWERING OFF", do_power_off);
    lv_obj_t *c = pill(o, "CANCEL", 160, 56, 322, COL_HAIR);
    lv_obj_set_style_border_opa(c, LV_OPA_50, 0);
    lv_obj_add_event_cb(c, cancel_cb, LV_EVENT_CLICKED, NULL);
    s_timer = lv_timer_create(menu_tick_cb, 20, NULL);
    bsp_display_unlock();
    ESP_LOGI(TAG, "power menu");
}

/* ---- Capture placeholder ---- */

static void notice_done(lv_timer_t *t) { s_timer = NULL; close_locked(); }

void overlay_capture_soon(void)
{
    bsp_display_lock(-1);
    lv_obj_t *o = ground();
    label(o, "CAPTURE", f_label, COL_ROSE, 190, TRACK);
    label(o, "Coming soon", f_headline, COL_INK, 220, 0);
    s_timer = lv_timer_create(notice_done, NOTICE_MS, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
    bsp_display_unlock();
}

/* Flashlight (Settings): the whole glass white at full brightness, ignoring touch, until
   PWR or BOOT is pressed. The power task keeps it from dimming or sleeping. */
void overlay_flashlight_lv(void)
{
    lv_obj_t *o = ground();
    lv_obj_set_style_bg_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_user_data(o, (void *)overlay_flashlight_lv);
    compact_display_brightness(100);
    ESP_LOGI(TAG, "flashlight on");
}

bool overlay_flashlight_on(void)
{
    lv_obj_t *r = s_root;
    return r && lv_obj_get_user_data(r) == (void *)overlay_flashlight_lv;
}

void overlay_close_all(void)
{
    bsp_display_lock(-1);
    close_locked();
    bsp_display_unlock();
}

bool overlay_open(void)
{
    bsp_display_lock(-1);
    bool open = s_root != NULL;
    bsp_display_unlock();
    return open;
}

/* ---- Charging screen (Nov 11) ---- */

#define COL_NIGHT 0xB07C3A     /* the only ink on true-black screens (sleep version) */
#define CHG_IDLE_MS 60000
#define CHG_PLUG_MS 10000
#define CHG_POLL_MS 5000

static chg_mode_t s_chg_mode;
static uint32_t s_chg_open, s_chg_at;
static int s_chg_min = -1, s_chg_dx, s_chg_dy, s_chg_pct = -2, s_chg_state = -1, s_chg_eta = -2;

static uint32_t chg_ink(void) { return s_chg_mode == CHG_SLEEP ? COL_NIGHT : COL_INK; }
static uint32_t chg_rose(void) { return s_chg_mode == CHG_SLEEP ? COL_NIGHT : COL_ROSE; }

static void chg_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    bool sleep = s_chg_mode == CHG_SLEEP;
    if (!sleep) dial_draw_base(l, COL_ROSE);
    bool low = s_chg_state == PWR_CHG_BATTERY && s_chg_pct >= 0 && s_chg_pct < 20;
    if (s_chg_pct >= 0) {
        dial_draw_arc_opa(l, 197, 3, 0, 359.9f, COL_HAIR, LV_OPA_30);
        if (s_chg_pct > 0) dial_draw_arc(l, 197, 3, 0, s_chg_pct * 3.6f, low ? COL_AMBER : chg_rose());
    }
    dial_draw_bolt(l, 233 + s_chg_dx, 112 + s_chg_dy, 28, chg_rose());
    dial_draw_time(l, s_chg_dx, s_chg_dy, chg_ink(), chg_rose());
}

static void chg_text(char *b, size_t n)
{
    int pct = s_chg_pct, eta = s_chg_eta;
    char p[24] = ""; if (pct >= 0) snprintf(p, sizeof p, "%d%% \xC2\xB7 ", pct);
    switch (s_chg_state) {
    case PWR_CHG_CHARGING:
        if (eta >= 60) snprintf(b, n, "%sFULL IN %d:%02d", p, eta / 60, eta % 60);
        else if (eta > 0) snprintf(b, n, "%sFULL IN %d MIN", p, eta);
        else snprintf(b, n, "%sCHARGING", p);
        break;
    case PWR_CHG_FULL: snprintf(b, n, "CHARGED"); break;
    case PWR_CHG_USB: snprintf(b, n, "%sPLUGGED IN", p); break;
    default: snprintf(b, n, "%sON BATTERY", p); break;
    }
}

static void chg_place(void)
{
    if (s_chg_line) lv_obj_align(s_chg_line, LV_ALIGN_TOP_MID, s_chg_dx, 296 + s_chg_dy);
}

/* Read the battery and redraw only what changed: the label, and the ring when the % moved. */
static void chg_refresh(bool read)
{
    if (read) power_battery_refresh();
    s_chg_at = lv_tick_get();
    int pct = power_battery_pct(), st = power_charge_state(), eta = st == PWR_CHG_CHARGING ? power_full_eta_min() : -1;
    if (pct == s_chg_pct && st == s_chg_state && eta == s_chg_eta) return;
    bool ring = pct != s_chg_pct || (st == PWR_CHG_BATTERY) != (s_chg_state == PWR_CHG_BATTERY);
    s_chg_pct = pct; s_chg_state = st; s_chg_eta = eta;
    char b[40]; chg_text(b, sizeof b);
    if (s_chg_line) {
        lv_label_set_text(s_chg_line, b);
        lv_obj_set_style_text_color(s_chg_line, lv_color_hex(s_chg_mode == CHG_SLEEP ? COL_NIGHT : COL_INK2), 0);
        chg_place();
    }
    if (ring && s_root) lv_obj_invalidate(s_root);
}

static void chg_tick(lv_timer_t *t)
{
    if (!s_root) return;
    if (s_chg_mode == CHG_PLUG && lv_tick_elaps(s_chg_open) >= CHG_PLUG_MS) { close_locked(); return; }
    if (s_chg_mode == CHG_TAP && lv_display_get_inactive_time(NULL) >= CHG_IDLE_MS) { close_locked(); return; }
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    if (lt.tm_min != s_chg_min) {
        s_chg_min = lt.tm_min;
        if (s_chg_mode == CHG_SLEEP) {                      /* drift against burn-in, as the rest face does */
            int m = lt.tm_hour * 60 + lt.tm_min;
            s_chg_dx = (m * 7) % 33 - 16; s_chg_dy = (m * 13) % 33 - 16;
            chg_place();
        }
        lv_obj_invalidate(s_root);
    }
    if (s_bat_poke) { s_bat_poke = false; chg_refresh(true); }
    else if (lv_tick_elaps(s_chg_at) >= CHG_POLL_MS) chg_refresh(true);
}

static void chg_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_BOTTOM) return;
    lv_indev_wait_release(lv_indev_active());
    close_locked();
}

void overlay_charging(chg_mode_t mode)
{
    bsp_display_lock(-1);
    lv_obj_t *o = ground();
    s_chg_mode = mode;
    s_chg_open = lv_tick_get();
    s_chg_min = -1; s_chg_dx = s_chg_dy = 0; s_chg_pct = -2; s_chg_state = -1; s_chg_eta = -2;
    lv_obj_set_user_data(o, (void *)overlay_charging);
    if (mode == CHG_SLEEP) lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    s_chg_line = label(o, "", f_label15, mode == CHG_SLEEP ? COL_NIGHT : COL_INK2, 296, TRACK);
    s_bat_poke = false;
    chg_refresh(true);
    if (mode == CHG_SLEEP) {
        time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
        int m = lt.tm_hour * 60 + lt.tm_min;
        s_chg_min = lt.tm_min; s_chg_dx = (m * 7) % 33 - 16; s_chg_dy = (m * 13) % 33 - 16;
        chg_place();
    }
    lv_obj_add_event_cb(o, chg_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(o, chg_gesture, LV_EVENT_GESTURE, NULL);
    s_timer = lv_timer_create(chg_tick, 250, NULL);
    lv_obj_invalidate(o);
    bsp_display_unlock();
    ESP_LOGI(TAG, "charging screen mode %d", (int)mode);
}

bool overlay_charging_on(void)
{
    bsp_display_lock(-1);
    bool on = s_root && lv_obj_get_user_data(s_root) == (void *)overlay_charging;
    bsp_display_unlock();
    return on;
}
