/* Host test of the FREE Moment logic (main/mom_logic.c and the free handling in main/rim12.c): a free Moment is not
   busy (no running, no next-busy target, no free-stretch edge), never an overlap, takes no rim lane.
   cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain \
      test/host/mom_logic_test.c main/mom_logic.c main/rim12.c -lm -o /tmp/mom_logic_test && /tmp/mom_logic_test */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mom_logic.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static time_t day(int h, int mi)
{
    struct tm t = { .tm_year = 2026 - 1900, .tm_mon = 10, .tm_mday = 23, .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    return mktime(&t);
}
static rim_in_t busy(time_t s, time_t e) { return (rim_in_t){ s, e, true, false }; }
static rim_in_t fre(time_t s, time_t e) { return (rim_in_t){ s, e, e > s, true }; }
static rim_in_t fpt(time_t s) { return (rim_in_t){ s, s, false, true }; }

static void test_scan(void)
{
    time_t d0 = day(0, 0), d1 = day(0, 0) + 86400;
    mom_scan_t o;
    /* 7:15: a free 7:00-8:00 is running; busy 9-10 and a free point 8:30 ahead */
    rim_in_t a[] = { fre(day(7, 0), day(8, 0)), fpt(day(8, 30)), busy(day(9, 0), day(10, 0)) };
    mom_scan(a, 3, day(7, 15), d0, d1, 60, &o);
    CHECK(o.running == -1, "a free Moment is not a busy one running: %d", o.running);
    CHECK(o.free_running == 0, "free_running %d", o.free_running);
    CHECK(o.next == 1, "next is the free point: %d", o.next);
    CHECK(o.busy_next == 2, "busy_next skips the free point: %d", o.busy_next);
    CHECK(o.upcoming == 2, "upcoming %d", o.upcoming);
    CHECK(o.gap_from == 0, "gap_from %ld", (long)o.gap_from);

    /* 8:15: the free span is over; it does not start a free stretch (gap_from stays 0) */
    mom_scan(a, 3, day(8, 15), d0, d1, 60, &o);
    CHECK(o.gap_from == 0, "a free end is not a gap start: %ld", (long)o.gap_from);
    CHECK(o.free_running == -1 && o.running == -1, "nothing running");

    /* a busy one ended at 6:30 and a free one at 8:00: the stretch began at 6:30 */
    rim_in_t b[] = { busy(day(5, 30), day(6, 30)), fre(day(7, 0), day(8, 0)), busy(day(11, 0), day(12, 0)) };
    mom_scan(b, 3, day(8, 15), d0, d1, 60, &o);
    CHECK(o.gap_from == day(6, 30), "gap_from from busy only: %ld", (long)o.gap_from);

    /* busy running with a free one overlapping: running is the busy one; the free one is free_running */
    rim_in_t c[] = { busy(day(9, 0), day(10, 0)), fre(day(9, 30), day(9, 45)) };
    mom_scan(c, 2, day(9, 35), d0, d1, 60, &o);
    CHECK(o.running == 0 && o.free_running == 1, "running %d free_running %d", o.running, o.free_running);

    /* old data (no free anywhere) behaves as before */
    rim_in_t d[] = { busy(day(9, 0), day(10, 0)), busy(day(11, 0), day(12, 0)) };
    mom_scan(d, 2, day(8, 0), d0, d1, 60, &o);
    CHECK(o.next == 0 && o.busy_next == 0 && o.running == -1 && o.free_running == -1, "plain");

    /* beyond the window: neither next nor busy_next (7:00 + 11 h = 18:00 edge at the 1 h gap) */
    rim_in_t e[] = { fpt(day(19, 0)), busy(day(20, 0), day(21, 0)) };
    mom_scan(e, 2, day(7, 0), d0, d1, 60, &o);
    CHECK(o.next == -1 && o.busy_next == -1, "outside the window");
}

static void test_overlap_gap(void)
{
    rim_in_t x = busy(day(8, 0), day(9, 0)), y = busy(day(8, 45), day(9, 30)), f = fre(day(8, 0), day(9, 30));
    CHECK(mom_overlap_min(&x, &y) == 15, "busy pair %d", mom_overlap_min(&x, &y));
    CHECK(mom_overlap_min(&x, &f) == 0 && mom_overlap_min(&f, &x) == 0, "free vs busy is not an overlap");
    rim_in_t g = fre(day(8, 30), day(9, 15));
    CHECK(mom_overlap_min(&f, &g) == 0, "free vs free is not an overlap");
    rim_in_t nb = busy(day(11, 0), day(12, 0)), nf = fre(day(10, 0), day(10, 30));
    CHECK(mom_gap_min(&nb, day(9, 0)) == 120, "gap before a busy row spans a free one: %d", mom_gap_min(&nb, day(9, 0)));
    CHECK(mom_gap_min(&nf, day(9, 0)) == 0, "no gap row before a free row");
    CHECK(mom_gap_min(&nb, 0) == 0, "no gap row before any busy row has been listed");
}

static void test_rim(void)
{
    time_t now = day(7, 0);
    /* a free span overlapping a busy one takes no lane (the busy overlapping pair still takes 1 and 2, as without it) */
    rim_in_t in[] = { fre(day(8, 0), day(9, 0)), busy(day(8, 30), day(9, 30)), busy(day(9, 0), day(10, 0)) };
    uint8_t lane[3];
    rim_lanes(in, 3, now, lane);
    CHECK(lane[0] == 0 && lane[1] == 1 && lane[2] == 2, "lanes %d %d %d (free takes none; the busy pair takes 1, 2)", lane[0], lane[1], lane[2]);
    rim_in_t one[] = { fre(day(8, 0), day(9, 0)), busy(day(8, 30), day(9, 30)) };
    rim_lanes(one, 2, now, lane);
    CHECK(lane[0] == 0 && lane[1] == 1, "a busy span overlapping a free one stays on lane 1 (not amber): %d", lane[1]);
    rim_lanes(in, 3, now, lane);
    rim_t r;
    rim_build(&r, in, 3, now, 60);
    int nfree = 0, nbusy = 0;
    for (int i = 0; i < r.n; i++) { if (r.m[i].free) { nfree++; CHECK(r.m[i].lane == 0 && r.m[i].kind == RIM_SPAN, "free span lane"); } else nbusy++; }
    CHECK(nfree == 1 && nbusy == 2 && r.hidden == 0, "free %d busy %d hidden %d", nfree, nbusy, r.hidden);
    /* the free span sits where the Moment sits */
    CHECK(fabsf(r.m[0].a0 - (r.planet + 30.0f)) < 0.01f, "free span start %f", r.m[0].a0);

    /* a free point is a point mark flagged free; a free third overlap is not "hidden" */
    rim_in_t p[] = { busy(day(8, 0), day(10, 0)), busy(day(8, 30), day(10, 0)), busy(day(9, 0), day(10, 0)), fre(day(9, 0), day(9, 30)), fpt(day(9, 15)) };
    rim_build(&r, p, 5, now, 60);
    int pts = 0, spans = 0;
    for (int i = 0; i < r.n; i++) { if (r.m[i].kind == RIM_POINT && r.m[i].free) pts++; if (r.m[i].kind == RIM_SPAN) spans++; }
    CHECK(r.hidden == 1, "only the third busy one is hidden: %d", r.hidden);
    CHECK(pts == 1 && spans == 3, "points %d spans %d", pts, spans);

    /* without any free Moment the output equals the old behaviour: busy spans take lanes 1, 2 */
    rim_in_t q[] = { busy(day(8, 0), day(9, 0)), busy(day(8, 30), day(9, 30)) };
    rim_lanes(q, 2, now, lane);
    CHECK(lane[0] == 1 && lane[1] == 2, "plain lanes");
}

int main(void)
{
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    test_scan();
    test_overlap_gap();
    test_rim();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
