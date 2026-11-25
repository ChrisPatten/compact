#pragma once
#include <stdbool.h>
#include "lvgl.h"

/*
 * Push-to-talk (spec 7.5, 8.1, 8.2; board 05/b). BOOT press starts the microphones at once (so the first
 * word is not lost); a hold past BOOT_HOLD_MS shows Listening on LVGL's top layer (above every alert;
 * only Capture outranks it) and streams the audio to the bus (E92 frames). Release sends; a PWR press
 * while holding cancels with nothing sent; 60 s stops and sends by itself. After the release the Thread
 * module shows "Understood" with what was heard and the agent's answer.
 *
 * The audio lives in PSRAM until the next press, so TRY AGAIN can send the same recording (same
 * client_msg_id, so the bus never sends it twice).
 */
void ptt_init(void);                  /* once, after fonts_init(), under no lock */

/* BOOT (power.c boot task) */
void ptt_arm(void);                   /* pressed: start recording into the buffer, nothing shown or sent */
void ptt_disarm(void);                /* released before the hold: discard */
void ptt_hold(void);                  /* held: Listening, start streaming */
void ptt_release(void);               /* released after the hold: send */
void ptt_cancel(void);                /* PWR press: drop it, nothing sent (no-op unless listening) */
bool ptt_listening(void);
bool ptt_recording(void);             /* the microphones are open (armed, listening or still closing) */

/* Thread module: send the kept recording again, as `cmid`. False when it is gone (a newer press). A bound recording
   (E105 notification reply) is resent bound. */
bool ptt_retry(const char *cmid);
/* E105 TOO LATE: send the kept recording again unbound, as a new `cmid`, to the followed conversation. */
bool ptt_resend_unbound(const char *cmid);

/* Socket task: every `transcript` frame (interim ones update Listening; finals go to the Thread module). */
void ptt_on_transcript(const char *cmid, bool final, const char *status, const char *text, const char *reason);

/* Fonts the talk screens share (created by ptt_init): Hanken 600 22 and 23, Lexend 200 28. */
extern lv_font_t *g_hanken22, *g_hanken23, *g_lexend28;
