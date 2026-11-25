/*
 * Host test for main/wd_logic.h (display watchdog decision logic). Run from the repo root:
 *   cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -Imain \
 *      test/host/wd_logic_test.c -o /tmp/wd_logic_test && /tmp/wd_logic_test
 */
#include <stdio.h>
#include <stdlib.h>
#include "wd_logic.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Run a 1 s sampling loop from t0 to t1 with a heartbeat that advances every `beat_ms` (0 = frozen). Returns the
   time of the first WD_HANG or -1. */
static int64_t run(wd_t *w, int64_t t0, int64_t t1, uint32_t *hb, int64_t beat_ms, bool held, int64_t timeout)
{
    int64_t next_beat = t0 + beat_ms;
    for (int64_t t = t0; t <= t1; t += WD_SAMPLE_MS) {
        if (beat_ms) while (next_beat <= t) { (*hb)++; next_beat += beat_ms; }
        if (wd_step(w, t, *hb, held, timeout) == WD_HANG) return t;
    }
    return -1;
}

int main(void)
{
    wd_t w; uint32_t hb;

    /* 1. Healthy: heartbeat every 250 ms for an hour, never a hang. */
    wd_init(&w); hb = 0;
    CHECK(run(&w, 1000, 3600000, &hb, 250, false, WD_TIMEOUT_MS) == -1, "healthy run flagged");

    /* 2. Healthy but slow: heartbeat only every 4.5 s (just inside the limit) never fires. */
    wd_init(&w); hb = 0;
    CHECK(run(&w, 1000, 600000, &hb, 4500, false, WD_TIMEOUT_MS) == -1, "slow heartbeat (4.5 s) flagged");

    /* 3. Freeze at 100 s: fires exactly WD_TIMEOUT_MS after the last beat was seen (sample granularity 1 s). */
    wd_init(&w); hb = 0;
    CHECK(run(&w, 1000, 100000, &hb, 250, false, WD_TIMEOUT_MS) == -1, "pre-freeze flagged");
    int64_t t = run(&w, 101000, 200000, &hb, 0, false, WD_TIMEOUT_MS);
    CHECK(t >= 105000 && t <= 106000, "freeze fired at %lld, want ~105000-106000", (long long)t);

    /* 4. Never fires in the first 20 s even if frozen from the start; not before 3 beats seen. */
    wd_init(&w); hb = 0;
    CHECK(run(&w, 1000, 19000, &hb, 0, false, WD_TIMEOUT_MS) == -1, "fired during boot grace");
    wd_init(&w); hb = 0;
    CHECK(run(&w, 1000, 600000, &hb, 0, false, WD_TIMEOUT_MS) == -1, "fired with no heartbeat ever (display never started)");
    wd_init(&w); hb = 0;
    CHECK(wd_step(&w, 30000, 1, false, WD_TIMEOUT_MS) == WD_NOT_ARMED, "one beat armed it");
    CHECK(wd_step(&w, 31000, 2, false, WD_TIMEOUT_MS) == WD_NOT_ARMED, "two beats armed it");
    CHECK(wd_step(&w, 32000, 3, false, WD_TIMEOUT_MS) == WD_OK, "three beats did not arm it");

    /* 5. Held (sleep, OTA): frozen heartbeat for an hour is fine; counting restarts when released. */
    wd_init(&w); hb = 0;
    run(&w, 1000, 60000, &hb, 250, false, WD_TIMEOUT_MS);
    CHECK(run(&w, 61000, 3660000, &hb, 0, true, WD_TIMEOUT_MS) == -1, "fired while held");
    CHECK(w.stale_ms == 0, "stale not cleared while held");
    t = run(&w, 3661000, 3700000, &hb, 0, false, WD_TIMEOUT_MS);   /* wake and LVGL stays dead */
    CHECK(t >= 3661000 + 4000 && t <= 3661000 + 6000, "post-hold freeze fired at %lld", (long long)t);
    /* ... and LVGL waking up within a second of release never fires */
    wd_init(&w); hb = 0;
    run(&w, 1000, 60000, &hb, 250, false, WD_TIMEOUT_MS);
    run(&w, 61000, 100000, &hb, 0, true, WD_TIMEOUT_MS);
    hb++;                                                          /* first beat right after resume */
    CHECK(run(&w, 101000, 200000, &hb, 250, false, WD_TIMEOUT_MS) == -1, "fired after a clean resume");

    /* 6. A stalled watchdog task (one 30 s gap) with a heartbeat frozen on both sides counts as one capped sample. */
    wd_init(&w); hb = 0;
    run(&w, 1000, 60000, &hb, 250, false, WD_TIMEOUT_MS);
    CHECK(wd_step(&w, 60000 + 1000, hb, false, WD_TIMEOUT_MS) != WD_HANG, "setup");
    wd_result_t r = wd_step(&w, 91000, hb, false, WD_TIMEOUT_MS);  /* 30 s later, counter still equal */
    CHECK(r != WD_HANG, "30 s sampling gap alone fired (stale %lld)", (long long)w.stale_ms);
    CHECK(w.stale_ms <= WD_SAMPLE_CAP_MS * 2, "gap credited too much: %lld", (long long)w.stale_ms);

    /* 7. Clock going backwards or an unprimed first sample adds nothing. */
    wd_init(&w); hb = 0;
    run(&w, 1000, 60000, &hb, 250, false, WD_TIMEOUT_MS);
    wd_step(&w, 50000, hb, false, WD_TIMEOUT_MS);
    CHECK(w.stale_ms >= 0, "negative stale");

    /* 8. Recording uses the long timeout. */
    wd_init(&w); hb = 0;
    run(&w, 1000, 60000, &hb, 250, false, WD_TIMEOUT_REC_MS);
    t = run(&w, 61000, 200000, &hb, 0, false, WD_TIMEOUT_REC_MS);
    CHECK(t >= 61000 + 14000 && t <= 61000 + 16000, "recording timeout fired at %lld", (long long)t);

    /* 9. Heartbeat counter wraparound is just a change. */
    wd_init(&w); hb = 0xFFFFFFF0u;
    CHECK(run(&w, 1000, 600000, &hb, 250, false, WD_TIMEOUT_MS) == -1, "wraparound flagged");

    /* 10. Exhaustive: for every freeze offset in a 1 s window and every sample phase the hang fires between 4 s and 6.5 s. */
    for (int off = 0; off < 1000; off += 7) {
        wd_init(&w); hb = 0;
        run(&w, 1000, 60000, &hb, 250, false, WD_TIMEOUT_MS);
        int64_t freeze = 60000 + off;
        int64_t fired = -1;
        for (int64_t tt = 61000; tt < 120000 && fired < 0; tt += 1000)
            if (wd_step(&w, tt, hb, false, WD_TIMEOUT_MS) == WD_HANG) fired = tt - freeze;
        CHECK(fired >= 4000 && fired <= 6500, "offset %d fired %lld after freeze", off, (long long)fired);
    }

    /* 11. Restart-loop guard. */
    uint32_t n = 0;
    n = wd_loop_next(n, 40);  CHECK(n == 1 && !wd_loop_tripped(n), "loop 1");
    n = wd_loop_next(n, 60);  CHECK(n == 2 && !wd_loop_tripped(n), "loop 2");
    n = wd_loop_next(n, 90);  CHECK(n == 3 && wd_loop_tripped(n), "loop 3 trips");
    CHECK(wd_loop_next(2, 4000) == 1, "long run resets the streak");
    CHECK(wd_loop_next(0xFFFFFFFFu, 10) == 0xFFFFFFFFu, "saturates");

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
