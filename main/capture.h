#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

/*
 * Capture (spec 6.4, 7.6, 8.1, 8.2; architecture.md s15): BOOT double-press records a conversation or a note.
 *
 * Audio: 16 kHz mono from the microphones, IMA ADPCM (adpcm.h, 4:1, ~8.3 KB/s), in 2 s chunks numbered from 0.
 * Chunks go into a PSRAM ring; an uploader task streams them to the bus over the Wi-Fi socket as JSON text frames
 * (capture_start / capture_chunk / capture_end, base64; agentbus docs/SURFACE_API.md "Capture") and deletes each one
 * when the bus acks it (the bus writes it to disk first). With no bus, chunks spill to the raw `storage` partition
 * (about 12 min) and upload once Wi-Fi is back, also after a restart. The phone link does not carry captures (v1).
 *
 * Stops: HOLD TO STOP (0.8 s), BOOT double-press, 2 h, battery at 5% (on battery), or storage full (ring and
 * flash both full). Starting is refused under 10% on battery. While recording: no push-to-talk, no sleep or rest,
 * spoken replies do not play, and an urgent Moment waits ("1 ALERT") instead of taking the glass.
 */
void capture_init(void);                  /* once, after fonts_init() and ptt_init(), under no lock */

/* BOOT (power task's boot handler) */
void capture_toggle(void);                /* double-press: start, or stop and save */
void capture_touch(void);                 /* single BOOT press or PWR click while recording: the full screen again */
void capture_touch_lv(void);              /* the same from LVGL context (display lock held): a touch woke a dimmed screen */
void capture_dismiss(void);               /* close the Saved screen now (PWR); the upload carries on */

bool capture_recording(void);             /* the microphones belong to Capture */
bool capture_busy(void);                  /* recording, or the Saved screen is showing: keep the glass awake */
void capture_note_alert(void);            /* an urgent Moment arrived while recording (power task) */
void capture_flush_now(void);             /* battery about to cut out: stop, and spill unsent audio to flash (blocks) */

/* socket task: a `capture_ack` frame */
void capture_on_ack(const cJSON *frame);

/* /info and the test hook */
size_t capture_info_json(char *out, size_t n);
bool capture_debug_ui(int seconds);   /* /debug/ui capture-ui <s>: the recording screen for s seconds, nothing recorded */
bool capture_debug_start(int seconds, bool tone);   /* record `seconds` (mics, or a test tone), then stop */
void capture_debug_offline(int seconds);             /* act as if the bus were unreachable for `seconds` (spill test) */
