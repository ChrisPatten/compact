#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "reply_logic.h"

/*
 * Alert Moments (spec 6.2, 7.3, 8.2; boards 03a and 03b): what takes the face over Now.
 *   Urgent: the rose flood with the rim draining to expires_at, the changed time and consequence,
 *           two pills (the agent's, else DISMISS). Wakes from anything.
 *   Decision: periwinkle, NEEDS YOU, headline, preview, the agent's buttons and LATER. A button with a
 *           draft, and every approval, goes through REVIEW to "sends exactly this" and a 0.8 s hold.
 *   Notice: plum Inform screen for 20 s of lit glass (a 20 s drain arc, GOT IT), then it clears. It never
 *           wakes the glass; one that arrives in the dark shows when the glass lights, while fresh.
 * Sources: the `events` resource (weight, else level) and the `notices` resource (E89; level info =
 * notice, attention = decision, urgent = urgent). The highest weight owns the glass; SENT shows only
 * once the bus answers.
 *
 * The screen sits on the active screen above the face; the power menu and push-to-talk overlays
 * (LVGL's top layer) stay above it.
 */
void decision_init(void);                                    /* once, after dial_create(), under no lock */
void decision_on_events(const cJSON *events);                /* `events` resource, id -> Event (any task) */
void decision_on_notices(const cJSON *notices);              /* `notices` resource, id -> notice (any task) */
void decision_on_action_result(const char *key, const char *status, const char *reason);   /* any task */
void decision_set_aside(void);                               /* PWR press: LATER for whatever is showing */

/* 0 = none (ambient or ended), 1 = notice, 2 = decision, 3 = urgent. `notice`: an item of `notices`. */
int decision_weight(const cJSON *item, bool notice);

/* LVGL context (display lock held): is an alert / confirm / SENT screen up? Bring one back from Earlier. */
bool decision_showing(void);
bool decision_flood_showing(void);                          /* the urgent rose flood is on screen (the ring draws dark on it) */
void decision_reopen(const char *id);
bool decision_open_notice(void);                            /* a notice is only a ring cue; this opens the pending one (swipe up on Now) */
/* LVGL context (display lock held): a swipe down on a takeover dismisses it (swipe up before Nov 23). Decision, urgent
   and the confirm screen are set aside (filed open in Earlier, still waiting), a notice is cleared and
   acked, SENT and TOO LATE close. Any other swipe on a takeover does nothing. */
void decision_swipe_down_lv(void);

/* The breathing ring (cue.c) reads this: top weight of the open, unfinished items (0 none) and how many share it.
   LVGL context (display lock held). */
void decision_cue_query(int *level, int *count);

void decision_debug_notice(void);                           /* test hook: inject one info notice */

/*
 * Notification replies (E105; agentbus docs/NOTIFICATION_REPLIES_CONTRACT.md). Everything here does nothing unless the
 * NVS flag prefs/notif_reply is on (default off). A `notices` item with a reply mode that awaits an answer shows as the
 * Reply view: the decision layout with the prompt, its choices and SKIP; voice is BOOT only. Answers go back by the
 * frames in reply_logic.h; a refusal because the question closed shows TOO LATE with SEND TO PEGGY.
 */
/* Push-to-talk (ptt_hold, not the LVGL task): bind this press? id gets the notification id when BIND_YES; title gets
   the Reply view's title (caps, for the Listening screen) and raw its title as sent (for a late reply) whenever a Reply
   view is up. Bounded wait for the lock; with the flag off it returns BIND_OFF at once. */
reply_bind_t decision_reply_bind(char *id, size_t idn, char *title, size_t tn, char *raw, size_t rn);
void decision_reply_handed(const char *id);                 /* a bound answer was released: Understood shows */
void decision_on_notification(const cJSON *data);           /* v1 `notification` event (socket task) */
void decision_on_ack(const char *cmid, const char *status, const char *reason, const char *interaction_id);
const char *decision_reply_debug(const char *arg);          /* POST /debug/ui "reply ..." */
void decision_reply_info(char *out, size_t n);              /* /info "reply" */
/* Entry points from Now (proposal 3.2 b, c; flag on only). Display lock held. */
int decision_needs_count(void);                             /* items awaiting an answer, for the "N NEEDS YOU" caption */
bool decision_open_needs(void);                             /* a tap on the caption: the oldest awaiting item's Reply view */
