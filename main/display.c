#include "display.h"
#include <string.h>
#include <stdlib.h>
#include "capture.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "src/display/lv_display_private.h"   /* the two draw buffers, borrowed by breath steps between refreshes */
#include "power.h"
#include "ring_px.h"
#include "touch_logic.h"
#include "screenshot.h"

#define BUF_ROWS 20   /* 466 x 20 x 2 B = 18.6 KB per buffer, two buffers, internal RAM */

static const char *TAG = "display";
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static compact_perf_t s_perf;                 /* GET /debug/perf */

/*
 * Panel ownership (see AGENTS.md "Panel ownership"). This file is the only code that talks to the panel or its
 * SPI IO. esp_lcd's SPI panel IO is not safe from two tasks: it keeps one shared count of queued transfers and every
 * polling command (CASET, RASET, brightness, sleep, ...) first collects that many results with an endless wait. Two
 * tasks doing it at once can take the same result, and one then waits for ever. LVGL flushes (draw_bitmap) run inside
 * lv_timer_handler() with the display lock held, so every other panel command takes that same lock: panel_locked().
 */
#define PANEL_LOCK_MS 2000    /* bounded wait for the display lock; a longer wait means LVGL is stuck (the watchdog's job) */

typedef struct { volatile uint32_t ms; volatile uint8_t op; char task[configMAX_TASK_NAME_LEN]; } last_op_t;
static last_op_t s_lastop;
static const char *const OP_NAME[] = { "none", "brightness", "sleep", "sleep_out", "display_on", "lock_timeout", "hammer" };
static bool lvgl_task(void);
enum { OP_NONE, OP_BRIGHT, OP_SLEEP, OP_SLEEP_OUT, OP_ON, OP_LOCK_TIMEOUT, OP_HAMMER, OP_N };
static volatile uint32_t s_flush_ms, s_flush_n;     /* last band handed to the panel (LVGL task) */
static volatile uint32_t s_hb;                      /* LVGL heartbeat: bumped by an lv_timer in the LVGL task */
static volatile bool s_lvgl_paused;                 /* compact_display_lvgl_pause() ... resume() */
static int s_wd_hold;                               /* compact_display_wd_hold() nesting (atomic ops) */

static uint32_t now_ms32(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Is the caller the adapter's LVGL task? (Handle looked up once; the task exists for the life of the program.) */
static TaskHandle_t s_lvgl_task;
static bool lvgl_task(void)
{
    if (!s_lvgl_task) s_lvgl_task = xTaskGetHandle("lvgl");
    return s_lvgl_task && xTaskGetCurrentTaskHandle() == s_lvgl_task;
}
bool compact_display_in_lvgl_task(void) { return lvgl_task(); }

static void note_op(uint8_t op)
{
    const char *n = pcTaskGetName(NULL);
    strlcpy(s_lastop.task, n ? n : "?", sizeof s_lastop.task);
    s_lastop.op = op;
    s_lastop.ms = now_ms32();
}

/* Run one panel command with the display lock held. The lock is recursive, so this is safe from the LVGL task (it
   already holds the lock inside lv_timer_handler and event callbacks) and while LVGL is paused (the worker task is
   parked and holds nothing). Lock order: the display lock is taken first and nothing else is taken while holding
   it except the SPI driver's own bus lock, which never calls back into app code. Callers never hold another lock
   that a display-lock holder may wait for (checked for power.c, overlay.c, setscreen.c, ota.c); the bounded wait is
   the backstop and the command is dropped, and logged, rather than blocking for ever. */
static esp_err_t panel_locked(uint8_t op, esp_err_t (*fn)(int), int arg)
{
    if (esp_lv_adapter_lock(PANEL_LOCK_MS) != ESP_OK) {
        note_op(OP_LOCK_TIMEOUT);
        ESP_LOGE(TAG, "panel command %s dropped: display lock busy for %d ms", OP_NAME[op], PANEL_LOCK_MS);
        return ESP_ERR_TIMEOUT;
    }
    note_op(op);
    esp_err_t r = fn(arg);
    esp_lv_adapter_unlock();
    return r;
}

static esp_err_t do_brightness(int pct) { return bsp_display_brightness_set(pct); }

esp_err_t compact_display_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return panel_locked(OP_BRIGHT, do_brightness, pct);
}

/* How far the current (or last) press travelled from where it started, in either axis. */
static lv_point_t s_press_from;
static bool s_press_moved;           /* latched: this press reached the dead zone (touch_logic.h) */
static bool s_press_stop;            /* this press landed on a list still coasting (momentum): it only stops the list */
static bool s_touch_was_down;        /* previous read was a press (finds the press edge in touch_read_cb) */

static void press_track_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) { s_press_from = p; s_press_moved = false; return; }
    if (s_press_stop && !s_press_moved) {
        /* A press that stopped a coasting list is not a tap or a hold: cancel any hold it started
           (PRESSED has been handled by now) and let compact_press_was_drag() veto the click on release. */
        s_press_moved = true;
        lv_obj_t *o0 = lv_indev_get_active_obj();
        if (o0) lv_obj_send_event(o0, LV_EVENT_PRESS_LOST, in);
        return;
    }
    if (s_press_moved || !touch_beyond_deadzone(s_press_from.x, s_press_from.y, p.x, p.y)) return;
    s_press_moved = true;
    /* Left the dead zone: this is a swipe or a scroll, not a tap or a hold. LVGL only sends PRESS_LOST when a scroll
       starts, and screens that do not scroll never get one, so cancel every hold here. A later release is guarded by
       each handler (it only acts while its hold is running). */
    lv_obj_t *o = lv_indev_get_active_obj();
    if (o) lv_obj_send_event(o, LV_EVENT_PRESS_LOST, in);
}

bool compact_press_was_drag(void)
{
    lv_indev_t *in = lv_indev_active();
    return s_press_moved || s_press_stop || (in && lv_indev_get_gesture_dir(in) != LV_DIR_NONE);
}

/*
 * Tap-to-wake: the touch read callback is wrapped. A new press on a dimmed or resting screen
 * wakes it to the full face and is reported as released until the finger lifts, so the tap triggers nothing.
 * While asleep every touch is swallowed with no activity (touch never wakes sleep).
 */
static lv_indev_read_cb_t s_touch_read;
static bool s_touch_swallow;

static void touch_read_cb(lv_indev_t *in, lv_indev_data_t *d)
{
    s_touch_read(in, d);
    bool down = d->state == LV_INDEV_STATE_PRESSED;
    if (down && !s_touch_was_down) s_press_stop = lv_indev_get_scroll_obj(in) != NULL;   /* before LVGL stops the throw */
    s_touch_was_down = down;
    if (d->state != LV_INDEV_STATE_PRESSED) { s_touch_swallow = false; return; }
    if (!s_touch_swallow) {
        power_state_t st = power_state();
        if (st == PWR_DIM || st == PWR_REST) { lv_display_trigger_activity(NULL); capture_touch_lv(); s_touch_swallow = true; }
        else if (st == PWR_SLEEP) s_touch_swallow = true;
    }
    if (s_touch_swallow) d->state = LV_INDEV_STATE_RELEASED;
}

/* One CO5300 command without parameters over QSPI (opcode 0x02 = write command, as the driver does): NOP, sleep and
   display on/off only. Never a window or pixel command: those go only through panel_write(). Caller holds the display
   lock. */
static esp_err_t panel_cmd(int cmd)
{
    return esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (cmd << 8), NULL, 0);
}
static esp_err_t do_dispoff_slpin(int unused)
{
    (void)unused;
    panel_cmd(0x28);                  /* DISPOFF */
    return panel_cmd(0x10);           /* SLPIN */
}

/*
 * The driver's disp_sleep() puts the CO5300 into deep standby because the reset pin is wired.
 * Waking from that means a hardware reset and the full Waveshare init sequence, which sets
 * brightness to 0xFF, waits 600 ms after sleep-out and turns the display on before LVGL has
 * drawn anything (the visible top-to-bottom wipe). Plain sleep-in avoids all of that.
 */
void compact_display_sleep(void) { panel_locked(OP_SLEEP, do_dispoff_slpin, 0); }
void compact_display_sleep_out(void) { panel_locked(OP_SLEEP_OUT, panel_cmd, 0x11); }   /* SLPOUT; panel stays dark until DISPON */
void compact_display_on(void)        { panel_locked(OP_ON, panel_cmd, 0x29); }          /* DISPON */

/* ---- pixels: one writer, one compositor (proposals/2026-11-22-stage2-ring-compositor.md) ----
 *
 * panel_write() is the ONLY code that sends pixels to the glass, and it does so only through esp_lcd's public
 * draw_bitmap call: the driver adds the CO5300's 6-column gap and sends CASET, RASET and RAMWR the same way
 * for every write. LVGL's flushes reach it through the adapter's custom_draw_bitmap hook (flush_hook), breath steps
 * from ring_step(). Both run in the LVGL task's handler pass (or a refresh_now) with the display lock held, which is
 * what serialises the SPI IO (see panel_locked). The screenshot records every write here, so it is a model of panel
 * memory built from the bytes actually sent. (The Nov 22 20:37 colour fault came from a second writer: ring steps
 * sent their own CASET without the gap and so landed 6 px left of LVGL's pixels. tools/check_panel_owner.sh now fails
 * the build on a second draw_bitmap call, on window commands, tx_color, SPI device calls or esp_lcd private structs.)
 */
static esp_err_t panel_write(int xs, int ys, int xe, int ye, const void *px, bool lvgl)
{
    screenshot_record(xs, ys, xe, ye, px, lvgl);
    return esp_lcd_panel_draw_bitmap(s_panel, xs, ys, xe, ye, px);
}

/* A polling command first collects every queued transfer (esp_lcd SPI IO), so a NOP waits until all pixels sent so far
   have left the draw buffers. Display lock held. */
static void panel_drain(void) { panel_cmd(0x00); }

/* The breathing ring (cue.c decides colour, opacity and shape; this file draws it). LVGL never draws the ring: the
   flush hook blends it into every band on its way to the panel and keeps a copy of the face under it (the snapshot,
   exactly the bytes LVGL sent); a breath step blends the ring over the snapshot and sends only the push set
   (ring_px.c), with no LVGL redraw. All state below is touched only with the display lock held. */
#define RING_CX 233
#define RING_R_OUT 233
#define RING_PS_RIN 219.0f   /* push set: pixel centres from r 219 to r 234, the widest ring plus half a pixel each side */
#define RING_PS_ROUT 234.0f
#define RING_RECT_COST 4000  /* push-set layout: a panel write costs about this many pixels (7 strips, 12 rects) */
#define RING_SLOW_US 50000   /* a step over 50 ms three times running turns the direct path off for this boot */
static rp_t s_rp;            /* about 25 KB: PSRAM .bss (linker.lf) */
static struct {
    bool ok, failed;         /* push set allocated / allocation failed */
    volatile bool active;    /* the flush hook keeps the snapshot and blends */
    bool direct_off;         /* steps through LVGL invalidations instead (slow or failed direct steps, or tuned) */
    bool tables;             /* the blend tables hold rgb / opa */
    uint32_t rgb; int opa;
    int w, inner, planet;    /* mask built for; w < 0: none */
    int rect_cost, slow_run;
    uint32_t steps, steps_lvgl, verifies, step_us_max, hook_us_max, last_compose_us, last_io_us, last_drain_us;
    int bench[5];
} s_ring = { .w = -1, .rect_cost = RING_RECT_COST, .bench = { -1, -1, -1, -1, -1 } };

static bool ring_ready(void)
{
    if (s_ring.ok) return true;
    if (s_ring.failed) return false;
    lv_display_t *d = lv_display_get_default();
    if (!d || !d->buf_1 || !d->buf_2) return false;
    uint32_t cap = (d->buf_1->data_size < d->buf_2->data_size ? d->buf_1->data_size : d->buf_2->data_size) / 2;
    uint32_t n = rp_geometry(&s_rp, RING_CX, RING_PS_RIN, RING_PS_ROUT, s_ring.rect_cost, cap);
    s_rp.snap = heap_caps_malloc((size_t)n * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rp.mask = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rp.pmask = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);    /* optional: compose falls back to the mask */
    if (!s_rp.snap || !s_rp.mask) {
        ESP_LOGE(TAG, "ring: no PSRAM for the push set (%u px)", (unsigned)n);
        free(s_rp.snap); free(s_rp.mask); free(s_rp.pmask);
        s_rp.snap = NULL; s_rp.mask = NULL; s_rp.pmask = NULL; s_rp.n = 0; s_rp.ns = 0;
        s_ring.failed = true;
        return false;
    }
    ESP_LOGI(TAG, "ring push set: %d strips, %d rects, %u px", s_rp.ns, s_rp.n, (unsigned)n);
    s_ring.w = -1;                                                     /* new layout: rebuild the mask */
    s_ring.ok = true;
    return true;
}

static void ring_free(void)
{
    s_ring.active = false;
    s_ring.ok = false;
    free(s_rp.snap); free(s_rp.mask); free(s_rp.pmask);
    s_rp.snap = NULL; s_rp.mask = NULL; s_rp.pmask = NULL; s_rp.n = 0; s_rp.ns = 0;
}

/* LVGL's flush, after the adapter packed and byte-swapped the band (panel order): keep the face, blend the ring,
   send. Returning an error makes the adapter complete the flush handshake itself. */
static esp_err_t flush_hook(lv_display_t *disp, esp_lcd_panel_handle_t panel, int xs, int ys, int xe, int ye,
                            const void *color_map, void *ctx)
{
    (void)disp; (void)panel; (void)ctx;
    if (s_ring.active && s_ring.ok) {
        int64_t t0 = esp_timer_get_time();
        rp_flush(&s_rp, xs, ys, xe - 1, ye - 1, (uint16_t *)color_map, xe - xs);   /* LVGL's own draw buffer: writable */
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > s_ring.hook_us_max) s_ring.hook_us_max = us;
        s_perf.draw_us[PERF_DRAW_RING_HOOK] += us;
    }
    return panel_write(xs, ys, xe, ye, color_map, true);
}

/* Through LVGL: invalidate the push set (the face is redrawn and the flush hook blends the ring in). Rects grouped
   four strips at a time per side, about 45 areas, under LV_INV_BUF_SIZE (96). */
static void ring_invalidate(void)
{
    if (!s_ring.ok) return;
    const int G = 4;
    for (int s0 = 0; s0 < s_rp.ns; s0 += G) {                         /* bounded: ns <= RP_MAXS */
        lv_area_t box[3];
        bool used[3] = { false, false, false };
        for (int s = s0; s < s0 + G && s < s_rp.ns; s++)
            for (int j = 0; j < s_rp.cnt[s]; j++) {
                const rp_rect_t *r = &s_rp.r[s_rp.first[s] + j];
                int side = r->x2 < RING_CX ? 0 : r->x1 >= RING_CX ? 1 : 2;
                if (!used[side]) { box[side] = (lv_area_t){ r->x1, r->y1, r->x2, r->y2 }; used[side] = true; continue; }
                if (r->x1 < box[side].x1) box[side].x1 = r->x1;
                if (r->y1 < box[side].y1) box[side].y1 = r->y1;
                if (r->x2 > box[side].x2) box[side].x2 = r->x2;
                if (r->y2 > box[side].y2) box[side].y2 = r->y2;
            }
        for (int i = 0; i < 3; i++) if (used[i]) lv_obj_invalidate_area(lv_layer_sys(), &box[i]);
    }
    s_ring.steps_lvgl++;
}

/*
 * A breath step: snapshot + ring at the current blend, every push-set rect, straight to the panel. LVGL task, inside
 * an lv_timer (lv_timer_handler holds the display lock and no refresh is running). LVGL's two draw buffers are idle
 * then, except that its last band may still be on its way from the one that is not buf_act; so rect 0 is composed
 * into buf_act, and each draw_bitmap begins with polling commands that wait for every queued transfer, so when rect k
 * is composed into the buffer of rect k-2, that transfer has finished. A fixed loop over at most RP_MAXR rects; the
 * only waits are the SPI driver's (the same as for LVGL's own flushes). The final drain returns both buffers to LVGL
 * idle and ensures no colour-done interrupt of ours can arrive during LVGL's next flush. verify: before sending,
 * count the bytes that differ from the screenshot (which holds what LVGL's flushes sent).
 */
static bool ring_step(bool verify)
{
    lv_display_t *d = lv_display_get_default();
    if (!d || !d->buf_1 || !d->buf_2 || !d->buf_1->data || !d->buf_2->data || !d->buf_act) return false;
    lv_draw_buf_t *b[2] = { d->buf_act, d->buf_act == d->buf_1 ? d->buf_2 : d->buf_1 };
    bool ok = true;
    uint32_t diff = 0, npx = 0;
    int64_t t0 = esp_timer_get_time(), tc = 0, t;
    for (int k = 0; k < s_rp.n && ok; k++) {
        const rp_rect_t *r = &s_rp.r[k];
        lv_draw_buf_t *buf = b[k & 1];
        uint32_t px = rp_rect_px(r);
        if (px * 2 > buf->data_size) { ok = false; break; }
        t = esp_timer_get_time();
        rp_compose(&s_rp, k, (uint16_t *)buf->data);
        tc += esp_timer_get_time() - t;
        if (verify) { diff += screenshot_diff(r->x1, r->y1, r->x2 + 1, r->y2 + 1, buf->data); npx += px; }
        ok = panel_write(r->x1, r->y1, r->x2 + 1, r->y2 + 1, buf->data, false) == ESP_OK;
    }
    t = esp_timer_get_time();
    panel_drain();
    s_ring.last_drain_us = (uint32_t)(esp_timer_get_time() - t);
    s_ring.last_compose_us = (uint32_t)tc;
    s_ring.last_io_us = (uint32_t)(t - t0 - tc);
    if (verify) screenshot_verify_done(ok ? (int32_t)diff : -1, npx);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_ring.step_us_max) s_ring.step_us_max = us;
    s_perf.draw_us[PERF_DRAW_RING_STEP] += us;
    s_ring.slow_run = us > RING_SLOW_US ? s_ring.slow_run + 1 : 0;
    if (!ok || s_ring.slow_run >= 3) {
        s_ring.direct_off = true;
        ESP_LOGE(TAG, "ring: direct step %s (%u us); steps go through LVGL for this boot", ok ? "too slow" : "failed", (unsigned)us);
    }
    if (ok) s_ring.steps++;
    return ok;
}

/* The ring on the glass at the current blend: a direct step when the snapshot is complete, else through LVGL. */
static void ring_push(void)
{
    if (!s_ring.direct_off && rp_all_valid(&s_rp) && ring_step(false)) return;
    ring_invalidate();
}

/*
 * Read-back probe (POST /debug/ui "cue readback"): can the CO5300 return its memory over this QSPI wiring? Right after a
 * direct step, the panel's window is still the last rect that step wrote (draw_bitmap set it; the drain is a NOP), so a
 * RAMRD (0x2E) reads that rect from its top-left with no window command of ours. Also reads the ID (0x04) and the power
 * mode (0x0A) as a check of the read path itself. Reads use esp_lcd's public rx_param (opcode 0x03, single-line read on
 * D1), LVGL task, display lock held; each is one polling transfer. The result is a hex dump for the agent to compare.
 */
static volatile int s_rb_req;      /* 1: wanted, 2: done */
static char s_rb_out[700];

static void hex(char *o, size_t n, const uint8_t *b, int len)
{
    size_t k = strlen(o);
    for (int i = 0; i < len && k + 3 < n; i++, k += 2) snprintf(o + k, n - k, "%02x", b[i]);
}

static void ring_readback(void)
{
    enum { NID = 4, NDPM = 2, NRAM = 1 + 96, NEXP = 96 };
    static uint8_t exp[NEXP];
    uint8_t *rx = heap_caps_calloc(1, NID + NDPM + NRAM, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);   /* SPI DMA reads into internal RAM */
    if (!rx) { snprintf(s_rb_out, sizeof s_rb_out, "no internal memory\n"); return; }
    uint8_t *id = rx, *dpm = rx + NID, *ram = rx + NID + NDPM;
    lv_display_t *d = lv_display_get_default();
    int k = s_rp.n - 1;
    esp_err_t e1 = esp_lcd_panel_io_rx_param(s_io, (0x03 << 24) | (0x04 << 8), id, NID);
    esp_err_t e2 = esp_lcd_panel_io_rx_param(s_io, (0x03 << 24) | (0x0A << 8), dpm, NDPM);
    esp_err_t e3 = esp_lcd_panel_io_rx_param(s_io, (0x03 << 24) | (0x2E << 8), ram, NRAM);
    if (k >= 0 && d && d->buf_act && d->buf_act->data) {               /* idle after the step's drain */
        rp_compose(&s_rp, k, (uint16_t *)d->buf_act->data);
        memcpy(exp, d->buf_act->data, NEXP);
    }
    snprintf(s_rb_out, sizeof s_rb_out, "rect %d at %d,%d err %d %d %d\nid ", k, k >= 0 ? s_rp.r[k].x1 : -1,
             k >= 0 ? s_rp.r[k].y1 : -1, e1, e2, e3);
    hex(s_rb_out, sizeof s_rb_out, id, NID);
    strlcat(s_rb_out, "\ndpm ", sizeof s_rb_out); hex(s_rb_out, sizeof s_rb_out, dpm, NDPM);
    strlcat(s_rb_out, "\nram ", sizeof s_rb_out); hex(s_rb_out, sizeof s_rb_out, ram, NRAM);
    strlcat(s_rb_out, "\nexp ", sizeof s_rb_out); hex(s_rb_out, sizeof s_rb_out, exp, NEXP);
    strlcat(s_rb_out, "\n", sizeof s_rb_out);
    free(rx);
    lv_obj_invalidate(lv_screen_active());                             /* buf_act was used: LVGL repaints anyway */
}

/* Any task but LVGL's; needs a ring showing. Waits at most 1 s. */
const char *compact_ring_readback(void)
{
    if (!s_ring.active) return "no ring active\n";
    s_rb_req = 1;
    for (int i = 0; i < 50 && s_rb_req != 2; i++) vTaskDelay(pdMS_TO_TICKS(20));
    bool done = s_rb_req == 2;
    s_rb_req = 0;
    return done ? s_rb_out : "no direct step in 1 s\n";
}

bool compact_ring_show(uint32_t rgb, int opa, int width, int inner_w, int planet_deg)
{
    if (!lvgl_task() || !ring_ready()) return false;
    if (opa < 0) opa = 0;
    if (opa > 255) opa = 255;
    if (!s_ring.active) {                                              /* start: the snapshot fills from the next flushes */
        rp_invalidate(&s_rp);
        rp_set_blend(&s_rp, rgb, 0);
        s_ring.rgb = rgb; s_ring.opa = 0; s_ring.tables = true;
        s_ring.active = true;
    }
    /* A screenshot asked for proof: one step at the blend the glass already shows, compared byte for byte with what
       the flushes sent, before anything changes. */
    if (screenshot_verify_wanted()) {
        if (s_ring.tables && !s_ring.direct_off && rp_all_valid(&s_rp)) { s_ring.verifies++; ring_step(true); }
        else screenshot_verify_done(-1, 0);
    }
    if (s_rb_req == 1 && s_ring.tables && !s_ring.direct_off && rp_all_valid(&s_rp) && ring_step(false)) {
        ring_readback();
        s_rb_req = 2;
    }
    bool changed = false;
    if (opa > 0 && (width != s_ring.w || inner_w != s_ring.inner || planet_deg != s_ring.planet)) {
        rp_mask_build(&s_rp, RING_CX, RING_R_OUT, width, inner_w, planet_deg, 22.0f);
        s_ring.w = width; s_ring.inner = inner_w; s_ring.planet = planet_deg;
        changed = true;
    }
    if (opa != s_ring.opa || (opa > 0 && rgb != s_ring.rgb)) {
        rp_set_blend(&s_rp, rgb, opa);
        s_ring.rgb = rgb; s_ring.opa = opa;
        changed = true;
    }
    if (changed) ring_push();
    return true;
}

void compact_ring_stop(void)
{
    if (!s_ring.active || !lvgl_task()) return;
    if (s_ring.opa > 0 && !s_ring.direct_off && rp_all_valid(&s_rp)) {   /* erase directly */
        rp_set_blend(&s_rp, s_ring.rgb, 0);
        s_ring.opa = 0;
        ring_step(false);
        s_ring.active = false;
    } else {
        bool was = s_ring.opa > 0;
        s_ring.active = false;                                         /* later flushes go to the panel untouched */
        if (was) ring_invalidate();
    }
    s_ring.opa = 0;
    if (s_ring.ok) rp_invalidate(&s_rp);
}

bool compact_ring_active(void) { return s_ring.active; }

/* cue tune: mode 1 sends every step through LVGL (the old cost); rect_cost changes the push-set layout. Display lock
   held by the caller. */
void compact_ring_tune(int mode_lvgl, int rect_cost)
{
    s_ring.direct_off = mode_lvgl != 0;
    s_ring.slow_run = 0;
    if (rect_cost >= 0 && rect_cost != s_ring.rect_cost) {
        s_ring.rect_cost = rect_cost;
        if (s_ring.ok) {
            ring_free();                                               /* the next show rebuilds it */
            lv_obj_invalidate(lv_screen_active());                     /* repaint without the old ring */
        }
    }
}

/* cue bench: what a step's parts cost (us): drain, compose all, zero-fill all, a 2 x 2 write, copy all. Display lock
   held by the caller (any task). LVGL is between passes, but its last band may still be in flight: drain first. The
   2 x 2 write is black in the top-left corner, outside the round glass. */
void compact_ring_bench(void)
{
    lv_display_t *d = lv_display_get_default();
    if (!s_ring.ok || !d || !d->buf_1 || !d->buf_2) return;
    uint16_t *b[2] = { (uint16_t *)d->buf_1->data, (uint16_t *)d->buf_2->data };
    panel_drain();
    int64_t t = esp_timer_get_time();
    for (int i = 0; i < 50; i++) panel_drain();
    s_ring.bench[0] = (int)((esp_timer_get_time() - t) / 50);
    t = esp_timer_get_time();
    for (int i = 0; i < 5; i++) for (int k = 0; k < s_rp.n; k++) rp_compose(&s_rp, k, b[k & 1]);
    s_ring.bench[1] = (int)((esp_timer_get_time() - t) / 5);
    t = esp_timer_get_time();
    for (int i = 0; i < 5; i++) for (int k = 0; k < s_rp.n; k++) memset(b[k & 1], 0, rp_rect_px(&s_rp.r[k]) * 2);
    s_ring.bench[2] = (int)((esp_timer_get_time() - t) / 5);
    memset(b[0], 0, 8);
    t = esp_timer_get_time();
    for (int i = 0; i < 20; i++) panel_write(0, 0, 2, 2, b[0], false);
    panel_drain();
    s_ring.bench[3] = (int)((esp_timer_get_time() - t) / 20);
    t = esp_timer_get_time();
    for (int i = 0; i < 5; i++) for (int k = 0; k < s_rp.n; k++) memcpy(b[k & 1], s_rp.snap + s_rp.r[k].off, rp_rect_px(&s_rp.r[k]) * 2);
    s_ring.bench[4] = (int)((esp_timer_get_time() - t) / 5);
    lv_obj_invalidate(lv_screen_active());                             /* LVGL's buffers were scribbled on: repaint */
}

size_t compact_ring_info_json(char *out, size_t n)
{
    return (size_t)snprintf(out, n, "{\"active\":%s,\"direct_off\":%s,\"opa\":%d,\"steps\":%u,\"steps_lvgl\":%u,\"verifies\":%u,"
                            "\"step_us_max\":%u,\"hook_us_max\":%u,\"rects\":%d,\"px\":%u,\"last_us\":[%u,%u,%u],\"bench\":[%d,%d,%d,%d,%d]}",
                            s_ring.active ? "true" : "false", s_ring.direct_off ? "true" : "false", s_ring.opa,
                            (unsigned)s_ring.steps, (unsigned)s_ring.steps_lvgl, (unsigned)s_ring.verifies,
                            (unsigned)s_ring.step_us_max, (unsigned)s_ring.hook_us_max, s_rp.n, (unsigned)s_rp.npx,
                            (unsigned)s_ring.last_compose_us, (unsigned)s_ring.last_io_us, (unsigned)s_ring.last_drain_us,
                            s_ring.bench[0], s_ring.bench[1], s_ring.bench[2], s_ring.bench[3], s_ring.bench[4]);
}

/* Park or release the LVGL task for panel sleep. The watchdog is held for the whole pause and released only after
   resume has returned; its staleness count restarts then, so a legitimate pause is never read as a hang. */
void compact_display_lvgl_pause(int timeout_ms)
{
    __atomic_fetch_add(&s_wd_hold, 1, __ATOMIC_RELAXED);
    s_lvgl_paused = true;
    esp_lv_adapter_pause(timeout_ms);
}

bool compact_display_quiesce(int timeout_ms)
{
    if (lvgl_task()) return false;
    s_ring.active = false;              /* the flush hook stops blending from the next band (a volatile flag: no lock, so a
                                           caller inside an LVGL timer cannot make this wait) */
    __atomic_fetch_add(&s_wd_hold, 1, __ATOMIC_RELAXED);           /* a parked LVGL is not a hang */
    s_lvgl_paused = true;
    return esp_lv_adapter_pause(timeout_ms) == ESP_OK;
}

void compact_display_lvgl_resume(void)
{
    esp_lv_adapter_resume();
    s_lvgl_paused = false;
    compact_display_wd_hold(false);
}

void compact_display_refresh_now(void) { esp_lv_adapter_refresh_now(NULL); }

void compact_display_wd_hold(bool on)
{
    if (on) { __atomic_fetch_add(&s_wd_hold, 1, __ATOMIC_RELAXED); return; }
    int v = __atomic_load_n(&s_wd_hold, __ATOMIC_RELAXED);
    while (v > 0 && !__atomic_compare_exchange_n(&s_wd_hold, &v, v - 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}

/* Heartbeat: an lv_timer in the LVGL task. It only runs when lv_timer_handler() completes a pass. */
static void heartbeat_cb(lv_timer_t *t) { (void)t; s_hb++; }

void compact_display_diag(compact_display_diag_t *d)
{
    memset(d, 0, sizeof *d);
    d->hb = s_hb;
    d->hold = __atomic_load_n(&s_wd_hold, __ATOMIC_RELAXED);
    d->paused = s_lvgl_paused;
    d->held = d->hold > 0 || d->paused;
    uint32_t now = now_ms32();
    uint8_t op = s_lastop.op;
    d->op = OP_NAME[op < OP_N ? op : 0];
    strlcpy(d->op_task, s_lastop.task, sizeof d->op_task);
    d->op_age_ms = s_lastop.ms ? now - s_lastop.ms : 0;
    d->flush_n = s_flush_n;
    d->flush_age_ms = s_flush_ms ? now - s_flush_ms : 0;
    TaskHandle_t t = xTaskGetHandle("lvgl");
    d->lvgl_state = t ? (int)eTaskGetState(t) : -1;        /* 0 running 1 ready 2 blocked 3 suspended 4 deleted */
    d->lvgl_stack_min = t ? (int)uxTaskGetStackHighWaterMark(t) : -1;
    /* Which side of the display lock is stuck: a non-blocking try. Free = nobody holds it (the LVGL task is stuck
       elsewhere, outside a handler pass). Held = some task, usually LVGL inside a handler pass, has it. */
    if (esp_lv_adapter_lock(0) == ESP_OK) { d->lock_free = true; esp_lv_adapter_unlock(); }
}

/* ---- debug hooks (POST /debug/ui, Bearer only; see ota.c) ---- */

static void hang_cb(void *arg)                      /* runs in the LVGL task inside lv_timer_handler(), lock held */
{
    uint32_t ms = (uint32_t)(uintptr_t)arg;
    ESP_LOGW(TAG, "debug: blocking the LVGL task for %lu ms", (unsigned long)ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* Block the LVGL task, lock held, for ms (capped at 60 s): the same symptom as the SPI hang, for testing the watchdog. */
bool compact_display_debug_hang(uint32_t ms)
{
    if (ms < 100 || ms > 60000) return false;
    if (esp_lv_adapter_lock(PANEL_LOCK_MS) != ESP_OK) return false;
    bool ok = lv_async_call(hang_cb, (void *)(uintptr_t)ms) == LV_RESULT_OK;   /* LVGL is not thread safe: under the lock */
    esp_lv_adapter_unlock();
    return ok;
}

typedef struct { int seconds; bool nolock; } hammer_t;
static void hammer_task(void *arg)
{
    hammer_t h = *(hammer_t *)arg;
    free(arg);
    int64_t end = esp_timer_get_time() + (int64_t)h.seconds * 1000000;
    int n = 0;
    while (esp_timer_get_time() < end) {               /* bounded by the deadline; each pass sleeps 20 ms */
        if (esp_lv_adapter_lock(PANEL_LOCK_MS) == ESP_OK) {
            lv_obj_invalidate(lv_screen_active());     /* keep LVGL flushing */
            esp_lv_adapter_unlock();
        }
        int pct = 40 + (n++ % 2) * 20;
        if (h.nolock) { note_op(OP_HAMMER); bsp_display_brightness_set(pct); }   /* the old, racy path: reproduces the hang */
        else compact_display_brightness(pct);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    power_apply_brightness();
    vTaskDelete(NULL);
}

/* /debug/ui "hammer <s> [nolock]": brightness flips ~50 times a second against continuous full redraws for s seconds
   (1-300). With "nolock" the flips skip the display lock, which is the old racy behaviour and is expected to hang
   LVGL (so the watchdog fires); without it they use the normal locked entry point and must never hang. */
bool compact_display_debug_hammer(int seconds, bool nolock)
{
    if (seconds < 1 || seconds > 300) return false;
    hammer_t *h = malloc(sizeof *h);
    if (!h) return false;
    h->seconds = seconds; h->nolock = nolock;
    if (xTaskCreate(hammer_task, "hammer", 3072, h, 5, NULL) != pdPASS) { free(h); return false; }
    return true;
}

/* CO5300 needs even start and odd end coordinates (copied from the BSP's static rounder). */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    s_perf.inval++;
    a->x1 = (a->x1 >> 1) << 1;
    a->y1 = (a->y1 >> 1) << 1;
    a->x2 = ((a->x2 >> 1) << 1) + 1;
    a->y2 = ((a->y2 >> 1) << 1) + 1;
}

/* Frame timing: how long each render pass (all dirty areas of one refresh) takes, for /info; and, for
   GET /debug/perf, what went into the passes (invalidations, flushed bands and pixels, draw-callback time). */
static int64_t s_fr_t0;
static uint32_t s_fr_n, s_fr_sum, s_fr_max;
static void frame_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) { s_fr_t0 = esp_timer_get_time(); return; }
    if (!s_fr_t0) return;
    uint32_t ms = (uint32_t)((esp_timer_get_time() - s_fr_t0) / 1000);
    s_fr_t0 = 0;
    s_fr_n++; s_fr_sum += ms;
    if (ms > s_fr_max) s_fr_max = ms;
    if (ms > 35) s_perf.over35++;
    if (ms > 120) s_perf.over120++;
}

void compact_display_frames(int *n, int *avg, int *max, bool reset)
{
    *n = (int)s_fr_n; *avg = s_fr_n ? (int)(s_fr_sum / s_fr_n) : -1; *max = (int)s_fr_max;
    if (reset) { s_fr_n = s_fr_sum = s_fr_max = 0; memset(&s_perf, 0, sizeof s_perf); }
}

void compact_display_perf(compact_perf_t *out) { *out = s_perf; }
void compact_perf_draw_us(int which, uint32_t us) { if (which >= 0 && which < PERF_DRAW_N) s_perf.draw_us[which] += us; }

static void flush_count_cb(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    if (!a) return;
    s_perf.bands++;
    s_perf.px += (uint32_t)lv_area_get_size(a);
    s_flush_ms = now_ms32();
    s_flush_n++;
}

/* Testing (/debug/ui night): the last RGB565 value flushed at two points, (12, 12) outside the round glass (pure
   ground) and (233, 30) just inside the rim at 12 o'clock (ground, under the crescent shade by day). Bit 16 marks
   a value seen since the last read. */
static const lv_point_t PROBE[2] = { { 12, 12 }, { 233, 30 } };
static volatile uint32_t s_probe[2];
static void probe_cb(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    lv_draw_buf_t *b = lv_display_get_buf_active(lv_display_get_default());
    if (!a || !b || !b->data) return;
    for (int i = 0; i < 2; i++) {
        if (PROBE[i].x < a->x1 || PROBE[i].x > a->x2 || PROBE[i].y < a->y1 || PROBE[i].y > a->y2) continue;
        const uint8_t *px = b->data + (PROBE[i].y - a->y1) * b->header.stride + (PROBE[i].x - a->x1) * 2;
        s_probe[i] = 0x10000 | px[0] | (px[1] << 8);
    }
}

void compact_display_probe(uint32_t out[2], bool reset)
{
    out[0] = s_probe[0]; out[1] = s_probe[1];
    if (reset) s_probe[0] = s_probe[1] = 0;
}

lv_display_t *compact_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg.lv_adapter_cfg));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t dcfg = { .max_transfer_sz = BSP_LCD_H_RES * BUF_ROWS * 2 };
    ESP_ERROR_CHECK(bsp_display_new(&dcfg, &panel, &io));
    s_io = io;
    s_panel = panel;

    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = cfg.rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = BUF_ROWS,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = cfg.tear_avoid_mode,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) { ESP_LOGE(TAG, "display register failed"); return NULL; }
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, frame_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, frame_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, probe_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, flush_count_cb, LV_EVENT_FLUSH_START, NULL);
    /* Every LVGL flush goes through flush_hook -> panel_write: the ring is blended in and the screenshot recorded from
       the bytes as sent. Without the hook neither works, so the ring stays off (compact_ring_show refuses). */
    const esp_lv_adapter_draw_bitmap_callbacks_t cbs = { .custom_draw_bitmap = flush_hook };
    if (esp_lv_adapter_set_draw_bitmap_callbacks(disp, &cbs, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap hook refused: no ring, no screenshot");
        s_ring.failed = true;
    }

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(&cfg, &tp) == ESP_OK && tp) {
        const esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
        lv_indev_t *in = esp_lv_adapter_register_touch(&tcfg);
        if (in) {
            lv_indev_set_scroll_limit(in, TOUCH_DEADZONE_PX);   /* no scroll starts inside the dead zone */
            lv_indev_set_gesture_min_distance(in, SWIPE_MIN_PX);
            /* LVGL zeroes the swipe's travel whenever one read moves less than this; 2 px (default 3)
               lets a slower, longer swipe still reach SWIPE_MIN_PX */
            lv_indev_set_gesture_min_velocity(in, 2);
            lv_indev_add_event_cb(in, press_track_cb, LV_EVENT_PRESSED, NULL);
            lv_indev_add_event_cb(in, press_track_cb, LV_EVENT_PRESSING, NULL);
            s_touch_read = lv_indev_get_read_cb(in);
            if (s_touch_read) lv_indev_set_read_cb(in, touch_read_cb);
        }
    } else {
        ESP_LOGW(TAG, "touch init failed; continuing without touch");
    }
    bsp_display_brightness_init();                /* before the LVGL task exists: nothing else can touch the panel yet */
    lv_timer_create(heartbeat_cb, 250, NULL);     /* the watchdog's heartbeat; LVGL is initialised, its task not started */
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    return disp;
}
