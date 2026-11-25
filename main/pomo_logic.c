#include "pomo_logic.h"

void pomo_start(pomo_t *p, int64_t focus_ms, int64_t break_ms, int64_t long_ms, int64_t now_ms)
{
    *p = (pomo_t){ .active = true, .phase = POMO_FOCUS, .focus_ms = focus_ms, .break_ms = break_ms, .long_ms = long_ms,
                   .since_ms = now_ms };
}

void pomo_stop(pomo_t *p) { p->active = false; p->paused = false; }

int64_t pomo_len_ms(const pomo_t *p)
{
    return p->phase == POMO_FOCUS ? p->focus_ms : p->phase == POMO_BREAK ? p->break_ms : p->long_ms;
}

static int64_t raw_elapsed(const pomo_t *p, int64_t now_ms)
{
    return p->acc_ms + (p->paused ? 0 : now_ms - p->since_ms);
}

int64_t pomo_elapsed_ms(const pomo_t *p, int64_t now_ms)
{
    int64_t e = raw_elapsed(p, now_ms), len = pomo_len_ms(p);
    return e < 0 ? 0 : e > len ? len : e;
}

void pomo_pause(pomo_t *p, int64_t now_ms)
{
    if (!p->active || p->paused) return;
    p->acc_ms += now_ms - p->since_ms;
    p->paused = true;
}

void pomo_resume(pomo_t *p, int64_t now_ms)
{
    if (!p->active || !p->paused) return;
    p->paused = false;
    p->since_ms = now_ms;
}

/* Leave the current phase; `carry` is the overshoot (ms) the next phase starts with. */
static pomo_ev_t advance(pomo_t *p, int64_t now_ms, int64_t carry)
{
    if (p->phase == POMO_FOCUS) {
        if (p->done < POMO_ROUNDS) p->done++;
        p->phase = p->done >= POMO_ROUNDS ? POMO_LONG : POMO_BREAK;
    } else if (p->phase == POMO_BREAK) {
        p->phase = POMO_FOCUS;
    } else {
        p->active = false;
        p->paused = false;
        return POMO_FINISHED;
    }
    p->acc_ms = carry > 0 ? carry : 0;
    p->since_ms = now_ms;
    return POMO_NEXT;
}

pomo_ev_t pomo_update(pomo_t *p, int64_t now_ms)
{
    if (!p->active || p->paused) return POMO_NONE;
    int64_t over = raw_elapsed(p, now_ms) - pomo_len_ms(p);
    if (over < 0) return POMO_NONE;
    pomo_ev_t ev = advance(p, now_ms, over);
    if (ev == POMO_NEXT && raw_elapsed(p, now_ms) >= pomo_len_ms(p)) p->acc_ms = pomo_len_ms(p) - 1;   /* one phase per call */
    return ev;
}

pomo_ev_t pomo_skip(pomo_t *p, int64_t now_ms)
{
    if (!p->active) return POMO_NONE;
    bool was_paused = p->paused;
    pomo_ev_t ev = advance(p, now_ms, 0);
    if (ev == POMO_NEXT && was_paused) p->paused = true;       /* a paused timer stays paused on the next phase */
    return ev;
}

const char *pomo_phase_name(pomo_phase_t ph) { return ph == POMO_FOCUS ? "focus" : ph == POMO_BREAK ? "break" : "long"; }
