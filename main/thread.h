#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "lvgl.h"

/*
 * The Thread face and the "Understood" view (spec 7.5, 8.1, 8.3; board 05/a and 05/c).
 *
 * Thread: the followed conversation (the `compact` surface follows the Telegram topic "Compact"), newest
 * at the bottom, scrolled by drag with 40 px fades, a scroll thumb on the right rim, the newest agent
 * message's buttons in the capsule. Reached by swiping left from Agents (Day · Now · Agents · Thread) or
 * OPEN after push-to-talk; swipe right goes back to Agents, PWR, sleep or 60 s untouched to Now. A tap on
 * an Agents row (or OPEN on a waiting job) opens that job's conversation instead (thread_open_conv_lv);
 * leaving the Thread makes the followed one the default again. While it shows, the device's `view` is
 * the conversation on show, so new messages arrive live.
 *
 * Understood: after a push-to-talk release. SENDING / WAITING / UNDERSTOOD + check / TRY AGAIN, what was
 * heard, then the agent's answer (its `reply` in this interaction). OPEN goes to the Thread.
 *
 * Both are a face on the active screen: above Now, below Earlier, Settings and the alert takeovers
 * (spec 8, 8.2), so thread_init() runs after dial_create() and before earlier_init(). Horizontal swipes
 * reach it through the gesture router in earlier.c (agents_swipe_lv, thread_swipe_lv, dayview_swipe_lv).
 * Functions without `_lv` take the display lock themselves.
 */
void thread_init(void);
void thread_close(void);                       /* PWR press, sleep: back to Now */
void thread_open_lv(void);                     /* LVGL context: the followed conversation (swipe left from Agents) */
void thread_open_conv_lv(const char *conv_id); /* LVGL context: a job's conversation (the bus lists it in `sessions`) */
void thread_sanitize(char *dst, size_t n, const char *src);   /* keep what the Hanken subset draws; drop emoji, Markdown */
bool thread_is_open(void);                     /* the Thread face or Understood is up */
bool thread_swipe_lv(lv_dir_t dir);            /* LVGL context, gesture router: true when the Thread took it */

/* push-to-talk (ptt.c) */
void thread_talk_begin(const char *cmid, const char *heard);   /* released: Understood, SENDING */
/* The same for an answer bound to a notification (E105): REPLY SENT on success; a refusal because the question closed
   shows TOO LATE with SEND TO PEGGY (title: the notification's title as sent, for the late body). */
void thread_talk_begin_reply(const char *cmid, const char *heard, const char *notification_id, const char *title);
void thread_talk_net(const char *cmid, bool ok);               /* the audio reached the bus, or not */

/* socket / fetch callbacks (main.c wires them) */
void thread_on_transcript(const char *cmid, const char *status, const char *text, const char *reason);
void thread_on_ack(const char *cmid, const char *status, const char *reason, const char *interaction_id);
void thread_on_reply(const cJSON *data);
void thread_on_conv_append(const char *conv_id, const cJSON *message);
void thread_on_conversation(const char *conv_id, const cJSON *page, int http_status);
void thread_on_action_result(const char *key, const char *status, const char *reason);
void thread_on_activity(const char *state, const char *status_text);
bool thread_debug_lv(const char *what, int dy);   /* testing: "thread", "now", "scroll" */
