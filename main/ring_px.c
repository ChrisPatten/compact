#include "ring_px.h"
#include <math.h>
#include <string.h>

_Static_assert(RP_SH % 2 == 0 && RP_SH >= 2, "strip rows must be even");
_Static_assert(RP_W % 2 == 0, "panel width must be even");

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* per row: the push-set run left of the centre (l1..l2) and right of it (r1..r2); l2 < 0 / r2 < 0 for none */
typedef struct { int16_t l1, l2, r1, r2; } rowrun_t;
static rowrun_t s_rows[RP_W];

/* Rects from the union of row runs (aligned even/odd, merged when they touch). Returns 0, 1 or 2. */
static int union_rects(int l1, int l2, int r1, int r2, int x1[2], int x2[2])
{
    bool L = l2 >= 0, R = r2 >= 0;
    if (L) { l1 &= ~1; l2 |= 1; }
    if (R) { r1 &= ~1; r2 |= 1; }
    if (L && R && l2 + 1 >= r1) { x1[0] = l1; x2[0] = imax(l2, r2); return 1; }
    int n = 0;
    if (L) { x1[n] = l1; x2[n] = l2; n++; }
    if (R) { x1[n] = r1; x2[n] = r2; n++; }
    return n;
}

/* Rects of rows a..b. */
static int seg_rects(int a, int b, int x1[2], int x2[2])
{
    int l1 = RP_W, l2 = -1, r1 = RP_W, r2 = -1;
    for (int y = a; y <= b; y++) {
        if (s_rows[y].l2 >= 0) { l1 = imin(l1, s_rows[y].l1); l2 = imax(l2, s_rows[y].l2); }
        if (s_rows[y].r2 >= 0) { r1 = imin(r1, s_rows[y].r1); r2 = imax(r2, s_rows[y].r2); }
    }
    return union_rects(l1, l2, r1, r2, x1, x2);
}

static void add_strip(rp_t *p, int a, int b)
{
    if (p->ns >= RP_MAXS) return;
    int s = p->ns++;
    p->sy1[s] = (int16_t)a; p->sy2[s] = (int16_t)b;
    p->first[s] = (uint16_t)p->n;
    for (int y = a; y <= b; y++) p->strip_of[y] = (uint16_t)s;
    int x1[2], x2[2], nr = seg_rects(a, b, x1, x2);
    uint16_t rows = 0;
    for (int k = 0; k < p->n; k++) rows = (uint16_t)(rows + (p->r[k].y2 - p->r[k].y1 + 1));
    for (int j = 0; j < nr && p->n < RP_MAXR; j++) {
        rp_rect_t *r = &p->r[p->n++];
        r->x1 = (int16_t)x1[j]; r->y1 = (int16_t)a; r->x2 = (int16_t)x2[j]; r->y2 = (int16_t)b;
        r->off = p->npx;
        r->row0 = rows;
        rows = (uint16_t)(rows + (b - a + 1));
        p->npx += rp_rect_px(r);
    }
    p->cnt[s] = (uint8_t)(p->n - p->first[s]);
}

uint32_t rp_geometry(rp_t *p, float cx, float rin, float rout, int rect_cost, uint32_t max_px)
{
    p->n = 0; p->ns = 0; p->npx = 0;
    memset(p->valid, 0, sizeof p->valid);
    for (int i = 0; i < RP_MAXROWS; i++) {
        p->span_a[i] = RP_W; p->span_b[i] = -1;                        /* no ring until rp_mask_build */
        p->gap_a[i] = RP_W; p->gap_b[i] = -1;
        p->pm_off[i] = 0;
        p->nb_a[i] = 0; p->nb_b[i] = RP_W - 1;                         /* face unknown: treat it all as not black */
    }
    const float rin2 = rin * rin, rout2 = rout * rout;
    for (int y = 0; y < RP_W; y++) {
        int l1 = RP_W, l2 = -1, r1 = RP_W, r2 = -1;
        float dy = y + 0.5f - cx;
        for (int x = 0; x < RP_W; x++) {
            float dx = x + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 < rin2 || d2 > rout2) continue;
            if (dx < 0) { l1 = imin(l1, x); l2 = imax(l2, x); }
            else        { r1 = imin(r1, x); r2 = imax(r2, x); }
        }
        s_rows[y] = (rowrun_t){ (int16_t)l1, (int16_t)l2, (int16_t)r1, (int16_t)r2 };
    }
    if (rect_cost <= 0) {
        for (int a = 0; a < RP_W; a += RP_SH) add_strip(p, a, imin(a + RP_SH - 1, RP_W - 1));
        return p->npx;
    }
    /* Shortest path over even row boundaries: f[e] = cheapest cover of rows 0..e-1. A segment a..e-1 costs
       rect_cost per rect plus its pixels; growing a segment upward only grows its rects, so the inner loop stops at
       the first rect over max_px. */
    static int64_t f[RP_W + 1];
    static int16_t ch[RP_W + 1];
    f[0] = 0;
    for (int e = 2; e <= RP_W; e += 2) {
        f[e] = INT64_MAX; ch[e] = (int16_t)(e - 2);
        int l1 = RP_W, l2 = -1, r1 = RP_W, r2 = -1;
        for (int a = e - 2; a >= 0; a -= 2) {
            for (int y = a; y <= a + 1; y++) {                                 /* the segment grows by rows a, a+1 */
                if (s_rows[y].l2 >= 0) { l1 = imin(l1, s_rows[y].l1); l2 = imax(l2, s_rows[y].l2); }
                if (s_rows[y].r2 >= 0) { r1 = imin(r1, s_rows[y].r1); r2 = imax(r2, s_rows[y].r2); }
            }
            int x1[2], x2[2], nr = union_rects(l1, l2, r1, r2, x1, x2);
            int64_t area = 0;
            bool over = false;
            for (int j = 0; j < nr; j++) {
                uint32_t px = (uint32_t)(x2[j] - x1[j] + 1) * (uint32_t)(e - a);
                if (px > max_px) over = true;
                area += px;
            }
            if (over && a < e - 2) break;
            int64_t c = f[a] + (int64_t)rect_cost * nr + area;
            if (c < f[e]) { f[e] = c; ch[e] = (int16_t)a; }
        }
    }
    int seg[RP_MAXS], nseg = 0;
    for (int e = RP_W; e > 0 && nseg < RP_MAXS; e = ch[e]) seg[nseg++] = e;   /* ch[e] <= e - 2: terminates */
    for (int i = nseg - 1; i >= 0; i--) add_strip(p, ch[seg[i]], seg[i] - 1);
    return p->npx;
}

void rp_mask_build(rp_t *p, float cx, float r_out, int w, int inner_w, int planet_deg, float gap_deg)
{
    if (!p->mask) return;
    float px = 0, py = 0;
    if (planet_deg >= 0) { px = sinf(planet_deg * 3.14159265f / 180); py = -cosf(planet_deg * 3.14159265f / 180); }
    const float cos_gap = cosf(gap_deg * 3.14159265f / 180);
    uint32_t pm = 0;
    for (int k = 0; k < p->n; k++) {
        const rp_rect_t *r = &p->r[k];
        uint8_t *m = p->mask + r->off;
        for (int y = r->y1; y <= r->y2; y++) {
            float dy = y + 0.5f - cx;
            int a = RP_W, b = -1;
            const uint8_t *row_m = m;
            for (int x = r->x1; x <= r->x2; x++, m++) {
                float dx = x + 0.5f - cx;
                float d = sqrtf(dx * dx + dy * dy);
                float outer = clamp01(d - (r_out - w) + 0.5f) * clamp01(r_out - d + 0.5f);
                float inner = inner_w ? clamp01(d - (r_out - w - inner_w) + 0.5f) * clamp01((r_out - w) - d + 0.5f) : 0;
                if (inner > 0 && planet_deg >= 0 && (dx * px + dy * py) / d > cos_gap) inner = 0;
                float v = fmaxf(outer * 255, inner * 85);
                *m = v >= 1 ? (uint8_t)(v + 0.5f) : 0;
                if (*m) { a = imin(a, x - r->x1); b = x - r->x1; }
            }
            int row = r->row0 + (y - r->y1);
            if (row >= RP_MAXROWS) continue;
            p->span_a[row] = (int16_t)a; p->span_b[row] = (int16_t)b;
            int ga = RP_W, gb = -1, z0 = -1;                                    /* widest zero run inside a..b */
            for (int x = a; x <= b; x++) {
                if (!row_m[x]) { if (z0 < 0) z0 = x; continue; }
                if (z0 >= 0 && x - z0 > gb - ga + 1) { ga = z0; gb = x - 1; }
                z0 = -1;
            }
            if (gb - ga + 1 < 8) { ga = RP_W; gb = -1; }                        /* not worth a split */
            p->gap_a[row] = (int16_t)ga; p->gap_b[row] = (int16_t)gb;
            p->pm_off[row] = pm;                                                /* span minus gap, packed (pm <= npx) */
            for (int x = a; x <= b && p->pmask; x++)
                if (x < ga || x > gb) p->pmask[pm++] = row_m[x];
        }
    }
}

void rp_set_blend(rp_t *p, uint32_t rgb888, int opa)
{
    if (opa < 0) opa = 0;
    if (opa > 255) opa = 255;
    /* RGB888 to RGB565 channels as LVGL does (truncation) */
    uint32_t cr = (rgb888 >> 19) & 31, cg = (rgb888 >> 10) & 63, cb = (rgb888 >> 3) & 31;
    p->blend = opa > 0;
    for (int m = 0; m < 256; m++) {
        uint32_t a = ((uint32_t)m * (uint32_t)opa + 127) / 255;          /* 0..255 */
        /* out = f + (c - f) * a / 255, rounded: f * (255 - a) / 255 + c * a / 255 in 8.8 fixed point */
        p->k[m] = (uint16_t)(((255 - a) * 256 + 127) / 255);
        p->add_r[m] = (uint16_t)((cr * a * 256 + 127) / 255 + 128);
        p->add_g[m] = (uint16_t)((cg * a * 256 + 127) / 255 + 128);
        p->add_b[m] = (uint16_t)((cb * a * 256 + 127) / 255 + 128);
        p->black[m] = rp_swap(rp_blend1(p, 0, (uint8_t)m));
    }
}

void rp_flush(rp_t *p, int x1, int y1, int x2, int y2, uint16_t *buf, int stride_px)
{
    if (!p->snap || !p->mask || !buf || p->n == 0) return;
    if (x2 < 0 || y2 < 0 || x1 >= RP_W || y1 >= RP_W || x1 > x2 || y1 > y2 || stride_px < x2 - x1 + 1) return;
    int s0 = p->strip_of[imax(y1, 0)], s1 = p->strip_of[imin(y2, RP_W - 1)];
    for (int s = s0; s <= s1 && s < p->ns; s++) {
        for (int j = 0; j < p->cnt[s]; j++) {
            int k = p->first[s] + j;
            const rp_rect_t *r = &p->r[k];
            int ix1 = imax(x1, r->x1), ix2 = imin(x2, r->x2), iy1 = imax(y1, r->y1), iy2 = imin(y2, r->y2);
            if (ix1 > ix2 || iy1 > iy2) continue;
            int rw = r->x2 - r->x1 + 1, n = ix2 - ix1 + 1;
            bool full = ix1 == r->x1 && ix2 == r->x2;
            for (int y = iy1; y <= iy2; y++) {
                uint16_t *src = buf + (size_t)(y - y1) * (size_t)stride_px + (size_t)(ix1 - x1);
                uint32_t o = r->off + (uint32_t)(y - r->y1) * (uint32_t)rw + (uint32_t)(ix1 - r->x1);
                uint16_t *sn = p->snap + o;
                int lo = RP_W, hi = -1;
                for (int i = 0; i < n; i++) {                                      /* the face as sent, panel order */
                    sn[i] = src[i];
                    if (src[i]) { if (lo == RP_W) lo = i; hi = i; }
                }
                int row = r->row0 + (y - r->y1);
                if (row < RP_MAXROWS) {                                            /* where the face is not black */
                    int sh0 = ix1 - r->x1;
                    if (full) { p->nb_a[row] = (int16_t)(hi >= 0 ? lo + sh0 : RP_W); p->nb_b[row] = (int16_t)(hi >= 0 ? hi + sh0 : -1); }
                    else if (hi >= 0) {
                        if (lo + sh0 < p->nb_a[row]) p->nb_a[row] = (int16_t)(lo + sh0);
                        if (hi + sh0 > p->nb_b[row]) p->nb_b[row] = (int16_t)(hi + sh0);
                    }
                }
                if (full) p->valid[y] |= (uint8_t)(1u << j);
                if (!p->blend || row >= RP_MAXROWS) continue;
                int a = imax(p->span_a[row], ix1 - r->x1), b = imin(p->span_b[row], ix2 - r->x1);   /* rect-relative */
                const uint8_t *m = p->mask + r->off + (uint32_t)(y - r->y1) * (uint32_t)rw;
                int sh = ix1 - r->x1;                                                            /* src[0] is rect x = sh */
                for (int x = a; x <= b; x++) if (m[x]) src[x - sh] = rp_blend_panel(p, src[x - sh], m[x]);
            }
        }
    }
}

uint32_t rp_compose(const rp_t *p, int k, uint16_t *dst)
{
    if (k < 0 || k >= p->n || !p->snap || !p->mask) return 0;
    const rp_rect_t *r = &p->r[k];
    uint32_t n = rp_rect_px(r);
    int rw = r->x2 - r->x1 + 1, h = r->y2 - r->y1 + 1;
    for (int yy = 0; yy < h; yy++) {
        int row = r->row0 + yy;
        const uint16_t *f = p->snap + r->off + (uint32_t)yy * (uint32_t)rw;
        const uint8_t *m = p->mask + r->off + (uint32_t)yy * (uint32_t)rw;
        uint16_t *d = dst + (uint32_t)yy * (uint32_t)rw;
        /* the face: zeros, with the part that is not black copied (panel order, as sent) */
        int na = row < RP_MAXROWS ? p->nb_a[row] : 0, nb = row < RP_MAXROWS ? p->nb_b[row] : rw - 1;
        if (na < 0) na = 0;
        if (nb > rw - 1) nb = rw - 1;
        if (na > nb) memset(d, 0, (size_t)rw * 2);
        else {
            memset(d, 0, (size_t)na * 2);
            memcpy(d + na, f + na, (size_t)(nb - na + 1) * 2);
            memset(d + nb + 1, 0, (size_t)(rw - 1 - nb) * 2);
        }
        if (!p->blend || row >= RP_MAXROWS) continue;
        int a = p->span_a[row], b = p->span_b[row], ga = p->gap_a[row], gb = p->gap_b[row];
        int seg[2][2] = { { a, b }, { 1, 0 } };
        if (ga <= gb) { seg[0][1] = ga - 1; seg[1][0] = gb + 1; seg[1][1] = b; }   /* skip the hole */
        const uint8_t *pm = p->pmask ? p->pmask + p->pm_off[row] : NULL;
        for (int sgi = 0; sgi < 2; sgi++) {
            int sa = seg[sgi][0], sb = seg[sgi][1];
            if (sa < 0 || sb > rw - 1) { pm = NULL; if (sa < 0) sa = 0; if (sb > rw - 1) sb = rw - 1; }   /* never: spans lie in the rect */
            const uint8_t *mm = pm ? pm : m + sa;                               /* mm[i] = coverage at x = sa + i */
            if (sb < na || sa > nb) {                                           /* black face under the whole segment */
                const uint16_t *bl = p->black;
                uint16_t *dd = d + sa;
                for (int i = 0; i <= sb - sa; i++) dd[i] = bl[mm[i]];
            } else {
                for (int x = sa; x <= sb; x++) d[x] = rp_blend_panel(p, f[x], mm[x - sa]);
            }
            if (pm && sb >= sa) pm += sb - sa + 1;
        }
    }
    return n;
}

void rp_invalidate(rp_t *p)
{
    memset(p->valid, 0, sizeof p->valid);
    for (int i = 0; i < RP_MAXROWS; i++) { p->nb_a[i] = 0; p->nb_b[i] = RP_W - 1; }
}

bool rp_all_valid(const rp_t *p)
{
    for (int s = 0; s < p->ns; s++) {
        uint8_t need = (uint8_t)((1u << p->cnt[s]) - 1);
        for (int y = p->sy1[s]; y <= p->sy2[s]; y++)
            if ((p->valid[y] & need) != need) return false;
    }
    return p->n > 0;
}
