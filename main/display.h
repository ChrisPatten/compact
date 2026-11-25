#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "esp_err.h"

/*
 * Bring up the CO5300 panel, touch and LVGL. Replaces bsp_display_start(): the BSP puts LVGL's
 * draw buffers in PSRAM, so every SPI flush mallocs an internal DMA bounce buffer of the whole
 * chunk (~46 KB), which fails once Wi-Fi is up. Here the draw buffers are small and internal.
 */
lv_display_t *compact_display_start(void);

/*
 * PANEL OWNERSHIP: display.c is the only code that talks to the panel or its SPI IO (esp_lcd_*, panel IO, the BSP's
 * brightness call). Everything below is the whole panel API. Each call takes the recursive display lock (the same lock
 * as bsp_display_lock) with a bounded 2 s wait, so it is safe from any task, from inside LVGL callbacks (already
 * holding the lock) and while LVGL is paused. LVGL's own flushes already run under that lock. tools/check_panel_owner.sh
 * (run by CMake) fails the build if any other file includes esp_lcd headers or calls these BSP functions.
 */

/* Panel brightness 0-100 (CO5300 register 0x51). ESP_ERR_TIMEOUT: the display lock was busy 2 s, command dropped. */
esp_err_t compact_display_brightness(int pct);

/* Panel sleep without deep standby (no reset or re-init on wake). */
void compact_display_sleep(void);       /* DISPOFF + SLPIN */
void compact_display_sleep_out(void);   /* SLPOUT, display still off */
void compact_display_on(void);          /* DISPON */

/* Park / release the LVGL task around panel sleep (power task). The display watchdog is held while paused. */
void compact_display_lvgl_pause(int timeout_ms);
/* Before a restart (restart.c): the breathing ring off and LVGL parked, each wait bounded by timeout_ms. Never call it
   from the LVGL task. True if LVGL acknowledged the pause. The device restarts afterwards; nothing resumes it. */
bool compact_display_quiesce(int timeout_ms);
void compact_display_lvgl_resume(void);
void compact_display_refresh_now(void);  /* redraw everything now (takes the display lock) */

/* Display watchdog (displaywd.c) support. hold(true) .. hold(false) around a known long operation in which LVGL may
   legitimately stall (nestable). Currently: LVGL pause for sleep, the OTA upload. */
void compact_display_wd_hold(bool on);
typedef struct {
    uint32_t hb;                 /* LVGL heartbeat counter */
    bool held, paused, lock_free;
    int hold;
    const char *op;              /* last panel operation from another task: brightness, sleep, sleep_out, display_on, lock_timeout, hammer */
    char op_task[16];
    uint32_t op_age_ms, flush_n, flush_age_ms;
    int lvgl_state, lvgl_stack_min;
} compact_display_diag_t;
void compact_display_diag(compact_display_diag_t *d);

/* Debug hooks for /debug/ui (Bearer): block the LVGL task for ms (100-60000); flip brightness ~50/s for s seconds
   against continuous redraws (nolock = the old racy path, expected to hang LVGL). */
bool compact_display_debug_hang(uint32_t ms);
bool compact_display_debug_hammer(int seconds, bool nolock);
void compact_display_frames(int *n, int *avg, int *max, bool reset);   /* render passes since last reset: count, avg ms, max ms */
/* GET /debug/perf: counts since the last /info?reset. inval = invalidate calls (before LVGL joins them), bands and
   px = flushed strips and their pixels, draw_us = time in the named draw callbacks (they build LVGL draw tasks;
   the pixels are drawn by the swdraw tasks). */
enum { PERF_DRAW_CAPTURE, PERF_DRAW_FACE, PERF_DRAW_RING_HOOK, PERF_DRAW_RING_STEP, PERF_DRAW_N };   /* ring: flush-path blend, direct breath steps */
typedef struct { uint32_t inval, bands, px, over35, over120; uint64_t draw_us[PERF_DRAW_N]; } compact_perf_t;
void compact_display_perf(compact_perf_t *out);
void compact_perf_draw_us(int which, uint32_t us);
void compact_display_probe(uint32_t out[2], bool reset);   /* testing: RGB565 flushed at (12,12) and (233,30); bit 16 = seen */

/*
 * The breathing ring's compositor (cue.c decides what; display.c draws it). LVGL never draws the ring: every LVGL band
 * gets it blended in on its way to the panel and a copy of the face under it is kept; a breath step sends only the
 * ring area, blended over that copy, through the same single panel writer. All of these run in the LVGL task (an
 * lv_timer, display lock held); show and stop refuse from any other task.
 */
bool compact_ring_show(uint32_t rgb888, int opa, int width, int inner_w, int planet_deg);   /* every tick while a ring is pending; opa 0 = hidden (the copy is still kept) */
void compact_ring_stop(void);                       /* nothing pending: erase and stop keeping the copy */
bool compact_ring_active(void);                     /* any task */
void compact_ring_tune(int mode_lvgl, int rect_cost);   /* debug, display lock held: 1 = every step through LVGL; push-set layout */
void compact_ring_bench(void);                      /* debug, display lock held: cost of a step's parts (info "bench") */
size_t compact_ring_info_json(char *out, size_t n);
const char *compact_ring_readback(void);            /* debug, any task but LVGL's: CO5300 ID, power mode and RAMRD after a step, as hex */
bool compact_display_in_lvgl_task(void);

/* Swipes trigger only after SWIPE_MIN_PX of travel (half the 466 px screen). A press that
   reached TOUCH_DEADZONE_PX of travel (touch_logic.h) is a drag, not a tap or a hold: tap handlers call this (LVGL
   still sends CLICKED after a drag that fell short of a swipe, and after a swipe), and display.c sends PRESS_LOST to
   the pressed object the moment the press leaves the dead zone, which cancels every hold. */
#define SWIPE_MIN_PX 233
bool compact_press_was_drag(void);
