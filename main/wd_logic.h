#pragma once
/*
 * Display watchdog decision logic. Pure C, no ESP-IDF includes, so test/host/wd_logic_test.c can run it on the Mac.
 *
 * The watchdog task samples once a second: wd_step(now, heartbeat, held). The heartbeat is a counter bumped by an
 * lv_timer, which only runs when the LVGL task completes lv_timer_handler(). "Stale" time is the time the counter
 * has not moved while the display was meant to be running. Each sample credits at most WD_SAMPLE_CAP_MS, so if the
 * watchdog task itself was delayed (the whole system stalled for a while) the late wake-up counts as one short
 * sample, not as seconds of staleness: a hang needs several consecutive samples with the counter frozen.
 * Everything is integer arithmetic on int64 milliseconds; there are no loops.
 */
#include <stdbool.h>
#include <stdint.h>

#define WD_SAMPLE_MS      1000    /* sampling period of the watchdog task */
#define WD_SAMPLE_CAP_MS  1500    /* most staleness one sample can add */
#define WD_TIMEOUT_MS     5000    /* frozen heartbeat for this long (>= 4 samples) = hang */
#define WD_TIMEOUT_REC_MS 15000   /* the same while Capture is recording (a restart loses the recording) */
#define WD_ARM_UPTIME_MS  20000   /* never act in the first 20 s after boot */
#define WD_ARM_BEATS      3       /* and not before the heartbeat has advanced this many times */

typedef struct {
    uint32_t last_hb;        /* heartbeat at the previous sample */
    int64_t  last_ms;        /* time of the previous sample */
    int64_t  stale_ms;       /* credited time with the heartbeat frozen */
    uint32_t beats;          /* heartbeat advances seen (saturates at WD_ARM_BEATS) */
    bool     primed;         /* have a previous sample */
} wd_t;

typedef enum { WD_OK = 0, WD_HELD, WD_NOT_ARMED, WD_STALE, WD_HANG } wd_result_t;

static inline void wd_init(wd_t *w) { w->last_hb = 0; w->last_ms = 0; w->stale_ms = 0; w->beats = 0; w->primed = false; }

/*
 * now_ms: monotonic time since boot. hb: the heartbeat counter. held: the display is legitimately not running
 * LVGL (paused for sleep, OTA upload, a debug hold): staleness is cleared, and counting restarts from zero when it
 * ends. timeout_ms: WD_TIMEOUT_MS or WD_TIMEOUT_REC_MS.
 */
static inline wd_result_t wd_step(wd_t *w, int64_t now_ms, uint32_t hb, bool held, int64_t timeout_ms)
{
    int64_t dt = w->primed ? now_ms - w->last_ms : 0;
    if (dt < 0) dt = 0;                                   /* a clock that went backwards adds nothing */
    if (dt > WD_SAMPLE_CAP_MS) dt = WD_SAMPLE_CAP_MS;
    w->last_ms = now_ms;
    w->primed = true;
    if (held) { w->stale_ms = 0; w->last_hb = hb; return WD_HELD; }
    if (hb != w->last_hb) {                               /* LVGL ran a timer pass since the last sample */
        w->last_hb = hb;
        w->stale_ms = 0;
        if (w->beats < WD_ARM_BEATS) w->beats++;
        return now_ms < WD_ARM_UPTIME_MS || w->beats < WD_ARM_BEATS ? WD_NOT_ARMED : WD_OK;
    }
    if (now_ms < WD_ARM_UPTIME_MS || w->beats < WD_ARM_BEATS) { w->stale_ms = 0; return WD_NOT_ARMED; }
    w->stale_ms += dt;
    if (w->stale_ms >= timeout_ms) return WD_HANG;
    return w->stale_ms > 0 ? WD_STALE : WD_OK;
}

/*
 * Restart-loop guard. `recent` counts consecutive hang restarts that came less than WD_LOOP_UPTIME_S after boot.
 * At WD_LOOP_MAX the watchdog stays off for that boot so a misbehaving watchdog cannot trap the device in a loop.
 */
#define WD_LOOP_UPTIME_S 300
#define WD_LOOP_MAX      3
static inline uint32_t wd_loop_next(uint32_t recent, uint32_t hang_uptime_s)
{
    if (hang_uptime_s >= WD_LOOP_UPTIME_S) return 1;      /* a long healthy run before this one: start counting again */
    return recent >= 0xFFFFu ? recent : recent + 1;
}
static inline bool wd_loop_tripped(uint32_t recent) { return recent >= WD_LOOP_MAX; }
