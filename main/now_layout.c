/*
 * The Now face as a table (design: "Now Face Slots", spec 4.4).
 *
 * A slot is a fixed place on the dial (crown, heading, hero, sub, footer, base, arc_upper, arc_lower, east). Each
 * slot lists candidate components in priority order; the first one that is visible wins. A component owns its
 * content and visibility and never knows its position. To swap a component or change priority, edit the table at
 * the bottom of this file; nothing else changes. The table is compile-time fixed.
 *
 * Rules (see the artifact): hero and sub are a pair (sub empty: hero centres; a tap swaps them); sub has a fixed
 * baseline; the two arcs are gauges drawn whole by gauge_draw, and a gauge component only returns a gauge_t.
 * dial.c computes one now_ctx_t per redraw strip; everything here reads it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dial.h"
#include "dial_priv.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fonts.h"
#include "power.h"
#include "reply_logic.h"

typedef enum { K_CURVED, K_LABEL, K_NUMERAL, K_GAUGE, K_ICON } kind_t;
typedef enum { G_FILL, G_RANGE, G_CENTRE, G_STEPPER } gmode_t;   /* from start · dot in a band · off the middle · stops */
typedef enum { HEAD_FILL, HEAD_HANDLE, HEAD_NONE } head_t;

typedef struct {                       /* what a gauge component returns; the slot draws it */
    gmode_t mode;
    float frac;                        /* FILL, RANGE, STEPPER: 0..1.  CENTRE: -1..1, 0 = on track */
    uint32_t col, col_neg;  lv_opa_t opa;   /* col_neg: CENTRE when left of the middle */
    bool start_dot;  head_t head;  uint32_t head_col;   /* head_col 0 = col */
    float lo, hi;                      /* RANGE: the band, 0..1 */
    uint8_t n_marks;  float marks[3];  /* optional ticks at fixed fractions */
    char label[24];                    /* optional; the paired label slot shows it */
    char start_label[6], end_label[6]; /* optional; inside the ends, dropped if too wide */
    time_t target;                     /* optional; the countdown counts to it (upper gauge only) */
    float lead;                        /* STEPPER: share of the span used by the dashed lead-in */
    uint8_t n_stops;  float stops[12]; /* STEPPER: stop positions, 0..1 */
} gauge_t;

typedef struct {                       /* a component's output; which fields matter depends on the slot kind */
    char text[112];                    /* curved and label text; numeral digits */
    char unit[8];                      /* numeral unit */
    uint32_t col;                      /* label colour override, 0 = the slot's */
    gauge_t g;
} comp_out_t;

typedef struct {
    const char *name;  kind_t kind;
    bool tap_only;                     /* gauges: never the default, only reached by tapping the arc */
    bool (*visible)(const now_ctx_t *c);   /* has something to show (tap-only comps: has data) */
    void (*fill)(const now_ctx_t *c, comp_out_t *o);
} comp_t;

typedef enum { R_CROWN, R_HEADING, R_HERO, R_SUB, R_FOOTER, R_BASE, R_GAUGE, R_EAST } role_t;

typedef struct {
    const char *name;  kind_t kind;  role_t role;
    int a, b;                          /* label: y_top, max width (0 = no fit); numeral: baseline; gauge: dir */
    float r, a0;                       /* gauge: radius and start angle (dial degrees) */
    const comp_t *const *cands;        /* NULL-terminated; first visible wins */
    bool tap;                          /* gauge: a tap on the arc steps through the visible candidates */
} slot_t;

/* ---- helpers ---- */

/* x, y_top of text whose baseline sits at `base` */
static int top_for_base(const lv_font_t *f, int base) { return base - (lv_font_get_line_height(f) - f->base_line); }

static bool focus_range(const now_ctx_t *c, time_t *from, time_t *to)
{
    if (c->has_running && c->running_has_end) { *from = c->running_start; *to = c->running_end; }
    else if (c->has_busy_next) {                    /* free Moments do not end or start a free stretch */
        *to = c->busy_next_start;
        *from = c->gap_from ? c->gap_from : (*to - 3 * 3600 > c->d0 ? *to - 3 * 3600 : c->d0);
        if (*from > c->now) *from = c->now;   /* the window can name tomorrow's 7:00 at 11 PM: never a negative fill */
    } else return false;
    return *to > *from;
}

/* ---- components ---- */

static bool always(const now_ctx_t *c) { (void)c; return true; }

/* crown */
static bool busy_until_vis(const now_ctx_t *c) { return c->has_running && c->running_has_end; }
static void busy_until_fill(const now_ctx_t *c, comp_out_t *o)
{
    char t[16]; dv_fmt_clock(t, sizeof t, c->running_end);
    snprintf(o->text, sizeof o->text, "BUSY UNTIL %s", t);
}
static void date_fill(const now_ctx_t *c, comp_out_t *o)
{
    static const char *const wd[] = { "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY" };
    static const char *const mo[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    snprintf(o->text, sizeof o->text, "%s \xC2\xB7 %d %s", wd[c->lt.tm_wday % 7], c->lt.tm_mday, mo[c->lt.tm_mon % 12]);
}
/* E105: notification replies still awaiting an answer. Outranks the date, yields to BUSY UNTIL (proposal 3.2 c). */
static bool needs_you_vis(const now_ctx_t *c) { return c->needs_you > 0; }
static void needs_you_fill(const now_ctx_t *c, comp_out_t *o)
{
    reply_needs_caption(true, c->needs_you, o->text, sizeof o->text);
    o->col = COL_PERI;
}
static const comp_t needs_you  = { "needs_you",  K_CURVED, false, needs_you_vis,  needs_you_fill };
/* sports: text from the `sports` widget (a score line), first choice of the crown while it is valid */
static bool sports_vis(const now_ctx_t *c);
static void sports_fill(const now_ctx_t *c, comp_out_t *o);
static const comp_t sports     = { "sports",     K_CURVED, false, sports_vis,     sports_fill };
static const comp_t busy_until = { "busy_until", K_CURVED, false, busy_until_vis, busy_until_fill };
static const comp_t date       = { "date",       K_CURVED, false, always,         date_fill };

/* heading */
static bool upper_label_vis(const now_ctx_t *c) { return c->upper_label[0]; }
static void upper_label_fill(const now_ctx_t *c, comp_out_t *o) { snprintf(o->text, sizeof o->text, "%s", c->upper_label); }
static bool next_time_vis(const now_ctx_t *c) { return c->has_next; }
static void next_time_fill(const now_ctx_t *c, comp_out_t *o)
{
    char t[16]; dv_fmt_clock(t, sizeof t, c->next_start);
    struct tm nt; localtime_r(&c->next_start, &nt);
    bool tomorrow = nt.tm_mday != c->lt.tm_mday || nt.tm_mon != c->lt.tm_mon;   /* the 12 h window reaches past midnight */
    snprintf(o->text, sizeof o->text, tomorrow ? "NEXT %s TOMORROW" : "NEXT %s", t);
}
/* A free next Moment: the same heading with FREE after it, in ink-2 (rose is the busy countdown's colour). */
static void next_time_fill_free(const now_ctx_t *c, comp_out_t *o)
{
    next_time_fill(c, o);
    if (!c->next_free) return;
    size_t n = strlen(o->text);
    snprintf(o->text + n, sizeof o->text - n, " \xC2\xB7 FREE");
    o->col = COL_INK2;
}
/* Only a free Moment running, nothing next: it is on, but it is not busy time. */
static bool free_now_vis(const now_ctx_t *c) { return c->has_free_running; }
static void free_now_fill(const now_ctx_t *c, comp_out_t *o)
{
    char t[16]; dv_fmt_clock(t, sizeof t, c->free_running_end);
    snprintf(o->text, sizeof o->text, "ON NOW \xC2\xB7 UNTIL %s \xC2\xB7 FREE", t);
    o->col = COL_INK2;
}
static const comp_t upper_label = { "upper_label", K_LABEL, false, upper_label_vis, upper_label_fill };
static const comp_t next_time   = { "next_time",   K_LABEL, false, next_time_vis,   next_time_fill_free };
static const comp_t free_now    = { "free_now",    K_LABEL, false, free_now_vis,    free_now_fill };

/* hero / sub */
static void clock_fill(const now_ctx_t *c, comp_out_t *o)
{
    dv_fmt_clock(o->text, sizeof o->text, c->now);
    snprintf(o->unit, sizeof o->unit, "%s", c->lt.tm_hour < 12 ? "AM" : "PM");
}
/* No countdown for a free Moment: it is not something to be ready for. */
static bool countdown_vis(const now_ctx_t *c) { return (c->has_next && !c->next_free) || c->upper_target; }
static void countdown_fill(const now_ctx_t *c, comp_out_t *o)
{
    /* counts to the upper gauge's target when it has one, else to the next Moment */
    int mins = (int)(((c->upper_target ? c->upper_target : c->next_start) - c->now + 59) / 60);
    if (mins < 0) mins = 0;
    if (mins < 100) { snprintf(o->text, sizeof o->text, "%d", mins); snprintf(o->unit, sizeof o->unit, "MIN"); }
    else { snprintf(o->text, sizeof o->text, "%d:%02d", mins / 60, mins % 60); snprintf(o->unit, sizeof o->unit, "HRS"); }
}
static const comp_t clock_c   = { "clock",     K_NUMERAL, false, always,        clock_fill };
static const comp_t countdown = { "countdown", K_NUMERAL, false, countdown_vis, countdown_fill };

/* footer */
static bool link_state_vis(const now_ctx_t *c) { return !c->link_up && !c->phone; }
static void link_state_fill(const now_ctx_t *c, comp_out_t *o) { snprintf(o->text, sizeof o->text, "%s", c->link); }
static bool agent_status_vis(const now_ctx_t *c) { return c->agent_busy; }
static void agent_status_fill(const now_ctx_t *c, comp_out_t *o)
{
    snprintf(o->text, sizeof o->text, "%s", c->status[0] ? c->status : "WORKING");
    o->col = COL_ROSE;
}
static bool phone_vis(const now_ctx_t *c) { return c->phone; }
static void phone_fill(const now_ctx_t *c, comp_out_t *o) { (void)c; snprintf(o->text, sizeof o->text, "PHONE"); }
static bool lower_label_vis(const now_ctx_t *c) { return c->lower_label[0]; }
static void lower_label_fill(const now_ctx_t *c, comp_out_t *o) { snprintf(o->text, sizeof o->text, "%s", c->lower_label); }
static bool left_today_vis(const now_ctx_t *c) { return c->upcoming > 0; }
static void left_today_fill(const now_ctx_t *c, comp_out_t *o) { snprintf(o->text, sizeof o->text, "%d LEFT TODAY", c->upcoming); }
static const comp_t link_state   = { "link_state",   K_LABEL, false, link_state_vis,   link_state_fill };
static const comp_t agent_status = { "agent_status", K_LABEL, false, agent_status_vis, agent_status_fill };
static const comp_t phone_link   = { "phone_link",   K_LABEL, false, phone_vis,        phone_fill };
static const comp_t lower_label  = { "lower_label",  K_LABEL, false, lower_label_vis,  lower_label_fill };
static const comp_t left_today   = { "left_today",   K_LABEL, false, left_today_vis,   left_today_fill };

/* base */
static bool moment_title_vis(const now_ctx_t *c) { return c->has_next || c->has_running || c->has_free_running; }
static void moment_title_fill(const now_ctx_t *c, comp_out_t *o)
{
    snprintf(o->text, sizeof o->text, "%s", c->has_next ? c->next_title : c->has_running ? c->running_title : c->free_running_title);
}
static const comp_t moment_title = { "moment_title", K_CURVED, false, moment_title_vis, moment_title_fill };

/* gauges */
static bool focus_vis(const now_ctx_t *c) { time_t f, t; return focus_range(c, &f, &t); }
static void focus_fill(const now_ctx_t *c, comp_out_t *o)
{
    time_t from = 0, to = 1; focus_range(c, &from, &to);
    float f = (float)(c->now - from) / (float)(to - from);
    o->g = (gauge_t){ .mode = G_FILL, .frac = f < 0 ? 0 : f > 1 ? 1 : f, .col = COL_ROSE, .opa = LV_OPA_COVER,
                      .start_dot = true, .head = HEAD_FILL };
}
#define DAY_START_H 6.0f
#define DAY_END_H   23.0f
static void day_fill(const now_ctx_t *c, comp_out_t *o)
{
    float f = (c->hrs - DAY_START_H) / (DAY_END_H - DAY_START_H);        /* the waking day: 6 AM to 11 PM */
    o->g = (gauge_t){ .mode = G_FILL, .frac = f < 0 ? 0 : f > 1 ? 1 : f, .col = COL_ROSE, .opa = 140 /* 55% */,
                      .start_dot = false, .head = HEAD_HANDLE };
}
static const comp_t focus_progress = { "focus_progress", K_GAUGE, false, focus_vis, focus_fill };
static const comp_t day_progress   = { "day_progress",   K_GAUGE, false, always,   day_fill };

/* the device battery: data whenever it has one, so it is tap only */
static bool battery_vis(const now_ctx_t *c) { (void)c; return power_battery_pct() >= 0; }
static void battery_fill(const now_ctx_t *c, comp_out_t *o)
{
    (void)c;
    int p = power_battery_pct();
    o->g = (gauge_t){ .mode = G_FILL, .frac = p / 100.0f, .col = COL_AMBER, .opa = 204 /* 80% */,
                      .start_dot = false, .head = HEAD_HANDLE };
    snprintf(o->g.label, sizeof o->g.label, "%s %d%%", power_on_usb() ? "CHARGING" : "BATTERY", p);
}
static const comp_t battery = { "battery", K_GAUGE, true, battery_vis, battery_fill };

/* ---- gauge payloads from the bus (the `widgets` resource; data/intercom/gauge-payload.md) ----
   Ids `leave_by` and `weather`; each item is {type:"gauge", data:{...}}. The bus republishes on change; the device
   keeps only the last payload of each (a few dozen bytes) and drops it by its own clock, so a dead publisher never
   leaves a stale gauge. Everything here runs with the display lock held. */
typedef struct { bool ok; time_t to, span; char label[24]; time_t valid_until; } leave_data_t;
typedef struct { bool ok, has_now; char date[11]; int low, high, now; char cond[16]; time_t valid_until; } weather_data_t;
static leave_data_t s_leave;
static weather_data_t s_wx;
typedef struct { bool ok; char text[40]; time_t valid_until; } sports_data_t;
static sports_data_t s_sports;
static int s_morning_override = -1;     /* testing: 1 = it is morning, 0 = it is not, -1 = by the clock */

static time_t json_time(const cJSON *d, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(d, key);
    return cJSON_IsString(v) ? dial_parse_iso(v->valuestring) : 0;
}

static void parse_leave(const cJSON *d)
{
    memset(&s_leave, 0, sizeof s_leave);
    if (!cJSON_IsObject(d)) return;
    s_leave.to = json_time(d, "to");
    if (!s_leave.to) return;
    const cJSON *sp = cJSON_GetObjectItem(d, "span_min"), *lb = cJSON_GetObjectItem(d, "label");
    int m = cJSON_IsNumber(sp) ? sp->valueint : 30;
    s_leave.span = (m < 1 ? 1 : m > 120 ? 120 : m) * 60;
    if (cJSON_IsString(lb)) dial_caps(s_leave.label, sizeof s_leave.label, lb->valuestring);
    s_leave.valid_until = json_time(d, "valid_until");
    s_leave.ok = true;
}

static int clamp_temp(double v) { return v < -99 ? -99 : v > 199 ? 199 : (int)lround(v); }   /* fits the 5-character end labels */

static void parse_weather(const cJSON *d)
{
    memset(&s_wx, 0, sizeof s_wx);
    if (!cJSON_IsObject(d)) return;
    const cJSON *dt = cJSON_GetObjectItem(d, "date"), *lo = cJSON_GetObjectItem(d, "low"), *hi = cJSON_GetObjectItem(d, "high");
    const cJSON *nw = cJSON_GetObjectItem(d, "now"), *cd = cJSON_GetObjectItem(d, "cond");
    if (!cJSON_IsString(dt) || strlen(dt->valuestring) != 10 || !cJSON_IsNumber(lo) || !cJSON_IsNumber(hi)) return;
    memcpy(s_wx.date, dt->valuestring, 11);
    s_wx.low = clamp_temp(lo->valuedouble);
    s_wx.high = clamp_temp(hi->valuedouble);
    if (cJSON_IsNumber(nw)) { s_wx.has_now = true; s_wx.now = clamp_temp(nw->valuedouble); }
    if (cJSON_IsString(cd)) {
        dial_caps(s_wx.cond, sizeof s_wx.cond, cd->valuestring);
        static const char *const shorts[][2] = {      /* long condition words that would crowd the footer */
            { "THUNDERSTORMS", "T-STORMS" }, { "THUNDERSTORM", "T-STORM" }, { "FREEZING RAIN", "FRZ RAIN" },
            { "FREEZING DRIZZLE", "FRZ DRIZZLE" }, { "PARTLY CLOUDY", "P CLOUDY" }, { "MOSTLY CLOUDY", "M CLOUDY" },
            { "MOSTLY SUNNY", "M SUNNY" }, { "SNOW SHOWERS", "SNOW SHWRS" }, { "RAIN SHOWERS", "RAIN SHWRS" },
        };
        for (size_t i = 0; i < sizeof shorts / sizeof shorts[0]; i++)
            if (!strcmp(s_wx.cond, shorts[i][0])) { snprintf(s_wx.cond, sizeof s_wx.cond, "%s", shorts[i][1]); break; }
    }
    s_wx.valid_until = json_time(d, "valid_until");
    s_wx.ok = true;
}

static void parse_sports(const cJSON *d)
{
    memset(&s_sports, 0, sizeof s_sports);
    if (!cJSON_IsObject(d)) return;
    const cJSON *t = cJSON_GetObjectItem(d, "text");
    if (!cJSON_IsString(t)) return;
    dial_caps(s_sports.text, sizeof s_sports.text, t->valuestring);
    if (!s_sports.text[0]) return;
    s_sports.valid_until = json_time(d, "valid_until");
    s_sports.ok = true;
}

static bool sports_vis(const now_ctx_t *c)
{
    return s_sports.ok && (!s_sports.valid_until || c->now < s_sports.valid_until);
}
static void sports_fill(const now_ctx_t *c, comp_out_t *o)
{
    (void)c;
    snprintf(o->text, sizeof o->text, "%s", s_sports.text);
}

static const cJSON *widget_data(const cJSON *widgets, const char *id)
{
    return cJSON_GetObjectItem(cJSON_GetObjectItem(widgets, id), "data");
}

/* The whole resource: a widget that is absent is cleared. */
void now_gauges_set(const cJSON *widgets)
{
    parse_leave(widget_data(widgets, "leave_by"));
    parse_weather(widget_data(widgets, "weather"));
    parse_sports(widget_data(widgets, "sports"));
    ESP_LOGI("now", "gauges: leave_by %s, weather %s, sports %s", s_leave.ok ? "yes" : "no", s_wx.ok ? "yes" : "no", s_sports.ok ? "yes" : "no");
}

static bool leave_by_vis(const now_ctx_t *c)
{
    return s_leave.ok && c->now < s_leave.to && c->now >= s_leave.to - s_leave.span &&
           (!s_leave.valid_until || c->now < s_leave.valid_until);
}
static void leave_by_fill(const now_ctx_t *c, comp_out_t *o)
{
    float f = (float)(c->now - (s_leave.to - s_leave.span)) / (float)s_leave.span;
    char t[16]; dv_fmt_clock(t, sizeof t, s_leave.to);
    o->g = (gauge_t){ .mode = G_FILL, .frac = f < 0 ? 0 : f > 1 ? 1 : f, .col = COL_AMBER, .opa = LV_OPA_COVER,
                      .start_dot = true, .head = HEAD_FILL, .target = s_leave.to };
    if (s_leave.label[0]) snprintf(o->g.label, sizeof o->g.label, "%s", s_leave.label);
    else snprintf(o->g.label, sizeof o->g.label, "LEAVE %s", t);
    snprintf(o->g.end_label, sizeof o->g.end_label, "%.5s", t);
}
static const comp_t leave_by = { "leave_by", K_GAUGE, false, leave_by_vis, leave_by_fill };

static bool weather_has_data(const now_ctx_t *c)
{
    char today[32];
    snprintf(today, sizeof today, "%04d-%02d-%02d", c->lt.tm_year + 1900, c->lt.tm_mon + 1, c->lt.tm_mday);
    return s_wx.ok && !strcmp(s_wx.date, today) && (!s_wx.valid_until || c->now < s_wx.valid_until);
}
static bool is_morning(const now_ctx_t *c) { return s_morning_override >= 0 ? s_morning_override : c->hrs < 10.0f; }
/* Two entries for one gauge: before 10:00 it is a default of the lower arc; afterwards only a tap reaches it. */
static bool weather_am_vis(const now_ctx_t *c) { return weather_has_data(c) && is_morning(c); }
static bool weather_tap_vis(const now_ctx_t *c) { return weather_has_data(c) && !is_morning(c); }
static void weather_fill(const now_ctx_t *c, comp_out_t *o)
{
    (void)c;
    int span = s_wx.high - s_wx.low;
    float f = s_wx.has_now && span > 0 ? (float)(s_wx.now - s_wx.low) / (float)span : 0;
    o->g = (gauge_t){ .mode = G_RANGE, .frac = f < 0 ? 0 : f > 1 ? 1 : f, .col = COL_AMBER, .opa = 102 /* 40% */,
                      .lo = 0, .hi = 1, .head = s_wx.has_now ? HEAD_FILL : HEAD_NONE, .head_col = COL_INK };
    snprintf(o->g.start_label, sizeof o->g.start_label, "%d\xC2\xB0", s_wx.low);
    snprintf(o->g.end_label, sizeof o->g.end_label, "%d\xC2\xB0", s_wx.high);
    if (s_wx.has_now) snprintf(o->g.label, sizeof o->g.label, "%d\xC2\xB0%s%s", s_wx.now, s_wx.cond[0] ? " " : "", s_wx.cond);
    else snprintf(o->g.label, sizeof o->g.label, "%s", s_wx.cond);
}
static const comp_t weather     = { "weather",     K_GAUGE, false, weather_am_vis,  weather_fill };
static const comp_t weather_tap = { "weather_tap", K_GAUGE, true,  weather_tap_vis, weather_fill };

/* east */
static bool icons_vis(const now_ctx_t *c) { (void)c; return dv_icon_count() > 0; }
static const comp_t status_icons = { "status_icons", K_ICON, false, icons_vis, NULL };

/* ---- gauge: one routine for both arcs ---- */

static float g_at(const slot_t *s, float x) { return s->a0 + (float)s->b * 120.0f * x; }

static void g_band(lv_layer_t *l, const slot_t *s, float x0, float x1, float w, uint32_t col, lv_opa_t opa)
{
    float a = g_at(s, x0), b = g_at(s, x1);
    dv_arc(l, s->r, w, a < b ? a : b, a < b ? b : a, col, opa);
}

static void g_dot(lv_layer_t *l, const slot_t *s, float x, float r, uint32_t col, lv_opa_t opa)
{
    float px, py; dv_pt(g_at(s, x), s->r, &px, &py);
    dv_disc(l, px, py, r, col, opa);
}

#define END_ZONE_W 54                  /* widest end label: 5 characters at 13 px, tracked */

/* start_label is always the left end; the text sits just inside the arc end and is dropped, never cut, if too wide. */
static void g_end_label(lv_layer_t *l, const slot_t *s, const char *txt, bool left)
{
    if (!txt[0]) return;
    int w = dv_text_w(txt, f_meta13, 1);
    if (w > END_ZONE_W) return;
    float px, py; dv_pt(g_at(s, left ? 0 : 1), s->r, &px, &py);
    int base = (int)(s->b > 0 ? py + 21 : py - 13);          /* inside: below the upper arc's ends, above the lower's */
    dv_text_at(l, txt, f_meta13, 1, COL_INK2, LV_OPA_COVER, left ? (int)px + 2 : (int)px - 2 - w, top_for_base(f_meta13, base));
}

/* The slot owns the whole arc: the track is always drawn (a gauge with nothing to show is the bare hairline);
   v adds fill, start dot, head, marks and end labels. */
static void gauge_draw(lv_layer_t *l, const slot_t *s, const gauge_t *v)
{
    float lead = (v && v->mode == G_STEPPER) ? v->lead : 0;
    g_band(l, s, lead, 1, 2.5f, COL_HAIR, LV_OPA_COVER);
    if (!v) return;
    float pos = v->frac;
    uint32_t col = v->col;
    switch (v->mode) {
    case G_FILL:
        if (v->frac > 0.01f) g_band(l, s, 0, v->frac, 3, v->col, v->opa);
        if (v->start_dot) g_dot(l, s, 0, 4, v->col, LV_OPA_COVER);
        break;
    case G_RANGE:
        if (v->hi > v->lo) g_band(l, s, v->lo, v->hi, 5, v->col, v->opa);
        break;
    case G_CENTRE: {
        pos = 0.5f + 0.5f * v->frac;
        if (v->frac < 0) col = v->col_neg;
        if (fabsf(v->frac) > 0.02f) g_band(l, s, 0.5f, pos, 3, col, v->opa);
        float ax, ay; dv_pt(g_at(s, 0.5f), s->r - 9, &ax, &ay);
        float bx, by; dv_pt(g_at(s, 0.5f), s->r + 9, &bx, &by);
        dv_seg(l, ax, ay, bx, by, COL_INK);
        break; }
    case G_STEPPER:
        if (lead > 0) {                /* dashed lead-in: 3 px dashes every 8 px */
            float pitch = 8.0f / (120.0f * PI_F / 180 * s->r);   /* share of the span per 8 px of arc */
            for (float x = 0; x + 0.0001f < lead; x += pitch) {
                float x1 = x + pitch * 3.0f / 8.0f; if (x1 > lead) x1 = lead;
                g_band(l, s, x, x1, 2, COL_HAIR, LV_OPA_COVER);
            }
        }
        if (v->frac > lead) g_band(l, s, lead, v->frac, 3, v->col, v->opa);
        for (int i = 0; i < v->n_stops; i++) {
            bool passed = v->stops[i] <= v->frac;
            g_dot(l, s, v->stops[i], passed ? 3.5f : 2.5f, passed ? v->col : COL_RIM, LV_OPA_COVER);
        }
        break;
    }
    for (int i = 0; i < v->n_marks; i++) g_dot(l, s, v->marks[i], 2.5f, COL_INK, LV_OPA_COVER);
    if (v->head != HEAD_NONE)
        g_dot(l, s, pos, 6.5f, v->head == HEAD_HANDLE ? COL_HAIR : v->head_col ? v->head_col : col, LV_OPA_COVER);
    g_end_label(l, s, v->start_label, true);
    g_end_label(l, s, v->end_label, false);
}

/* ---- the table ---- */

static const comp_t *const CROWN[] = { &sports, &busy_until, &needs_you, &date, NULL };
static const comp_t *const HEAD[]  = { &upper_label, &next_time, &free_now, NULL };
static const comp_t *const PAIR[]  = { &clock_c, &countdown, NULL };
static const comp_t *const FOOT[]  = { &link_state, &agent_status, &phone_link, &lower_label, &left_today, NULL };
static const comp_t *const BASE[]  = { &moment_title, NULL };
static const comp_t *const UPPER[] = { &leave_by, &focus_progress, NULL };
static const comp_t *const LOWER[] = { &weather, &day_progress, &weather_tap, &battery, NULL };   /* weather: default until 10:00, then tap only; battery: tap only */
static const comp_t *const EAST[]  = { &status_icons, NULL };

#define SUB_BASELINE 292
static const slot_t NOW_FACE[] = {
    { "crown",     K_CURVED,  R_CROWN,   0,  0,  0,   0,    CROWN, false },
    { "heading",   K_LABEL,   R_HEADING, 124, 0, 0,   0,    HEAD,  false },
    { "hero",      K_NUMERAL, R_HERO,    236, 0,  0,   0,    PAIR, false },   /* centres when sub is empty */
    { "sub",       K_NUMERAL, R_SUB,     SUB_BASELINE, 0, 0, 0, PAIR, false },
    { "footer",    K_LABEL,   R_FOOTER,  318, 206, 0,  0,    FOOT, false },
    { "base",      K_CURVED,  R_BASE,    0,  285, 0,   0,    BASE, false },
    { "arc_upper", K_GAUGE,   R_GAUGE,   0,  +1, 150, -60,  UPPER, false },
    { "arc_lower", K_GAUGE,   R_GAUGE,   0,  -1, 150, 240,  LOWER, true },
    { "east",      K_ICON,    R_EAST,    0,  0,  0,   0,    EAST, false },
};

/* ---- engine ---- */

static bool s_crown_needs;           /* set by now_draw: the crown shows NEEDS YOU */

/* The first visible candidate of a slot, or NULL. */
static const comp_t *first_visible(const slot_t *s, const now_ctx_t *c)
{
    for (const comp_t *const *p = s->cands; *p; p++) if ((*p)->visible(c)) return *p;
    return NULL;
}

/* A gauge slot's winner. Steps 0: the default, the first visible candidate that is not tap only (none: bare
   track). Steps k > 0 (taps on a tappable slot): k places further along the visible candidates, tap-only ones
   included, wrapping; from "no default" the first tap lands on the first candidate with data. */
static const comp_t *pick_gauge(const slot_t *s, const now_ctx_t *c, int steps)
{
    const comp_t *vis[8]; int n = 0, def = -1;
    for (const comp_t *const *p = s->cands; *p && n < 8; p++) {
        if (!(*p)->visible(c)) continue;
        if (def < 0 && !(*p)->tap_only) def = n;
        vis[n++] = *p;
    }
    if (!n) return NULL;
    if (!steps || !s->tap) return def >= 0 ? vis[def] : NULL;
    return vis[(def >= 0 ? def + steps : steps - 1) % n];
}

/* Tap on the lower arc: step to the next candidate with data; back to the default after 30 s without a tap. */
#define LOWER_HOLD_MS 30000
static int s_lower_steps;
static int64_t s_lower_deadline_ms;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static const slot_t *slot_by_role(role_t r)
{
    for (size_t i = 0; i < sizeof NOW_FACE / sizeof NOW_FACE[0]; i++) if (NOW_FACE[i].role == r) return &NOW_FACE[i];
    return NULL;
}

void now_draw(lv_layer_t *l, const now_ctx_t *in)
{
    now_ctx_t c = *in;
    const size_t n = sizeof NOW_FACE / sizeof NOW_FACE[0];
    gauge_t gauges[2]; bool have[2] = { false, false };
    comp_out_t o;

    /* 1. gauges first: their labels feed the heading and the footer */
    for (size_t i = 0, gi = 0; i < n; i++) {
        if (NOW_FACE[i].kind != K_GAUGE) continue;
        const comp_t *w = pick_gauge(&NOW_FACE[i], &c, s_lower_steps);
        if (w) {
            memset(&o, 0, sizeof o); w->fill(&c, &o);
            gauges[gi] = o.g; have[gi] = true;
            char *dst = gi == 0 ? c.upper_label : c.lower_label;
            snprintf(dst, sizeof c.upper_label, "%s", o.g.label);
            if (gi == 0) c.upper_target = o.g.target;
        }
        gi++;
    }

    /* 2. the hero / sub pair: the visible comps of the shared list, in order; a swap reverses two */
    const comp_t *pair[2] = { NULL, NULL };
    {
        const comp_t *vis[4]; int nv = 0;
        for (const comp_t *const *p = slot_by_role(R_HERO)->cands; *p && nv < 4; p++) if ((*p)->visible(&c)) vis[nv++] = *p;
        if (nv >= 2 && c.swap) { pair[0] = vis[1]; pair[1] = vis[0]; }
        else if (nv) { pair[0] = vis[0]; pair[1] = nv >= 2 ? vis[1] : NULL; }
    }

    /* 3. draw in table order */
    for (size_t i = 0, gi = 0; i < n; i++) {
        const slot_t *s = &NOW_FACE[i];
        if (s->kind == K_GAUGE) { gauge_draw(l, s, have[gi] ? &gauges[gi] : NULL); gi++; continue; }
        const comp_t *w = (s->role == R_HERO) ? pair[0] : (s->role == R_SUB) ? pair[1] : first_visible(s, &c);
        if (!w) continue;
        memset(&o, 0, sizeof o);
        if (w->fill) w->fill(&c, &o);
        switch (s->role) {
        case R_CROWN: {      /* centred between r 195.5 and the upper arc (r 150); clear of lane 2 (r 196) */
            s_crown_needs = w == &needs_you;
            int tr = dv_fit_track(o.text, sizeof o.text, f_cap_top, TRACK, 280);
            dv_caption(l, o.text, f_cap_top, o.col ? o.col : COL_INK, 217 /* 85% */, (151.25f + 195.5f) / 2 - dv_cap_height(f_cap_top) / 2.0f, true, tr);
            break; }
        case R_BASE: {
            int tr = dv_fit_track(o.text, sizeof o.text, f_cap_bot, TRACK, s->b);
            dv_caption(l, o.text, f_cap_bot, COL_INK, LV_OPA_COVER, 182, false, tr);
            break; }
        case R_HEADING:
            {   /* inside the upper arc: its inner edge (r 147) at the caps' top line, 6 px to spare */
                int top = s->a + lv_font_get_line_height(f_label) - f_label->base_line - dv_cap_height(f_label), dy = C - top;
                int max_w = 2 * (int)(sqrtf(147.0f * 147.0f - (float)(dy * dy)) - 6);
                char *tm = strstr(o.text, "TOMORROW");
                if (tm && dv_text_w(o.text, f_label, 1) > max_w) memmove(tm + 4, tm + 8, strlen(tm + 8) + 1), memcpy(tm, "TMRW", 4);
                int tr = dv_fit_track(o.text, sizeof o.text, f_label, TRACK, max_w);
                dv_text_c(l, o.text, f_label, tr, o.col ? o.col : COL_ROSE, s->a);
            }
            break;
        case R_FOOTER:       /* inside the lower arc's ends and its handle */
            dv_fit(o.text, sizeof o.text, f_label15, TRACK, s->b);
            if (o.text[0]) dv_text_c(l, o.text, f_label15, TRACK, o.col ? o.col : COL_INK2, s->a);
            break;
        case R_HERO: {
            /* caps top at 166, clear of the upper arc's ends; alone, the time sits midway between the arcs' ends
               (y 158 and 308), centred on the dial */
            int base = pair[1] ? s->a : C + dv_cap_height(f_num_l) / 2;
            int nw = dv_text_w(o.text, f_num_l, -2), cx0 = C - nw / 2;
            if (dv_icon_count()) {         /* a wide time would run into the 3 o'clock icons: shift the group left */
                int over = cx0 + nw + 10 + dv_text_w(o.unit, f_label, TRACK) - 396;
                if (over > 0) cx0 -= over;
            }
            dv_text_at(l, o.text, f_num_l, -2, COL_INK, LV_OPA_COVER, cx0, top_for_base(f_num_l, base));
            dv_text_at(l, o.unit, f_label, TRACK, COL_ROSE, LV_OPA_COVER, cx0 + nw + 10, top_for_base(f_label, base));
            break; }
        case R_SUB: {        /* fixed baseline: no coupling to the footer */
            int w1 = dv_text_w(o.text, f_num_m, -1), w2 = dv_text_w(o.unit, f_label, TRACK), x0 = C - (w1 + 8 + w2) / 2;
            dv_text_at(l, o.text, f_num_m, -1, COL_INK, LV_OPA_COVER, x0, top_for_base(f_num_m, s->a));
            dv_text_at(l, o.unit, f_label, TRACK, COL_ROSE, LV_OPA_COVER, x0 + w1 + 8, top_for_base(f_label, s->a));
            break; }
        case R_EAST:
            dv_draw_icons(l);
            break;
        default: break;
        }
    }
}

bool now_crown_needs_shown(void) { return s_crown_needs; }

bool now_lower_step(void)
{
    s_lower_steps = (s_lower_steps + 1) % 840;       /* 840 = lcm(1..8): the cycle stays consistent when the list is shorter */
    s_lower_deadline_ms = now_ms() + LOWER_HOLD_MS;
    return true;
}

/* POST /debug/ui "gauge <args>": exercise the gauges without live data. The payloads go through the same parsers as
   the bus path.
     leave <min>                     leave_by in <min> minutes (0..30 shows it; > 30 waits; < 0 has passed)
     weather <low> <high> <now> [C]  today's weather; C is the condition word; "weather none" clears it
     json <widgets object>           the whole resource, as the bus would send it ({"leave_by":{"type":"gauge","data":{..}}})
     morning on|off|auto             force the before-10:00 rule
     step                            a lower-arc tap
     clear                           drop both payloads and the overrides */
void now_gauge_debug(const char *a, char *out, size_t n)
{
    char w[12] = {0}, cond[16] = {0};
    int x = 0, y = 0, z = 0;
    a += strspn(a, " ");                                       /* the caller passes the text after "gauge", leading space included */
    if (sscanf(a, "%11s", w) < 1) { snprintf(out, n, "gauge: leave|weather|sports|json|morning|step|clear\n"); return; }
    const char *rest = a + strlen(w);
    if (!strcmp(w, "leave") && sscanf(rest, "%d", &x) == 1) {
        time_t t = dial_now() + (time_t)x * 60; struct tm g; gmtime_r(&t, &g);
        char js[96], iso[24]; strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &g);
        snprintf(js, sizeof js, "{\"to\":\"%s\"}", iso);
        cJSON *d = cJSON_Parse(js); parse_leave(d); cJSON_Delete(d);
        snprintf(out, n, "leave_by in %d min (%s)\n", x, s_leave.ok ? "set" : "bad");
    } else if (!strcmp(w, "weather") && sscanf(rest, "%d %d %d %15s", &x, &y, &z, cond) >= 3) {
        time_t t = dial_now(); struct tm l; localtime_r(&t, &l);
        char js[128]; snprintf(js, sizeof js, "{\"date\":\"%04d-%02d-%02d\",\"low\":%d,\"high\":%d,\"now\":%d,\"cond\":\"%s\"}",
                               l.tm_year + 1900, l.tm_mon + 1, l.tm_mday, x, y, z, cond);
        cJSON *d = cJSON_Parse(js); parse_weather(d); cJSON_Delete(d);
        snprintf(out, n, "weather %d to %d, now %d (%s)\n", x, y, z, s_wx.ok ? "set" : "bad");
    } else if (!strcmp(w, "sports")) {
        const char *p = rest + strspn(rest, " ");
        if (!*p) { memset(&s_sports, 0, sizeof s_sports); snprintf(out, n, "sports cleared\n"); }
        else {
            cJSON *d = cJSON_CreateObject(); cJSON_AddStringToObject(d, "text", p);
            parse_sports(d); cJSON_Delete(d);
            snprintf(out, n, "sports %s\n", s_sports.ok ? "set" : "bad");
        }
    } else if (!strcmp(w, "weather")) {
        memset(&s_wx, 0, sizeof s_wx); snprintf(out, n, "weather cleared\n");
    } else if (!strcmp(w, "json")) {
        const char *p = rest + strspn(rest, " ");
        cJSON *j = cJSON_Parse(p);
        if (!cJSON_IsObject(j)) snprintf(out, n, "bad json\n");
        else { now_gauges_set(j); snprintf(out, n, "leave_by %s, weather %s\n", s_leave.ok ? "set" : "none", s_wx.ok ? "set" : "none"); }
        cJSON_Delete(j);
    } else if (!strcmp(w, "morning")) {
        char m[8] = {0}; sscanf(rest, "%7s", m);
        s_morning_override = !strcmp(m, "on") ? 1 : !strcmp(m, "off") ? 0 : -1;
        snprintf(out, n, "morning %s\n", s_morning_override < 0 ? "auto" : s_morning_override ? "on" : "off");
    } else if (!strcmp(w, "step")) {
        now_lower_step(); snprintf(out, n, "stepped\n");
    } else if (!strcmp(w, "clear")) {
        memset(&s_leave, 0, sizeof s_leave); memset(&s_wx, 0, sizeof s_wx); memset(&s_sports, 0, sizeof s_sports); s_morning_override = -1;
        s_lower_steps = 0; snprintf(out, n, "cleared\n");
    } else snprintf(out, n, "gauge: leave <min> | weather <lo> <hi> <now> [COND] | sports [text] | json {..} | morning on|off|auto | step | clear\n");
}

bool now_lower_tick(void)
{
    if (!s_lower_steps || now_ms() < s_lower_deadline_ms) return false;
    s_lower_steps = 0;
    return true;
}

/* Hit test for the lower arc (r 150, 8 to 4 o'clock under the bottom), with a finger's worth of slack. */
bool now_lower_arc_hit(int x, int y)
{
    int dx = x - C, dy = y - C;
    float d = sqrtf((float)(dx * dx + dy * dy));
    return d >= 134 && d <= 166 && 2 * dy >= d - 16;
}
