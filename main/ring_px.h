#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pixel side of the breathing-ring compositor (display.c), kept free of ESP-IDF and LVGL so it builds and is tested
 * on the host (test/host/ring_px_test.c). Every loop here is a bounded for loop over the fixed geometry.
 *
 * BYTE ORDER: every pixel buffer here is in PANEL order, the bytes exactly as the CO5300 receives them: RGB565 with
 * the high byte first in memory. On the little-endian ESP32-S3 a uint16_t read from such a buffer holds the colour
 * byte-swapped (rp_swap gives the native RGB565). LVGL renders native RGB565 and the adapter swaps each band in place
 * before the panel write; the compositor sees bands only after that swap (display.c, custom_draw_bitmap), so it never
 * touches native-order pixels except inside rp_blend_panel. Black is 0x0000 in both orders, which is why a byte-order
 * slip only shows over a face that is not black: the host test checks non-black faces in both orders.
 *
 * The "push set" is the annulus around the rim covered by horizontal strips, one or two rects per strip (left and
 * right of the hole), aligned to even start and odd end coordinates as the CO5300 needs. For every push-set pixel it
 * keeps the face pixel last sent by LVGL (snap, panel order) and the ring coverage (mask, 0..255), in rect order,
 * row-major inside each rect.
 */

#define RP_W 466                 /* square panel */
#define RP_SH 8                  /* strip height of the fixed layout (even) */
#define RP_MAXS (RP_W / 2)       /* strips are at least 2 rows */
#define RP_MAXR (2 * RP_MAXS)
#define RP_MAXROWS (2 * RP_W)    /* rect rows: every row is in one strip, at most two rects */

typedef struct { int16_t x1, y1, x2, y2; uint32_t off; uint16_t row0; } rp_rect_t;   /* inclusive; off = first pixel, row0 = first span */

typedef struct {
    int n;                                   /* rects */
    rp_rect_t r[RP_MAXR];
    int ns;                                  /* strips */
    int16_t sy1[RP_MAXS], sy2[RP_MAXS];
    uint16_t first[RP_MAXS];
    uint8_t cnt[RP_MAXS];                    /* rects of each strip: 1 or 2 (0 if the strip holds no ring) */
    uint16_t strip_of[RP_W];                 /* strip of each row */
    uint32_t npx;                            /* push-set pixels */
    uint16_t *snap;                          /* npx, caller allocates: the face as LVGL last sent it, panel order */
    uint8_t *mask;                           /* npx, caller allocates */
    uint8_t *pmask;                          /* npx, caller allocates (may be NULL): the coverage of each row's span segments, packed
                                                in row order, so a breath step reads it in one sequential sweep */
    int16_t span_a[RP_MAXROWS], span_b[RP_MAXROWS];   /* per rect row: first and last pixel (rect-relative x) with ring coverage; a > b if none */
    int16_t gap_a[RP_MAXROWS], gap_b[RP_MAXROWS];     /* per rect row: the widest run of no coverage inside the span (the hole of a merged row); a > b if none */
    uint32_t pm_off[RP_MAXROWS];                      /* per rect row: offset of its span segments in pmask */
    int16_t nb_a[RP_MAXROWS], nb_b[RP_MAXROWS];       /* per rect row: the face is black (0) outside nb_a..nb_b (a > b: all black) */
    uint8_t valid[RP_W];                     /* per row, bit j = rect j of the row's strip: snapshot row holds what the panel shows */
    /* blend tables for the current colour and opacity, by mask value: out = (f * k + add) >> 8 per channel */
    uint16_t k[256];
    uint16_t add_r[256], add_g[256], add_b[256];
    uint16_t black[256];                     /* the result over a black face, panel order */
    bool blend;                              /* false: opacity 0, nothing to blend */
} rp_t;

/* Push set for pixels whose centre lies within rin..rout of the panel centre (cx + 0.5 convention as dial.c).
   rect_cost <= 0: fixed strips of RP_SH rows. rect_cost > 0: strips chosen to minimise rect_cost * rects + pixels,
   with no rect over max_px pixels. Returns npx. */
uint32_t rp_geometry(rp_t *p, float cx, float rin, float rout, int rect_cost, uint32_t max_px);

/* Ring coverage: outer band width w at 255, inner band inner_w at 85 (skipped within gap_deg of the planet,
   planet_deg < 0 for none; inner_w 0 for no inner band). Also the spans. */
void rp_mask_build(rp_t *p, float cx, float r_out, int w, int inner_w, int planet_deg, float gap_deg);

/* Colour (RGB888) and opacity (0..255) for the next blends. */
void rp_set_blend(rp_t *p, uint32_t rgb888, int opa);

static inline uint16_t rp_swap(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* One native RGB565 pixel blended with the ring colour at coverage m (8-bit alpha, rounded). */
static inline uint16_t rp_blend1(const rp_t *p, uint16_t f, uint8_t m)
{
    uint32_t k = p->k[m];
    uint32_t r = ((uint32_t)(f >> 11) * k + p->add_r[m]) >> 8;
    uint32_t g = ((uint32_t)((f >> 5) & 63) * k + p->add_g[m]) >> 8;
    uint32_t b = ((uint32_t)(f & 31) * k + p->add_b[m]) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* THE blend of the compositor: one panel-order pixel in, one panel-order pixel out. Both the flush path and the
   breath-step path use only this, so they cannot disagree. */
static inline uint16_t rp_blend_panel(const rp_t *p, uint16_t f_panel, uint8_t m)
{
    return f_panel ? rp_swap(rp_blend1(p, rp_swap(f_panel), m)) : p->black[m];
}

/* Flush path: band (x1..x2, y1..y2) about to go to the panel, buf in panel order with stride_px pixels per row.
   Copies the face pixels inside the push set into snap (marking fully covered rows valid) and, if blending, blends
   the ring over them in place. */
void rp_flush(rp_t *p, int x1, int y1, int x2, int y2, uint16_t *buf, int stride_px);

/* Step path: composite rect k (snap + ring) into dst, packed, panel order. Black face rows are filled, not copied.
   Returns its pixel count (0 for a bad k). Uses only rp_blend_panel, as rp_flush does. */
uint32_t rp_compose(const rp_t *p, int k, uint16_t *dst);

void rp_invalidate(rp_t *p);                 /* snapshot no longer trusted */
bool rp_all_valid(const rp_t *p);
static inline uint32_t rp_rect_px(const rp_rect_t *r) { return (uint32_t)(r->x2 - r->x1 + 1) * (uint32_t)(r->y2 - r->y1 + 1); }
