#include "power.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psram_task.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "display.h"
#include "dial.h"
#include "intercom_ws.h"
#include "overlay.h"
#include "decision.h"
#include "earlier.h"
#include "prefs.h"
#include "setscreen.h"
#include "dayview.h"
#include "ptt.h"
#include "capture.h"
#include "pomodoro.h"
#include "audio_out.h"
#include "thread.h"
#include "agents.h"
#include "screenshot.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

#define AXP_ADDR        0x34
#define AXP_STATUS1     0x00   /* bit5 VBUS good, bit3 battery present */
#define AXP_COMMON_CFG  0x10   /* bit0 software power off */
#define AXP_INTEN2      0x41
#define AXP_INTSTS1     0x48
#define AXP_INTSTS2     0x49   /* bit3 PKEY short, bit2 PKEY long, bit7/6 VBUS insert/remove */
#define AXP_BAT_PCT     0xA4
#define AXP_PKEY_CFG    0x27   /* bits 5:4 long-press IRQ time (11 = 2.5 s), bits 3:2 forced power-off time (10 = 8 s) */
#define PKEY_SHORT      (1 << 3)
#define PKEY_LONG       (1 << 2)   /* fires 2.5 s into a hold */
#define PKEY_NEG        (1 << 1)   /* key pressed */
#define PKEY_POS        (1 << 0)   /* key released */
#define MENU_AFTER_LONG_MS 500     /* long-press IRQ at 2.5 s + 0.5 s = the spec's 3 s hold */
#define BOOT_GPIO       GPIO_NUM_0
#define BOOT_HOLD_MS    400        /* press and hold = push-to-talk */
#define CHORD_MS        400       /* BOOT + PWR pressed within this of each other = screenshot */
#define BOOT_DOUBLE_MS  300        /* window for the second press of a double-press */

#define POLL_MS         200
#define BATTERY_READ_MS 30000
#define BATTERY_USB_MS  10000   /* on USB: faster reads feed the time-to-full estimate */
#define ETA_SAMPLES     128     /* at >= 10 s apart: about 20 min of history */
#define ETA_MIN_SPAN_S  180
#define QUIET_FROM_MIN  (22 * 60)
#define QUIET_TO_MIN    (6 * 60 + 30)

static const char *TAG = "power";
static i2c_master_dev_handle_t s_axp;
static volatile power_state_t s_state = PWR_AWAKE;
static volatile bool s_wake_req, s_sleep_req, s_now_req, s_thread_req;
static bool s_chg_face;         /* asleep on USB with the Charging screen showing (panel on); power task only */
static int s_batt = -1;
static volatile int s_chg;      /* power_charge_t from AXP2101 REG01 bits 2:0 */
static bool s_usb = true, s_wifi_off;
static char s_seen_urgent[512];
static int s_wake_render_ms = -1, s_wake_total_ms = -1;

void power_wake_timing(int *render_ms, int *total_ms) { *render_ms = s_wake_render_ms; *total_ms = s_wake_total_ms; }

/* ---- AXP2101 ---- */

static esp_err_t axp_read(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, v, 1, 50);
}

static esp_err_t axp_write(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_axp, b, 2, 50);
}

static void axp_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AXP_ADDR, .scl_speed_hz = 400000 };
    if (!bus || i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) { ESP_LOGE(TAG, "AXP2101 not reachable"); s_axp = NULL; return; }
    uint8_t en = 0;
    axp_read(AXP_INTEN2, &en);
    axp_write(AXP_INTEN2, en | PKEY_SHORT | PKEY_LONG | PKEY_NEG | PKEY_POS | 0xC0);   /* PWR key + VBUS insert/remove */
    uint8_t k = 0;
    if (axp_read(AXP_PKEY_CFG, &k) == ESP_OK) {
        ESP_LOGI(TAG, "PWR key config 0x%02x (forced off after %d s)", k, (int[]){4, 6, 8, 10}[(k >> 2) & 3]);
        /* long-press IRQ at 2.5 s, the longest it offers; forced power-off at 8 s */
        axp_write(AXP_PKEY_CFG, (uint8_t)((k | 0x30) & ~0x0C) | 0x08);
    }
    for (int i = 0; i < 3; i++) axp_write(AXP_INTSTS1 + i, 0xFF);
}

/* Time to full: samples of the battery % while charging (power task and others call read_battery). */
static struct { int64_t t_us; int pct; } s_eta[ETA_SAMPLES];
static int s_eta_n;

static void eta_note(bool charging)
{
    int64_t now = esp_timer_get_time();
    if (!charging || s_batt < 0) { s_eta_n = 0; return; }
    if (s_eta_n && now - s_eta[s_eta_n - 1].t_us < 5000000LL) return;
    if (s_eta_n == ETA_SAMPLES) { memmove(&s_eta[0], &s_eta[1], sizeof s_eta - sizeof s_eta[0]); s_eta_n--; }
    s_eta[s_eta_n].t_us = now; s_eta[s_eta_n].pct = s_batt; s_eta_n++;
}

int power_full_eta_min(void)
{
    if (s_chg == PWR_CHG_FULL && s_batt >= 100) return 0;
    if (s_chg != PWR_CHG_CHARGING || s_batt < 0) return -1;
    int n = s_eta_n;
    if (n < 2) return -1;
    int64_t span = s_eta[n - 1].t_us - s_eta[0].t_us;
    int rise = s_eta[n - 1].pct - s_eta[0].pct;
    if (span < ETA_MIN_SPAN_S * 1000000LL || rise < 2) return -1;
    int remain = 100 - s_eta[n - 1].pct;
    if (remain <= 0) return 0;
    int64_t secs = (int64_t)remain * (span / 1000000LL) / rise;   /* minutes = remaining % / (% per minute) */
    int m = (int)((secs + 59) / 60);
    return m < 1 ? 1 : m;
}

/* `note`: feed the time-to-full history (power task only; other tasks just refresh the reading). */
static void read_battery_x(bool note)
{
    uint8_t st = 0, pct = 0;
    if (!s_axp || axp_read(AXP_STATUS1, &st) != ESP_OK) return;
    s_usb = st & (1 << 5);
    s_batt = (st & (1 << 3)) && axp_read(AXP_BAT_PCT, &pct) == ESP_OK ? pct : -1;
    uint8_t st2 = 0;                                          /* REG01 2:0: 1-3 charging, 4 done */
    int c = s_usb && axp_read(0x01, &st2) == ESP_OK ? st2 & 0x07 : 7;
    /* 100% is FULL even while REG01 still says constant-voltage charging */
    s_chg = !s_usb ? PWR_CHG_BATTERY : s_batt >= 100 ? PWR_CHG_FULL : (c >= 1 && c <= 3) ? PWR_CHG_CHARGING : c == 4 ? PWR_CHG_FULL : PWR_CHG_USB;
    if (note) eta_note(s_chg == PWR_CHG_CHARGING);
}

static void read_battery(void) { read_battery_x(true); }

/* PWR key and USB events since the last call (INTSTS2 bits). The BOOT task also polls (chord detection), so the
   read-and-clear accumulates into s_axp_pend under a mutex and the power task takes it from there. */
static SemaphoreHandle_t s_axp_mu;
static uint8_t s_axp_pend;
static volatile int64_t s_pwr_neg_us;                         /* when a PWR press was last seen (0 = none unclaimed) */

static void axp_poll(void)
{
    if (!s_axp || !s_axp_mu) return;
    xSemaphoreTake(s_axp_mu, portMAX_DELAY);
    uint8_t st = 0;
    if (axp_read(AXP_INTSTS2, &st) == ESP_OK && st) {
        axp_write(AXP_INTSTS2, st);                           /* write-1-to-clear */
        s_axp_pend |= st;
        if (st & PKEY_NEG) s_pwr_neg_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_axp_mu);
}

static uint8_t axp_events(void)
{
    axp_poll();
    if (!s_axp_mu) return 0;
    xSemaphoreTake(s_axp_mu, portMAX_DELAY);
    uint8_t st = s_axp_pend;
    s_axp_pend = 0;
    xSemaphoreGive(s_axp_mu);
    if (st & 0xC0) { read_battery(); overlay_battery_changed(); }   /* USB plugged or unplugged */
    return st;
}

/* Screenshot chord: BOOT and PWR pressed together. The BOOT task claims it (chord_check) and the
   power task then swallows the PWR key's own click / hold. */
static volatile int64_t s_chord_us;                           /* a chord happened; its PWR click is still to be swallowed */
static volatile int64_t s_boot_press_us;                      /* set by the BOOT interrupt */
static bool boot_down(void);

/* ---- helpers ---- */

static bool in_quiet_hours(void)                          /* the schedule alone */
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    struct tm lt; localtime_r(&now, &lt);
    int m = lt.tm_hour * 60 + lt.tm_min;
    return m >= QUIET_FROM_MIN || m < QUIET_TO_MIN;
}

/* Quiet hours: the schedule, unless flipped by hand; a flip holds until the schedule next changes (RAM only). */
static bool s_quiet, s_quiet_sched, s_quiet_init;

bool power_quiet(void)
{
    bool sched = in_quiet_hours();
    if (!s_quiet_init || sched != s_quiet_sched) { s_quiet_init = true; s_quiet_sched = sched; s_quiet = sched; }
    return s_quiet;
}

void power_quiet_toggle(void)
{
    power_quiet();                                            /* fold in any pending schedule change first */
    s_quiet = !s_quiet;
    ESP_LOGI(TAG, "quiet hours %s (manual)", s_quiet ? "on" : "off");
}

static uint32_t inactive_ms(void)
{
    bsp_display_lock(-1);
    uint32_t t = lv_display_get_inactive_time(NULL);
    bsp_display_unlock();
    return t;
}

static void set_pm(bool light_sleep)
{
#if CONFIG_PM_ENABLE
    /* Minimum 80 MHz, not 40: a switch down to the 40 MHz XTAL clock retimes the flash/PSRAM bus with the cache frozen,
       on whichever task released the last PM lock, and several tasks have PSRAM stacks (psram_task.h). Light sleep
       itself (idle task) is unchanged. */
    esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = light_sleep ? 80 : 240, .light_sleep_enable = light_sleep };
    esp_pm_configure(&pm);
#endif
}

static void report(void)
{
    intercom_send_state(s_state == PWR_SLEEP, s_batt);
}

/* ---- transitions ---- */

/* Brightness for a state: the Settings value awake, 40% of it dimmed, 50% of it resting. */
static int level(power_state_t st)
{
    int b = prefs_brightness();
    if (st == PWR_DIM) b = b * 40 / 100;
    else if (st == PWR_REST) b = b * 50 / 100;
    return b < 3 ? 3 : b;
}

static int chg_face_level(void)                           /* the Charging screen while asleep: 20% of the setting */
{
    int b = prefs_brightness() * 20 / 100;
    return b < 3 ? 3 : b;
}

void power_apply_brightness(void)
{
    if (s_state != PWR_SLEEP) compact_display_brightness(level(s_state));
    else if (s_chg_face) compact_display_brightness(chg_face_level());
}

static void close_screens(void)                           /* everything but a decision takeover */
{
    overlay_close_all();
    thread_close();
    agents_close();
    earlier_close();
    setscreen_close();
    dayview_close();
}

/* The normal sleep: panel off, LVGL paused, light sleep, Wi-Fi rule. */
static void sleep_panel_off(void)
{
    compact_display_brightness(0);
    compact_display_sleep();
    ptt_cancel();                                         /* nothing half-said is sent */
    audio_out_stop();                                     /* and nothing keeps talking in the dark */
    close_screens();                                      /* wake always lands on Now */
    compact_display_lvgl_pause(1000);
    if (!s_usb && s_batt >= 0 && s_batt < 10) { intercom_wifi_paused(true); esp_wifi_stop(); s_wifi_off = true; }
    set_pm(true);
}

/* Asleep on USB at 0-95%: the Charging screen stays on, dimmed. */
static bool chg_face_ok(void) { return s_usb && s_batt >= 0 && s_batt <= 95; }

static void enter(power_state_t to)
{
    power_state_t from = s_state;
    if (to == from) return;
    ESP_LOGI(TAG, "%d -> %d (battery %d%%, %s)", from, to, s_batt, s_usb ? "usb" : "battery");

    if (from == PWR_SLEEP && s_chg_face) {                    /* panel and LVGL are already running */
        s_chg_face = false;
        overlay_close_all();
        bsp_display_lock(-1);
        lv_display_trigger_activity(NULL);
        bsp_display_unlock();
    } else if (from == PWR_SLEEP) {                           /* wake the hardware first */
        set_pm(false);
        if (s_wifi_off) { esp_wifi_start(); s_wifi_off = false; intercom_wifi_paused(false); }
        int64_t t0 = esp_timer_get_time();
        compact_display_sleep_out();
        vTaskDelay(pdMS_TO_TICKS(10));                        /* commands allowed 5 ms after SLPOUT */
        /* Redraw the whole frame while the panel is dark (brightness is still 0 and DISPON not sent),
           so the face appears complete instead of wiping in. */
        dial_set_mode(to == PWR_REST ? DIAL_FACE_REST : DIAL_FACE_DAY);
        compact_display_lvgl_resume();
        bsp_display_lock(-1);
        lv_obj_invalidate(lv_scr_act());
        pomodoro_wake_lv();
        lv_display_trigger_activity(NULL);
        bsp_display_unlock();
        compact_display_refresh_now();
        int64_t t1 = esp_timer_get_time();
        int settle = 120 - (int)((t1 - t0) / 1000);           /* sleep-out needs 120 ms before display on */
        if (settle > 0) vTaskDelay(pdMS_TO_TICKS(settle));
        compact_display_on();
        s_wake_render_ms = (int)((t1 - t0) / 1000);
        s_wake_total_ms = (int)((esp_timer_get_time() - t0) / 1000);
        ESP_LOGI(TAG, "wake: render %d ms, display on at %d ms", s_wake_render_ms, s_wake_total_ms);
    }

    switch (to) {
    case PWR_AWAKE:
        dial_set_mode(DIAL_FACE_DAY);
        compact_display_brightness(level(to));
        break;
    case PWR_DIM:
        dial_set_mode(DIAL_FACE_DAY);
        compact_display_brightness(level(to));
        break;
    case PWR_REST:
        dial_set_mode(DIAL_FACE_REST);
        compact_display_brightness(level(to));
        break;
    case PWR_SLEEP:
        if (chg_face_ok()) {                                  /* charging face: panel on, LVGL running, no light sleep */
            ptt_cancel();
            audio_out_stop();
            close_screens();
            compact_display_brightness(chg_face_level());
            overlay_charging(CHG_SLEEP);
            s_chg_face = true;
        } else {
            sleep_panel_off();
        }
        break;
    }
    s_state = to;
    report();
}

static void shut_down_low_battery(void)
{
    ESP_LOGW(TAG, "battery %d%%: powering off", s_batt);
    capture_flush_now();                                      /* stop a recording and keep its unsent audio in flash */
    if (s_state == PWR_SLEEP) enter(PWR_AWAKE);
    dial_set_mode(DIAL_FACE_CHARGE);
    compact_display_brightness(40);
    vTaskDelay(pdMS_TO_TICKS(5000));
    power_off_now();
}

/* Back to Now: wake if asleep, full brightness, close anything covering the face. Power task only. */
static void go_now(void)
{
    ptt_cancel();                                             /* spec 7.5: PWR while holding BOOT cancels */
    audio_out_stop();                                         /* PWR silences a spoken reply at once */
    close_screens();
    decision_set_aside();                                     /* a decision waits; Now shows */
    bsp_display_lock(-1);
    lv_display_trigger_activity(NULL);                        /* restart the dim / rest / sleep clocks */
    bsp_display_unlock();
    enter(PWR_AWAKE);
}

/* BOOT click: Thread on the followed conversation, waking if needed. Power task only. */
static void go_thread(void)
{
    if (s_state != PWR_SLEEP && thread_is_open()) {           /* already there: only a dimmed screen wakes to full */
        if (s_state != PWR_AWAKE) {
            bsp_display_lock(-1);
            lv_display_trigger_activity(NULL);
            bsp_display_unlock();
            enter(PWR_AWAKE);
        }
        return;
    }
    ptt_cancel();
    audio_out_stop();
    close_screens();
    decision_set_aside();
    bsp_display_lock(-1);
    lv_display_trigger_activity(NULL);
    bsp_display_unlock();
    enter(PWR_AWAKE);
    bsp_display_lock(-1);
    thread_open_lv();
    bsp_display_unlock();
}

/* Plug-in: wake, close the side screens (a decision stays), show the Charging screen for 10 s. */
static void plug_in(void)
{
    ESP_LOGI(TAG, "USB plugged in");
    close_screens();
    bsp_display_lock(-1);
    lv_display_trigger_activity(NULL);
    bsp_display_unlock();
    enter(PWR_AWAKE);
    overlay_charging(CHG_PLUG);
}

/* PWR click: asleep / dimmed / resting -> Now; full Now with nothing over it -> sleep; else Now. */
static void pwr_click(void)
{
    if (pomodoro_showing()) {                                 /* PWR on the timer: dimmed wakes it, awake cancels it */
        if (s_state == PWR_AWAKE) { pomodoro_stop(); return; }
        bsp_display_lock(-1);
        lv_display_trigger_activity(NULL);
        bsp_display_unlock();
        enter(PWR_AWAKE);
        return;
    }
    if (capture_recording()) {                                /* Capture owns the glass: PWR only wakes it */
        capture_touch();
        bsp_display_lock(-1);
        lv_display_trigger_activity(NULL);
        bsp_display_unlock();
        enter(PWR_AWAKE);
        return;
    }
    if (capture_busy()) capture_dismiss();                    /* the Saved screen: straight to Now */
    bool covered = overlay_open() || overlay_flashlight_on() || overlay_charging_on() || dayview_is_open() || agents_is_open() ||
                   thread_is_open() || earlier_is_open() || setscreen_is_open() || decision_showing() || ptt_recording();
    if (s_state == PWR_AWAKE && !covered) { ESP_LOGI(TAG, "PWR click: sleep"); enter(PWR_SLEEP); }
    else go_now();
}

static void power_task(void *arg)
{
    uint32_t since_batt = BATTERY_READ_MS;
    int last_reported = -100;
    int64_t long_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if ((since_batt += POLL_MS) >= (s_usb ? BATTERY_USB_MS : BATTERY_READ_MS)) {
            since_batt = 0;
            read_battery();
            if (s_batt >= 0 && (s_batt / 5 != last_reported / 5)) { last_reported = s_batt; report(); }
            if (!s_usb && s_batt >= 0 && s_batt < 5) shut_down_low_battery();
        }
        /* PWR (spec 8.1): click = pwr_click(); hold 3 s = power menu;
           hold 8 s = the AXP2101's own forced power-off. */
        uint8_t k = axp_events();
        if (k & PKEY_SHORT) {                                 /* BOOT may be mid-chord: give its task the verdict first */
            while (esp_timer_get_time() - s_boot_press_us < 400000) vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_chord_us) {
            if (esp_timer_get_time() - s_chord_us > 6000000) s_chord_us = 0;
            else if (k & (PKEY_SHORT | PKEY_LONG | PKEY_POS)) {
                if (k & (PKEY_SHORT | PKEY_POS)) s_chord_us = 0;
                k &= ~(PKEY_SHORT | PKEY_LONG);               /* the chord's PWR press does nothing of its own */
                long_at = 0;
            }
        }
        if (k & (PKEY_NEG | PKEY_LONG | PKEY_POS)) ESP_LOGI(TAG, "PWR key events 0x%02x", k);
        if (k & PKEY_NEG) long_at = 0;
        if (k & PKEY_LONG) long_at = esp_timer_get_time();
        if (k & PKEY_POS) long_at = 0;                        /* let go between 2.5 and 3 s: nothing */
        if (long_at && capture_recording()) long_at = 0;     /* no power menu over a recording (8 s still forces off) */
        if (long_at && esp_timer_get_time() - long_at >= MENU_AFTER_LONG_MS * 1000LL) {
            long_at = 0;
            go_now();
            overlay_power_menu();
            continue;
        }
        if (k & 0x80) { plug_in(); continue; }                /* VBUS insert */
        if ((k & 0x40) && s_state != PWR_SLEEP && overlay_charging_on()) overlay_close_all();   /* VBUS remove */
        if (k & PKEY_SHORT) { s_now_req = false; pwr_click(); continue; }
        if (s_now_req) { s_now_req = false; go_now(); continue; }
        if (s_thread_req) { s_thread_req = false; go_thread(); continue; }
        if (s_wake_req) { s_wake_req = false; enter(PWR_AWAKE); continue; }
        if (s_sleep_req) { s_sleep_req = false; if (!capture_recording()) enter(PWR_SLEEP); continue; }
        if (s_state == PWR_SLEEP) {
            if (s_chg_face && !chg_face_ok()) {               /* unplugged, or over 95%: the normal sleep */
                ESP_LOGI(TAG, "charging face off (battery %d%%, %s)", s_batt, s_usb ? "usb" : "battery");
                s_chg_face = false;
                overlay_close_all();
                sleep_panel_off();
            }
            continue;                                         /* only a press or an urgent Moment wakes */
        }
        if (overlay_flashlight_on()) continue;                /* no dimming or sleep under the flashlight */
        if (capture_busy()) {                                 /* recording or Saved: dim at most, never rest or sleep */
            enter(inactive_ms() >= 30000 ? PWR_DIM : PWR_AWAKE);
            continue;
        }
        if (pomodoro_active()) {                              /* the timer: never rest or sleep; on battery it dims at 30 s */
            enter(!s_usb && inactive_ms() >= 30000 ? PWR_DIM : PWR_AWAKE);
            continue;
        }

        /* on battery: rest at 60 s, sleep per the Settings value (0 = never); on USB: dim only. Quiet hours do not
           touch any of this. */
        uint32_t sleep_ms = !s_usb && prefs_sleep_min() ? prefs_sleep_min() * 60000u : UINT32_MAX;
        uint32_t rest_ms = s_usb ? UINT32_MAX : 60000;
        uint32_t idle = inactive_ms();

        if (idle >= sleep_ms) enter(PWR_SLEEP);
        else if (idle >= rest_ms) enter(PWR_REST);
        else if (idle >= 30000) enter(PWR_DIM);
        else enter(PWR_AWAKE);                                /* touched: back to the full face */
    }
}

/* ---- public ---- */

void power_off_now(void)
{
    ESP_LOGW(TAG, "power off");
    uint8_t v = 0;
    if (!s_axp) return;
    axp_read(AXP_COMMON_CFG, &v);
    axp_write(AXP_COMMON_CFG, v | 0x01);
}

void power_now(const char *why)
{
    ESP_LOGI(TAG, "back to Now: %s", why);
    s_now_req = true;
}

/* ---- BOOT (GPIO 0): click = Thread, double-click = Capture, press and hold = push-to-talk; never sleeps ---- */

static TaskHandle_t s_boot_task;

static void IRAM_ATTR boot_isr(void *arg)
{
    gpio_intr_disable(BOOT_GPIO);                             /* level interrupt: the task re-arms it */
    s_boot_press_us = esp_timer_get_time();
    BaseType_t woke = pdFALSE;
    vTaskNotifyGiveFromISR(s_boot_task, &woke);
    if (woke) portYIELD_FROM_ISR();
}

static bool boot_down(void) { return gpio_get_level(BOOT_GPIO) == 0; }

/* Wait until BOOT reads `down` for 30 ms straight (debounce), at most `limit_ms`. */
static bool boot_wait(bool down, int limit_ms)
{
    int stable = 0;
    for (int t = 0; t < limit_ms; t += 10) {
        stable = boot_down() == down ? stable + 10 : 0;
        if (stable >= 30) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

/* Was PWR pressed within CHORD_MS of this BOOT press? Polls the AXP2101 every 10 ms for that window (the power task
   only looks every 200 ms), or until BOOT is let go. */
static bool chord_check(void)
{
    int64_t t0 = s_boot_press_us;
    for (;;) {
        axp_poll();
        int64_t n = s_pwr_neg_us;
        if (n && n >= t0 - CHORD_MS * 1000LL) return true;
        if (!boot_down() || esp_timer_get_time() - t0 >= CHORD_MS * 1000LL) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    axp_poll();
    int64_t n = s_pwr_neg_us;
    return n && n >= t0 - CHORD_MS * 1000LL;
}

static void boot_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool was_asleep = s_state == PWR_SLEEP;
        /* from sleep, the wake waits for the verdict: a click wakes straight into Thread, a hold wakes for push-to-talk */

        if (capture_recording()) {                            /* no push-to-talk while recording */
            if (!boot_wait(false, BOOT_HOLD_MS)) {
                ESP_LOGI(TAG, "BOOT hold while recording: ignored");
                while (!boot_wait(false, 60000)) {}
            } else if (boot_wait(true, BOOT_DOUBLE_MS)) {
                capture_toggle();                             /* double-press again: stop and save */
                boot_wait(false, 5000);
            } else {
                capture_touch();                              /* a click wakes the full recording screen */
                if (s_state != PWR_AWAKE) power_wake("BOOT while recording");
            }
            boot_wait(false, 5000);
            gpio_intr_enable(BOOT_GPIO);
            continue;
        }
        bool flash = overlay_flashlight_on();
        audio_out_stop();                                     /* BOOT silences a reply; the mics share its I2S */
        if (!flash) ptt_arm();                                /* record from the press, so the first word is kept */
        bool chord = !flash && !was_asleep && chord_check();
        int hold_left = BOOT_HOLD_MS - (int)((esp_timer_get_time() - s_boot_press_us) / 1000);
        if (chord) {                                          /* BOOT + PWR together: screenshot, nothing else */
            ESP_LOGI(TAG, "BOOT + PWR chord: screenshot");
            ptt_disarm();
            /* ptt_arm() started the mic recorder at the BOOT press; disarming only flips its phase, so screenshot_take()
               would refuse ("push-to-talk") if it ran now. Wait, bounded (600 ms), for the recorder to wind down. */
            for (int i = 0; i < 30 && ptt_recording(); i++) vTaskDelay(pdMS_TO_TICKS(20));
            s_pwr_neg_us = 0;
            s_chord_us = esp_timer_get_time();
            screenshot_take();
        } else if (flash) {                                   /* BOOT turns the flashlight off, nothing else */
            ESP_LOGI(TAG, "BOOT press: flashlight off");
            power_now("flashlight off");
        } else if (!boot_wait(false, hold_left > 30 ? hold_left : 30)) {   /* still down: push-to-talk */
            ESP_LOGI(TAG, "BOOT hold: push-to-talk");
            if (was_asleep) { power_wake("BOOT hold"); vTaskDelay(pdMS_TO_TICKS(450)); }   /* let the wake land first */
            ptt_hold();
            while (!boot_wait(false, 60000)) {}
            ptt_release();                                    /* release sends (a PWR press already cancelled it) */
        } else if (ptt_disarm(), boot_wait(true, BOOT_DOUBLE_MS)) {   /* a click: drop the audio; second press: Capture */
            ESP_LOGI(TAG, "BOOT double-press: Capture");
            power_now("BOOT double-press");
            vTaskDelay(pdMS_TO_TICKS(was_asleep ? 250 : 50));
            capture_toggle();
            boot_wait(false, 5000);
        } else {
            ESP_LOGI(TAG, "BOOT click: Thread");
            s_thread_req = true;                              /* the power task closes screens, wakes and opens it */
        }
        boot_wait(false, 5000);
        gpio_intr_enable(BOOT_GPIO);
    }
}

static void boot_start(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_LOW_LEVEL };
    gpio_config(&io);
    task_create_psram(boot_task, "boot_btn", 6144, NULL, 5, &s_boot_task);   /* builds the Listening, Understood and Capture screens */
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "gpio isr service: %s", esp_err_to_name(e));
    gpio_isr_handler_add(BOOT_GPIO, boot_isr, NULL);
    gpio_wakeup_enable(BOOT_GPIO, GPIO_INTR_LOW_LEVEL);       /* a press also wakes the chip from light sleep */
    esp_sleep_enable_gpio_wakeup();
}

void power_start(void)
{
    s_axp_mu = xSemaphoreCreateMutex();
    axp_init();
    boot_start();
    read_battery();
    ESP_LOGI(TAG, "battery %d%%, %s", s_batt, s_usb ? "on USB" : "on battery");
    task_create_psram(power_task, "power", 12288, NULL, 4, NULL);   /* wake renders a full frame here */
}

void power_wake(const char *why)
{
    ESP_LOGI(TAG, "wake requested: %s", why);
    s_wake_req = true;
}

/* A new urgent Moment wakes the device from anything; a new decision too, except in quiet hours
   (spec 6.2; Phase does the same). Notices never wake it. "New" = an id|updated_at not seen before. */
static void wake_on(const cJSON *items, bool notices, char *seen_buf, size_t seen_n)
{
    char seen[512] = "";
    bool fresh = false;
    const cJSON *e;
    cJSON_ArrayForEach(e, items) {
        int w = decision_weight(e, notices);
        if (w < 2 || (w == 2 && power_quiet())) continue;
        const cJSON *up = cJSON_GetObjectItem(e, "updated_at");
        char key[160];
        snprintf(key, sizeof key, "%s|%s;", e->string ? e->string : "", cJSON_IsString(up) ? up->valuestring : "");
        if (!strstr(seen_buf, key)) { fresh = true; if (w == 3) capture_note_alert(); }
        strlcat(seen, key, sizeof seen);
    }
    strlcpy(seen_buf, seen, seen_n);
    if (fresh) power_wake(notices ? "urgent or decision notice" : "urgent or decision Moment");
}

void power_on_moments(const cJSON *events)
{
    wake_on(events, false, s_seen_urgent, sizeof s_seen_urgent);
}

void power_on_notices(const cJSON *notices)
{
    static char seen[512];
    wake_on(notices, true, seen, sizeof seen);
}

void power_sleep(const char *why)
{
    ESP_LOGI(TAG, "sleep requested: %s", why);
    s_sleep_req = true;
}

power_state_t power_state(void) { return s_state; }
int power_battery_pct(void) { return s_batt; }
power_charge_t power_charge_state(void) { return s_chg; }
void power_battery_refresh(void) { read_battery_x(false); }
bool power_on_usb(void) { return s_usb; }
