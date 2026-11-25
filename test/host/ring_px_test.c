/*
 * Host test for main/ring_px.c (the breathing ring's compositor maths) and main/panel_img.c (the screenshot's model of
 * panel memory). Run from the repo root with the sanitizers:
 *   cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain \
 *      test/host/ring_px_test.c main/ring_px.c main/panel_img.c -lm -o /tmp/ring_px_test && /tmp/ring_px_test
 * Uses the device's inputs: 466 px panel, centre 233, outer radius 233, band widths 6 and 8, inner band 5 px,
 * 22 degree gap at every planet degree, all rows.
 *
 * The simulation follows the real pipeline: LVGL renders native RGB565, the adapter byte-swaps each band in place
 * (panel order), the compositor's flush path (rp_flush) runs on the swapped band, and every write goes through one
 * panel_write() that both feeds a simulated CO5300 (GRAM addressed with the driver's 6-column gap, as
 * esp_lcd_co5300's draw_bitmap does) and records the screenshot (panel_img.c). Breath steps (rp_compose) go through the
 * same panel_write(). The test then decodes the simulated glass as the panel does and compares it with the expected
 * colours computed natively, and with the screenshot.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ring_px.h"
#include "panel_img.h"

#define W RP_W
#define CX 233
#define R_OUT 233
#define INNER_W 5
#define GAP_DEG 22
#define PS_RIN 219.0f
#define PS_ROUT 234.0f
#define XGAP 6                     /* bsp: esp_lcd_panel_set_gap(panel, 6, 0); draw_bitmap adds it to CASET */
#define GW (W + 2 * XGAP)          /* simulated GRAM width */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* The old cue.c mask_build (986a58e), verbatim apart from parameters, as the reference. */
static void old_mask(uint8_t *s_mask, int w, int pdeg, int s_inner)
{
    memset(s_mask, 0, (size_t)W * W);
    const float rin = R_OUT - w - INNER_W - 1.5f, rout = R_OUT + 1.5f;
    float px = 0, py = 0;
    if (pdeg >= 0) { px = sinf(pdeg * 3.14159265f / 180); py = -cosf(pdeg * 3.14159265f / 180); }
    const float cos_gap = cosf(GAP_DEG * 3.14159265f / 180);
    for (int y = 0; y < W; y++) {
        float dy = y + 0.5f - CX;
        if (fabsf(dy) > rout) continue;
        float hx = sqrtf(rout * rout - dy * dy), lo = CX - hx - 1, hi = CX + hx + 1;
        float hin = fabsf(dy) < rin ? sqrtf(rin * rin - dy * dy) : 0;
        int xlo = lo < 0 ? 0 : (int)lo, xhi = hi > W - 1 ? W - 1 : (int)hi;
        int hole_a = hin > 0 ? (int)(CX - hin) - 1 : xhi + 1, hole_b = hin > 0 ? (int)(CX + hin) + 1 : xhi + 1;
        for (int x = xlo; x <= xhi; x++) {
            if (x > hole_a && x < hole_b) { x = hole_b - 1; continue; }
            float dx = x + 0.5f - CX;
            float d = sqrtf(dx * dx + dy * dy);
            float outer = clamp01(d - (R_OUT - w) + 0.5f) * clamp01(R_OUT - d + 0.5f);
            float inner = clamp01(d - (R_OUT - w - INNER_W) + 0.5f) * clamp01((R_OUT - w) - d + 0.5f);
            if (!s_inner) inner = 0;
            if (inner > 0 && pdeg >= 0 && (dx * px + dy * py) / d > cos_gap) inner = 0;
            float v = fmaxf(outer * 255, inner * 85);
            if (v >= 1) s_mask[y * W + x] = (uint8_t)(v + 0.5f);
        }
    }
}

/* LVGL 9 lv_color_16_16_mix, for comparison only */
static uint16_t lv_mix(uint16_t c1, uint16_t c2, uint8_t mix)
{
    if (mix == 255) return c1;
    if (mix == 0) return c2;
    if (c1 == c2) return c1;
    mix = (uint8_t)(((uint32_t)mix + 4) >> 3);
    uint32_t bg = (uint32_t)(c2 | ((uint32_t)c2 << 16)) & 0x7E0F81F;
    uint32_t fg = (uint32_t)(c1 | ((uint32_t)c1 << 16)) & 0x7E0F81F;
    uint32_t result = ((((fg - bg) * mix) >> 5) + bg) & 0x7E0F81F;
    return (uint16_t)((result >> 16) | result);
}

static uint16_t to565(uint32_t c) { return (uint16_t)((((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31)); }

/* native RGB565 to RGB888 as LVGL and PNG viewers widen it */
static void rgb_of(uint16_t v, uint8_t o[3])
{
    uint8_t R = (uint8_t)(v >> 11), G = (uint8_t)((v >> 5) & 63), B = (uint8_t)(v & 31);
    o[0] = (uint8_t)((R << 3) | (R >> 2)); o[1] = (uint8_t)((G << 2) | (G >> 4)); o[2] = (uint8_t)((B << 3) | (B >> 2));
}

/* push-set membership map: rect index per pixel, -1 outside */
static int16_t owner[W * W];

static void build_owner(const rp_t *p)
{
    for (int i = 0; i < W * W; i++) owner[i] = -1;
    for (int k = 0; k < p->n; k++)
        for (int y = p->r[k].y1; y <= p->r[k].y2; y++)
            for (int x = p->r[k].x1; x <= p->r[k].x2; x++) {
                CHECK(owner[y * W + x] < 0, "rects %d and %d overlap at %d,%d", owner[y * W + x], k, x, y);
                owner[y * W + x] = (int16_t)k;
            }
}

static uint32_t idx(const rp_t *p, int x, int y)
{
    const rp_rect_t *r = &p->r[owner[y * W + x]];
    return r->off + (uint32_t)(y - r->y1) * (uint32_t)(r->x2 - r->x1 + 1) + (uint32_t)(x - r->x1);
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* F: the face as LVGL renders it (native RGB565). GRAM: the simulated panel memory, bytes as received, with the
   column gap. SHOT: the screenshot model (panel_img.c), recording every write at draw_bitmap coordinates. */
static uint16_t F[W * W], buf[W * 40], dst[9320];
static uint8_t GRAM[GW * W * 2], SHOT[W * W * 2];
static uint16_t shot_rows[W];
static int step_gap = XGAP;        /* what the step path adds to x: XGAP through draw_bitmap; 0 reproduces the old bug */

/* The one writer: what display.c panel_write() does. xe, ye exclusive; px packed, panel order. */
static void panel_write(int xs, int ys, int xe, int ye, const uint16_t *px, int gap)
{
    const uint8_t *b = (const uint8_t *)px;
    for (int y = ys; y < ye; y++)
        for (int x = xs; x < xe; x++) {
            size_t i = (size_t)((y - ys) * (xe - xs) + (x - xs)) * 2;
            int gx = x + gap;
            if (gx < 0 || gx >= GW) continue;
            GRAM[((size_t)y * GW + (size_t)gx) * 2] = b[i];
            GRAM[((size_t)y * GW + (size_t)gx) * 2 + 1] = b[i + 1];
        }
    CHECK(pimg_write(SHOT, W, W, xs, ys, xe, ye, b, shot_rows), "screenshot write %d,%d %d,%d", xs, ys, xe, ye);
}

/* The glass at (x, y), decoded as the panel shows it */
static void glass_rgb(int x, int y, uint8_t o[3]) { pimg_row_rgb(GRAM + ((size_t)y * GW + (size_t)(x + XGAP)) * 2, 1, o); }

/* Expected: native face blended natively, then widened. Also checks the snapshot holds the face in panel order. */
static void expect_panel(rp_t *p, const char *tag)
{
    int bad = 0, bad_snap = 0, bad_shot = 0;
    static uint8_t row[W * 3];
    for (int y = 0; y < W; y++) {
        pimg_row_rgb(SHOT + (size_t)y * W * 2, W, row);
        for (int x = 0; x < W; x++) {
            uint16_t e = F[y * W + x];
            if (owner[y * W + x] >= 0) {
                uint32_t q = idx(p, x, y);
                if (p->snap[q] != rp_swap(e)) bad_snap++;
                if (p->blend && p->mask[q]) e = rp_blend1(p, e, p->mask[q]);
            }
            uint8_t want[3], got[3];
            rgb_of(e, want);
            glass_rgb(x, y, got);
            if (memcmp(want, got, 3)) bad++;
            if (memcmp(got, row + 3 * x, 3)) bad_shot++;
        }
    }
    CHECK(bad == 0, "%s: %d glass pixels wrong", tag, bad);
    CHECK(bad_snap == 0, "%s: %d snapshot pixels wrong", tag, bad_snap);
    CHECK(bad_shot == 0, "%s: %d screenshot pixels differ from the glass", tag, bad_shot);
}

/* LVGL flush of rows y0..y1, x0..x1 through the adapter (pack, swap) and the compositor's flush path */
static void lvgl_flush(rp_t *p, int x0, int y0, int x1, int y1)
{
    int w = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) buf[(y - y0) * w + (x - x0)] = rp_swap(F[y * W + x]);   /* adapter: swap in place */
    rp_flush(p, x0, y0, x1, y1, buf, w);
    panel_write(x0, y0, x1 + 1, y1 + 1, buf, XGAP);
}

static void full_flush(rp_t *p)                             /* full-width bands of 20 rows, as LVGL flushes */
{
    for (int y0 = 0; y0 < W; y0 += 20) lvgl_flush(p, 0, y0, W - 1, y0 + 19 < W ? y0 + 19 : W - 1);
}

static uint32_t direct_step(rp_t *p, bool verify)           /* compose every rect and write it; verify: count bytes that differ from the screenshot first */
{
    uint32_t diff = 0;
    for (int k = 0; k < p->n; k++) {
        uint32_t n = rp_compose(p, k, dst);
        const rp_rect_t *r = &p->r[k];
        CHECK(n == rp_rect_px(r), "compose count");
        if (verify) diff += pimg_diff(SHOT, W, W, r->x1, r->y1, r->x2 + 1, r->y2 + 1, (const uint8_t *)dst);
        panel_write(r->x1, r->y1, r->x2 + 1, r->y2 + 1, dst, step_gap);
    }
    return diff;
}

static uint16_t face_px(int black_pct) { return (int)(rnd() % 100) < black_pct ? 0 : (uint16_t)rnd(); }

static void layout(int cost, int planet_step, int black_pct, bool pmask)
{
    static rp_t P;
    rp_t *p = &P;
    free(p->snap); free(p->mask); free(p->pmask);
    memset(p, 0, sizeof *p);
    uint32_t npx = rp_geometry(p, CX, PS_RIN, PS_ROUT, cost, 9320);
    p->snap = calloc(npx, 2);
    p->mask = calloc(npx, 1);
    p->pmask = pmask ? calloc(npx, 1) : NULL;
    uint32_t maxpx = 0, rows = 0;

    /* geometry */
    CHECK(p->n > 0 && p->n <= RP_MAXR, "rect count %d", p->n);
    uint32_t sum = 0;
    for (int k = 0; k < p->n; k++) {
        const rp_rect_t *r = &p->r[k];
        CHECK(r->x1 >= 0 && r->x2 < W && r->y1 >= 0 && r->y2 < W && r->x1 <= r->x2 && r->y1 <= r->y2, "rect %d bounds", k);
        CHECK(r->x1 % 2 == 0 && r->y1 % 2 == 0 && r->x2 % 2 == 1 && r->y2 % 2 == 1, "rect %d alignment %d,%d %d,%d", k, r->x1, r->y1, r->x2, r->y2);
        CHECK(r->off == sum, "rect %d offset", k);
        CHECK(r->row0 == rows, "rect %d row0", k);
        sum += rp_rect_px(r);
        rows += (uint32_t)(r->y2 - r->y1 + 1);
        if (rp_rect_px(r) > maxpx) maxpx = rp_rect_px(r);
    }
    CHECK(sum == npx && rows <= RP_MAXROWS, "npx / rows");
    int ycov = 0;
    for (int s = 0; s < p->ns; s++) {
        CHECK(p->sy1[s] == ycov && p->sy2[s] >= p->sy1[s], "strip %d rows", s);
        ycov = p->sy2[s] + 1;
        CHECK(p->cnt[s] >= 1 && p->cnt[s] <= 2, "strip %d rect count", s);
        for (int j = 0; j < p->cnt[s]; j++) {
            const rp_rect_t *r = &p->r[p->first[s] + j];
            CHECK(r->y1 == p->sy1[s] && r->y2 == p->sy2[s], "strip %d table", s);
        }
        for (int y = p->sy1[s]; y <= p->sy2[s]; y++) CHECK(p->strip_of[y] == s, "strip_of %d", y);
    }
    CHECK(ycov == W, "strips cover every row");
    CHECK(maxpx <= 9320, "rect larger than a draw buffer");
    build_owner(p);

    /* mask equals the old full-screen mask, and nothing of the old mask is outside the push set; spans hold it */
    static uint8_t ref[W * W];
    int nbuild = 0;
    for (int w = 6; w <= 8; w += 2)
        for (int inner = 0; inner <= 1; inner++)
            for (int pd = -1; pd < 360; pd += inner ? planet_step : 45) {
                old_mask(ref, w, pd, inner);
                rp_mask_build(p, CX, R_OUT, w, inner ? INNER_W : 0, pd, GAP_DEG);
                nbuild++;
                int bad = 0, outside_span = 0;
                for (int y = 0; y < W; y++)
                    for (int x = 0; x < W; x++) {
                        uint8_t o = ref[y * W + x];
                        if (owner[y * W + x] < 0) { if (o) bad++; continue; }
                        if (p->mask[idx(p, x, y)] != o) bad++;
                        const rp_rect_t *r = &p->r[owner[y * W + x]];
                        int row = r->row0 + (y - r->y1), rx = x - r->x1;
                        if (o && (rx < p->span_a[row] || rx > p->span_b[row] || (rx >= p->gap_a[row] && rx <= p->gap_b[row]))) outside_span++;
                    }
                CHECK(bad == 0, "mask w=%d inner=%d planet=%d: %d pixels differ", w, inner, pd, bad);
                CHECK(outside_span == 0, "mask w=%d inner=%d planet=%d: %d ring pixels outside the spans", w, inner, pd, outside_span);
            }

    /* LVGL flushes and direct steps through the simulated panel against the expected colours; non-black faces */
    memset(GRAM, 0, sizeof GRAM); memset(SHOT, 0, sizeof SHOT); memset(shot_rows, 0, sizeof shot_rows);
    for (int i = 0; i < W * W; i++) F[i] = face_px(black_pct);
    rp_mask_build(p, CX, R_OUT, 8, INNER_W, 100, GAP_DEG);
    rp_set_blend(p, 0xF7C95C, 200);
    CHECK(!rp_all_valid(p), "fresh snapshot must not be valid");
    full_flush(p);
    CHECK(rp_all_valid(p), "full-screen flush must leave the snapshot valid");
    for (int y = 0; y < W; y++) CHECK(shot_rows[y] >= W, "screenshot row %d not covered", y);
    expect_panel(p, "full flush");
    CHECK(direct_step(p, true) == 0, "verify step after a flush: the step path must send the bytes the flush path sent");

    /* small redraws (tiles narrower than a rect) change the face under the ring */
    for (int t = 0; t < 400; t++) {
        int x1 = (int)(rnd() % W) & ~1, y1 = (int)(rnd() % W) & ~1;
        int x2 = (x1 + (int)(rnd() % 40)) | 1, y2 = (y1 + (int)(rnd() % 40)) | 1;
        if (x2 >= W) x2 = W - 1;
        if (y2 >= W) y2 = W - 1;
        for (int y = y1; y <= y2; y++) for (int x = x1; x <= x2; x++) F[y * W + x] = face_px(t & 1 ? 100 : black_pct);
        lvgl_flush(p, x1, y1, x2, y2);
    }
    CHECK(rp_all_valid(p), "partial redraws must not drop validity");
    expect_panel(p, "tiles");
    CHECK(direct_step(p, true) == 0, "verify step after tiles");

    /* the rp_flush stride guard: a stride narrower than the band is refused, nothing touched */
    rp_flush(p, 0, 0, 99, 1, buf, 50);

    const int opas[] = { 31, 32, 0, 77, 255, 140 };
    const uint32_t scol[] = { 0xF7C95C, 0xB9A3BF, 0xF5A99B, 0x2A1A2C, 0xF5A99B, 0xB9A3BF };
    for (int s = 0; s < 6; s++) {
        rp_set_blend(p, scol[s], opas[s]);
        if (s == 3) rp_mask_build(p, CX, R_OUT, 6, INNER_W, 271, GAP_DEG);
        if (s == 5) rp_mask_build(p, CX, R_OUT, 8, 0, -1, GAP_DEG);
        direct_step(p, false);
        char tag[32]; snprintf(tag, sizeof tag, "direct step %d", s);
        expect_panel(p, tag);
        CHECK(direct_step(p, true) == 0, "verify step %d", s);
    }
    /* an LVGL redraw after direct steps blends at the current opacity */
    full_flush(p);
    expect_panel(p, "flush after steps");

    /* the Nov 22 fault: steps written without the driver's column gap (the old win() CASET) leave the glass wrong
       over a non-black face while the screenshot (flush path only, at the right place) still looks right */
    if (black_pct < 100) {
        rp_set_blend(p, 0xF7C95C, 230);
        full_flush(p);
        step_gap = 0;
        direct_step(p, false);
        step_gap = XGAP;
        int wrong = 0, wrong_out = 0;
        for (int y = 0; y < W; y++) for (int x = 0; x < W; x++) {
            uint16_t e = F[y * W + x];
            if (owner[y * W + x] >= 0 && p->mask[idx(p, x, y)]) e = rp_blend1(p, e, p->mask[idx(p, x, y)]);
            uint8_t want[3], got[3]; rgb_of(e, want); glass_rgb(x, y, got);
            bool w = memcmp(want, got, 3) != 0;
            wrong += w;
            wrong_out += w && owner[y * W + x] < 0;
        }
        CHECK(wrong > 1000, "the no-gap step must show on the simulated glass (%d px)", wrong);
        CHECK(wrong_out > 0, "the no-gap step also writes outside the push set (%d px)", wrong_out);
        if (cost == 4000 && black_pct == 20)
            printf("no-gap fault reproduced: %d glass pixels wrong, %d of them outside the push set (only an LVGL redraw repairs those)\n", wrong, wrong_out);
        direct_step(p, false);                                         /* the right path cannot repair what lies outside the push set */
        full_flush(p);                                                 /* an LVGL redraw does */
        expect_panel(p, "redraw after the fault");
    }

    /* validity: invalidate, a partial row does not validate it, a full band does */
    rp_invalidate(p);
    CHECK(!rp_all_valid(p), "invalidate");
    for (int y = 0; y < 8; y++) for (int x = 0; x <= 100; x++) buf[y * 101 + x] = 0;
    rp_flush(p, 0, 0, 100, 7, buf, 101);
    CHECK(!rp_all_valid(p), "partial row validated");
    full_flush(p);
    CHECK(rp_all_valid(p), "revalidate");
    rp_flush(p, -50, -50, -1, -1, buf, 10);
    rp_flush(p, W, W, W + 10, W + 10, buf, 10);
    rp_flush(p, 10, 10, 5, 5, buf, 10);
    CHECK(rp_compose(p, -1, dst) == 0 && rp_compose(p, p->n, dst) == 0, "compose range");

    rp_set_blend(p, 0xF5A99B, 180);
    double t1 = now_s();
    for (int it = 0; it < 50; it++) for (int k = 0; k < p->n; k++) rp_compose(p, k, dst);
    double t2 = now_s();
    printf("layout cost %7d black %3d%% pmask %d: %2d strips %3d rects %6u px (%4.1f%%), largest %4u px, %d masks, host compose %.0f us\n",
           cost, black_pct, pmask, p->ns, p->n, npx, 100.0 * npx / (W * W), maxpx, nbuild, (t2 - t1) / 50 * 1e6);
}

/* Byte order and edges of the screenshot model, independent of the ring */
static void panel_img_tests(void)
{
    /* decode: the panel takes the high byte first */
    const struct { uint8_t b[2]; uint8_t rgb[3]; } dec[] = {
        { { 0xF8, 0x00 }, { 255, 0, 0 } }, { { 0x07, 0xE0 }, { 0, 255, 0 } }, { { 0x00, 0x1F }, { 0, 0, 255 } },
        { { 0x00, 0x00 }, { 0, 0, 0 } }, { { 0xFF, 0xFF }, { 255, 255, 255 } },
    };
    for (unsigned i = 0; i < sizeof dec / sizeof dec[0]; i++) {
        uint8_t o[3]; pimg_row_rgb(dec[i].b, 1, o);
        CHECK(!memcmp(o, dec[i].rgb, 3), "decode %02X%02X", dec[i].b[0], dec[i].b[1]);
    }
    /* both byte orders: every RGB565 value, native in memory (what LVGL renders) vs swapped (what the panel gets) */
    int same = 0;
    for (uint32_t v = 0; v < 65536; v++) {
        uint16_t native = (uint16_t)v, panel = rp_swap(native);
        uint8_t want[3], got[3], slip[3];
        rgb_of(native, want);
        pimg_row_rgb((const uint8_t *)&panel, 1, got);         /* host is little-endian like the S3 */
        pimg_row_rgb((const uint8_t *)&native, 1, slip);       /* a byte-order slip: native bytes sent as they lie */
        CHECK(!memcmp(want, got, 3), "panel order %04X", v);
        same += !memcmp(want, slip, 3);
    }
    CHECK(same == 256, "a byte-order slip must change every colour whose two bytes differ (%d unchanged)", same);
    /* amber, the ring's decision colour, and what a slip would show */
    uint16_t amb = to565(0xF7C95C), amb_p = rp_swap(amb);
    uint8_t a1[3], a2[3];
    pimg_row_rgb((const uint8_t *)&amb_p, 1, a1);
    pimg_row_rgb((const uint8_t *)&amb, 1, a2);
    printf("amber F7C95C: RGB565 %04X, decoded %02X%02X%02X; with the bytes the wrong way round %02X%02X%02X\n",
           amb, a1[0], a1[1], a1[2], a2[0], a2[1], a2[2]);
    CHECK(a1[0] == 0xF7 && a1[1] == 0xCB && a1[2] == 0x5A, "amber decode");

    /* edges: first and last columns and rows land where addressed; out-of-range writes are refused untouched */
    static uint8_t img[W * W * 2];
    static uint16_t rows[W];
    memset(img, 0xAA, sizeof img); memset(rows, 0, sizeof rows);
    uint16_t px[4] = { rp_swap(0xF800), rp_swap(0x07E0), rp_swap(0x001F), rp_swap(0xFFFF) };
    CHECK(pimg_write(img, W, W, 0, 0, 2, 2, (const uint8_t *)px, rows), "write top-left");
    CHECK(pimg_write(img, W, W, W - 2, W - 2, W, W, (const uint8_t *)px, rows), "write bottom-right");
    uint8_t o[3];
    pimg_row_rgb(img, 1, o); CHECK(o[0] == 255 && !o[1] && !o[2], "top-left red");
    pimg_row_rgb(img + 2, 1, o); CHECK(!o[0] && o[1] == 255 && !o[2], "(1,0) green");
    pimg_row_rgb(img + (size_t)W * 2, 1, o); CHECK(!o[0] && !o[1] && o[2] == 255, "(0,1) blue");
    pimg_row_rgb(img + ((size_t)(W - 1) * W + W - 1) * 2, 1, o); CHECK(o[0] == 255 && o[1] == 255 && o[2] == 255, "bottom-right white");
    pimg_row_rgb(img + ((size_t)(W - 1) * W + W - 2) * 2, 1, o); CHECK(!o[0] && !o[1] && o[2] == 255, "(464,465) blue");
    CHECK(rows[0] == 2 && rows[1] == 2 && rows[W - 1] == 2 && rows[2] == 0, "row counts");
    CHECK(!pimg_write(img, W, W, W - 2, 0, W + 1, 2, (const uint8_t *)px, rows), "x past the edge refused");
    CHECK(!pimg_write(img, W, W, -1, 0, 1, 2, (const uint8_t *)px, rows), "negative x refused");
    CHECK(!pimg_write(img, W, W, 4, 4, 4, 6, (const uint8_t *)px, rows), "empty refused");
    CHECK(rows[0] == 2, "refused writes count nothing");
    CHECK(pimg_diff(img, W, W, 0, 0, 2, 2, (const uint8_t *)px) == 0, "diff same");
    px[3] = 0;
    CHECK(pimg_diff(img, W, W, 0, 0, 2, 2, (const uint8_t *)px) == 1, "diff one");
    CHECK(pimg_diff(img, W, W, 0, 0, W + 2, 2, (const uint8_t *)px) == 0, "diff out of range");
}

int main(void)
{
    static rp_t P;
    rp_t *p = &P;
    double t0 = now_s();
    rp_geometry(p, CX, PS_RIN, PS_ROUT, 0, 9320);

    /* blend is never a whole LSB off at 8-bit alpha, every face value, mask value and opacity; the panel-order blend
       is the native blend between two swaps */
    const uint32_t cols[] = { 0xB9A3BF, 0xF7C95C, 0xF5A99B, 0x2A1A2C, 0xFFFFFF, 0x000000 };
    double maxerr = 0;
    for (unsigned ci = 0; ci < sizeof cols / sizeof cols[0]; ci++) {
        uint32_t c = cols[ci];
        int cc[3] = { (int)((c >> 19) & 31), (int)((c >> 10) & 63), (int)((c >> 3) & 31) };
        for (int opa = 0; opa <= 255; opa += (ci < 3 ? 1 : 17)) {
            rp_set_blend(p, c, opa);
            CHECK(p->blend == (opa > 0), "blend flag");
            for (int m = 0; m < 256; m++) {
                int a = (m * opa + 127) / 255;
                CHECK(p->black[m] == rp_swap(rp_blend1(p, 0, (uint8_t)m)), "black table");
                for (uint32_t f = 0; f < 65536; f += (m == 0 || m == 85 || m == 255) ? 1 : 97) {
                    uint16_t o = rp_blend1(p, (uint16_t)f, (uint8_t)m);
                    CHECK(rp_blend_panel(p, rp_swap((uint16_t)f), (uint8_t)m) == rp_swap(o), "panel blend f %04X m %d", f, m);
                    int fc[3] = { (int)(f >> 11), (int)((f >> 5) & 63), (int)(f & 31) };
                    int oc[3] = { o >> 11, (o >> 5) & 63, o & 31 };
                    for (int ch = 0; ch < 3; ch++) {
                        double r = fc[ch] + (cc[ch] - fc[ch]) * a / 255.0;
                        double e = fabs(oc[ch] - r);
                        if (e > maxerr) maxerr = e;
                        CHECK(e <= 0.65, "blend col %06X opa %d m %d f %04X ch %d: %d vs %.3f", c, opa, m, f, ch, oc[ch], r);
                    }
                    if (m == 0) CHECK(o == f, "m=0 must leave the pixel");
                }
            }
        }
    }
    printf("blend: max error %.3f LSB (never a whole LSB off)\n", maxerr);

    /* distinct outer-band outputs over black at the 20 levels, ours vs LVGL's 5-bit mix (information) */
    for (int ci = 0; ci < 3; ci++) {
        const float amin[] = { 0.12f, 0.12f, 0.12f }, amax[] = { 0.90f, 1.00f, 1.00f };
        int n = 20, d_ours = 0, d_lv = 0, d_ours_in = 0, d_lv_in = 0;
        uint16_t last = 0xFFFF, lastl = 0xFFFF, last_in = 0xFFFF, lastl_in = 0xFFFF;
        for (int k = 0; k < n; k++) {
            float cq = (float)(k * k) / (float)((n - 1) * (n - 1));
            int opa = (int)lroundf(255 * (amin[ci] + (amax[ci] - amin[ci]) * cq));
            rp_set_blend(p, cols[ci], opa);
            uint16_t o = rp_blend1(p, 0, 255), l = lv_mix(to565(cols[ci]), 0, (uint8_t)opa);
            uint16_t oi = rp_blend1(p, 0, 85), li = lv_mix(to565(cols[ci]), 0, (uint8_t)((85 * opa) >> 8));
            d_ours += o != last; d_lv += l != lastl; d_ours_in += oi != last_in; d_lv_in += li != lastl_in;
            last = o; lastl = l; last_in = oi; lastl_in = li;
        }
        printf("distinct shades over black, 20 levels, %06X: outer %d (LVGL %d), inner %d (LVGL %d)\n", cols[ci], d_ours, d_lv, d_ours_in, d_lv_in);
    }

    panel_img_tests();
    layout(0, 1, 50, true);
    const int costs[] = { 200, 600, 1500, 4000, 1000000 };
    for (unsigned i = 0; i < sizeof costs / sizeof costs[0]; i++) layout(costs[i], i == 2 ? 1 : 7, i & 1 ? 97 : 50, i != 1);
    layout(4000, 5, 100, true);                                 /* the device layout over a black face */
    layout(4000, 3, 20, true);                                  /* the device layout over a busy face */
    layout(1500, 30, 90, false);                                /* no packed mask: compose reads the mask */
    printf("whole test %.1f s\n", now_s() - t0);
    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
