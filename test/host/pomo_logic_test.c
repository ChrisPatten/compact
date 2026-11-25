/* cc -I../../main pomo_logic_test.c ../../main/pomo_logic.c -o /tmp/pomo_test && /tmp/pomo_test */
#include <stdio.h>
#include "pomo_logic.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    pomo_t p;
    pomo_start(&p, 25 * 60000, 5 * 60000, 15 * 60000, 1000);
    CHECK(p.active && p.phase == POMO_FOCUS && pomo_elapsed_ms(&p, 1000) == 0);
    CHECK(pomo_update(&p, 1000 + 25 * 60000 - 1) == POMO_NONE);
    CHECK(pomo_update(&p, 1000 + 25 * 60000 + 7) == POMO_NEXT);
    CHECK(p.phase == POMO_BREAK && p.done == 1 && pomo_elapsed_ms(&p, 1000 + 25 * 60000 + 7) == 7);   /* overshoot carried */
    /* pause freezes the clock */
    int64_t t = 1000 + 25 * 60000 + 7;
    pomo_pause(&p, t + 1000);
    CHECK(pomo_elapsed_ms(&p, t + 999999) == 1007);
    CHECK(pomo_update(&p, t + 99999999) == POMO_NONE);
    pomo_resume(&p, t + 5000);
    CHECK(pomo_elapsed_ms(&p, t + 6000) == 2007);
    /* run the rest of the cycle with skips */
    t += 6000;
    CHECK(pomo_skip(&p, t) == POMO_NEXT && p.phase == POMO_FOCUS && p.done == 1);
    CHECK(pomo_skip(&p, t) == POMO_NEXT && p.phase == POMO_BREAK && p.done == 2);
    CHECK(pomo_skip(&p, t) == POMO_NEXT && p.phase == POMO_FOCUS);
    CHECK(pomo_skip(&p, t) == POMO_NEXT && p.phase == POMO_BREAK && p.done == 3);
    CHECK(pomo_skip(&p, t) == POMO_NEXT && p.phase == POMO_FOCUS);
    CHECK(pomo_skip(&p, t) == POMO_NEXT && p.phase == POMO_LONG && p.done == 4);
    CHECK(pomo_len_ms(&p) == 15 * 60000);
    CHECK(pomo_update(&p, t + 15 * 60000) == POMO_FINISHED && !p.active);
    CHECK(pomo_update(&p, t + 99999999) == POMO_NONE);
    /* skip while paused stays paused */
    pomo_start(&p, 60000, 60000, 60000, 0);
    pomo_pause(&p, 100);
    CHECK(pomo_skip(&p, 200) == POMO_NEXT && p.paused && pomo_elapsed_ms(&p, 99999) == 0);
    /* a very late update moves one phase only */
    pomo_start(&p, 1000, 1000, 1000, 0);
    CHECK(pomo_update(&p, 10000) == POMO_NEXT && p.phase == POMO_BREAK);
    printf(fails ? "%d failures\n" : "pomo_logic: all ok\n", fails);
    return fails != 0;
}
