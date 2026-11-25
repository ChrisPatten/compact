#pragma once
#include <stdbool.h>
#include <time.h>
#include "cJSON.h"
#include "lvgl.h"

typedef enum { DIAL_FACE_DAY, DIAL_FACE_REST, DIAL_FACE_CHARGE } dial_face_t;

/* Which face to draw (power states choose). Thread-safe. */
void dial_set_mode(dial_face_t mode);

/* Day Dial ("Home" face of the Plum Dusk spec). Call dial_create() once, under the display lock. */
void dial_create(void);

/* Parse the bus `calendar` payload ({events:[{id,title,start,end,all_day}]}). Thread-safe. */
void dial_set_calendar(const cJSON *payload);

/* Moments from the Surface v2 `events` resource (object id -> Event). Thread-safe. */
void dial_set_moments(const cJSON *events);

/* Gauge payloads from the Surface v2 `widgets` resource (id -> widget; ids leave_by and weather feed the Now arcs). Thread-safe. */
void dial_set_widgets(const cJSON *widgets);

/* POST /debug/ui "gauge ...": inject a test payload or step the lower arc, without live data. Writes a one-line result. */
void dial_gauge_debug(const char *args, char *out, size_t n);

/* Agent activity: state "idle"|"running"|"queued", optional status text. Thread-safe. */
void dial_set_activity(const char *state, const char *status_text);

/* Working agent jobs (from the `sessions` resource): the count ring at 9 o'clock shows while it is above 0. Thread-safe. */
void dial_set_agent_count(int n);

/* LVGL context: a tap on the Now numeral swaps the big time and the smaller countdown. False (nothing
   changes) when there is no next Moment today, so no countdown. */
bool dial_toggle_numeral(void);
bool now_crown_needs_shown(void);    /* the last Now draw showed "N NEEDS YOU" in the crown (a tap on it opens the oldest) */
void dial_refresh_face(void);        /* LVGL context: redraw the Now face (display lock held) */

/* Connection / link status word for the whisper line ("Wi-Fi up", "Paired", ...). Thread-safe. */
void dial_set_link(const char *text);

/* Call once the network is up: starts SNTP and sets the local time zone. */
void dial_net_up(void);

/* Night ground: from sunset to the next sunrise (17:30-06:30 if the sun times fail) every screen on
   the plum glass ground is true black instead, and the crescent shade is not drawn. Screens give their full-screen
   ground object dial_use_ground() rather than a bg colour, so it switches live at sunset and sunrise (the Now tick
   checks every second). A local bg colour set later (the rose flood, a black sleep screen) still wins over it. */
void dial_use_ground(lv_obj_t *o);   /* LVGL context: o's background follows the ground */
uint32_t dial_ground(void);          /* the ground colour now: plum glass by day, black at night (for fades) */
bool dial_is_night(void);            /* the night ground is showing */
void dial_set_night_override(int mode);   /* testing (/debug/ui night): 1 night, 0 day, -1 auto; any task */
int dial_night_override(void);

/* Drawing shared with other plum screens (call from an LV_EVENT_DRAW_MAIN handler). */
void dial_draw_base(lv_layer_t *l, uint32_t moon_col);   /* crescent, rim, now dot and moon */
void dial_draw_crescent(lv_layer_t *l);                  /* the daylight crescent only (Settings); nothing at night */
float dial_crescent_cx(void);                            /* centre x of the shade circle (r 260) right now */
void dial_draw_alert_base(lv_layer_t *l, time_t arrival, time_t expires);   /* rose flood: shade, drain rim, now dot */
int dial_alert_dots_gone(time_t arrival, time_t expires);                  /* rim dots drained so far (0-60) */
void dial_draw_arc_opa(lv_layer_t *l, float r, float w, float from_deg, float to_deg, uint32_t col, lv_opa_t opa);
void dial_draw_arc(lv_layer_t *l, float r, float w, float from_deg, float to_deg, uint32_t col);   /* clockwise from 12 */
void dial_draw_disc(lv_layer_t *l, float x, float y, float r, uint32_t col);
void dial_draw_disc_opa(lv_layer_t *l, float x, float y, float r, uint32_t col, lv_opa_t opa);
void dial_draw_ring(lv_layer_t *l, float x, float y, float r, float w, uint32_t col);
void dial_draw_ring_opa(lv_layer_t *l, float x, float y, float r, float w, uint32_t col, lv_opa_t opa);
void dial_draw_top_caption(lv_layer_t *l, const char *text, uint32_t col);
int dial_top_caption_bottom(const char *text, int half_w);   /* lowest y of the caption within half_w of the centre line */
void dial_draw_bottom_caption(lv_layer_t *l, const char *text, uint32_t col);
/* Hold to confirm: fill the bottom `h` px of the screen (draw callback), and move it to `h` (invalidates the change). */
void dial_draw_hold_fill(lv_layer_t *l, int h, uint32_t col, lv_opa_t opa);
void dial_hold_fill_step(lv_obj_t *o, int *cur, int h);
/* The 12-hour fixed rim (spec 4.2; main/rim12.h): 12 at the top, one hour is 30 degrees, the planet (now dot)
   circling like a clock hand's tip. The rim shows Moments from now to the leading edge, (12 h - gap) ahead; the gap
   (Settings RIM GAP, or the bus widget `dial_config`) stays empty behind the planet. */
#define DIAL_LANE1_R 204                                 /* span lane 1 (rose) */
#define DIAL_FREE_R  208                                 /* a FREE Moment's span: hairline outline between lane 1 and the rim dots */
#define DIAL_LANE2_R 196                                 /* span lane 2, an overlapping span (amber) */
float dial_clock_deg(time_t t);                          /* position of t on the 12-hour rim, 0..360 degrees */
float dial_window_deg(time_t t);                         /* t on the rim, unwrapped and clamped to planet .. edge */
bool dial_in_window(time_t t);                           /* dial_now() <= t and t is no further than the edge */
int dial_gap_min(void);                                  /* the gap behind the planet: 30, 60, 120 or 180 min */
void dial_draw_rim_marks(lv_layer_t *l);                 /* the windowed Moment marks and the midnight tick (Now, Day) */
void dial_draw_rim_dots(lv_layer_t *l);                  /* the 60 rim dots: hour and quarter dots, gap zone dimmed */
/* The clock the faces draw with: time(NULL), or a test time set by POST /debug/ui "clock HH:MM" (drawing only; SNTP,
   power, quiet hours and the bus keep the real time). dial_clock_offset() is the difference, 0 when off. */
time_t dial_now(void);
time_t dial_clock_offset(void);
void dial_clock_debug(const char *args, char *out, size_t n);     /* "clock HH:MM [+days]" | "clock off" | "clock" */
/* POST /debug/ui "testday rel|abs|off": test Moments on the Now and Day faces only (the live `events` resource is set
   aside for them until "off", which asks the bus link for a fresh reset). */
void dial_testday_debug(const char *args, char *out, size_t n);
bool dial_testday_active(void);
void dial_widgets_debug(const char *json, char *out, size_t n);   /* "widgets {..}": the bus widgets path, by hand */
void dial_gap_debug(int min, char *out, size_t n);                /* "gap <min>": what the Settings row does */
bool dial_sun_hours(time_t day, float *rise, float *set);   /* local sunrise / sunset hours that day; false if polar */
void dial_show_face(bool show);                          /* hide the face while a takeover covers it */
/* Nov 11: status icons on Now (3 o'clock, inside the rim). */
bool dial_bolt_hit(int x, int y);            /* LVGL context: is (x, y) on Now's charging bolt (only true while on USB) */
void dial_draw_bolt(lv_layer_t *l, float cx, float cy, float h, uint32_t col);   /* the charging bolt glyph, h px tall */
bool dial_moment_is_free(const cJSON *e);               /* the `free` boolean of an `events` item (absent: false); one parser for Now and Day */
time_t dial_parse_iso(const char *s);                   /* ISO 8601 (Z or offset) to epoch; 0 if bad */
void dial_caps(char *dst, size_t n, const char *src);    /* uppercase, emoji dropped */
void dial_draw_time(lv_layer_t *l, int dx, int dy, uint32_t num_col, uint32_t unit_col);   /* the clock as Now draws it with no next Moment (Charging screen) */
/* LVGL context: a tap at (x, y) on the lower arc steps it to the next candidate with data (back to the default after
   30 s). True when the tap was on the arc (handled). */
bool dial_lower_arc_tap(int x, int y);
