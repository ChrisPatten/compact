#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "lvgl.h"

/*
 * The Agents face (spec 7.4, 5.8, 5.9, 8.1; board 04). It sits right of Now and left of Thread: swipe left
 * from Now opens it, swipe left again goes to Thread, swipe right goes back. PWR, sleep or 60 s untouched
 * returns to Now. It reads the `sessions` resource and never wakes the device or takes the glass.
 *
 * States (the same mapping as the Phase web client's sessionState / agentsModel):
 *   nothing running  two still rings + dot, NOTHING RUNNING, "LAST: <NAME>, 2:41 PM" from the newest done
 *                    job, "Hold BOOT and ask".
 *   several          top caption "1 failed · 2 working", every job as a pill (waiting, failed, working, done) in a
 *                    scrollable pill_list (3 fit; drag vertically for more, 40 px fades). A row with a conversation opens it in Thread.
 *   one waiting      the top job asks for an answer (needs_you): NAME · NEEDS YOU, its question (from the
 *                    answer-needed message, fetched from that job's conversation page) and the capsule with
 *                    its options and OPEN. A tap sends the button as a Thread button does; the bus clears
 *                    needs_you and the list comes back.
 * Durations refresh once a minute. The working-job count also feeds the count ring on Now (dial.c).
 * Functions without `_lv` take the display lock themselves.
 */
void agents_init(void);                         /* once, after dial_create(), under no lock */
void agents_on_sessions(const cJSON *sessions); /* `sessions` resource, id -> job (socket task) */
void agents_on_conversation(const char *conv_id, const cJSON *page, int http_status);   /* a page we asked for */
void agents_on_action_result(const char *key, const char *status, const char *reason);

void agents_close(void);                        /* any task: PWR press, sleep, push-to-talk */
bool agents_is_open(void);

/* LVGL context (gesture router and Thread) */
bool agents_swipe_lv(lv_dir_t dir);             /* true when Agents took the swipe, or Now's swipe left opened it */
void agents_open_lv(void);
void agents_close_lv(void);

bool agents_debug(const char *what, int dy);   /* /debug/ui: "agents-test <n|0>", "agents-scroll <dy>" */
