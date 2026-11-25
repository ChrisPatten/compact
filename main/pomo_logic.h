#pragma once
/* Pomodoro cycle logic (pure, no LVGL or RTOS; host tested by test/host/pomo_logic_test.c).
 * Cycle: focus, break, focus, break, focus, break, focus, long break, then finished. `done` counts focus rounds
 * finished (a skipped focus round counts: skipping means "move on"). Time is passed in as monotonic milliseconds.
 * A phase that ends late (the caller was slow) carries the overshoot into the next phase, so there is no drift. */
#include <stdbool.h>
#include <stdint.h>

#define POMO_ROUNDS 4

typedef enum { POMO_FOCUS, POMO_BREAK, POMO_LONG } pomo_phase_t;
typedef enum { POMO_NONE, POMO_NEXT, POMO_FINISHED } pomo_ev_t;

typedef struct {
    bool active, paused;
    pomo_phase_t phase;
    int done;                       /* focus rounds finished, 0..4 */
    int64_t focus_ms, break_ms, long_ms;
    int64_t acc_ms;                 /* elapsed in this phase before the current run */
    int64_t since_ms;               /* when the current run began (ignored while paused) */
} pomo_t;

void pomo_start(pomo_t *p, int64_t focus_ms, int64_t break_ms, int64_t long_ms, int64_t now_ms);
void pomo_stop(pomo_t *p);
int64_t pomo_len_ms(const pomo_t *p);                       /* length of the current phase */
int64_t pomo_elapsed_ms(const pomo_t *p, int64_t now_ms);   /* clamped to 0..len */
void pomo_pause(pomo_t *p, int64_t now_ms);
void pomo_resume(pomo_t *p, int64_t now_ms);
pomo_ev_t pomo_update(pomo_t *p, int64_t now_ms);           /* call often: moves on when the phase has run out */
pomo_ev_t pomo_skip(pomo_t *p, int64_t now_ms);             /* to the next phase now */
const char *pomo_phase_name(pomo_phase_t ph);               /* "focus" | "break" | "long" */
