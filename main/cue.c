#include "cue.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "capture.h"
#include "decision.h"
#include "dial.h"
#include "display.h"
#include "overlay.h"
#include "power.h"
#include "ptt.h"
#include "setscreen.h"

#define LEVELS_MAX 64
#define LEVELS_DEF 20        /* steps from dark to bright, even in perceived brightness (see tick_cb); 8 was too jerky, 16 too few */
#define LEVELS_REST_BATT 20  /* Rest on battery: the same steps, a coarser breath was jerky */
#define TICK_MS_DEF 40       /* 25 samples a second, so every level is shown (at the bottom of the urgent breath they pass at up to 25 a second); a tick that changes nothing sends nothing */
#define INNER_W 5            /* soft inner band (one third of the opacity); left out 22 degrees either side of the planet (display.c) */
#define LOCK_MS 2000         /* debug hooks: bounded wait for the display lock */

static const char *TAG = "cue";

/* Tuning knobs (POST /debug/ui "cue tune mode levels tick_ms inner nodraw rect_cost"); defaults are the shipped values.
   mode 0: breath steps sent straight to the panel; mode 1: every step through an LVGL redraw (the old cost, for
   comparison). rect_cost: push-set layout (display.c). */
static int LEVELS = LEVELS_DEF, TICK_MS = TICK_MS_DEF, s_inner = 1, s_nodraw = 0;

/* level 1 notice, 2 decision, 3 urgent (decision.c weights) */
static const struct { uint32_t col; float a_min, a_max, period_s; int width; } K[4] = {
    [1] = { 0xB9A3BF, 0.12f, 0.90f, 6.0f, 6 },
    [2] = { 0xF7C95C, 0.12f, 1.00f, 4.0f, 8 },
    [3] = { 0xF5A99B, 0.12f, 1.00f, 2.4f, 8 },
};

static lv_timer_t *s_tmr;
static int s_level, s_count;             /* what is pending (or pinned) */
static int s_opa;                        /* opacity asked for, 0..255 (0 = no ring) */
static int s_lvl = -1;                   /* quantised level, 0 = not shown */
static bool s_shown;
static struct { int level, count, phase, quiet; } s_pin = { .phase = -1 };   /* level 0: not pinned; phase -1: live */

/*
 * How it is drawn (proposals/2026-11-22-stage2-ring-compositor.md): this file only decides what the ring is (colour,
 * opacity, width, the planet's gap). display.c draws it: LVGL never renders the ring; it is blended into every band
 * on its way to the panel, and a breath step sends only the ring area, blended over a copy of the face as LVGL last
 * sent it, through the same single panel writer. Drawing the ring as an LVGL object made LVGL redraw the whole face
 * under the annulus for every step (18 to 27% of a core at 20 steps).
 */

static bool covered(void)
{
    return capture_busy() || ptt_recording() || ptt_listening() || overlay_open() ||
           overlay_charging_on() || overlay_flashlight_on() || setscreen_is_open();
}

static void stop(void)
{
    if (s_tmr) { lv_timer_delete(s_tmr); s_tmr = NULL; }
    compact_ring_stop();
    s_opa = 0;
    s_lvl = 0;
    s_shown = false;
    s_level = s_count = 0;
    ESP_LOGI(TAG, "ring off");
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    /* The compositor draws only from the LVGL task. cue_kick() may run from another task holding the display lock
       (a bus update); then the timer's first tick, a moment later, does the work. */
    if (!compact_display_in_lvgl_task()) return;
    int level, count;
    bool quiet = power_quiet();
    if (s_pin.level) { level = s_pin.level; count = s_pin.count; quiet = s_pin.quiet; }
    else decision_cue_query(&level, &count);
    if (level <= 0) { stop(); return; }
    s_level = level; s_count = count;

    power_state_t ps = power_state();
    bool hide = ps == PWR_SLEEP || (!s_pin.level && covered());
    if (level == 1 && quiet) hide = true;                              /* quiet hours: notice ring off */

    int lvl = 0, opa = 0;
    if (!hide) {
        float amin = K[level].a_min, amax = K[level].a_max;
        if (level == 2 && quiet) amax = amin + 0.6f * (amax - amin);   /* decision in quiet hours: 60% of its range */
        float T = K[level].period_s;
        float p = s_pin.phase >= 0 ? s_pin.phase / 100.0f : fmodf((float)(esp_timer_get_time() / 1000) / 1000.0f, T) / T;
        if (count >= 2) p = p < 0.6f ? fmodf(p / 0.3f, 1.0f) : 0;      /* heartbeat: two pulses in the first 60% */
        float curve = (1 - cosf(2 * 3.14159265f * p)) / 2;             /* 0..1 */
        /* Steps are even in perceived brightness (about the square root of the opacity), so the dark end, where the
           eye is most sensitive, gets the fine steps and the bright end the coarse ones. */
        int n = (ps == PWR_REST && !power_on_usb()) ? LEVELS_REST_BATT : LEVELS;
        if (n > LEVELS) n = LEVELS;
        float u = sqrtf(curve);
        int k = (int)lroundf(u * (n - 1));                              /* 0 .. n-1 */
        float cq = (float)(k * k) / (float)((n - 1) * (n - 1));
        lvl = k + 1;
        opa = (int)lroundf(255 * (amin + (amax - amin) * cq));
    }

    uint32_t col = level == 3 && decision_flood_showing() ? 0x2A1A2C : K[level].col;   /* dark on the rose flood */
    float planet = dial_clock_deg(dial_now());
    int pdeg = planet < 0 ? -1 : (int)lroundf(planet) % 360;          /* the mask only changes by whole degrees */
    s_opa = lvl && !s_nodraw ? opa : 0;
    compact_ring_show(col, s_opa, K[level].width, s_inner ? INNER_W : 0, pdeg);   /* sends only what changed */
    s_lvl = lvl;
    s_shown = lvl > 0;
}

void cue_kick(void)
{
    if (s_tmr) return;
    int level = 0, count = 0;
    if (s_pin.level) level = s_pin.level; else decision_cue_query(&level, &count);
    if (level <= 0) return;
    s_tmr = lv_timer_create(tick_cb, TICK_MS, NULL);
    ESP_LOGI(TAG, "ring on (level %d)", level);
    tick_cb(s_tmr);
}

void cue_debug(const char *args)
{
    char name[8] = {0};
    if (!strncmp(args, " readback", 9)) return;                         /* ota.c answers it with compact_ring_readback() */
    if (bsp_display_lock(LOCK_MS) != ESP_OK) { ESP_LOGW(TAG, "debug: display busy"); return; }
    if (!strncmp(args, " bench", 6)) compact_ring_bench();
    else if (!strncmp(args, " tune", 5)) {
        int mode = 0, lv = LEVELS_DEF, tk = TICK_MS_DEF, in = 1, nd = 0, rc = -1;
        sscanf(args + 5, " %d %d %d %d %d %d", &mode, &lv, &tk, &in, &nd, &rc);
        compact_ring_tune(mode, rc);
        LEVELS = lv < 2 ? 2 : lv > LEVELS_MAX ? LEVELS_MAX : lv; TICK_MS = tk < 20 ? 20 : tk;
        s_inner = in; s_nodraw = nd;
        if (s_tmr) lv_timer_set_period(s_tmr, TICK_MS);
    } else {
        int phase = -1, count = 1, quiet = 0;
        char q[8] = {0};
        int n = sscanf(args, " %7s %d %d %7s", name, &phase, &count, q);
        memset(&s_pin, 0, sizeof s_pin);
        s_pin.phase = -1;
        int lv = !strcmp(name, "plum") ? 1 : !strcmp(name, "amber") ? 2 : !strcmp(name, "rose") ? 3 : 0;
        if (lv) {
            if (n >= 4 && !strcmp(q, "quiet")) quiet = 1;
            s_pin.level = lv; s_pin.phase = n >= 2 && phase >= 0 && phase <= 100 ? phase : -1;
            s_pin.count = n >= 3 && count > 0 ? count : 1; s_pin.quiet = quiet;
        }
        /* Not called inline: the compositor runs only in the LVGL task (its timer), which picks this up within a tick. */
        if (lv && !s_tmr) { s_tmr = lv_timer_create(tick_cb, TICK_MS, NULL); ESP_LOGI(TAG, "ring on (pinned %d)", lv); }
    }
    bsp_display_unlock();
}

size_t cue_info_json(char *out, size_t n)
{
    int k = snprintf(out, n, "{\"level\":%d,\"count\":%d,\"shown\":%s,\"pinned\":%s,\"lvl\":%d,\"levels\":%d,\"tick_ms\":%d,\"opa\":%d,\"comp\":",
                     s_level, s_count, s_shown ? "true" : "false", s_pin.level ? "true" : "false", s_lvl, LEVELS, TICK_MS, s_opa);
    if (k < 0 || (size_t)k >= n) return n ? n - 1 : 0;
    size_t m = compact_ring_info_json(out + k, n - (size_t)k);
    if ((size_t)k + m + 2 > n) { out[k] = 0; return (size_t)k; }
    out[k + m] = '}'; out[k + m + 1] = 0;
    return (size_t)k + m + 1;
}
