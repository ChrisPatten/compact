/* The 12-hour fixed rim: see rim12.h. Pure C (time.h only), host tested by test/host/rim12_test.c. */
#include "rim12.h"
#include <math.h>
#include <string.h>

int rim_gap_valid(int g) { return g == 30 || g == 60 || g == 120 || g == 180 ? g : RIM_GAP_DEFAULT; }

/* Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar (H. Hinnant's days_from_civil). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t rim_wall(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 +
           lt.tm_sec;
}

float rim_deg(time_t t)
{
    int64_t s = rim_wall(t) % RIM_HALF_DAY_S;
    if (s < 0) s += RIM_HALF_DAY_S;
    return (float)s / 120.0f;
}

int32_t rim_window_s(int gap_min) { return RIM_HALF_DAY_S - rim_gap_valid(gap_min) * 60; }

int64_t rim_wall_offset(time_t now, time_t t) { return rim_wall(t) - rim_wall(now); }

bool rim_in_window(time_t now, int gap_min, time_t t)
{
    return t >= now && rim_wall_offset(now, t) <= rim_window_s(gap_min);
}

float rim_window_deg(time_t now, int gap_min, time_t t)
{
    int64_t w = t <= now ? 0 : rim_wall_offset(now, t), W = rim_window_s(gap_min);
    if (w < 0) w = 0;                 /* a fall-back hour: a later instant can read earlier on the wall clock */
    if (w > W) w = W;
    return rim_deg(now) + (float)w / 120.0f;
}

void rim_lanes(const rim_in_t *in, int n, time_t now, uint8_t *lane)
{
    time_t end1 = 0, end2 = 0;        /* where lanes 1 and 2 come free */
    if (n > RIM_MAX) n = RIM_MAX;
    for (int i = 0; i < n; i++) {
        const rim_in_t *m = &in[i];
        lane[i] = 0;
        if (!m->has_end || m->end <= m->start) continue;              /* points sit on the rim, not a lane */
        if (m->free) continue;                                        /* a free Moment is not busy: no lane, no overlap */
        if (m->end <= now - RIM_LOOKBACK_S || m->start >= now + RIM_LOOKAHEAD_S) continue;
        if (m->start >= end1) { lane[i] = 1; end1 = m->end; }
        else if (m->start >= end2) { lane[i] = 2; end2 = m->end; }
    }
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static uint8_t opa_of(float f) { return (uint8_t)lroundf(255.0f * clamp01(f)); }

void rim_build(rim_t *r, const rim_in_t *in, int n, time_t now, int gap_min)
{
    if (n > RIM_MAX) n = RIM_MAX;
    if (n < 0) n = 0;
    gap_min = rim_gap_valid(gap_min);
    const int64_t W = rim_window_s(gap_min);
    memset(r, 0, sizeof *r);
    r->now = now;
    r->gap_min = gap_min;
    r->planet = rim_deg(now);
    r->gap_deg = gap_min / 2.0f;
    r->edge = r->planet + (float)W / 120.0f;
    int64_t to_mid = 86400 - rim_wall(now) % 86400;                  /* wall-clock seconds to the next midnight */
    if (to_mid <= W) { r->midnight = true; r->midnight_deg = r->planet + (float)to_mid / 120.0f; }

    uint8_t lane[RIM_MAX];
    rim_lanes(in, n, now, lane);
    for (int i = 0; i < n && r->n < RIM_MAX; i++) {
        const rim_in_t *m = &in[i];
        rim_mark_t k;
        memset(&k, 0, sizeof k);
        k.src = (int16_t)i;
        k.free = m->free;
        if (m->has_end && m->end > m->start) {
            if (m->end <= now) continue;                               /* past: gone from the rim at once */
            int64_t ws = m->start <= now ? 0 : rim_wall_offset(now, m->start);
            if (ws < 0) ws = 0;
            if (ws > W) continue;                                       /* not revealed yet */
            if (!lane[i] && !m->free) { r->hidden++; continue; }
            int64_t we = rim_wall_offset(now, m->end);
            bool cut = we > W;
            if (we > W) we = W;
            if (we < ws) we = ws;
            float p = m->start <= now ? 1.0f : clamp01((float)(W - ws) / RIM_REVEAL_S);
            k.kind = RIM_SPAN;
            k.lane = lane[i];
            k.a0 = r->planet + (float)ws / 120.0f;
            k.a1 = r->planet + (float)we / 120.0f;
            k.opa = opa_of(p);
            if (cut) {                                                  /* the tip reads as growing, not cut */
                float t0 = r->edge - RIM_TIP_DEG;
                if (t0 < k.a0) t0 = k.a0;
                k.t0 = t0;
                k.t1 = r->edge;
                k.tip_opa = opa_of(0.5f * p);
                k.a1 = t0;
            }
            if (!k.opa && !k.tip_opa) continue;
        } else {
            if (m->start <= now) continue;                              /* a point Moment ends when it starts */
            int64_t ws = rim_wall_offset(now, m->start);
            if (ws < 0) ws = 0;
            if (ws > W) continue;
            float p = clamp01((float)(W - ws) / RIM_REVEAL_S);
            k.kind = RIM_POINT;
            k.a0 = k.a1 = r->planet + (float)ws / 120.0f;
            k.opa = opa_of(p);
            k.rad = 2.0f + 2.0f * p;
            if (!k.opa) continue;
        }
        r->m[r->n++] = k;
    }
}

bool rim_dot_in_gap(const rim_t *r, float deg)
{
    float off = fmodf(deg - r->edge + 720.0f, 360.0f);
    return off > 0.25f && off < r->gap_deg - 0.25f;
}
