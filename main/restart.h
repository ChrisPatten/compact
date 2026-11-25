#pragma once
/*
 * Quiesce, then restart (Nov 23). Every restart path goes through here, never straight to esp_restart().
 *
 * Why: esp_restart() runs its shutdown handlers (esp_wifi_stop, panicdiag's cache writeback) on the calling task while
 * every other task keeps running. Twice (05900ce at an OTA, then 13a0f6c on the 7th of 7 OTA restarts) the LVGL task,
 * still drawing during that window, tripped the PSRAM heap's "block already marked as free" assert in lv_draw's task
 * cleanup, so the restart became a panic. Wi-Fi and lwIP buffers share that heap (SPIRAM_TRY_ALLOCATE_WIFI_LWIP).
 *
 * The sequence, in a task of its own (so the caller may be an esp_timer callback, the LVGL task or a socket task):
 *   1. arm a hard fallback: a ROM system reset RESTART_HARD_MS after the start, whatever happens below;
 *   2. ring off and LVGL paused (bounded wait; skipped when LVGL is hung: compact_restart_hung);
 *   3. the bus websocket stopped (bounded wait; the client may need up to a second to notice);
 *   4. the final step: esp_restart(), or the caller's (the OTA rollback).
 * A second call while one is under way does nothing. If the task cannot be created the final step runs inline.
 */
#include <stdbool.h>

#define RESTART_HARD_MS     1500   /* the ROM reset fallback, from the first call */
#define RESTART_LVGL_MS      400   /* LVGL pause (it acks between passes; a pass is under 300 ms) */
#define RESTART_WS_MS        500   /* websocket stop */

void compact_restart(const char *why);                             /* the normal path */
void compact_restart_hung(const char *why);                        /* the display watchdog: never waits on LVGL */
void compact_restart_with(const char *why, void (*final_step)(void));   /* e.g. the OTA rollback */
bool compact_restarting(void);
