#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

/*
 * Notification replies (E105), the pure part: no LVGL, no ESP-IDF, host tested by test/host/reply_logic_test.c.
 * Contract: agentbus docs/NOTIFICATION_REPLIES_CONTRACT.md (frozen v1, Nov 22 2026). Decisions:
 * push-to-talk by BOOT only, a spoken reply binds only while the Reply view is the top screen, no confirm screen, no
 * private flag.
 *
 * Fields read from a `notices` item (all optional; a bus without E105 sends none of them and every item is mode none):
 *   reply: {mode, prompt, hints, on_expire} or null, reply_state, replied_at, reply_via, resolution, state, title.
 * Frames built here: audio_start with notification_id, notification_ack with skip, and the late `send` body.
 */

typedef enum { RM_NONE, RM_VOICE, RM_CHOICES, RM_BOTH } reply_mode_t;
typedef enum { RS_NONE, RS_AWAITING, RS_RECEIVED, RS_SKIPPED, RS_CLOSED } reply_state_t;

#define REPLY_PROMPT_MAX 80        /* contract: prompt <= 80 characters */
#define REPLY_RESOLUTION_MAX 60    /* contract: resolution <= 60 characters */
#define REPLY_TITLE_MAX 60         /* bus: title <= 60 characters (longer is cut with an ellipsis) */
#define REPLY_LATE_MAX 4000        /* bus: a `send` body is 1-4,000 characters */

typedef struct {
    reply_mode_t mode;
    reply_state_t state;           /* reply_state; RS_NONE when absent or unknown */
    bool open;                     /* `state` is active or acknowledged (a reply may still be possible) */
    char prompt[REPLY_PROMPT_MAX * 4 + 1];           /* UTF-8: up to 4 bytes a character */
    char resolution[REPLY_RESOLUTION_MAX * 4 + 1];
    char title[REPLY_TITLE_MAX * 4 + 8];             /* as sent, for the late reply's body */
    char via[8];                   /* reply_via: voice, choice, skip or "" */
} reply_info_t;

/* Read the E105 fields of one `notices` item (or v1 `notification` event data). Never fails: missing or malformed
   fields leave mode none, state none. Strings are cut at a UTF-8 boundary to fit. */
void reply_parse(const cJSON *item, reply_info_t *out);

bool reply_mode_voice(reply_mode_t m);      /* voice or both */
bool reply_mode_choices(reply_mode_t m);    /* choices or both */

/* The device may offer an answer: a reply mode, reply_state awaiting, item open (contract section 2). */
bool reply_can_answer(const reply_info_t *r);

/* ---- push-to-talk binding (contract 3.2; decision 4: only while the Reply view is the top screen) ---- */
typedef enum {
    BIND_YES,             /* audio_start carries notification_id */
    BIND_OFF,             /* the feature flag is off */
    BIND_NO_VIEW,         /* no Reply view is the top screen (Now, Day, Agents, Thread, an overlay above it ...) */
    BIND_NOT_VOICE,       /* the Reply view's mode is choices only: "NOT A VOICE QUESTION", unbound */
    BIND_CLOSED,          /* not awaiting any more (replied, skipped, resolved, expired) */
    BIND_TEST,            /* the debug reply-test item: never bound, nothing reaches the bus for it */
} reply_bind_t;

typedef struct {
    bool enabled;         /* NVS flag prefs/notif_reply */
    bool view_top;        /* a Reply view is up and nothing covers it */
    bool test;            /* the item is the reply-test item */
    bool expired;         /* expires_at has passed on the device clock */
    const reply_info_t *info;   /* the item on the Reply view (NULL when none) */
} reply_bind_ctx_t;

reply_bind_t reply_bind_decide(const reply_bind_ctx_t *c);
const char *reply_bind_word(reply_bind_t b);   /* short reason for logs and /info */

/* ---- refusals (contract 3.1, 3.2, 7): what the device does with a reason ---- */
typedef enum {
    OUT_OK,               /* not a refusal */
    OUT_LATE,             /* notification_not_found / not_found, notification_expired, already_replied: TOO LATE,
                             offer SEND TO PEGGY (contract 3.4) */
    OUT_NOT_ALLOWED,      /* reply_not_allowed: the input does not fit the mode, or no binding: NOT SENT */
    OUT_RETRY,            /* internal_error (the only reason that may reuse a client_msg_id) */
    OUT_FAILED,           /* no_route, invalid_body, invalid, anything else: NOT SENT */
} reply_outcome_t;

reply_outcome_t reply_outcome(const char *reason);

/* ---- the Reply view's buttons ---- */
typedef struct {
    int nchoice;          /* choice buttons, 0-3, in bus order */
    int choice[3];        /* index into the device's parsed actions (0..nact-1) */
    bool skip;            /* SKIP (notification_ack skip:true) */
    bool voice_hint;      /* "HOLD BOOT TO ANSWER" */
} reply_layout_t;

/* nact: the actions the device parsed (at most 3). Choice buttons only for choices / both; voice mode never shows the
   actions (a tap would be refused reply_not_allowed). SKIP whenever an answer is possible. */
void reply_layout(const reply_info_t *r, int nact, reply_layout_t *out);

/* ---- frames (each returns the length written, or -1 when it does not fit or an argument is missing) ---- */

/* {"type":"audio_start","client_msg_id":"..","rate":16000,"format":"pcm_s16le","notification_id":".."}
   notification_id NULL or "" gives the unbound frame, byte for byte the one the firmware has always sent. */
int reply_audio_start_json(char *buf, size_t n, const char *cmid, int rate, const char *notification_id);

/* {"type":"notification_ack","id":"..","skip":true} */
int reply_skip_json(char *buf, size_t n, const char *id);

/* The late reply's text (contract 3.4): (late reply to "<title>") <answer>, cut to REPLY_LATE_MAX bytes at a UTF-8
   boundary. An empty title gives (late reply) <answer>. */
int reply_late_body(char *buf, size_t n, const char *title, const char *answer);

/* {"type":"send","client_msg_id":"..","body":".."} with JSON escaping; returns a cJSON_malloc'd string (free with
   cJSON_free) or NULL. */
char *reply_send_json(const char *cmid, const char *body);

/* true when s is a reply-capable id: 1-150 bytes, no quote, backslash or control character (ids go into frames). */
bool reply_id_ok(const char *s);

/* ---- entry points (proposal 3.2 b and c) ---- */

/* The REPLY pill on the Inform screen: only with the flag on, on a notice-weight item (a decision-weight one takes the
   face as the Reply view itself) that still awaits an answer. */
bool reply_pill_visible(bool flag, const reply_info_t *r, bool notice_weight);

/* One item counts toward "N NEEDS YOU" when it still awaits an answer and is neither finished on the device (answered,
   skipped, cleared) nor past expires_at. Set aside to Earlier (LATER) still counts: that is the point of the caption. */
bool reply_needs_you(const reply_info_t *r, bool done, bool expired);

/* The Now face's top caption: "N NEEDS YOU" (a count of 1 reads "1 NEEDS YOU"). Returns the length, or 0 with buf
   emptied when the flag is off or n < 1 (the caption does not show). The count is capped at 99. */
int reply_needs_caption(bool flag, int n, char *buf, size_t sz);
