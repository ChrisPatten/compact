/*
 * Day Dial: Plum Dusk "Home" face (spec sections 4-5), drawn in one LVGL draw callback.
 * Plum glass ground, NOAA daylight crescent, 60-dot 12 h fixed rim (12 at the top; rim12.h), the Moments of the
 * next 12 h minus the gap as arcs/dots with an overlap lane, now dot + second-hand moon, curved captions on r 168 / r 182,
 * NEXT label, countdown numeral + unit, upper (gap) and lower (day gone) progress arcs.
 * Redraw: the moon's box every second, the whole face on the minute or when data changes.
 * Agent count ring (spec 5, row 9) at 9 o'clock while any job is working.
 * Night ground: from sunset to the next sunrise every plum-glass screen is true black, with no
 * crescent shade (dial_is_night, dial_ground, dial_use_ground).
 * TODO: offline staleness marks.
 */
#include "dial.h"
#include "dial_priv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "fonts.h"
#include "power.h"
#include "display.h"
#include "esp_timer.h"
#include "prefs.h"
#include "rim12.h"
#include "mom_logic.h"
#include "intercom_ws.h"
#include "dayview.h"

/* Location for sunrise and sunset, set in menuconfig (thousandths of a degree). */
#define SITE_LAT (CONFIG_INTERCOM_LATITUDE_MILLIDEG / 1000.0f)
#define SITE_LON (CONFIG_INTERCOM_LONGITUDE_MILLIDEG / 1000.0f)
#include "decision.h"
#include "src/misc/lv_text_private.h"
#include "src/misc/lv_area_private.h"

#define MAX_EV RIM_MAX   /* 64: today, tomorrow morning and the bus's 24 h of ended tombstones */

typedef struct { char id[66], title[96]; time_t start, end; bool has_end, free; } ev_t;

static const char *TAG = "dial";
static lv_obj_t *s_face;
static bool s_swap;                /* the countdown is the big numeral, the time the small one */
static ev_t s_ev[MAX_EV];
static int s_nev;
static char s_state[12] = "idle", s_status[64], s_link[32] = "CONNECTING";
static int s_last_min = -1;
static int s_agents;               /* working agent jobs (agents.c); the count ring shows when > 0 */
static lv_area_t s_moon_box;
static dial_face_t s_mode = DIAL_FACE_DAY;
static unsigned s_ev_gen;          /* bumped whenever s_ev changes: the rim cache rebuilds */
static time_t s_clock_off;         /* POST /debug/ui "clock": drawing time minus real time (whole minutes) */
static bool s_testday;             /* POST /debug/ui "testday": test Moments stand in for the live resource */
static time_t s_gap_bus_seen = -1; /* dial_config without updated_at: the last value applied */

/* The rim cache: one rim_t and the Now face's derived values, rebuilt once a minute and when the Moments or the gap
   change, not on every 20-row strip (proposal 1.3). */
static struct {
    long key; unsigned gen; int gap; bool valid;
    rim_t rim;
    int next, busy_next, running, free_running;   /* indices into s_ev, -1 none (mom_logic.h): the window's next Moment, the next busy one, the busy one running, a free one running */
    time_t gap_from;               /* end of the last busy Moment finished today, 0 none */
    int upcoming;                  /* Moments still ahead today ("N LEFT TODAY") */
} s_rc;
static rim_in_t s_rim_in[MAX_EV];

/* Night ground: one shared style carries the ground colour for every glass screen (dial_use_ground). */
static lv_style_t s_ground_style;
static bool s_ground_ready, s_night;
static int s_night_override = -1;  /* /debug/ui "night on|off|auto": 1, 0, -1 */

/* ---- text helpers ---- */

/* Uppercase ASCII and Latin-1/Latin Ext-A letters; drop emoji, variation selectors and joiners; tidy spaces. */
static void clean_caps(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && o + 4 < n) {
        uint32_t cp; int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else { cp = 0x1F000; len = ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) ? 4 : 1; }
        p += len;
        bool keep = cp < 0x2190 || cp == 0x25C6;
        if (cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)) keep = false;
        if (!keep) continue;
        if (cp >= 'a' && cp <= 'z') cp -= 32;
        else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 0x20;
        else if (cp >= 0x100 && cp < 0x180 && (cp & 1) && cp != 0x131 && cp != 0x138 && cp != 0x149 && cp != 0x17F) cp -= 1;
        if (cp == ' ' && (o == 0 || dst[o - 1] == ' ')) continue;
        if (cp < 0x80) dst[o++] = (char)cp;
        else if (cp < 0x800) { dst[o++] = 0xC0 | (cp >> 6); dst[o++] = 0x80 | (cp & 0x3F); }
        else { dst[o++] = 0xE0 | (cp >> 12); dst[o++] = 0x80 | ((cp >> 6) & 0x3F); dst[o++] = 0x80 | (cp & 0x3F); }
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

/*
 * Measuring text through tiny_ttf costs ~80 us a glyph, and the face is laid out once per
 * 20-row strip (24 times a frame), so measurements are remembered. Fonts never change, so an
 * entry stays valid until it is replaced.
 */
#define TW_MEMO 32
static struct { const lv_font_t *f; int space; int w; char s[64]; } s_tw[TW_MEMO];
static int s_tw_next;

static int text_w_raw(const char *s, const lv_font_t *f, int space)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

static int text_w(const char *s, const lv_font_t *f, int space)
{
    bool memo = strlen(s) < sizeof s_tw[0].s;
    if (memo)
        for (int i = 0; i < TW_MEMO; i++)
            if (s_tw[i].f == f && s_tw[i].space == space && !strcmp(s_tw[i].s, s)) return s_tw[i].w;
    int w = text_w_raw(s, f, space);
    if (memo) {
        s_tw[s_tw_next].f = f; s_tw[s_tw_next].space = space; s_tw[s_tw_next].w = w;
        strcpy(s_tw[s_tw_next].s, s);
        s_tw_next = (s_tw_next + 1) % TW_MEMO;
    }
    return w;
}

/* Cut s in place with "..." until it is at most max_w px wide. */
static void fit_uncached(char *s, size_t n, const lv_font_t *f, int space, int max_w);

/* fit() with its last few answers remembered (input text -> cut length). */
#define FIT_MEMO 4
static struct { const lv_font_t *f; int space, max_w; size_t cut; char s[112]; } s_fit[FIT_MEMO];
static int s_fit_next;

static void fit(char *s, size_t n, const lv_font_t *f, int space, int max_w)
{
    bool memo = strlen(s) < sizeof s_fit[0].s;
    if (memo)
        for (int i = 0; i < FIT_MEMO; i++)
            if (s_fit[i].f == f && s_fit[i].space == space && s_fit[i].max_w == max_w && !strcmp(s_fit[i].s, s)) {
                if (s_fit[i].cut != (size_t)-1 && s_fit[i].cut + 4 <= n) memcpy(s + s_fit[i].cut, "...", 4);
                return;
            }
    char in[sizeof s_fit[0].s];
    if (memo) strcpy(in, s);
    size_t before = strlen(s);
    fit_uncached(s, n, f, space, max_w);
    if (memo) {
        size_t after = strlen(s);
        s_fit[s_fit_next].f = f; s_fit[s_fit_next].space = space; s_fit[s_fit_next].max_w = max_w;
        s_fit[s_fit_next].cut = (after == before && !strcmp(in, s)) ? (size_t)-1 : after - 3;
        strcpy(s_fit[s_fit_next].s, in);
        s_fit_next = (s_fit_next + 1) % FIT_MEMO;
    }
}

static void fit_uncached(char *s, size_t n, const lv_font_t *f, int space, int max_w)
{
    if (text_w_raw(s, f, space) <= max_w) return;
    size_t len = strlen(s);
    while (len > 0) {
        do { len--; } while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80);
        while (len > 0 && s[len - 1] == ' ') len--;
        if (len + 4 > n) continue;
        memcpy(s + len, "...", 4);
        if (text_w_raw(s, f, space) <= max_w) return;
    }
}

/* Squeeze s into max_w px: tighten the tracking from `space` down to 1 first, then cut with "...". Returns the tracking. */
static int fit_track(char *s, size_t n, const lv_font_t *f, int space, int max_w)
{
    while (space > 1 && text_w(s, f, space) > max_w) space--;
    fit(s, n, f, space, max_w);
    return space;
}

/* ---- time / solar helpers ---- */

void dial_net_up(void)
{
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

/* NOAA sunrise/sunset as local fractional hours; false if polar day/night. */
static bool sun_times(const struct tm *lt, float lat, float lon, float *rise, float *set)
{
    float g = 2 * PI_F / 365.0f * (lt->tm_yday + (lt->tm_hour - 12) / 24.0f);
    float eqt = 229.18f * (0.000075f + 0.001868f * cosf(g) - 0.032077f * sinf(g)
                           - 0.014615f * cosf(2 * g) - 0.040849f * sinf(2 * g));
    float decl = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g)
                 - 0.006758f * cosf(2 * g) + 0.000907f * sinf(2 * g)
                 - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
    float la = lat * PI_F / 180;
    float ha = (cosf(90.833f * PI_F / 180) / (cosf(la) * cosf(decl))) - tanf(la) * tanf(decl);
    if (ha > 1 || ha < -1) return false;
    ha = acosf(ha) * 180 / PI_F;
    float tz = lt->tm_isdst > 0 ? -4.0f : -5.0f;
    float noon = 720 - 4 * lon - eqt + tz * 60;
    *rise = (noon - 4 * ha) / 60.0f;
    *set = (noon + 4 * ha) / 60.0f;
    return true;
}

/* "2026-11-08T09:00:00-05:00", "2026-11-08T21:25:00.000Z" or "...+00:00" -> epoch seconds */
static time_t parse_iso(const char *s)
{
    int Y, M, D, h, m, sec = 0, n = 0;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d%n", &Y, &M, &D, &h, &m, &sec, &n) < 6) return 0;
    const char *p = s + n;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
    int off = 0;
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        sscanf(p + 1, "%d:%d", &oh, &om);
        off = (oh * 3600 + om * 60) * (*p == '-' ? -1 : 1);
    }
    int days = 0;
    for (int y = 1970; y < Y; y++) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    static const int md[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int i = 0; i < M - 1; i++) days += md[i] + (i == 1 && (Y % 4 == 0 && (Y % 100 != 0 || Y % 400 == 0)));
    days += D - 1;
    return (time_t)days * 86400 + h * 3600 + m * 60 + sec - off;
}

static void fmt_clock(char *b, size_t n, time_t t)
{
    struct tm x; localtime_r(&t, &x);
    snprintf(b, n, "%d:%02d", x.tm_hour % 12 ? x.tm_hour % 12 : 12, x.tm_min);
}

/* ---- drawing primitives ---- */

static void pt(float theta_deg, float r, float *x, float *y)
{
    float t = theta_deg * PI_F / 180;
    *x = C + r * sinf(t);
    *y = C - r * cosf(t);
}

/*
 * LVGL renders the screen in 20-row strips and runs this draw code once per strip. It does not
 * skip shapes outside the strip: each one is still allocated and scheduled (the scheduler checks
 * every pending task against all earlier ones), which made a full frame cost ~1.3 s. So every
 * helper below drops shapes that miss the strip being drawn.
 */
static inline bool on_strip(lv_layer_t *l, const lv_area_t *a) { return lv_area_is_on(a, &l->_clip_area); }

static void disc(lv_layer_t *l, float x, float y, float r, uint32_t col, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col); d.bg_opa = opa; d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { (int)lroundf(x - r), (int)lroundf(y - r), (int)lroundf(x + r), (int)lroundf(y + r) };
    if (on_strip(l, &a)) lv_draw_rect(l, &d, &a);
}

static void ring(lv_layer_t *l, float cx, float cy, float r, float w, uint32_t col, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.center.x = (int)lroundf(cx); d.center.y = (int)lroundf(cy); d.radius = (int)r; d.width = (int)(w + 0.5f);
    d.start_angle = 0; d.end_angle = 360; d.color = lv_color_hex(col); d.opa = opa;
    lv_area_t a = { d.center.x - d.radius, d.center.y - d.radius, d.center.x + d.radius, d.center.y + d.radius };
    if (on_strip(l, &a)) lv_draw_arc(l, &d);
}

/* Arc in dial degrees (clockwise from 12 o'clock). LVGL's radius is the outer edge, so centre the stroke on r. */
static void arc_dial(lv_layer_t *l, float r, float w, float from, float to, uint32_t col, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.center.x = C; d.center.y = C; d.radius = (int)lroundf(r + w / 2); d.width = (int)lroundf(w); d.rounded = 1;
    d.start_angle = fmodf(from - 90 + 720, 360); d.end_angle = fmodf(to - 90 + 720, 360);
    d.color = lv_color_hex(col); d.opa = opa;
    lv_area_t a;
    lv_draw_arc_get_area(d.center.x, d.center.y, d.radius, d.start_angle, d.end_angle, d.width, d.rounded, &a);
    if (on_strip(l, &a)) lv_draw_arc(l, &d);
}

static void text_at(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, lv_opa_t opa,
                    int x, int y_top)
{
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = s; d.font = f; d.color = lv_color_hex(col); d.opa = opa; d.letter_space = space; d.text_local = 1;
    lv_area_t a = { x, y_top, x + text_w(s, f, space) + 2, y_top + lv_font_get_line_height(f) };
    if (on_strip(l, &a)) lv_draw_label(l, &d, &a);
}

static void text_c(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, int y_top)
{
    text_at(l, s, f, space, col, LV_OPA_COVER, C - text_w(s, f, space) / 2, y_top);
}

/*
 * Curved caption (spec 4.3/5.5): baseline on radius r, centred on 12 o'clock (top, reading
 * left to right over the top) or 6 o'clock (bottom, upright, left to right along the bottom).
 */
#define CAP_MAX 64
typedef struct { const lv_font_t *f; float r; bool top; int track; int n; char s[112];
                 uint32_t uni[CAP_MAX]; lv_point_t p[CAP_MAX]; int16_t rot[CAP_MAX]; } cap_layout_t;
/* Arc length a caption may span: the gauge arcs end 60 deg either side of 12 and 6 o'clock, so keep about 45 deg
   (top r 173.4, bottom r 182) clear of their end dots. */
#define CAP_TOP_W 280
#define CAP_BOT_W 285
static cap_layout_t s_cap[2];      /* top and bottom captions; laid out once per text */

static const cap_layout_t *cap_layout(const char *s, const lv_font_t *f, float r, bool top, int track)
{
    cap_layout_t *c = &s_cap[top ? 0 : 1];
    if (c->f == f && c->r == r && c->track == track && !strcmp(c->s, s)) return c;
    c->f = f; c->r = r; c->top = top; c->track = track; c->n = 0;
    snprintf(c->s, sizeof c->s, "%s", s);
    int total = text_w(s, f, track);
    float pos = -total / 2.0f;                      /* arc-length position, + = rightwards */
    uint32_t letter, next, i = 0;
    while (s[i] && c->n < CAP_MAX) {
        lv_text_encoded_letter_next_2(s, &letter, &next, &i);
        if (!letter) break;
        int w = lv_font_get_glyph_width(f, letter, next);
        float a = (pos + w / 2.0f) / r;             /* radians from 12 or 6 o'clock */
        float bx = C + r * sinf(a), by = top ? C - r * cosf(a) : C + r * cosf(a);
        c->uni[c->n] = letter;
        c->rot[c->n] = (int16_t)lroundf((top ? a : -a) * 1800 / PI_F);
        /* lv_draw_letter takes the glyph's pivot point itself: (advance centre, baseline) */
        c->p[c->n] = (lv_point_t){ (int)lroundf(bx), (int)lroundf(by) };
        c->n++;
        pos += w + track;
    }
    return c;
}

static void caption(lv_layer_t *l, const char *s, const lv_font_t *f, uint32_t col, lv_opa_t opa,
                    float r, bool top, int track)
{
    const cap_layout_t *c = cap_layout(s, f, r, top, track);
    int h = lv_font_get_line_height(f);             /* a rotated glyph stays within this of its pivot */
    for (int k = 0; k < c->n; k++) {
        lv_area_t box = { c->p[k].x - h, c->p[k].y - h, c->p[k].x + h, c->p[k].y + h };
        if (!on_strip(l, &box)) continue;
        lv_draw_letter_dsc_t d; lv_draw_letter_dsc_init(&d);
        d.font = f; d.color = lv_color_hex(col); d.opa = opa; d.unicode = c->uni[k]; d.rotation = c->rot[k];
        lv_point_t p = c->p[k];
        lv_draw_letter(l, &d, &p);
    }
}

static int cap_height(const lv_font_t *f)
{
    static const lv_font_t *fs[8]; static int hs[8], n;
    for (int i = 0; i < n; i++) if (fs[i] == f) return hs[i];
    lv_font_glyph_dsc_t g;
    int h = lv_font_get_glyph_dsc(f, &g, 'H', 0) ? g.box_h + g.ofs_y : 11;
    if (n < 8) { fs[n] = f; hs[n] = h; n++; }
    return h;
}

/* 5.3 daylight crescent: one shade circle r 260 sliding with the sun. */
static void draw_crescent(lv_layer_t *l, const struct tm *lt, uint32_t col, lv_opa_t opa)
{
    float hrs = lt->tm_hour + lt->tm_min / 60.0f + lt->tm_sec / 3600.0f;
    float rise = 6.5f, set = 17.5f;
    sun_times(lt, SITE_LAT, SITE_LON, &rise, &set);
    float dd = (hrs - rise) / (set - rise), cx = C;
    if (dd > 0 && dd < 1) cx = dd <= 0.5f ? C + 2 * dd * 493 : C + (2 * dd - 2) * 493;
    disc(l, cx, C, 260, col, opa);
}

/* 5.1 rim on the 12-hour dial: 60 dots on r 214, one per 12 minutes. Hour dots (every fifth) r 2.0, the quarter
   dots (12, 3, 6, 9) r 2.8, the rest r 1.5. Dots inside the gap zone (behind the planet, back to the leading edge)
   at 35%, so the zone reads as an empty stretch. */
static void draw_rim_dots(lv_layer_t *l, time_t now)
{
    float planet = rim_deg(now), gap = dial_gap_min() / 2.0f;
    float edge = planet + rim_window_s(dial_gap_min()) / 120.0f;
    for (int i = 0; i < 60; i++) {
        float d = i * 6.0f, off = fmodf(d - edge + 720.0f, 360.0f), x, y;
        bool in_gap = off > 0.25f && off < gap - 0.25f;
        pt(d, 214, &x, &y);
        disc(l, x, y, i % 15 == 0 ? 2.8f : i % 5 == 0 ? 2.0f : 1.5f, COL_RIM, in_gap ? 89 /* 35% */ : LV_OPA_80);
    }
}

void dial_draw_rim_dots(lv_layer_t *l)
{
    time_t now = dial_now();
    if (now >= 1700000000) draw_rim_dots(l, now);
}

/* 5.3 crescent + 5.1 rim. Shared by every plum screen. */
static void draw_crescent_rim(lv_layer_t *l, const struct tm *lt, time_t now)
{
    if (!s_night) draw_crescent(l, lt, COL_SHADE, LV_OPA_50);   /* no shade on the black night ground */
    draw_rim_dots(l, now);
}

/* 5.2 now dot + second-hand moon at theta(now) on the 12-hour rim; records the box the moon ticks in. */
static void draw_now_dot_c(lv_layer_t *l, const struct tm *lt, uint32_t moon_col, uint32_t dot_col,
                           uint32_t ring_col, lv_opa_t ring_opa)
{
    float hrs = lt->tm_hour % 12 + lt->tm_min / 60.0f + lt->tm_sec / 3600.0f;
    float nx, ny; pt(hrs / 12 * 360, 214, &nx, &ny);
    ring(l, nx, ny, 13, 1, ring_col, ring_opa);
    float ma = lt->tm_sec * 6 * PI_F / 180;
    disc(l, nx + 13 * sinf(ma), ny - 13 * cosf(ma), 3, moon_col, LV_OPA_COVER);
    disc(l, nx, ny, 5, dot_col, LV_OPA_COVER);
    s_moon_box = (lv_area_t){ (int)nx - 20, (int)ny - 20, (int)nx + 20, (int)ny + 20 };
}

static void draw_now_dot(lv_layer_t *l, const struct tm *lt, uint32_t moon_col)
{
    draw_now_dot_c(l, lt, moon_col, COL_INK, COL_HAIR, LV_OPA_60);
}

/* ---- exported drawing for other plum screens (dial.h) ---- */

time_t dial_parse_iso(const char *s) { return parse_iso(s); }
void dial_caps(char *dst, size_t n, const char *src) { clean_caps(dst, n, src); }
bool dial_moment_is_free(const cJSON *e) { return cJSON_IsTrue(cJSON_GetObjectItem(e, "free")); }

/* Centre x of the daylight shade circle (r 260) right now, as draw_crescent places it; screens that paint
   the ground themselves (fades over scrolling text) use it to match the shaded and lit parts. */
float dial_crescent_cx(void)
{
    time_t now = dial_now();
    struct tm lt; localtime_r(&now, &lt);
    float hrs = lt.tm_hour + lt.tm_min / 60.0f + lt.tm_sec / 3600.0f;
    float rise = 6.5f, set = 17.5f;
    sun_times(&lt, SITE_LAT, SITE_LON, &rise, &set);
    float dd = (hrs - rise) / (set - rise), cx = C;
    if (dd > 0 && dd < 1) cx = dd <= 0.5f ? C + 2 * dd * 493 : C + (2 * dd - 2) * 493;
    return cx;
}

void dial_draw_crescent(lv_layer_t *l)
{
    time_t now = dial_now();
    struct tm lt; localtime_r(&now, &lt);
    if (!s_night) draw_crescent(l, &lt, COL_SHADE, LV_OPA_50);
}

void dial_draw_base(lv_layer_t *l, uint32_t moon_col)
{
    time_t now = dial_now();
    if (now < 1700000000) return;
    struct tm lt; localtime_r(&now, &lt);
    draw_crescent_rim(l, &lt, now);
    draw_now_dot(l, &lt, moon_col);
}

int dial_alert_dots_gone(time_t arrival, time_t expires);

/* 7.1 the alert face: crescent in `alert-shade`, every mark in `rose-ink`, and the rim drain. All 60 dots
   at 22%; the part of the window still left at 80%, from the drain point clockwise round to 12 o'clock.
   One dot goes out per 1/60 of the window from arrival to expiry. No expiry: the whole rim stays lit. */
void dial_draw_alert_base(lv_layer_t *l, time_t arrival, time_t expires)
{
    time_t now = dial_now();
    if (now < 1700000000) return;
    struct tm lt; localtime_r(&now, &lt);
    const uint32_t ink = 0x2A1A2C;
    draw_crescent(l, &lt, 0x501428, 36 /* 14% */);
    int gone = dial_alert_dots_gone(arrival, expires);
    for (int i = 0; i < 60; i++) {
        float x, y; pt(i * 6.0f, 214, &x, &y);
        disc(l, x, y, 1.75f, ink, i < gone ? 56 /* 22% */ : 204 /* 80% */);
    }
    draw_now_dot_c(l, &lt, ink, ink, ink, 89 /* 35% */);
}

int dial_alert_dots_gone(time_t arrival, time_t expires)
{
    time_t now = time(NULL);
    if (!arrival || expires <= arrival) return 0;
    float f = (float)(now - arrival) / (float)(expires - arrival);
    return f <= 0 ? 0 : f >= 1 ? 60 : (int)(f * 60);
}

void dial_draw_arc_opa(lv_layer_t *l, float r, float w, float from_deg, float to_deg, uint32_t col, lv_opa_t opa)
{
    arc_dial(l, r, w, from_deg, to_deg, col, opa);
}

void dial_draw_arc(lv_layer_t *l, float r, float w, float from_deg, float to_deg, uint32_t col)
{
    arc_dial(l, r, w, from_deg, to_deg, col, LV_OPA_COVER);
}

void dial_draw_disc(lv_layer_t *l, float x, float y, float r, uint32_t col) { disc(l, x, y, r, col, LV_OPA_COVER); }

void dial_draw_disc_opa(lv_layer_t *l, float x, float y, float r, uint32_t col, lv_opa_t opa) { disc(l, x, y, r, col, opa); }

void dial_draw_ring(lv_layer_t *l, float x, float y, float r, float w, uint32_t col) { ring(l, x, y, r, w, col, LV_OPA_COVER); }
void dial_draw_ring_opa(lv_layer_t *l, float x, float y, float r, float w, uint32_t col, lv_opa_t opa) { ring(l, x, y, r, w, col, opa); }

void dial_draw_top_caption(lv_layer_t *l, const char *text, uint32_t col)
{
    char buf[112];
    clean_caps(buf, sizeof buf, text);
    int tr = fit_track(buf, sizeof buf, f_cap_top, TRACK, CAP_TOP_W);
    caption(l, buf, f_cap_top, col, LV_OPA_COVER, (151.25f + 195.5f) / 2 - cap_height(f_cap_top) / 2.0f, true, tr);
}

/* The lowest y (screen px, rounded up) the top caption `text` reaches, counting only the letters whose box lies within
   `half_w` px of the vertical centre line: the Event detail title is centred under this (spec 7.2). A letter's lowest
   point is its baseline end: p.y + (w / 2) |sin a|. Same layout as dial_draw_top_caption (fit to 300, cached). */
int dial_top_caption_bottom(const char *text, int half_w)
{
    char buf[112];
    clean_caps(buf, sizeof buf, text);
    int tr = fit_track(buf, sizeof buf, f_cap_top, TRACK, CAP_TOP_W);
    const cap_layout_t *c = cap_layout(buf, f_cap_top, (151.25f + 195.5f) / 2 - cap_height(f_cap_top) / 2.0f, true, tr);
    float low = 0;
    for (int k = 0; k < c->n; k++) {
        float hw = lv_font_get_glyph_width(f_cap_top, c->uni[k], 0) / 2.0f, a = c->rot[k] * PI_F / 1800.0f;
        if (fabsf((float)(c->p[k].x - C)) - hw * cosf(a) > half_w) continue;
        float y = c->p[k].y + hw * fabsf(sinf(a));
        if (y > low) low = y;
    }
    return (int)ceilf(low);
}

void dial_draw_bottom_caption(lv_layer_t *l, const char *text, uint32_t col)
{
    char buf[112];
    clean_caps(buf, sizeof buf, text);
    int tr = fit_track(buf, sizeof buf, f_cap_bot, TRACK, CAP_BOT_W);
    caption(l, buf, f_cap_bot, col, LV_OPA_COVER, 182, false, tr);
}

/* Hold to confirm: the screen, not the button, fills from the bottom while it is held, so
   the progress shows around the finger. Drawn by the screen's own draw callback under its children. */
void dial_draw_hold_fill(lv_layer_t *l, int h, uint32_t col, lv_opa_t opa)
{
    if (h <= 0) return;
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(col); d.bg_opa = opa;
    lv_area_t a = { 0, 466 - h, 465, 465 };
    lv_draw_rect(l, &d, &a);
}

void dial_hold_fill_step(lv_obj_t *o, int *cur, int h)
{
    if (h < 0) h = 0;
    if (h > 466) h = 466;
    if (h == *cur) return;
    int lo = h > *cur ? h : *cur, hi = h > *cur ? *cur : h;     /* only the strip that changed */
    lv_area_t a = { 0, 466 - lo, 465, 465 - hi };
    lv_obj_invalidate_area(o, &a);
    *cur = h;
}

bool dial_sun_hours(time_t day, float *rise, float *set)
{
    struct tm lt; localtime_r(&day, &lt);
    *rise = 6.5f; *set = 17.5f;                              /* spec 5.3 fallback */
    return sun_times(&lt, SITE_LAT, SITE_LON, rise, set);
}

float dial_clock_deg(time_t t) { return rim_deg(t); }
int dial_gap_min(void) { return rim_gap_valid(prefs_gap_min()); }
float dial_window_deg(time_t t) { return rim_window_deg(dial_now(), dial_gap_min(), t); }
bool dial_in_window(time_t t) { return rim_in_window(dial_now(), dial_gap_min(), t); }
time_t dial_now(void) { return time(NULL) + s_clock_off; }
time_t dial_clock_offset(void) { return s_clock_off; }

void dial_show_face(bool show)
{
    bsp_display_lock(-1);
    if (show) lv_obj_remove_flag(s_face, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_face, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

/* ---- the face ---- */

/* ---- status icons at 3 o'clock (Nov 11), mirroring the agent ring at 9; ~22 px, 2 px strokes ---- */
#define ICON_X 420
#define ICON_LIM 396          /* the time group's right edge stays left of the icons */

static void seg(lv_layer_t *l, float x1, float y1, float x2, float y2, uint32_t col)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col); d.width = 2; d.round_start = 1; d.round_end = 1;
    d.p1 = (lv_point_precise_t){ x1, y1 }; d.p2 = (lv_point_precise_t){ x2, y2 };
    lv_draw_line(l, &d);
}

void dial_draw_bolt(lv_layer_t *l, float cx, float cy, float h, uint32_t col)
{
    static const float b[] = { 2, -10, -6, 2, -1, 2, -2, 10, 6, -2, 1, -2 };   /* 20 tall, 12 wide, centred */
    float s = h / 20;
    lv_area_t a = { (int)(cx - 7 * s - 2), (int)(cy - 10 * s - 2), (int)(cx + 7 * s + 2), (int)(cy + 10 * s + 2) };
    if (!on_strip(l, &a)) return;
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        seg(l, cx + b[2 * i] * s, cy + b[2 * i + 1] * s, cx + b[2 * j] * s, cy + b[2 * j + 1] * s, col);
    }
}

static void draw_battery(lv_layer_t *l, float cx, float cy, int pct)
{
    lv_area_t a = { (int)cx - 13, (int)cy - 8, (int)cx + 13, (int)cy + 8 };
    if (!on_strip(l, &a)) return;
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP; d.border_width = 2; d.border_color = lv_color_hex(COL_AMBER); d.radius = 3;
    lv_area_t body = { (int)cx - 11, (int)cy - 6, (int)cx + 8, (int)cy + 6 };      /* 20 x 13 */
    lv_draw_rect(l, &d, &body);
    lv_draw_rect_dsc_t n; lv_draw_rect_dsc_init(&n);
    n.bg_color = lv_color_hex(COL_AMBER); n.radius = 1;
    lv_area_t nub = { (int)cx + 10, (int)cy - 2, (int)cx + 11, (int)cy + 2 };
    lv_draw_rect(l, &n, &nub);
    int w = pct * 14 / 100; if (w < 1 && pct > 0) w = 1;                            /* inner 14 px */
    if (w > 0) { lv_area_t f = { (int)cx - 8, (int)cy - 3, (int)cx - 8 + w - 1, (int)cy + 3 }; lv_draw_rect(l, &n, &f); }
}

static void draw_moon(lv_layer_t *l, float cx, float cy)
{
    lv_area_t a = { (int)cx - 13, (int)cy - 13, (int)cx + 13, (int)cy + 13 };
    if (!on_strip(l, &a)) return;
    /* crescent: the big circle (r 10) outside a cut circle (r 8) at (+6, -5), plus the cut circle inside the big one */
    const float R = 10, ox = 6, oy = -5, r = 8;
    for (int k = 0; k < 2; k++) {
        float rr = k ? r : R, px = 0, py = 0; bool pk = false;
        for (int deg = 0; deg <= 360; deg += 10) {
            float t = deg * PI_F / 180, x = (k ? ox : 0) + rr * cosf(t), y = (k ? oy : 0) + rr * sinf(t);
            float cd = k ? hypotf(x, y) : hypotf(x - ox, y - oy);
            bool keep = k ? cd <= R + 0.5f : cd >= r - 0.5f;
            if (keep && pk) seg(l, cx + px, cy + py, cx + x, cy + y, COL_INK2);
            px = x; py = y; pk = keep;
        }
    }
}

static bool icon_low(void) { int p = power_battery_pct(); return !power_on_usb() && p >= 0 && p < 20; }
static int icon_count(void) { return (power_on_usb() || icon_low() ? 1 : 0) + (power_quiet() ? 1 : 0); }
static unsigned icon_sig(void) { return (power_on_usb() ? 1u : 0) | (icon_low() ? 2u : 0) | (power_quiet() ? 4u : 0); }
/* y of the bolt/battery icon: 233 alone, 251 under the quiet icon */
static int power_icon_y(void) { return power_quiet() ? 251 : C; }

static void draw_icons(lv_layer_t *l)
{
    bool q = power_quiet(), usb = power_on_usb(), low = icon_low();
    if (q) draw_moon(l, ICON_X, (usb || low) ? 215 : C);
    if (usb) dial_draw_bolt(l, ICON_X, power_icon_y(), 22, COL_ROSE);
    else if (low) draw_battery(l, ICON_X, power_icon_y(), power_battery_pct());
}

bool dial_bolt_hit(int x, int y)
{
    if (!power_on_usb()) return false;
    int dx = x - ICON_X, dy = y - power_icon_y();
    return dx * dx + dy * dy <= 36 * 36;
}

/* The time as Now draws it with no next Moment, for the Charging screen: numeral + AM/PM, centred on the dial. */
void dial_draw_time(lv_layer_t *l, int dx, int dy, uint32_t num_col, uint32_t unit_col)
{
    time_t now = dial_now();
    if (now < 1700000000) return;
    struct tm lt; localtime_r(&now, &lt);
    char clk[16]; fmt_clock(clk, sizeof clk, now);
    int nw = text_w(clk, f_num_l, -2), cx0 = C - nw / 2 + dx, base = C + cap_height(f_num_l) / 2 + dy;
    text_at(l, clk, f_num_l, -2, num_col, LV_OPA_COVER, cx0, base - (lv_font_get_line_height(f_num_l) - f_num_l->base_line));
    text_at(l, lt.tm_hour < 12 ? "AM" : "PM", f_label, TRACK, unit_col, LV_OPA_COVER, cx0 + nw + 10,
            base - (lv_font_get_line_height(f_label) - f_label->base_line));
}

/* ---- shared with now_layout.c (dial_priv.h) ---- */
void dv_pt(float deg, float r, float *x, float *y) { pt(deg, r, x, y); }
void dv_disc(lv_layer_t *l, float x, float y, float r, uint32_t col, lv_opa_t opa) { disc(l, x, y, r, col, opa); }
void dv_arc(lv_layer_t *l, float r, float w, float from, float to, uint32_t col, lv_opa_t opa) { arc_dial(l, r, w, from, to, col, opa); }
void dv_text_at(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, lv_opa_t opa, int x, int y_top)
{ text_at(l, s, f, space, col, opa, x, y_top); }
void dv_text_c(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, int y_top) { text_c(l, s, f, space, col, y_top); }
int dv_text_w(const char *s, const lv_font_t *f, int space) { return text_w(s, f, space); }
void dv_fit(char *s, size_t n, const lv_font_t *f, int space, int max_w) { fit(s, n, f, space, max_w); }
int dv_fit_track(char *s, size_t n, const lv_font_t *f, int space, int max_w) { return fit_track(s, n, f, space, max_w); }
void dv_caption(lv_layer_t *l, const char *s, const lv_font_t *f, uint32_t col, lv_opa_t opa, float r, bool top, int track)
{ caption(l, s, f, col, opa, r, top, track); }
int dv_cap_height(const lv_font_t *f) { return cap_height(f); }
void dv_fmt_clock(char *b, size_t n, time_t t) { fmt_clock(b, n, t); }
void dv_seg(lv_layer_t *l, float x1, float y1, float x2, float y2, uint32_t col)
{
    lv_area_t a = { (int)fminf(x1, x2) - 2, (int)fminf(y1, y2) - 2, (int)fmaxf(x1, x2) + 2, (int)fmaxf(y1, y2) + 2 };
    if (on_strip(l, &a)) seg(l, x1, y1, x2, y2, col);
}
void dv_draw_icons(lv_layer_t *l) { draw_icons(l); }
int dv_icon_count(void) { return icon_count(); }

/* Local midnight at the start of now's day (a DST day is 23 or 25 h). */
static time_t day_start(time_t now)
{
    struct tm lt; localtime_r(&now, &lt);
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0; lt.tm_isdst = -1;
    return mktime(&lt);
}

/* Rebuild the rim cache when the minute, the Moments or the gap changed. LVGL context (display lock held). */
static void rim_refresh(time_t now)
{
    long key = (long)(now / 60);
    int gap = dial_gap_min();
    if (s_rc.valid && s_rc.key == key && s_rc.gen == s_ev_gen && s_rc.gap == gap) return;
    for (int i = 0; i < s_nev; i++) s_rim_in[i] = (rim_in_t){ s_ev[i].start, s_ev[i].end, s_ev[i].has_end, s_ev[i].free };
    rim_build(&s_rc.rim, s_rim_in, s_nev, now, gap);
    time_t d0 = day_start(now);
    mom_scan_t sc;
    mom_scan(s_rim_in, s_nev, now, d0, d0 + 86400, gap, &sc);
    s_rc.next = sc.next; s_rc.busy_next = sc.busy_next; s_rc.running = sc.running; s_rc.free_running = sc.free_running;
    s_rc.gap_from = sc.gap_from; s_rc.upcoming = sc.upcoming;
    s_rc.key = key; s_rc.gen = s_ev_gen; s_rc.gap = gap; s_rc.valid = true;
}

/* The windowed marks (spec 4.2, 5.1): past Moments are gone, Moments beyond the edge not yet drawn, a new one fades in
   at the edge over 15 min, a span still cut by the edge ends in a 50% tip; and the midnight tick when local midnight
   is inside the window. */
#define FREE_OPA 165      /* a free mark at 65% */
#define FREE_W 2          /* hairline outline width */
static void draw_marks(lv_layer_t *l, time_t now)
{
    rim_refresh(now);
    const rim_t *r = &s_rc.rim;
    if (r->midnight) {                                   /* 2 px radial tick, r 205 to 221, ink-2 at 70% */
        float x0, y0, x1, y1; pt(r->midnight_deg, 205, &x0, &y0); pt(r->midnight_deg, 221, &x1, &y1);
        lv_area_t a = { (int)fminf(x0, x1) - 2, (int)fminf(y0, y1) - 2, (int)fmaxf(x0, x1) + 2, (int)fmaxf(y0, y1) + 2 };
        if (on_strip(l, &a)) {
            lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
            d.color = lv_color_hex(COL_INK2); d.opa = 179; d.width = 2;
            d.p1 = (lv_point_precise_t){ x0, y0 }; d.p2 = (lv_point_precise_t){ x1, y1 };
            lv_draw_line(l, &d);
        }
    }
    for (int i = 0; i < r->n; i++) {
        const rim_mark_t *m = &r->m[i];
        if (m->free) {                                   /* spec 5.1 "Free Moment": an outline, off the lanes, quieter */
            lv_opa_t fo = (lv_opa_t)(m->opa * FREE_OPA / 255), ft = (lv_opa_t)(m->tip_opa * FREE_OPA / 255);
            if (m->kind == RIM_SPAN) {
                if (fo && m->a1 - m->a0 >= 0.5f) arc_dial(l, DIAL_FREE_R, FREE_W, m->a0, m->a1, COL_ROSE, fo);
                if (ft && m->t1 - m->t0 >= 0.5f) arc_dial(l, DIAL_FREE_R, FREE_W, m->t0, m->t1, COL_ROSE, ft);
            } else {
                float x, y; pt(m->a0, 214, &x, &y); ring(l, x, y, m->rad, FREE_W, COL_ROSE, fo);
            }
        } else if (m->kind == RIM_SPAN) {
            float rr = m->lane == 2 ? DIAL_LANE2_R : DIAL_LANE1_R; uint32_t col = m->lane == 2 ? COL_AMBER : COL_ROSE;
            if (m->opa && m->a1 - m->a0 >= 0.5f) arc_dial(l, rr, 3, m->a0, m->a1, col, m->opa);
            if (m->tip_opa && m->t1 - m->t0 >= 0.5f) arc_dial(l, rr, 3, m->t0, m->t1, col, m->tip_opa);
        } else {
            float x, y; pt(m->a0, 214, &x, &y); disc(l, x, y, m->rad, COL_ROSE, m->opa);
        }
    }
}

void dial_draw_rim_marks(lv_layer_t *l)
{
    time_t now = dial_now();
    if (now >= 1700000000) draw_marks(l, now);
}

static void day_draw(lv_layer_t *l)
{
    time_t now = dial_now();
    if (now < 1700000000) {            /* clock not synced yet */
        text_c(l, s_link, f_label, TRACK, COL_INK2, 222);
        return;
    }
    struct tm lt; localtime_r(&now, &lt);
    float hrs = lt.tm_hour + lt.tm_min / 60.0f + lt.tm_sec / 3600.0f;

    draw_crescent_rim(l, &lt, now);
    draw_marks(l, now);                /* also refreshes s_rc: next, running, gap start */
    draw_now_dot(l, &lt, COL_ROSE);

    /* bands 1-7 (captions, heading, numerals, both arcs, footer): the slot table in now_layout.c. The next and running
       Moments come from the rim window, so what the numeral says matches what the rim shows (Nov 22). */
    const ev_t *next = s_rc.next >= 0 ? &s_ev[s_rc.next] : NULL, *running = s_rc.running >= 0 ? &s_ev[s_rc.running] : NULL;
    const ev_t *bnext = s_rc.busy_next >= 0 ? &s_ev[s_rc.busy_next] : NULL, *frun = s_rc.free_running >= 0 ? &s_ev[s_rc.free_running] : NULL;
    now_ctx_t c = {0};
    c.now = now; c.lt = lt; c.hrs = hrs; c.d0 = day_start(now); c.gap_from = s_rc.gap_from; c.upcoming = s_rc.upcoming;
    if (next) { c.has_next = true; c.next_start = next->start; c.next_title = next->title; c.next_free = next->free; }
    if (bnext) { c.has_busy_next = true; c.busy_next_start = bnext->start; }
    if (frun) { c.has_free_running = true; c.free_running_end = frun->end; c.free_running_title = frun->title; }
    if (running) {
        c.has_running = true; c.running_has_end = running->has_end; c.running_start = running->start;
        c.running_end = running->end; c.running_title = running->title;
    }
    c.link = s_link; c.phone = !strcmp(s_link, "PHONE"); c.link_up = !strcmp(s_link, "PAIRED");
    c.agent_busy = strcmp(s_state, "idle") != 0; c.status = s_status;
    c.swap = s_swap;
    c.needs_you = decision_needs_count();      /* replies still awaiting an answer (0 with the flag off) */
    now_draw(l, &c);

    /* band 9: agent count ring, centre (46, 233), r 16, 1.5 px hair, numeral 15 px 600 ink; working jobs only */
    if (s_agents > 0) {
        char buf[16];
        ring(l, 46, C, 16, 1.5f, COL_HAIR, LV_OPA_COVER);
        snprintf(buf, sizeof buf, "%d", s_agents > 99 ? 99 : s_agents);
        int w = text_w(buf, f_label15, 0);
        text_at(l, buf, f_label15, 0, COL_INK, LV_OPA_COVER, 46 - w / 2,
                C + cap_height(f_label15) / 2 - (lv_font_get_line_height(f_label15) - f_label15->base_line));
    }
}

/* The window's next upcoming Moment (the one the Now numeral names), or NULL. */
static const ev_t *next_moment(time_t now)
{
    for (int i = 0; i < s_nev; i++)
        if (s_ev[i].start > now) return dial_in_window(s_ev[i].start) ? &s_ev[i] : NULL;   /* start order */
    return NULL;
}

bool dial_lower_arc_tap(int x, int y)
{
    if (!now_lower_arc_hit(x, y)) return false;
    if (now_lower_step() && s_face) lv_obj_invalidate(s_face);
    return true;
}

void dial_refresh_face(void)
{
    if (s_face) lv_obj_invalidate(s_face);
}

bool dial_toggle_numeral(void)
{
    const ev_t *n = next_moment(dial_now());
    if (!n || n->free) return false;                            /* no countdown to swap with (none for a free Moment) */
    s_swap = !s_swap;
    if (s_face) lv_obj_invalidate(s_face);
    return true;
}

/* 7.7 Resting: true black, clock in `night`, one line under it; the block drifts each minute (burn-in). */
static void rest_draw(lv_layer_t *l)
{
    time_t now = dial_now();
    if (now < 1700000000) return;
    struct tm lt; localtime_r(&now, &lt);
    int m = lt.tm_hour * 60 + lt.tm_min;
    int dx = (m * 7) % 33 - 16, dy = (m * 13) % 33 - 16;
    char clock[16], line[128];
    fmt_clock(clock, sizeof clock, now);
    int cw = text_w(clock, f_num_l, -2);
    text_at(l, clock, f_num_l, -2, COL_NIGHT, LV_OPA_COVER, C - cw / 2 + dx, 150 + dy);
    const ev_t *n = next_moment(now);
    if (power_quiet()) snprintf(line, sizeof line, "QUIET UNTIL 6:30 AM");
    else if (n) { char t[16]; fmt_clock(t, sizeof t, n->start); snprintf(line, sizeof line, "NEXT %s \xC2\xB7 %s", t, n->title); }
    else line[0] = 0;
    fit(line, sizeof line, f_label15, TRACK, 300);
    if (line[0]) text_at(l, line, f_label15, TRACK, COL_NIGHT, LV_OPA_COVER, C - text_w(line, f_label15, TRACK) / 2 + dx, 276 + dy);
}

static void charge_draw(lv_layer_t *l)
{
    text_c(l, "BATTERY LOW", f_label, TRACK, COL_NIGHT, 196);
    text_c(l, "CHARGE ME", f_cap_bot, TRACK, COL_ROSE, 236);
}

static void face_draw(lv_event_t *e)
{
    int64_t t0 = esp_timer_get_time();
    lv_layer_t *l = lv_event_get_layer(e);
    if (s_mode == DIAL_FACE_REST) rest_draw(l);
    else if (s_mode == DIAL_FACE_CHARGE) charge_draw(l);
    else day_draw(l);
    compact_perf_draw_us(PERF_DRAW_FACE, (uint32_t)(esp_timer_get_time() - t0));
}

/* ---- night ground: true black from sunset to the next sunrise ---- */

static void ground_init(void)
{
    if (s_ground_ready) return;
    lv_style_init(&s_ground_style);
    lv_style_set_bg_color(&s_ground_style, lv_color_hex(COL_GLASS));
    s_ground_ready = true;
}

/* Sunset to sunrise by the NOAA times for the configured location (17:30-06:30 if they fail); day until the clock is set. */
static bool night_now(void)
{
    if (s_night_override >= 0) return s_night_override;
    time_t now = dial_now();
    if (now < 1700000000) return false;
    struct tm lt; localtime_r(&now, &lt);
    float hrs = lt.tm_hour + lt.tm_min / 60.0f + lt.tm_sec / 3600.0f;
    float rise = 6.5f, set = 17.5f;
    sun_times(&lt, SITE_LAT, SITE_LON, &rise, &set);
    return hrs < rise || hrs >= set;
}

/* LVGL context: switch the shared ground if sunset or sunrise has passed. Every object using the ground style
   repaints; the whole screen and the top layer are invalidated for the crescent and the fades. */
static void ground_refresh(void)
{
    ground_init();
    bool n = night_now();
    if (n == s_night) return;
    s_night = n;
    lv_style_set_bg_color(&s_ground_style, lv_color_hex(n ? COL_BLACK : COL_GLASS));
    lv_obj_report_style_change(&s_ground_style);
    lv_obj_invalidate(lv_scr_act());
    lv_obj_invalidate(lv_layer_top());
    ESP_LOGI(TAG, "ground: %s", n ? "night (black)" : "day (plum glass)");
}

bool dial_is_night(void) { return s_night; }
uint32_t dial_ground(void) { return s_night ? COL_BLACK : COL_GLASS; }

void dial_use_ground(lv_obj_t *o)
{
    ground_init();
    lv_obj_remove_local_style_prop(o, LV_STYLE_BG_COLOR, LV_PART_MAIN);
    lv_obj_add_style(o, &s_ground_style, LV_PART_MAIN);
}

void dial_set_night_override(int mode)
{
    bsp_display_lock(-1);
    s_night_override = mode < 0 ? -1 : mode ? 1 : 0;
    ground_refresh();
    bsp_display_unlock();
}

int dial_night_override(void) { return s_night_override; }

void dial_set_mode(dial_face_t mode)
{
    bsp_display_lock(-1);
    ground_refresh();
    if (mode != s_mode) {
        s_mode = mode;
        if (mode == DIAL_FACE_DAY) lv_obj_remove_local_style_prop(lv_scr_act(), LV_STYLE_BG_COLOR, LV_PART_MAIN);
        else lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COL_BLACK), 0);   /* Rest, Charge: always black */
        lv_obj_invalidate(lv_scr_act());
    }
    bsp_display_unlock();
}

static void tick_cb(lv_timer_t *t)
{
    time_t now = dial_now();
    struct tm lt; localtime_r(&now, &lt);
    static unsigned s_icon_sig;
    ground_refresh();                                   /* sunset / sunrise: the ground follows within a second */
    unsigned sig = icon_sig();
    if (now_lower_tick()) { lv_obj_invalidate(lv_scr_act()); return; }   /* the lower arc's 30 s tap hold ran out */
    /* Invalidate the screen, not the face, so a screen covering the face (a decision) redraws too. */
    if (sig != s_icon_sig) { s_icon_sig = sig; lv_obj_invalidate(lv_scr_act()); return; }   /* a status icon appeared or went */
    if (lt.tm_min != s_last_min) { s_last_min = lt.tm_min; lv_obj_invalidate(lv_scr_act()); return; }
    if (s_mode == DIAL_FACE_DAY) lv_obj_invalidate_area(lv_scr_act(), &s_moon_box);   /* only the second hand moves */
}

void dial_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    dial_use_ground(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_face = lv_obj_create(scr);
    lv_obj_remove_style_all(s_face);
    lv_obj_set_size(s_face, 466, 466);
    lv_obj_center(s_face);
    lv_obj_add_event_cb(s_face, face_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_moon_box = (lv_area_t){ 0, 0, 465, 465 };
    lv_timer_create(tick_cb, 1000, NULL);
}

static int by_start(const void *a, const void *b);

void dial_set_calendar(const cJSON *payload)
{
    const cJSON *evs = cJSON_GetObjectItem(payload, "events");
    if (!cJSON_IsArray(evs)) return;
    bsp_display_lock(-1);
    s_nev = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, evs) {
        if (s_nev >= MAX_EV) break;
        if (cJSON_IsTrue(cJSON_GetObjectItem(e, "all_day"))) continue;
        const cJSON *ti = cJSON_GetObjectItem(e, "title"), *st = cJSON_GetObjectItem(e, "start"), *en = cJSON_GetObjectItem(e, "end");
        if (!cJSON_IsString(st)) continue;
        ev_t *m = &s_ev[s_nev];
        const cJSON *id = cJSON_GetObjectItem(e, "id");
        snprintf(m->id, sizeof m->id, "%s", cJSON_IsString(id) ? id->valuestring : "");
        clean_caps(m->title, sizeof m->title, cJSON_IsString(ti) ? ti->valuestring : "");
        m->start = parse_iso(st->valuestring);
        m->has_end = cJSON_IsString(en);
        m->end = m->has_end ? parse_iso(en->valuestring) : m->start;
        m->free = false;
        /* A timed block of 24 h or more (e.g. a multi-day "Tentative" hold) behaves like an
           all-day event: it would otherwise fill the rim and push real Moments to the overlap lane. */
        if (m->has_end && m->end - m->start >= 86400) continue;
        if (m->start) s_nev++;
    }
    qsort(s_ev, s_nev, sizeof s_ev[0], by_start);
    s_ev_gen++;
    ESP_LOGI(TAG, "calendar: %d timed events", s_nev);
    lv_obj_invalidate(s_face);
    bsp_display_unlock();
}

/* Start order, ties by id: the lane rule depends on it, and dayview.c sorts its list the same way. */
static int by_start(const void *a, const void *b)
{
    const ev_t *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return strcmp(x->id, y->id);
}

/* Moments from the Surface v2 `events` resource: items of kind event/approval that carry a start time
   `at`. The rim draws only the window (rim12.h); ended ones are kept for half a day because the lane rule looks back
   12 h and the Now face's free-stretch arc starts at the last end today. Only a withdrawn Moment (no longer happening)
   leaves at once. Ones that ended more than 12 h ago (the bus keeps tombstones 24 h) are dropped so the 64 slots go to
   what is still ahead. */
static void set_moments_locked(const cJSON *events)
{
    s_nev = 0;
    time_t now = dial_now(), keep_from = now - RIM_LOOKBACK_S, d0 = day_start(now);
    if (d0 < keep_from) keep_from = d0;
    const cJSON *e;
    cJSON_ArrayForEach(e, events) {
        if (s_nev >= MAX_EV) { ESP_LOGW(TAG, "moments: more than %d, the rest dropped", MAX_EV); break; }
        const cJSON *kind = cJSON_GetObjectItem(e, "kind"), *state = cJSON_GetObjectItem(e, "state");
        if (cJSON_IsString(kind) && strcmp(kind->valuestring, "event") && strcmp(kind->valuestring, "approval")) continue;
        if (cJSON_IsString(state) && !strcmp(state->valuestring, "withdrawn")) continue;
        const cJSON *ti = cJSON_GetObjectItem(e, "title"), *at = cJSON_GetObjectItem(e, "at"), *en = cJSON_GetObjectItem(e, "end");
        if (!cJSON_IsString(at)) continue;
        ev_t *m = &s_ev[s_nev];
        snprintf(m->id, sizeof m->id, "%s", e->string ? e->string : "");
        clean_caps(m->title, sizeof m->title, cJSON_IsString(ti) ? ti->valuestring : "");
        m->start = parse_iso(at->valuestring);
        m->end = cJSON_IsString(en) ? parse_iso(en->valuestring) : 0;
        m->has_end = m->end > m->start;
        if (!m->has_end) m->end = m->start;
        m->free = dial_moment_is_free(e);
        if (m->has_end && m->end - m->start >= 86400) continue;
        if (m->end < keep_from) continue;
        if (m->start) {
            ESP_LOGI(TAG, "  moment %s", m->title);
            s_nev++;
        }
    }
    qsort(s_ev, s_nev, sizeof s_ev[0], by_start);   /* lanes are assigned in start order */
    s_ev_gen++;
    ESP_LOGI(TAG, "moments: %d timed", s_nev);
    lv_obj_invalidate(s_face);
}

void dial_set_moments(const cJSON *events)
{
    bsp_display_lock(-1);
    set_moments_locked(events);
    bsp_display_unlock();
}

/* Widget `dial_config` (`surface_write_widget`): data {"rim_gap_min": 30|60|120|180}. Applied when its updated_at
   is newer than the last one applied (or, without updated_at, when the value differs from the last bus value), and
   saved at once, so the last writer wins between Settings and the bus and it survives a reboot. The display lock is
   held. */
static void dial_config_set(const cJSON *widgets)
{
    const cJSON *w = cJSON_GetObjectItem(widgets, "dial_config");
    const cJSON *g = cJSON_GetObjectItem(cJSON_GetObjectItem(w, "data"), "rim_gap_min");
    if (!cJSON_IsNumber(g)) return;
    int min = g->valueint;
    if (rim_gap_valid(min) != min) { ESP_LOGW(TAG, "dial_config: rim_gap_min %d not one of 30, 60, 120, 180", min); return; }
    const cJSON *u = cJSON_GetObjectItem(w, "updated_at");
    time_t at = cJSON_IsString(u) ? parse_iso(u->valuestring) : 0;
    bool apply = at ? (uint32_t)at > prefs_gap_bus_at() : min != s_gap_bus_seen;
    s_gap_bus_seen = min;
    if (!apply) return;
    prefs_set_gap_from_bus(min, at ? (uint32_t)at : prefs_gap_bus_at());
    lv_obj_invalidate(lv_scr_act());
}

void dial_set_widgets(const cJSON *widgets)
{
    bsp_display_lock(-1);
    now_gauges_set(widgets);
    dial_config_set(widgets);
    if (s_face) lv_obj_invalidate(s_face);
    bsp_display_unlock();
}

/* ---- test hooks: POST /debug/ui "clock ..." and "testday ..." ---- */

void dial_clock_debug(const char *a, char *out, size_t n)
{
    int h = -1, m = -1, days = 0;
    a += strspn(a, " ");
    if (!strncmp(a, "off", 3)) {
        bsp_display_lock(-1);
        s_clock_off = 0; s_rc.valid = false;
        lv_obj_invalidate(lv_scr_act());
        bsp_display_unlock();
        snprintf(out, n, "clock off (real time)\n");
        return;
    }
    if (sscanf(a, "%d:%d %d", &h, &m, &days) >= 2 && h >= 0 && h < 24 && m >= 0 && m < 60 && days >= -7 && days <= 7) {
        time_t real = time(NULL);
        struct tm lt; localtime_r(&real, &lt);
        int sec = lt.tm_sec;                            /* keep the seconds: the offset is whole minutes */
        lt.tm_mday += days; lt.tm_hour = h; lt.tm_min = m; lt.tm_sec = sec; lt.tm_isdst = -1;
        time_t want = mktime(&lt);
        bsp_display_lock(-1);
        s_clock_off = want - real;
        s_rc.valid = false;
        lv_obj_invalidate(lv_scr_act());
        bsp_display_unlock();
    } else if (a[0]) { snprintf(out, n, "clock HH:MM [+days] | clock off | clock\n"); return; }
    time_t now = dial_now();
    struct tm lt; localtime_r(&now, &lt);
    snprintf(out, n, "drawing clock %04d-%02d-%02d %02d:%02d:%02d (offset %ld s), gap %d min, planet %.1f deg\n",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, (long)s_clock_off,
             dial_gap_min(), dial_clock_deg(now));
}

bool dial_testday_active(void) { return s_testday; }

void dial_widgets_debug(const char *json, char *out, size_t n)
{
    cJSON *j = cJSON_Parse(json + strspn(json, " "));
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); snprintf(out, n, "bad json\n"); return; }
    dial_set_widgets(j);
    cJSON_Delete(j);
    snprintf(out, n, "widgets applied: rim gap %d min (bus stamp %lu)\n", dial_gap_min(), (unsigned long)prefs_gap_bus_at());
}

void dial_gap_debug(int min, char *out, size_t n)
{
    bsp_display_lock(-1);
    prefs_set_gap_min(min);
    prefs_save();
    lv_obj_invalidate(lv_scr_act());
    bsp_display_unlock();
    snprintf(out, n, "rim gap %d min\n", dial_gap_min());
}

static void test_ev(cJSON *o, const char *id, const char *title, time_t s, time_t e)
{
    char iso[24]; struct tm g;
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "kind", "event");
    cJSON_AddStringToObject(ev, "title", title);
    cJSON_AddStringToObject(ev, "weight", "ambient");
    cJSON_AddStringToObject(ev, "state", "open");
    gmtime_r(&s, &g); strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &g);
    cJSON_AddStringToObject(ev, "at", iso);
    if (e > s) { gmtime_r(&e, &g); strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &g); cJSON_AddStringToObject(ev, "end", iso); }
    cJSON_AddItemToObject(o, id, ev);
}

/* test_ev plus "free": true (the bus's FREE flag). */
static void test_free(cJSON *o, const char *id, const char *title, time_t s, time_t e)
{
    test_ev(o, id, title, s, e);
    cJSON_AddBoolToObject(cJSON_GetObjectItem(o, id), "free", true);
}

/* Wall time h:m on the drawing clock's date plus `days`. */
static time_t wall(int days, int h, int m)
{
    time_t now = dial_now();
    struct tm lt; localtime_r(&now, &lt);
    lt.tm_mday += days; lt.tm_hour = h; lt.tm_min = m; lt.tm_sec = 0; lt.tm_isdst = -1;
    return mktime(&lt);
}

/* "testday rel": Moments around the drawing clock (minute aligned): one past, one running, points and spans ahead,
   an overlap pair, a span across the leading edge (at the 1 h default) and two beyond it.
   "testday abs": fixed wall times on the drawing clock's date, for the proposal's checks at 7:00, 12:00, 18:30, 23:30
   and 0:15 (+1): 17:30 at 165 degrees, 18:30 absent at 7:00, a span 17:00-19:00 across the 7:00 edge, a third
   overlap at 9:45, tomorrow's 0:30 and 7:00.
   "testday free": busy and FREE Moments mixed on the drawing clock's date (use with clock 07:00, 07:45, 08:15, 09:15,
   20:30): free ones overlapping busy ones, a free point inside a free stretch, a free one running, a free one next.
   "testday off": back to the live `events` (the bus link reconnects for a fresh reset). */
void dial_testday_debug(const char *a, char *out, size_t n)
{
    a += strspn(a, " ");
    if (!strncmp(a, "off", 3)) {
        bool was = s_testday;
        s_testday = false;
        if (was) intercom_request_resync();
        snprintf(out, n, was ? "testday off: asked the bus for a fresh reset\n" : "testday was off\n");
        return;
    }
    bool abs_set = !strncmp(a, "abs", 3);
    bool det_set = !strncmp(a, "detail", 6);
    bool free_set = !strncmp(a, "free", 4);
    if (!abs_set && !det_set && !free_set && strncmp(a, "rel", 3)) { snprintf(out, n, "testday rel | abs | detail | free | off\n"); return; }
    cJSON *o = cJSON_CreateObject();
    if (abs_set) {
        test_ev(o, "t-a01", "PAST SPAN", wall(0, 6, 0), wall(0, 6, 45));
        test_ev(o, "t-a02", "POINT 7:30", wall(0, 7, 30), 0);
        test_ev(o, "t-a03", "LANE ONE", wall(0, 9, 0), wall(0, 10, 0));
        test_ev(o, "t-a04", "LANE TWO", wall(0, 9, 30), wall(0, 11, 0));
        test_ev(o, "t-a05", "THIRD OVERLAP", wall(0, 9, 45), wall(0, 10, 15));
        test_ev(o, "t-a06", "NOON POINT", wall(0, 12, 0), 0);
        test_ev(o, "t-a07", "AFTERNOON", wall(0, 14, 0), wall(0, 15, 30));
        test_ev(o, "t-a08", "CROSSES EDGE", wall(0, 17, 0), wall(0, 19, 0));
        test_ev(o, "t-a09", "POINT 5:30", wall(0, 17, 30), 0);
        test_ev(o, "t-a10", "POINT 6:30", wall(0, 18, 30), 0);
        test_ev(o, "t-a11", "LATE", wall(0, 22, 0), wall(0, 23, 0));
        test_ev(o, "t-a12", "AFTER MIDNIGHT", wall(1, 0, 30), 0);
        test_ev(o, "t-a13", "TOMORROW AM", wall(1, 7, 0), wall(1, 8, 0));
    } else if (free_set) {
        test_free(o, "t-f01", "FOCUS TIME", wall(0, 7, 28), wall(0, 8, 28));      /* free, overlaps the busy sync below */
        test_ev(o, "t-f02", "TEAM SYNC", wall(0, 8, 0), wall(0, 9, 0));
        test_free(o, "t-f03", "COFFEE CHAT", wall(0, 9, 30), 0);                    /* a free point inside the 9:00-10:00 stretch */
        test_ev(o, "t-f04", "PLANNING", wall(0, 10, 0), wall(0, 11, 0));
        test_free(o, "t-f05", "OFFICE HOURS", wall(0, 11, 30), wall(0, 12, 30));
        test_ev(o, "t-f06", "CLIENT REVIEW", wall(0, 13, 0), wall(0, 14, 0));
        test_free(o, "t-f07", "LUNCH AND LEARN", wall(0, 14, 30), wall(0, 15, 30));
        test_ev(o, "t-f08", "DEEP DIVE", wall(0, 15, 0), wall(0, 16, 0));          /* busy, overlaps the free one: no conflict */
        test_ev(o, "t-f09", "DOUBLE BOOKED", wall(0, 15, 30), wall(0, 16, 30));     /* a real overlap with t-f08 */
        test_free(o, "t-f10", "GYM", wall(0, 17, 0), 0);
        test_ev(o, "t-f11", "DINNER", wall(0, 18, 0), wall(0, 19, 0));
        test_free(o, "t-f12", "EVENING WALK", wall(0, 20, 0), wall(0, 21, 0));   /* clock 20:30: a free one running, nothing next */
    } else if (!strncmp(a, "detail", 6)) {
        /* Event detail title bands (use with clock 09:00): short, wrapping, truncating titles, with and without place or
           note, open and ended-cleared captions. Open one with "detail <id>". */
        test_ev(o, "t-d01", "SHORT", wall(0, 10, 0), wall(0, 11, 0));
        test_ev(o, "t-d02", "QUARTERLY PLANNING REVIEW WITH VENDORS", wall(0, 10, 0), wall(0, 11, 0));
        test_ev(o, "t-d03", "ANNUAL ENTERPRISE DATA PLATFORM ROADMAP AND BUDGET ALIGNMENT SESSION WITH THE EXECUTIVE LEADERSHIP TEAM", wall(0, 10, 0), wall(0, 11, 0));
        test_ev(o, "t-d04", "SHORT", wall(0, 10, 0), wall(0, 11, 0));
        test_ev(o, "t-d05", "QUARTERLY PLANNING REVIEW WITH VENDORS", wall(0, 10, 0), wall(0, 11, 0));
        test_ev(o, "t-d06", "ANNUAL ENTERPRISE DATA PLATFORM ROADMAP AND BUDGET ALIGNMENT SESSION WITH THE EXECUTIVE LEADERSHIP TEAM", wall(0, 10, 0), wall(0, 11, 0));
        test_ev(o, "t-d07", "DENTIST", wall(0, 9, 30), 0);
        test_ev(o, "t-d08", "QUARTERLY PLANNING REVIEW WITH VENDORS", wall(0, 7, 28), wall(0, 8, 28));
        test_ev(o, "t-d09", "DEEP DIVE: CLOUD MIGRATION", wall(0, 10, 0), wall(0, 11, 0));
        const char *ids[] = { "t-d04", "t-d05", "t-d06", "t-d08" };
        for (int i = 0; i < 4; i++) {
            cJSON *ev = cJSON_GetObjectItem(o, ids[i]), *det = cJSON_AddObjectToObject(ev, "detail");
            cJSON_AddStringToObject(det, "location", "Conference Room 4B, 200 Pier 4 Blvd");
            cJSON_AddStringToObject(ev, "body", "Bring the revised forecast and the vendor comparison.");
        }
        cJSON_ReplaceItemInObject(cJSON_GetObjectItem(o, "t-d08"), "state", cJSON_CreateString("cleared"));
    } else {
        time_t t = dial_now() / 60 * 60;
        test_ev(o, "t-r01", "PAST", t - 3 * 3600, t - 2 * 3600);
        test_ev(o, "t-r02", "RUNNING", t - 30 * 60, t + 30 * 60);
        test_ev(o, "t-r03", "POINT 45 MIN", t + 45 * 60, 0);
        test_ev(o, "t-r04", "LANE ONE", t + 2 * 3600, t + 3 * 3600);
        test_ev(o, "t-r05", "LANE TWO", t + 150 * 60, t + 4 * 3600);
        test_ev(o, "t-r06", "FIVE TO SIX", t + 5 * 3600, t + 6 * 3600);
        test_ev(o, "t-r07", "POINT 8 H", t + 8 * 3600, 0);
        test_ev(o, "t-r08", "CROSSES EDGE", t + 630 * 60, t + 12 * 3600);
        test_ev(o, "t-r09", "BEYOND POINT", t + 690 * 60, 0);
        test_ev(o, "t-r10", "BEYOND SPAN", t + 13 * 3600, t + 14 * 3600);
    }
    s_testday = true;
    dial_set_moments(o);
    dayview_on_events(o);
    int cnt = cJSON_GetArraySize(o);
    cJSON_Delete(o);
    snprintf(out, n, "testday %s: %d test Moments (live events set aside until \"testday off\")\n", abs_set ? "abs" : det_set ? "detail" : free_set ? "free" : "rel", cnt);
}

void dial_gauge_debug(const char *args, char *out, size_t n)
{
    bsp_display_lock(-1);
    now_gauge_debug(args, out, n);
    if (s_face) lv_obj_invalidate(s_face);
    bsp_display_unlock();
}

void dial_set_activity(const char *state, const char *status_text)
{
    bsp_display_lock(-1);
    snprintf(s_state, sizeof s_state, "%s", state ? state : "idle");
    clean_caps(s_status, sizeof s_status, status_text ? status_text : "");
    lv_obj_invalidate(s_face);
    bsp_display_unlock();
}

void dial_set_agent_count(int n)
{
    bsp_display_lock(-1);
    if (n != s_agents) { s_agents = n; lv_obj_invalidate(s_face); }
    bsp_display_unlock();
}

void dial_set_link(const char *text)
{
    bsp_display_lock(-1);
    clean_caps(s_link, sizeof s_link, text);
    lv_obj_invalidate(s_face);
    bsp_display_unlock();
}

