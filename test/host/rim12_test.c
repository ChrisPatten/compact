/* Host test of the 12-hour rim maths (main/rim12.c): angles, window and gap, reveal, past vanish, lanes, midnight,
   daylight-saving days, and a fuzz of the drawing invariants.
   cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain \
      test/host/rim12_test.c main/rim12.c -lm -o /tmp/rim12_test && /tmp/rim12_test */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rim12.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 0.01f)

/* Local wall time (America/New_York rules) to epoch, with an explicit DST flag for the ambiguous fall-back hour. */
static time_t at(int y, int mo, int d, int h, int mi, int dst)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = dst };
    return mktime(&t);
}
static time_t day(int h, int mi) { return at(2026, 11, 23, h, mi, -1); }   /* a plain Monday */
static time_t tmw(int h, int mi) { return at(2026, 11, 24, h, mi, -1); }

static rim_in_t pt(time_t s) { return (rim_in_t){ s, s, false, false }; }
static rim_in_t sp(time_t s, time_t e) { return (rim_in_t){ s, e, true, false }; }

static const rim_mark_t *find(const rim_t *r, int src)
{
    for (int i = 0; i < r->n; i++) if (r->m[i].src == src) return &r->m[i];
    return NULL;
}
static float mod360(float a) { return fmodf(a + 3600.0f, 360.0f); }

static void test_angles(void)
{
    CHECK(NEAR(rim_deg(day(0, 0)), 0), "0:00 -> %f", rim_deg(day(0, 0)));
    CHECK(NEAR(rim_deg(day(12, 0)), 0), "12:00 -> %f", rim_deg(day(12, 0)));
    CHECK(NEAR(rim_deg(day(3, 0)), 90), "3:00");
    CHECK(NEAR(rim_deg(day(15, 0)), 90), "15:00");
    CHECK(NEAR(rim_deg(day(6, 0)), 180), "6:00");
    CHECK(NEAR(rim_deg(day(21, 0)), 270), "21:00");
    CHECK(NEAR(rim_deg(day(8, 30)), 255), "8:30 -> %f", rim_deg(day(8, 30)));
    CHECK(NEAR(rim_deg(day(14, 48)), 84), "14:48 -> %f", rim_deg(day(14, 48)));
    CHECK(NEAR(rim_deg(day(15, 6)), 93), "15:06");
    CHECK(NEAR(rim_deg(day(14, 22)), 71), "14:22");
    CHECK(NEAR(rim_deg(day(23, 59) + 59), 359.99167f), "23:59:59 -> %f", rim_deg(day(23, 59) + 59));
    CHECK(NEAR(rim_deg(day(7, 0) + 120), 211), "one degree is two minutes");
}

static void test_gap_values(void)
{
    CHECK(rim_gap_valid(30) == 30 && rim_gap_valid(60) == 60 && rim_gap_valid(120) == 120 && rim_gap_valid(180) == 180, "set");
    CHECK(rim_gap_valid(0) == 60 && rim_gap_valid(45) == 60 && rim_gap_valid(-30) == 60 && rim_gap_valid(240) == 60, "others -> 60");
    CHECK(rim_window_s(60) == 11 * 3600 && rim_window_s(30) == 41400 && rim_window_s(180) == 9 * 3600, "window");
}

/* The proposal's worked example: 7:00 AM, default gap. */
static void test_worked_example(void)
{
    time_t now = day(7, 0);
    rim_in_t in[] = { pt(day(7, 30)), pt(day(12, 0)), pt(day(17, 30)), pt(day(18, 30)), pt(day(17, 55)),
                      sp(day(17, 0), day(19, 0)), sp(day(6, 30), day(7, 30)), sp(day(6, 0), day(7, 0)), pt(day(7, 0)) };
    rim_t r; rim_build(&r, in, 9, now, 60);
    CHECK(NEAR(r.planet, 210), "planet %f", r.planet);
    CHECK(NEAR(r.edge, 540) && NEAR(mod360(r.edge), 180), "edge %f", r.edge);
    const rim_mark_t *m = find(&r, 0);
    CHECK(m && m->kind == RIM_POINT && NEAR(m->a0, 225) && m->opa == 255 && NEAR(m->rad, 4), "7:30 at 225, full");
    m = find(&r, 1);
    CHECK(m && NEAR(mod360(m->a0), 0), "noon at the top");
    m = find(&r, 2);
    CHECK(m && NEAR(mod360(m->a0), 165) && m->opa == 255, "17:30 at 165");
    CHECK(!find(&r, 3), "18:30 not yet drawn");
    m = find(&r, 4);                                     /* 5 minutes past the edge's reach: p = 1/3 */
    CHECK(m && m->opa == 85 && NEAR(m->rad, 2.0f + 2.0f / 3), "17:55 fading in: opa %d rad %f", m ? m->opa : -1, m ? m->rad : 0);
    m = find(&r, 5);                                     /* 17:00-19:00 crosses the edge */
    CHECK(m && m->kind == RIM_SPAN && m->lane && NEAR(mod360(m->a0), 150) && NEAR(m->a1, 532.5f) && NEAR(m->t0, 532.5f) &&
          NEAR(m->t1, 540) && m->opa == 255 && m->tip_opa == 128, "edge span: main to 532.5, 50%% tip to the edge");
    m = find(&r, 6);                                     /* running: from the planet */
    CHECK(m && NEAR(m->a0, 210) && NEAR(m->a1, 225) && m->opa == 255 && !m->tip_opa, "running from the planet");
    CHECK(!find(&r, 7), "a span that ended at 7:00 is gone at 7:00");
    CHECK(!find(&r, 8), "a point at 7:00 is gone at 7:00");
    CHECK(!r.midnight, "midnight is 17 h away");
    for (int d = 0; d < 360; d += 6) {
        bool in_zone = d > 180 && d < 210;
        CHECK(rim_dot_in_gap(&r, (float)d) == in_zone, "dot %d in gap: %d", d, rim_dot_in_gap(&r, (float)d));
    }
}

/* A span that straddles the edge grows as the planet moves; its start fades in. */
static void test_reveal_growth(void)
{
    rim_in_t in[] = { sp(day(18, 0), day(19, 0)) };
    rim_t a, b, c;
    rim_build(&a, in, 1, day(6, 59), 60);                /* edge 17:59: not yet */
    rim_build(&b, in, 1, day(7, 5), 60);                 /* edge 18:05: 5 min showing, all tip, p 1/3 */
    rim_build(&c, in, 1, day(7, 40), 60);                /* edge 18:40: 40 min showing */
    CHECK(a.n == 0, "not revealed");
    CHECK(b.n == 1 && b.m[0].opa == 85 && b.m[0].tip_opa == 43 && NEAR(b.m[0].a0, b.m[0].a1) && NEAR(b.m[0].t1 - b.m[0].t0, 2.5f),
          "just revealed: opa %d tip %d", b.n ? b.m[0].opa : -1, b.n ? b.m[0].tip_opa : -1);
    CHECK(c.n == 1 && c.m[0].opa == 255 && c.m[0].tip_opa == 128 && NEAR(c.m[0].a1 - c.m[0].a0, 12.5f) && NEAR(c.m[0].t1 - c.m[0].t0, 7.5f),
          "growing: main %f", c.n ? c.m[0].a1 - c.m[0].a0 : 0);
    rim_t d; rim_build(&d, in, 1, day(8, 0), 60);        /* edge 19:00 = its end: whole, no tip */
    CHECK(d.n == 1 && !d.m[0].tip_opa && NEAR(d.m[0].a1 - d.m[0].a0, 30), "reached its true end");
}

static void test_gap_setting(void)
{
    rim_in_t in[] = { pt(day(17, 30)), pt(day(18, 15)), pt(day(16, 45)) };
    rim_t r;
    rim_build(&r, in, 3, day(7, 0), 120);                /* window 10 h: edge 17:00 (150) */
    CHECK(NEAR(mod360(r.edge), 150) && !find(&r, 0) && find(&r, 2), "2 h gap");
    for (int d = 0; d < 360; d += 6) CHECK(rim_dot_in_gap(&r, (float)d) == (d > 150 && d < 210), "2h dot %d", d);
    rim_build(&r, in, 3, day(7, 0), 30);                 /* window 11.5 h: edge 18:30 */
    CHECK(find(&r, 0) && find(&r, 1) && find(&r, 1)->opa == 255, "0:30 gap shows 18:15");
    for (int d = 0; d < 360; d += 6) CHECK(rim_dot_in_gap(&r, (float)d) == (d > 195 && d < 210), "0:30 dot %d", d);
    rim_build(&r, in, 3, day(7, 0), 180);                /* window 9 h: edge 16:00 */
    CHECK(!find(&r, 0) && !find(&r, 1) && !find(&r, 2) && NEAR(r.gap_deg, 90), "3 h gap");
    rim_build(&r, in, 3, day(7, 0), 45);                 /* not allowed: the default */
    CHECK(r.gap_min == 60, "45 -> 60");
}

static void test_midnight(void)
{
    rim_in_t in[] = { pt(tmw(0, 30)), sp(day(23, 0), tmw(1, 0)), pt(day(23, 45)) };
    rim_t r; rim_build(&r, in, 3, day(23, 30), 60);
    CHECK(r.midnight && NEAR(r.midnight_deg, 360), "midnight tick at the top: %f", r.midnight_deg);
    const rim_mark_t *m = find(&r, 0);
    CHECK(m && NEAR(m->a0, 375) && m->a0 > r.midnight_deg, "00:30 tomorrow after the tick");
    m = find(&r, 1);
    CHECK(m && NEAR(m->a0, 345) && NEAR(m->a1, 390) && !m->tip_opa, "span across midnight is not clipped");
    m = find(&r, 2);
    CHECK(m && m->a0 < r.midnight_deg, "23:45 before the tick");
    rim_build(&r, in, 3, day(13, 0), 60);
    CHECK(r.midnight, "13:00: midnight is exactly at the edge");
    rim_build(&r, in, 3, day(12, 59), 60);
    CHECK(!r.midnight, "12:59: midnight beyond the edge");
    rim_build(&r, in, 3, tmw(0, 15), 60);                /* just after midnight: the next one is 23 h 45 away */
    CHECK(!r.midnight && NEAR(r.planet, 7.5f), "00:15");
    CHECK(find(&r, 0) && find(&r, 1) && !find(&r, 2), "after midnight: 00:30 and the running span remain");
}

static void test_lanes(void)
{
    /* A 9-10, B 9:30-11, D 9:45-10:15 (third overlap), C 10:30-11:30 (lane 1 is free again) */
    rim_in_t in[] = { sp(day(9, 0), day(10, 0)), sp(day(9, 30), day(11, 0)), sp(day(9, 45), day(10, 15)),
                      sp(day(10, 30), day(11, 30)), pt(day(9, 50)) };
    uint8_t ln[5];
    rim_lanes(in, 5, day(7, 0), ln);
    CHECK(ln[0] == 1 && ln[1] == 2 && ln[2] == 0 && ln[3] == 1 && ln[4] == 0, "lanes %d %d %d %d %d", ln[0], ln[1], ln[2], ln[3], ln[4]);
    rim_t r; rim_build(&r, in, 5, day(7, 0), 60);
    CHECK(r.hidden == 1 && !find(&r, 2) && find(&r, 4), "the third overlap is hidden and counted");
    /* after A ends B keeps lane 2 (A is still inside the lookback) */
    rim_build(&r, in, 5, day(10, 20), 60);
    CHECK(!find(&r, 0) && find(&r, 1) && find(&r, 1)->lane == 2 && find(&r, 3)->lane == 1, "B stays amber after A ends");
    /* the same lane before and after the reveal */
    rim_in_t far[] = { sp(day(19, 0), day(20, 0)), sp(day(19, 30), day(21, 0)) };
    rim_lanes(far, 2, day(7, 0), ln);
    uint8_t ln2[2]; rim_lanes(far, 2, day(9, 0), ln2);
    CHECK(ln[0] == 1 && ln[1] == 2 && ln2[0] == 1 && ln2[1] == 2, "lanes hold across the reveal");
    rim_build(&r, far, 2, day(7, 0), 60);
    CHECK(r.n == 0 && r.hidden == 0, "not revealed at 7:00: not drawn, not counted");
    rim_build(&r, far, 2, day(9, 0), 60);
    CHECK(r.n == 2 && find(&r, 1)->lane == 2, "revealed at 9:00 in amber");
    /* lookahead: a span 30 h ahead takes no lane */
    rim_in_t la[] = { sp(tmw(13, 0), tmw(14, 0)) };
    rim_lanes(la, 1, day(7, 0), ln);
    CHECK(ln[0] == 0, "beyond the 24 h lookahead: no lane");
}

/* Spring forward, Sunday Mar 8 2026, 2:00 EST -> 3:00 EDT. */
static void test_dst_spring(void)
{
    time_t now = at(2026, 3, 7, 19, 0, -1);
    rim_in_t in[] = { pt(at(2026, 3, 8, 6, 30, -1)), pt(at(2026, 3, 8, 5, 45, -1)), pt(at(2026, 3, 8, 6, 0, -1)) };
    rim_t r; rim_build(&r, in, 3, now, 60);
    CHECK(NEAR(r.planet, 210) && NEAR(mod360(r.edge), 180), "planet and edge on the wall clock");
    CHECK(!find(&r, 0), "6:30 EDT is 11.5 wall hours ahead (10.5 elapsed): not drawn, so never over the planet");
    CHECK(find(&r, 1) && NEAR(mod360(find(&r, 1)->a0), 172.5f), "5:45 at 172.5");
    CHECK(!find(&r, 2), "6:00 at the edge: p 0");
    for (int d = 0; d < 360; d += 6) CHECK(rim_dot_in_gap(&r, (float)d) == (d > 180 && d < 210), "spring dot %d", d);
    time_t a = at(2026, 3, 8, 1, 59, 0) + 59, b = a + 1;
    CHECK(NEAR(rim_deg(a), 59.99167f) && NEAR(rim_deg(b), 90), "the planet jumps 30 degrees at the change: %f -> %f", rim_deg(a), rim_deg(b));
    CHECK(rim_in_window(now, 60, in[1].start) && !rim_in_window(now, 60, in[0].start), "window predicate");
}

/* Fall back, Sunday Nov 1 2026, 2:00 EDT -> 1:00 EST. */
static void test_dst_fall(void)
{
    time_t now = at(2026, 10, 31, 21, 0, -1);
    rim_in_t in[] = { pt(at(2026, 11, 1, 7, 30, 0)), pt(at(2026, 11, 1, 8, 15, 0)) };
    rim_t r; rim_build(&r, in, 2, now, 60);
    CHECK(NEAR(r.planet, 270), "planet");
    CHECK(find(&r, 0) && NEAR(mod360(find(&r, 0)->a0), 225), "7:30 EST at 225 (11.5 h elapsed, 10.5 on the wall)");
    CHECK(!find(&r, 1), "8:15 beyond");
    /* inside the repeated hour: 1:10 EST comes 20 min after 1:50 EDT but reads 40 min earlier */
    time_t n2 = at(2026, 11, 1, 1, 50, 1), t2 = at(2026, 11, 1, 1, 10, 0);
    CHECK(t2 > n2 && t2 - n2 == 20 * 60, "test setup: %ld", (long)(t2 - n2));
    rim_in_t in2[] = { pt(t2), sp(n2 - 600, t2) };
    rim_build(&r, in2, 2, n2, 60);
    const rim_mark_t *m = find(&r, 0);
    CHECK(m && NEAR(m->a0, r.planet), "a point in the repeated hour sits on the planet, not 11 h ahead");
    m = find(&r, 1);
    CHECK(m && NEAR(m->a0, r.planet) && m->a1 >= m->a0, "a running span through the change does not run backwards");
    CHECK(NEAR(rim_window_deg(n2, 60, t2), r.planet), "window_deg clamps a negative offset");
}

static void test_window_deg(void)
{
    time_t now = day(7, 0);
    CHECK(NEAR(rim_window_deg(now, 60, now + 14 * 3600), 540), "expiry 14 h ahead clamps to the edge");
    CHECK(NEAR(rim_window_deg(now, 60, now + 3600), 240), "an hour ahead");
    CHECK(NEAR(rim_window_deg(now, 60, now - 3600), 210), "past clamps to the planet");
    CHECK(rim_in_window(now, 60, now) && !rim_in_window(now, 60, now - 1) && rim_in_window(now, 60, now + 11 * 3600) &&
          !rim_in_window(now, 60, now + 11 * 3600 + 1), "in window bounds");
}

static unsigned s_rng = 12345;
static unsigned rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return (s_rng >> 8) & 0xFFFFFF; }

/* Every mark sits between the planet and the edge, opacities and radii are in range, nothing past is drawn, the
   window never covers the gap, and RIM_MAX inputs never overflow. */
static void test_fuzz(void)
{
    time_t base = at(2026, 1, 1, 0, 0, -1);
    rim_in_t in[RIM_MAX + 8];
    int bad = 0;
    for (int it = 0; it < 4000; it++) {
        time_t now = base + (time_t)(rnd() % (366u * 86400u));
        int gaps[] = { 30, 60, 120, 180, 7 }, gap = gaps[rnd() % 5];
        int n = (int)(rnd() % (RIM_MAX + 8));
        time_t s = now - 14 * 3600;
        for (int i = 0; i < n; i++) {
            s += (time_t)(rnd() % 5400);
            bool span = rnd() % 3;
            in[i] = span ? sp(s, s + 60 + (time_t)(rnd() % (20 * 3600))) : pt(s);
        }
        rim_t r; rim_build(&r, in, n, now, gap);
        float eps = 0.02f;
        if (r.n > RIM_MAX || r.n > n) bad++;
        if (!NEAR(r.edge - r.planet, rim_window_s(gap) / 120.0f)) bad++;
        for (int i = 0; i < r.n; i++) {
            const rim_mark_t *m = &r.m[i];
            const rim_in_t *src = &in[m->src];
            if (m->a0 < r.planet - eps || m->a1 > r.edge + eps || m->a1 < m->a0 - eps) bad++;
            if (src->has_end ? src->end <= now : src->start <= now) bad++;
            if (m->kind == RIM_SPAN && (m->lane < 1 || m->lane > 2)) bad++;
            if (m->kind == RIM_POINT && (m->rad < 2 - eps || m->rad > 4 + eps || !m->opa)) bad++;
            if (m->tip_opa && (m->t0 < m->a0 - eps || m->t1 > r.edge + eps || m->tip_opa > 128 || m->t1 - m->t0 > RIM_TIP_DEG + eps)) bad++;
            if (rim_dot_in_gap(&r, mod360(m->a0 + 0.5f)) && m->a0 + 0.5f < r.edge - eps && m->a0 > r.planet + 0.5f) bad++;
        }
    }
    CHECK(bad == 0, "fuzz invariants broken %d times", bad);
}

int main(void)
{
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);           /* the device's zone string (dial_net_up) */
    tzset();
    test_angles();
    test_gap_values();
    test_worked_example();
    test_reveal_growth();
    test_gap_setting();
    test_midnight();
    test_lanes();
    test_dst_spring();
    test_dst_fall();
    test_window_deg();
    test_fuzz();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
