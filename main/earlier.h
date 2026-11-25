#pragma once
#include <stdbool.h>
#include <time.h>
#include "cJSON.h"

/*
 * The Earlier tray (spec 7.3 "Earlier tray", 8.1-8.3; board 03b/c). Swipe up from Now (only there, and only when no
 * notice card is pending) opens it; swipe down, PWR or 60 s untouched closes it, back to Now. Horizontal swipes do nothing
 * while it is open. Rows, newest first: glyph, title, "2:14 PM · cleared" or
 * "open · tap to act" (a 2 px peri edge; a tap brings the Moment back). HOLD TO CLEAR empties the
 * ended rows. A tap on an ended row opens its read-only detail (title, how and when it ended, body, reply resolution) with
 * its own HOLD TO CLEAR; swipe down returns to the list. Entries come from the bus's ended Events (tombstones) and from what the device clears or
 * sets aside; they last 24 hours and live in RAM (bus tombstones come back after a restart).
 *
 * Kinds: 1 notice, 2 decision, 3 urgent. Sources: 0 events, 1 notices.
 * earlier_note / earlier_drop_open / earlier_settle run with the display lock held (from decision.c).
 */
void earlier_init(void);                 /* once, after dial_create() and before decision_init() */
void earlier_close(void);                /* any task: PWR press, sleep */
bool earlier_is_open(void);
bool earlier_debug_swipe(bool up);       /* any task: POST /debug/ui "swipe up|down" runs the router's vertical branch */

void earlier_note(const char *id, const char *title, int kind, const char *state, time_t at, int src, time_t expires);
void earlier_note_text(const char *id, const char *body, const char *resolution);   /* the body / reply resolution of a row, for its detail view; keeps what is set when empty */
const char *earlier_debug(const char *arg);   /* any task: "test [off]" | "row <n>" | "clear" (POST /debug/ui earlier-test|earlier-row|earlier-clear) */
void earlier_drop_open(const char *id);  /* the Moment owns the glass again */
void earlier_settle(const cJSON *items, int src);   /* open rows whose item is gone become withdrawn / expired */
