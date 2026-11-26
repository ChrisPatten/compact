#include "decision.h"
#include "cue.h"
#include "display.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "fonts.h"
#include "dial.h"
#include "power.h"
#include "intercom_ws.h"
#include "earlier.h"
#include "overlay.h"
#include "prefs.h"
#include "reply_logic.h"

/* Plum Dusk tokens (spec 3.1) */
#define COL_RAISE    0x4A3752
#define COL_HAIR     0x8B7098
#define COL_INK      0xF8ECE8
#define COL_INK2     0xCDB5C6
#define COL_ROSE     0xF5A99B
#define COL_ROSE_INK 0x2A1A2C
#define COL_AMBER    0xF7C95C
#define COL_PERI     0xA9B8FF
#define COL_PERI_INK 0x1D1A33

#define MAX_DEC     8
#define MAX_ASIDE   16
#define TEXT_MAX    2001        /* draft and approval detail: at most 2,000 characters */
#define HOLD_MS     800         /* spec 8.1 */
#define WAITING_MS  3000        /* spec 7.3: no ack in 3 s shows WAITING */
#define GIVE_UP_MS  15000       /* then TRY AGAIN */
#define SENT_MS     5000        /* "BACK TO NOW IN 5 S" */
#define NOTICE_MS   20000       /* spec 6.2: a notice holds the face for 20 s */
#define NOTICE_FRESH_S 600      /* a notice older than this never takes the face (reboots, long sleeps) */
#define LATE_MS     30000       /* TOO LATE waits this long untouched for SEND TO PEGGY, then closes */
#define CLOSED_MS   3000        /* DONE / EXPIRED on a Reply view that closed while open */
#define GONE_MS     5000        /* a Reply view that vanished this recently still gets its DONE / EXPIRED */
#define TEST_ID     "reply-test"

enum { W_NONE, W_NOTICE, W_DECISION, W_URGENT };
enum { SRC_EVENTS, SRC_NOTICES };

static const char *TAG = "decision";

typedef struct {
    char id[33], label[25], prompt[64];
    bool hold;
    char draft[TEXT_MAX];       /* empty when the button sends nothing on the owner's behalf */
} dact_t;

typedef struct {
    char id[66], updated[40], title[84], who[48], meta[64];
    char line[96], consequence[64], ch_old[28], ch_new[28], travel_word[12];
    int weight, src;
    bool approval, quoted;
    time_t expires, arrival, start, leave_by;   /* start / leave_by: an urgent Moment with `travel` */
    int travel_min;
    char preview[TEXT_MAX];
    char body[160];             /* the item's body / detail text as the bus sent it, for the Earlier detail view */
    int nact;
    dact_t act[3];
    char agent[24];             /* who asked ("PEGGY"), from `source` */
    reply_info_t rp;            /* E105 reply fields (notices, flag on); mode none otherwise */
    bool test;                  /* the reply-test item: nothing about it reaches the bus */
} dec_t;

typedef enum { M_NONE, M_ALERT, M_CONFIRM, M_SENT, M_LATE } mode_t_;

static bool is_leave(const dec_t *d);

static dec_t *s_dec;            /* PSRAM: MAX_DEC entries from both sources, newest first */
static int s_ndec;
static struct { char id[66], updated[40]; bool done; } s_aside[MAX_ASIDE];   /* done: answered, cleared or acked (stops the ring); else LATER */
static struct { char id[66], updated[40]; } s_seen[MAX_ASIDE];   /* notices seen on lit glass for 2 s (stops the ring) */
static int s_nseen;
static int s_naside;
static char s_agent[24] = "AGENT";

static lv_obj_t *s_take;        /* the takeover screen */
static lv_timer_t *s_tick;
static mode_t_ s_mode;
static char s_shown_id[66], s_shown_updated[40];
static char s_reopen_id[66];    /* brought back from Earlier: shows even if no longer fresh */
static int s_more;              /* other open decisions / urgent Moments waiting */
static int s_confirm_act;       /* action index shown on the confirm screen */
static char s_sent_words[96];
static char s_sent_title[16] = "SENT";   /* SENT, or DONE / EXPIRED when a Reply view closes while open */
static bool s_sent_glyph = true;         /* the check above the title */
static int64_t s_sent_ms = 5000;         /* how long it stays */
static int64_t s_sent_at;
static int64_t s_notice_lit_ms, s_last_tick;   /* how long the shown notice has been on lit glass */
static int s_drain_gone = -1;   /* urgent rim dots drained at the last draw */
static int s_notice_step = -1;  /* notice arc position at the last draw */
static lv_obj_t *s_clears;      /* "CLEARS IN N S" */
static time_t s_leave_built;    /* leave_by of a LEAVE screen already showing NOW */

/* the tap in flight (one at a time) */
static struct {
    bool active, failed;
    char key[40], object[80], action_id[33], label[25], dec_id[66];
    int64_t at;
    bool reply, test;                     /* a tap on a Reply view (E105); on the reply-test item */
    char title[REPLY_TITLE_MAX * 4 + 8];  /* the notification's title as sent, for a late reply */
    char caps[84];
} s_act;

/* ---- notification replies (E105; prefs_notif_reply) ---- */
static char s_reply_id[66];      /* the Reply view on the takeover now ("" = none) */
static char s_gone_id[66];       /* the last Reply view that gave way, and when */
static int64_t s_gone_at;
static bool s_test_late;         /* reply-test late: taps answer not_found */
/* TOO LATE (contract 3.4): the answer was refused because the question closed; SEND TO PEGGY sends it as an ordinary
   message into the followed conversation. */
static struct {
    char title[REPLY_TITLE_MAX * 4 + 8], caps[84], answer[200], cmid[40];
    int st;                      /* 0 offered, 1 sending, 2 failed */
    bool test;
    int64_t at, sent_at;
} s_late;
enum { L_OFFER, L_SENDING, L_FAILED };

/* a hold in progress */
static struct { int64_t from; int tag; } s_hold;
static int s_fill_h;             /* hold progress: px of screen filled from the bottom */
static bool s_flood;             /* the urgent rose flood is the ground (else the shared glass / night ground) */

static lv_obj_t *s_act_label[4];   /* labels of the action buttons, to show WAITING / TRY AGAIN */
static lv_obj_t *s_act_btn[4];
static int s_act_tag[4], s_nact_btn;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static bool reply_on(void) { return prefs_notif_reply(); }
/* An item shown as a Reply view: a notice with a reply mode that still awaits an answer, with the feature on. */
static bool is_reply(const dec_t *d) { return d && reply_on() && d->src == SRC_NOTICES && reply_can_answer(&d->rp); }
/* A notice-weight reply item first shows Inform with a REPLY pill (proposal 3.2 b); the pill, or any other way he opens
   it on purpose (the NEEDS YOU caption, Earlier, Agents), makes it the Reply view. s_reply_opened is that choice. */
static char s_reply_opened[66];
static int s_needs_shown;        /* the NEEDS YOU count the Now face last drew for */
static bool inform_stage(const dec_t *d) { return is_reply(d) && d->weight == W_NOTICE && strcmp(d->id, s_reply_opened); }
/* Drawn and handled as the Reply view (every reply item except one still at the Inform stage). */
static bool reply_view(const dec_t *d) { return is_reply(d) && !inform_stage(d); }
/* An E89 notification with no question attached: acking it is how he ends it. */
static bool plain_notice(const dec_t *d) { return d->src == SRC_NOTICES && !is_reply(d); }

/* ---- text ---- */

/* Keep case; drop what the fonts cannot draw (emoji and symbols from U+2190 up), like dial_caps. */
static void clean(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && o + 4 < n) {
        int len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
        uint32_t cp = len == 1 ? *p : len == 2 ? ((p[0] & 0x1F) << 6) | (p[1] & 0x3F)
                    : len == 3 ? ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F) : 0x1F000;
        for (int i = 1; i < len; i++) if (!p[i]) { len = i; cp = 0x1F000; break; }
        if (cp < 0x2190 && cp != 0x200D && !(cp >= 0xFE00 && cp <= 0xFE0F)) { memcpy(dst + o, p, len); o += len; }
        p += len;
    }
    dst[o] = 0;
}

/* "peggy-pool-4" (a cc-pool pane) -> "PEGGY": the agent's name, not its session. */
static void agent_name(char *dst, size_t n, const char *id)
{
    char tmp[48];
    snprintf(tmp, sizeof tmp, "%s", id);
    char *cut = strstr(tmp, "-pool");
    if (cut) *cut = 0;
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] >= '0' && tmp[len - 1] <= '9') tmp[--len] = 0;
    while (len > 1 && (tmp[len - 1] == '-' || tmp[len - 1] == '_')) tmp[--len] = 0;
    dial_caps(dst, n, tmp);
}

static void title_case(char *dst, size_t n, const char *upper)
{
    snprintf(dst, n, "%s", upper);
    for (size_t i = 1; dst[i]; i++) if (dst[i] >= 'A' && dst[i] <= 'Z') dst[i] += 32;
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void fmt_time(char *b, size_t n, time_t t)
{
    struct tm lt; localtime_r(&t, &lt);
    int h = lt.tm_hour % 12; if (!h) h = 12;
    snprintf(b, n, "%d:%02d %s", h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
}

/* ---- weights (spec 6.2) ---- */

/* The weight an item has (or had), whatever its state. */
static int raw_weight(const cJSON *e, bool notice)
{
    const char *lv = str(e, "level");
    if (notice) {                                    /* E89 notice: level info / attention / urgent */
        if (lv && !strcmp(lv, "urgent")) return W_URGENT;
        if (lv && !strcmp(lv, "attention")) return W_DECISION;
        return W_NOTICE;
    }
    const char *kind = str(e, "kind"), *w = str(e, "weight");
    if (kind && !strcmp(kind, "approval")) return W_DECISION;
    if (w) return !strcmp(w, "urgent") ? W_URGENT : !strcmp(w, "decision") ? W_DECISION : !strcmp(w, "notice") ? W_NOTICE : W_NONE;
    if (lv && !strcmp(lv, "urgent")) return W_URGENT;
    if (lv && !strcmp(lv, "attention")) return W_DECISION;
    return W_NONE;
}

/* E105: a reply notification that someone acknowledged ("seen") still awaits an answer (contract section 2). */
static bool ack_but_awaiting(const cJSON *e)
{
    const cJSON *rp = cJSON_GetObjectItem(e, "reply");
    const char *rs = str(e, "reply_state"), *mode = rp ? str(rp, "mode") : NULL;
    return rs && !strcmp(rs, "awaiting") && mode && strcmp(mode, "none");
}

int decision_weight(const cJSON *e, bool notice)
{
    const char *st = str(e, "state");
    if (st && strcmp(st, notice ? "active" : "open")) {
        if (!(notice && reply_on() && !strcmp(st, "acknowledged") && ack_but_awaiting(e))) return W_NONE;
    }
    return raw_weight(e, notice);
}

static void parse_one(dec_t *d, const cJSON *e, const char *id, int src, int weight)
{
    memset(d, 0, sizeof *d);
    snprintf(d->id, sizeof d->id, "%s", id);
    snprintf(d->updated, sizeof d->updated, "%s", str(e, "updated_at") ? str(e, "updated_at") : "");
    d->src = src;
    d->weight = weight;
    if (src == SRC_NOTICES && reply_on()) reply_parse(e, &d->rp);
    const char *kind = str(e, "kind");
    d->approval = src == SRC_EVENTS && kind && !strcmp(kind, "approval");
    dial_caps(d->title, sizeof d->title, str(e, "title") ? str(e, "title") : (src == SRC_NOTICES ? "Notice" : ""));
    d->expires = str(e, "expires_at") ? dial_parse_iso(str(e, "expires_at")) : 0;
    const char *ca = str(e, "created_at") ? str(e, "created_at") : str(e, "updated_at");
    d->arrival = ca ? dial_parse_iso(ca) : time(NULL);

    const char *srcname = str(e, "source");
    char who[24];
    if (srcname && !strncmp(srcname, "agent:", 6)) agent_name(who, sizeof who, srcname + 6);
    else snprintf(who, sizeof who, "%s", s_agent);
    snprintf(d->agent, sizeof d->agent, "%s", who);

    const cJSON *acts = cJSON_GetObjectItem(e, "actions"), *a;
    int idx = 0;
    cJSON_ArrayForEach(a, acts) {
        if (d->nact >= 3) break;
        dact_t *x = &d->act[d->nact];
        const char *aid = str(a, "id"), *lab = str(a, "label"), *k = str(a, "kind");
        char ibuf[12];
        if (src == SRC_NOTICES) { snprintf(ibuf, sizeof ibuf, "%d", idx); aid = ibuf; }   /* notice actions go by index */
        idx++;
        if (!aid || !lab) continue;
        snprintf(x->id, sizeof x->id, "%s", aid);
        dial_caps(x->label, sizeof x->label, lab);
        const cJSON *cf = cJSON_GetObjectItem(a, "confirm");
        if (cf) {
            if (str(cf, "draft")) snprintf(x->draft, sizeof x->draft, "%s", str(cf, "draft"));
            if (str(cf, "prompt")) dial_caps(x->prompt, sizeof x->prompt, str(cf, "prompt"));
        }
        x->hold = k ? !strcmp(k, "hold") : x->draft[0] != 0;   /* before E100: a draft means hold */
        d->nact++;
    }

    const cJSON *det = cJSON_GetObjectItem(e, "detail");
    const char *body = str(e, "body") ? str(e, "body") : det ? str(det, "description") : NULL;
    const char *place = det ? str(det, "location") : NULL;
    if (d->rp.mode != RM_NONE) clean(d->preview, sizeof d->preview, body ? body : "");   /* the Reply view's body line */
    clean(d->body, sizeof d->body, body ? body : place ? place : "");

    if (weight == W_URGENT) {
        const cJSON *tr = cJSON_GetObjectItem(e, "travel"), *mins = tr ? cJSON_GetObjectItem(tr, "minutes") : NULL;
        d->start = str(e, "at") ? dial_parse_iso(str(e, "at")) : 0;
        if (cJSON_IsNumber(mins) && mins->valueint > 0) {          /* spec 7.1 "Needs me now": LEAVE / NOW */
            d->travel_min = mins->valueint;
            const char *mode = str(tr, "mode");
            snprintf(d->travel_word, sizeof d->travel_word, "%s", !mode ? "TRAVEL" : !strcmp(mode, "walk") ? "WALK" :
                     !strcmp(mode, "drive") ? "DRIVE" : !strcmp(mode, "bike") ? "BIKE" : !strcmp(mode, "train") ? "TRAIN" :
                     !strcmp(mode, "transit") ? "TRANSIT" : "TRAVEL");
            d->leave_by = str(tr, "leave_by") ? dial_parse_iso(str(tr, "leave_by")) : d->start ? d->start - d->travel_min * 60 : 0;
        }
        if (str(e, "consequence")) dial_caps(d->consequence, sizeof d->consequence, str(e, "consequence"));
        const cJSON *ch = cJSON_GetObjectItem(e, "changed");
        if (ch && str(ch, "old") && str(ch, "new")) {
            clean(d->ch_old, sizeof d->ch_old, str(ch, "old"));
            clean(d->ch_new, sizeof d->ch_new, str(ch, "new"));
            if (body) dial_caps(d->line, sizeof d->line, body);          /* "16 MIN LATE · PROVIDENCE LINE" */
        } else {
            clean(d->preview, sizeof d->preview, body ? body : "");
            if (place) dial_caps(d->line, sizeof d->line, place);
        }
        for (int i = 0; i < d->nact; i++) if (d->act[i].draft[0]) { d->quoted = true; break; }
        return;
    }
    if (weight == W_NOTICE) {                       /* "FRONT PORCH · 2:14 PM" */
        char t[16], tmp[160];
        fmt_time(t, sizeof t, d->arrival);
        const char *what = place ? place : body;
        if (what) snprintf(tmp, sizeof tmp, "%.120s \xC2\xB7 %s", what, t); else snprintf(tmp, sizeof tmp, "%s", t);
        dial_caps(d->line, sizeof d->line, tmp);
        return;
    }
    if (d->approval) {
        snprintf(d->who, sizeof d->who, "NEEDS YOU \xC2\xB7 APPROVAL");
        snprintf(d->meta, sizeof d->meta, "ASKED BY %s", s_agent);
        const cJSON *ap = cJSON_GetObjectItem(e, "approval");
        const char *detail = ap ? str(ap, "detail") : NULL;
        clean(d->preview, sizeof d->preview, detail ? detail : body ? body : "");
        return;
    }
    snprintf(d->who, sizeof d->who, "NEEDS YOU \xC2\xB7 %s", who);
    for (int i = 0; i < d->nact; i++)
        if (d->act[i].draft[0]) {
            clean(d->preview, sizeof d->preview, d->act[i].draft);
            d->quoted = true;
            snprintf(d->meta, sizeof d->meta, "DRAFT BY %s", who);
            return;
        }
    clean(d->preview, sizeof d->preview, body ? body : "");
}

static bool aside(const dec_t *d)
{
    for (int i = 0; i < s_naside; i++)
        if (!strcmp(s_aside[i].id, d->id) && !strcmp(s_aside[i].updated, d->updated)) return true;
    return false;
}

static void set_aside(const char *id, const char *updated, bool done)
{
    for (int i = 0; i < s_naside; i++) if (!strcmp(s_aside[i].id, id)) { snprintf(s_aside[i].updated, sizeof s_aside[i].updated, "%s", updated); s_aside[i].done = done; return; }
    if (s_naside == MAX_ASIDE) { memmove(&s_aside[0], &s_aside[1], sizeof s_aside[0] * (MAX_ASIDE - 1)); s_naside--; }
    snprintf(s_aside[s_naside].id, sizeof s_aside[0].id, "%s", id);
    snprintf(s_aside[s_naside].updated, sizeof s_aside[0].updated, "%s", updated);
    s_aside[s_naside].done = done;
    s_naside++;
}

static bool glass_lit(void)
{
    power_state_t p = power_state();
    return p == PWR_AWAKE || p == PWR_DIM;
}

static bool aside_done(const dec_t *d)
{
    for (int i = 0; i < s_naside; i++)
        if (!strcmp(s_aside[i].id, d->id) && !strcmp(s_aside[i].updated, d->updated)) return s_aside[i].done;
    return false;
}

static bool seen(const dec_t *d)
{
    for (int i = 0; i < s_nseen; i++)
        if (!strcmp(s_seen[i].id, d->id) && !strcmp(s_seen[i].updated, d->updated)) return true;
    return false;
}

static void mark_seen(const dec_t *d)
{
    if (seen(d)) return;
    if (s_nseen == MAX_ASIDE) { memmove(&s_seen[0], &s_seen[1], sizeof s_seen[0] * (MAX_ASIDE - 1)); s_nseen--; }
    snprintf(s_seen[s_nseen].id, sizeof s_seen[0].id, "%s", d->id);
    snprintf(s_seen[s_nseen].updated, sizeof s_seen[0].updated, "%s", d->updated);
    s_nseen++;
}

/* The breathing ring (cue.c): the highest weight among open items that are not done, expired or, for a notice, seen
   for 2 s or older than 10 minutes. LATER and swipe-up keep a decision in the count: that is the point of the cue.
   LVGL context. level 0 = nothing; count = items at that top weight. */
#define CUE_NOTICE_S 600
void decision_cue_query(int *level, int *count)
{
    time_t now = time(NULL);
    int top = 0, n = 0;
    for (int i = 0; s_dec && i < s_ndec; i++) {
        const dec_t *d = &s_dec[i];
        if (aside_done(d) || (d->expires && d->expires <= now)) continue;
        if (d->weight == W_NOTICE && !is_reply(d) && (seen(d) || now - d->arrival > CUE_NOTICE_S)) continue;   /* a question waits */
        if (d->weight > top) { top = d->weight; n = 1; } else if (d->weight == top) n++;
    }
    *level = top;
    *count = n;
}

/* The newest notice the ring is breathing for, or NULL. LVGL context. */
static dec_t *pending_notice(void)
{
    time_t now = time(NULL);
    dec_t *best = NULL;
    for (int i = 0; s_dec && i < s_ndec; i++) {
        dec_t *d = &s_dec[i];
        if (d->weight != W_NOTICE || aside_done(d) || (d->expires && d->expires <= now)) continue;
        if (!is_reply(d) && (seen(d) || now - d->arrival > CUE_NOTICE_S)) continue;
        if (!best || d->arrival > best->arrival) best = d;
    }
    return best;
}

/* What owns the glass (spec 8.2): the highest weight, newest first, not set aside and not expired.
   A notice only while the glass is lit (it never wakes from rest), and only while it is fresh. */
static dec_t *current(int *more)
{
    time_t now = time(NULL);
    dec_t *top = NULL;
    for (int i = 0; i < s_ndec; i++) {
        dec_t *d = &s_dec[i];
        if (aside(d) || (d->expires && d->expires <= now)) continue;
        /* A notice is a background cue (the ring): its card opens only when asked (swipe up on Now), or is already up. */
        if (d->weight == W_NOTICE && strcmp(d->id, s_shown_id) && strcmp(d->id, s_reopen_id)) continue;
        if (!top || d->weight > top->weight) top = d;
    }
    int n = 0;
    for (int i = 0; top && i < s_ndec; i++) {
        dec_t *d = &s_dec[i];
        if (d != top && d->weight >= W_DECISION && !aside(d) && !(d->expires && d->expires <= now)) n++;
    }
    if (more) *more = n;
    return top;
}

static dec_t *find(const char *id)
{
    for (int i = 0; i < s_ndec; i++) if (!strcmp(s_dec[i].id, id)) return &s_dec[i];
    return NULL;
}

/* Set the shown Moment aside and file it in Earlier: "open" (LATER, DISMISS, PWR) or how it ended here. */
static void put_aside(dec_t *d, const char *state)
{
    set_aside(d->id, d->updated, strcmp(state, "open") != 0);
    if (!strcmp(d->id, s_reopen_id)) s_reopen_id[0] = 0;
    earlier_note(d->id, d->title, d->weight, state, time(NULL), d->src, d->expires);
    earlier_note_text(d->id, d->body, NULL);
}

/* ---- drawing (rim, glyphs) ---- */

/* A 24 px-grid icon path (spec 5.10) as straight strokes, scaled and placed at (ox, oy). */
static void strokes(lv_layer_t *l, const float *xy, int n, bool closed, float ox, float oy, float s, float w, uint32_t col)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col); d.width = w; d.round_start = 1; d.round_end = 1;
    for (int i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        int j = (i + 1) % n;
        d.p1 = (lv_point_precise_t){ ox + xy[2 * i] * s, oy + xy[2 * i + 1] * s };
        d.p2 = (lv_point_precise_t){ ox + xy[2 * j] * s, oy + xy[2 * j + 1] * s };
        lv_draw_line(l, &d);
    }
}

static void notice_icon(lv_layer_t *l, const dec_t *d)
{
    const float s = 1.4f, ox = 233 - 12 * s, oy = 128 - 12 * s;
    if (strstr(d->title, "PACKAGE") || strstr(d->title, "DELIVER") || strstr(d->title, "PARCEL")) {
        static const float box[] = { 3.5f, 7.5f, 12, 3, 20.5f, 7.5f, 20.5f, 16.5f, 12, 21, 3.5f, 16.5f };
        static const float lid[] = { 3.5f, 7.5f, 12, 12, 20.5f, 7.5f };
        static const float mid[] = { 12, 12, 12, 21 };
        strokes(l, box, 6, true, ox, oy, s, 2.4f, COL_INK);
        strokes(l, lid, 3, false, ox, oy, s, 2.4f, COL_INK);
        strokes(l, mid, 2, false, ox, oy, s, 2.4f, COL_INK);
    } else {                                                    /* the agent glyph */
        static const float star[] = { 12, 3, 13.8f, 9.2f, 20, 11, 13.8f, 12.8f, 12, 19, 10.2f, 12.8f, 4, 11, 10.2f, 9.2f };
        strokes(l, star, 8, true, ox, oy, s, 2.4f, COL_INK);
    }
}

static float notice_left(void)
{
    float f = 1.0f - (float)s_notice_lit_ms / NOTICE_MS;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void take_draw_screen(lv_layer_t *l)
{
    if (s_mode == M_LATE) { dial_draw_base(l, COL_AMBER); return; }
    if (s_mode == M_SENT && !s_sent_glyph) { dial_draw_base(l, COL_ROSE); return; }
    if (s_mode == M_SENT) {
        dial_draw_base(l, COL_ROSE);
        dial_draw_ring(l, 233, 146, 45.75f, 3.5f, COL_ROSE);
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
        d.color = lv_color_hex(COL_ROSE); d.width = 4; d.round_start = 1; d.round_end = 1;
        d.p1 = (lv_point_precise_t){ 214, 147 }; d.p2 = (lv_point_precise_t){ 228, 161 }; lv_draw_line(l, &d);
        d.p1 = (lv_point_precise_t){ 228, 161 }; d.p2 = (lv_point_precise_t){ 254, 132 }; lv_draw_line(l, &d);
        return;
    }
    dec_t *d = find(s_shown_id);
    if (s_mode == M_ALERT && d && d->weight == W_URGENT && !reply_view(d)) {
        dial_draw_alert_base(l, d->arrival, d->expires);
        s_drain_gone = dial_alert_dots_gone(d->arrival, d->expires);
        if (is_leave(d)) {
            char top[48], t[16];
            fmt_time(t, sizeof t, time(NULL));
            if (s_more > 0) snprintf(top, sizeof top, "%d MORE", s_more);
            else snprintf(top, sizeof top, "TIME TO GO \xC2\xB7 %s", t);
            dial_draw_top_caption(l, top, COL_ROSE_INK);
            char bot[112];
            if (d->start) { fmt_time(t, sizeof t, d->start); char *sp = strchr(t, ' '); if (sp) *sp = 0;
                            snprintf(bot, sizeof bot, "%.80s AT %s", d->title, t); }
            else snprintf(bot, sizeof bot, "%s", d->title);
            dial_draw_bottom_caption(l, bot, COL_ROSE_INK);
            return;
        }
        static const float tri[] = { 12, 3, 22, 20, 2, 20 };       /* the urgent icon at (219, 58), x 1.17 */
        static const float bar[] = { 12, 10, 12, 15 };
        strokes(l, tri, 3, true, 219, 58, 1.17f, 2.6f, COL_ROSE_INK);
        strokes(l, bar, 2, false, 219, 58, 1.17f, 2.6f, COL_ROSE_INK);
        dial_draw_disc(l, 219 + 12 * 1.17f, 58 + 17.5f * 1.17f, 1.5f, COL_ROSE_INK);
        return;
    }
    if (s_mode == M_ALERT && d && d->weight == W_NOTICE && !reply_view(d)) {
        dial_draw_base(l, COL_ROSE);
        dial_draw_arc(l, 150, 2.5f, -60, 60, COL_HAIR);            /* the 20 s drain, 10 to 2 o'clock */
        float left = notice_left();
        if (left > 0.005f) dial_draw_arc(l, 150, 3, -60, -60 + 120 * left, COL_INK);
        notice_icon(l, d);
        return;
    }
    dial_draw_base(l, COL_PERI);
    if (s_mode == M_CONFIRM) {
        dial_draw_top_caption(l, d && d->approval ? "APPROVE EXACTLY THIS" : "SENDS EXACTLY THIS", COL_ROSE);
        return;
    }
    /* waiting glyph: ring + dot (spec 5.9) */
    dial_draw_ring(l, 233, 78, 13.25f, 2.5f, COL_PERI);
    dial_draw_disc(l, 233, 78, 3.5f, COL_PERI);
    /* how long it stays open: a peri arc on lane 1 from the planet to the expiry, clamped to the rim window's edge
       (12-hour rim, proposal 1.2). The drawing clock's test offset moves the expiry with it. */
    time_t now = time(NULL);
    if (d && d->expires > now)
        dial_draw_arc(l, DIAL_LANE1_R, 4, dial_clock_deg(dial_now()), dial_window_deg(d->expires + dial_clock_offset()), COL_PERI);
}

static void take_draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    take_draw_screen(l);
    if (s_flood) dial_draw_hold_fill(l, s_fill_h, COL_ROSE_INK, 56);   /* 22% on the flood */
    else dial_draw_hold_fill(l, s_fill_h, COL_ROSE, 90);                           /* 35% on plum */
}

/* ---- widgets ---- */

static lv_obj_t *text(const char *t, const lv_font_t *f, uint32_t col, int x, int y, int w, int line_h, int lines, int space)
{
    lv_obj_t *l = lv_label_create(s_take);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(l, space, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (line_h) lv_obj_set_style_text_line_space(l, line_h - lv_font_get_line_height(f), 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, (line_h ? line_h : lv_font_get_line_height(f)) * lines);
    lv_obj_set_pos(l, x, y);
    return l;
}

enum { T_ACT = 0, T_LATER = 8, T_REVIEW, T_CANCEL, T_CONFIRM, T_DENY, T_GOTIT, T_SKIP, T_LATE_SEND, T_LATE_DROP, T_REPLY };

/* Pill styles. Plum: 0 filled peri (primary), 1 ghost (hair border, ink), 2 hold (rose border, rose wipe).
   On the rose flood (spec 5.7): 3 filled rose-ink with rose text, 4 ghost with a rose-ink border, 5 hold. */
enum { S_PERI, S_GHOST, S_HOLD, S_FLOOD, S_FLOOD_GHOST, S_FLOOD_HOLD };

static void btn_cb(lv_event_t *e);

static int label_w(const char *t, const lv_font_t *f, int space)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* x < 0 centres the pill on the dial. The pill grows to fit its label with 18 px either side. */
static lv_obj_t *button(const char *label, int x, int y, int w, int h, int style, int tag, const lv_font_t *f)
{
    int need = label_w(label, f, 2) + 36;
    if (need > w) { if (x >= 0) x -= (need - w) / 2; w = need; }
    if (x < 0) x = 233 - w / 2;
    bool hold = style == S_HOLD || style == S_FLOOD_HOLD, flood = style >= S_FLOOD;
    lv_obj_t *b = lv_obj_create(s_take);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 8);
    uint32_t ink = flood ? COL_ROSE_INK : COL_INK;
    if (style == S_PERI || style == S_FLOOD) {
        lv_obj_set_style_bg_color(b, lv_color_hex(style == S_PERI ? COL_PERI : COL_ROSE_INK), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        ink = style == S_PERI ? COL_PERI_INK : COL_ROSE;
    } else {
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(flood ? COL_ROSE_INK : style == S_HOLD ? COL_ROSE : COL_HAIR), 0);
        lv_obj_set_style_border_post(b, true, 0);
    }
    if (!hold) {                /* pressed: invert to ink within 100 ms (spec 5.6) */
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_INK), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    }                           /* a hold button stays still: the screen fills behind it */
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
    if (!hold) lv_obj_set_style_text_color(l, lv_color_hex(COL_ROSE_INK), LV_STATE_PRESSED);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(l, w - 12);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_ALL, (void *)(intptr_t)tag);
    if (s_nact_btn < 4) { s_act_btn[s_nact_btn] = b; s_act_label[s_nact_btn] = l; s_act_tag[s_nact_btn] = tag; s_nact_btn++; }
    return b;
}

/* Spec 7.3 actions at y 312: one = pill 134 at x 166; two = filled 134 at x 97 + ghost 128 at x 241;
   three = one capsule 296 x 60. On the rose flood the same two pills in rose-ink (spec 5.7). */
static void action_row(const char **labels, const int *tags, const bool *holds, int n, bool flood)
{
    int fill = flood ? S_FLOOD : S_PERI, ghost = flood ? S_FLOOD_GHOST : S_GHOST, hold = flood ? S_FLOOD_HOLD : S_HOLD;
    int h = flood ? 58 : 56;
    if (n == 1) { button(labels[0], 166, 312, 134, h, holds[0] ? hold : tags[0] == T_LATER ? ghost : fill, tags[0], f_whisper); return; }
    if (n == 2) {
        button(labels[0], 97, 312, 134, h, holds[0] ? hold : fill, tags[0], f_whisper);
        button(labels[1], 241, 312, 128, h, holds[1] ? hold : ghost, tags[1], f_whisper);
        return;
    }
    /* one capsule 296 x 60 at x 85: hair border, 2 px dividers, first segment filled; widths fit labels */
    lv_obj_t *cap = lv_obj_create(s_take);
    lv_obj_remove_style_all(cap);
    lv_obj_set_pos(cap, 85, 312);
    lv_obj_set_size(cap, 296, 60);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(cap, true, 0);
    lv_obj_set_style_border_width(cap, 2, 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(flood ? COL_ROSE_INK : COL_HAIR), 0);
    lv_obj_set_style_border_post(cap, true, 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    int lw[3], sum = 0;
    for (int i = 0; i < 3; i++) { lw[i] = label_w(labels[i], f_meta13, 1) + 24; sum += lw[i]; }
    int x = 0;
    for (int i = 0; i < 3; i++) {
        int w = i < 2 ? lw[i] * 292 / sum : 296 - x;
        lv_obj_t *seg = lv_obj_create(cap);
        lv_obj_remove_style_all(seg);
        lv_obj_set_pos(seg, x, 0);
        lv_obj_set_size(seg, w, 60);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        uint32_t ink = i == 0 && !holds[i] ? (flood ? COL_ROSE : COL_PERI_INK) : (flood ? COL_ROSE_INK : COL_INK);
        if (i == 0 && !holds[i]) {
            lv_obj_set_style_bg_color(seg, lv_color_hex(flood ? COL_ROSE_INK : COL_PERI), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        }
        if (!holds[i]) {
            lv_obj_set_style_bg_color(seg, lv_color_hex(COL_INK), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_STATE_PRESSED);
        }
        if (i > 0) {                                            /* divider */
            lv_obj_set_style_border_side(seg, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(seg, 2, 0);
            lv_obj_set_style_border_color(seg, lv_color_hex(flood ? COL_ROSE_INK : COL_HAIR), 0);
        }
        lv_obj_t *l = lv_label_create(seg);
        lv_label_set_text(l, labels[i]);
        lv_obj_set_style_text_font(l, f_meta13, 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_ROSE_INK), LV_STATE_PRESSED);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_width(l, w - 8);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_center(l);
        lv_obj_add_event_cb(seg, btn_cb, LV_EVENT_ALL, (void *)(intptr_t)tags[i]);
        if (s_nact_btn < 4) { s_act_btn[s_nact_btn] = seg; s_act_label[s_nact_btn] = l; s_act_tag[s_nact_btn] = tags[i]; s_nact_btn++; }
        x += w;
    }
}

/* ---- building the screens ---- */

static lv_obj_t *s_box;         /* the confirm screen's scrollable text box (NULL otherwise) */
static bool s_box_scrolled;     /* the current press on it scrolled it */

static void box_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) s_box_scrolled = false;
    else if (c == LV_EVENT_SCROLL_BEGIN) s_box_scrolled = true;
}

static void clear_take(void)
{
    lv_obj_clean(s_take);
    s_box = NULL;
    s_nact_btn = 0;
    s_clears = NULL;
    memset(&s_hold, 0, sizeof s_hold);
    s_fill_h = 0;
}

/* The takeover's ground: the rose flood, or the shared ground (plum glass by day, black at night). */
static void set_flood(bool flood)
{
    s_flood = flood;
    if (flood) lv_obj_set_style_bg_color(s_take, lv_color_hex(COL_ROSE), 0);
    else dial_use_ground(s_take);
}

static void more_line(uint32_t col)
{
    if (s_more <= 0) return;                                   /* spec 7.3: the others wait */
    char t[24]; snprintf(t, sizeof t, "%d MORE WAITING", s_more);
    text(t, f_meta13, col, 98, 384, 270, 0, 1, 2);
}

static bool has_draft(const dec_t *d)
{
    for (int i = 0; i < d->nact; i++) if (d->act[i].draft[0]) return true;
    return false;
}

static void build_decision(const dec_t *d)
{
    clear_take();
    set_flood(false);
    text(d->who, f_whisper, COL_PERI, 98, 100, 270, 0, 1, 2);
    text(d->title, f_head21, COL_INK, 83, 124, 300, 26, 2, 1);
    if (d->preview[0]) {
        char *q = heap_caps_malloc(TEXT_MAX + 8, MALLOC_CAP_SPIRAM);
        if (q) {
            snprintf(q, TEXT_MAX + 8, d->quoted ? "\xE2\x80\x9C%s\xE2\x80\x9D" : "%s", d->preview);
            text(q, f_preview, COL_INK, 83, 186, 300, 25, 3, 0);
            heap_caps_free(q);
        }
    }
    if (d->meta[0]) text(d->meta, f_meta13, COL_INK2, 60, 268, 346, 0, 1, 1);

    const char *labels[3]; int tags[3]; bool holds[3]; int n = 0;
    if (d->approval || has_draft(d)) {
        labels[n] = "REVIEW"; tags[n] = T_REVIEW; holds[n++] = false;
    } else {
        for (int i = 0; i < d->nact && n < 2; i++) { labels[n] = d->act[i].label; tags[n] = T_ACT + i; holds[n++] = d->act[i].hold; }
    }
    /* A notification with nothing to answer must be endable from here: GOT IT acks it, LATER only sets it aside. */
    if (plain_notice(d)) { labels[n] = "GOT IT"; tags[n] = T_GOTIT; holds[n++] = false; }
    if (n < 3) { labels[n] = "LATER"; tags[n] = T_LATER; holds[n++] = false; }
    action_row(labels, tags, holds, n, false);
    more_line(COL_INK2);
}

/* Spec 7.3 urgent: the rose flood. With a `changed` time: title, old time struck through, the new time
   large, the body as one line, the consequence. Without: the title as the headline and the body under it. */
static void build_urgent(const dec_t *d)
{
    clear_take();
    set_flood(true);
    if (d->ch_new[0]) {
        text(d->title, f_lab17, COL_ROSE_INK, 48, 96, 370, 0, 1, 3);
        lv_obj_t *o = text(d->ch_old, f_num30, COL_ROSE_INK, 48, 122, 370, 0, 1, 0);
        lv_obj_set_style_text_decor(o, LV_TEXT_DECOR_STRIKETHROUGH, 0);
        lv_obj_set_style_text_opa(o, 199, 0);                  /* 78% */
        if (label_w(d->ch_new, f_num92, -2) <= 340) text(d->ch_new, f_num92, COL_ROSE_INK, 33, 152, 400, 0, 1, -2);
        else text(d->ch_new, f_head21, COL_ROSE_INK, 63, 190, 340, 26, 2, 1);
    } else {
        text(d->title, f_head21, COL_ROSE_INK, 63, 104, 340, 26, 2, 1);
        if (d->preview[0]) text(d->preview, f_preview, COL_ROSE_INK, 73, 164, 320, 25, 3, 0);
    }
    if (d->line[0]) text(d->line, f_whisper, COL_ROSE_INK, 48, 252, 370, 0, 1, 1);
    if (d->consequence[0]) text(d->consequence, f_bold14, COL_ROSE_INK, 48, 274, 370, 0, 1, 1);

    const char *labels[3]; int tags[3]; bool holds[3]; int n = 0;
    if (d->approval || has_draft(d)) {
        labels[n] = "REVIEW"; tags[n] = T_REVIEW; holds[n++] = false;
    } else {
        for (int i = 0; i < d->nact && n < 2; i++) { labels[n] = d->act[i].label; tags[n] = T_ACT + i; holds[n++] = d->act[i].hold; }
    }
    if (plain_notice(d)) { labels[n] = "GOT IT"; tags[n] = T_GOTIT; holds[n++] = false; }
    else if (n < 2) { labels[n] = "DISMISS"; tags[n] = T_LATER; holds[n++] = false; }
    action_row(labels, tags, holds, n, true);
    more_line(COL_ROSE_INK);
}

/* A one-line label whose capitals are centred on y = cy. */
static void caps_line(const char *t, const lv_font_t *f, uint32_t col, int cy, int space)
{
    lv_font_glyph_dsc_t g;
    int cap = lv_font_get_glyph_dsc(f, &g, 'H', 0) ? g.box_h + g.ofs_y : (int)(f->line_height * 0.7f);
    int asc = lv_font_get_line_height(f) - f->base_line;          /* label top to baseline */
    text(t, f, col, 33, cy + cap / 2 - asc, 400, 0, 1, space);
}

/* The urgent Moment is a "time to go" (travel, no changed time): spec 7.1 "Needs me now". */
static bool is_leave(const dec_t *d) { return d->weight == W_URGENT && d->travel_min > 0 && !d->ch_new[0]; }
static bool leave_now(const dec_t *d) { return !d->leave_by || d->leave_by <= time(NULL) + 60; }

/* LEAVE / NOW in Display (or LEAVE / 2:20 until then), "24 MIN DRIVE", two pills at y 290;
   the curved captions ("TIME TO GO · 3:06 PM", "DESIGN REVIEW AT 3:30") are drawn in take_draw. */
static void build_leave(const dec_t *d)
{
    clear_take();
    set_flood(true);
    char v[16] = "NOW";
    if (!leave_now(d)) {
        struct tm lt; localtime_r(&d->leave_by, &lt);
        int h = lt.tm_hour % 12; if (!h) h = 12;
        snprintf(v, sizeof v, "%d:%02d", h, lt.tm_min);
    }
    /* Display 72: one label per line, caps centred at y 140 and 212 (the board's 96 + 76 / 2, moved
       down 6 px so the curved caption's ends clear LEAVE) */
    caps_line("LEAVE", f_num72, COL_ROSE_INK, 140, 1);
    caps_line(v, f_num72, COL_ROSE_INK, 212, 1);
    char sub[32]; snprintf(sub, sizeof sub, "%d MIN %s", d->travel_min, d->travel_word);
    text(sub, f_label, COL_ROSE_INK, 48, 258, 370, 0, 1, 3);
    int n = 0;
    for (int i = 0; i < d->nact && n < 2; i++, n++)
        button(d->act[i].label, n ? 239 : 103, 290, 124, 58, d->act[i].hold ? S_FLOOD_HOLD : n ? S_FLOOD_GHOST : S_FLOOD, T_ACT + i, f_whisper);
    if (n < 2) button("DISMISS", n ? 239 : -1, 290, 124, 58, n ? S_FLOOD_GHOST : S_FLOOD, T_LATER, f_whisper);
}

/* Spec 7.3 Inform: plum, no accent. Icon, title, "where · when", the 20 s drain, GOT IT. */
static void build_notice(const dec_t *d)
{
    clear_take();
    set_flood(false);
    text(d->title, f_lab17, COL_INK, 48, 168, 370, 0, 1, 3);
    if (d->line[0]) text(d->line, f_whisper, COL_INK2, 63, 198, 340, 0, 1, 2);
    s_clears = text("CLEARS IN 20 S", f_whisper, COL_INK2, 63, 262, 340, 0, 1, 3);
    if (reply_pill_visible(reply_on(), &d->rp, d->weight == W_NOTICE)) {   /* choices live on the Reply view */
        button("REPLY", 97, 300, 134, 58, S_PERI, T_REPLY, f_whisper);
        button("GOT IT", 241, 300, 128, 58, S_GHOST, T_GOTIT, f_whisper);
    } else if (d->nact > 0) {
        button(d->act[0].label, 97, 300, 134, 58, d->act[0].hold ? S_HOLD : S_GHOST, T_ACT, f_whisper);
        button("GOT IT", 241, 300, 128, 58, S_GHOST, T_GOTIT, f_whisper);
    } else {
        button("GOT IT", 151, 300, 164, 58, S_GHOST, T_GOTIT, f_whisper);
    }
}

static void build_confirm(const dec_t *d, int ai)
{
    clear_take();
    set_flood(false);
    const dact_t *a = &d->act[ai];
    char to[96];
    if (d->approval) snprintf(to, sizeof to, "%s", d->title);
    else if (a->prompt[0]) snprintf(to, sizeof to, "%s", a->prompt);
    else snprintf(to, sizeof to, "SENT AS %s", s_agent);
    text(to, f_whisper, COL_INK2, 98, 102, 270, 0, 1, 2);

    /* the exact text, scrollable when long */
    lv_obj_t *box = lv_obj_create(s_take);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 83, 126);
    lv_obj_set_size(box, 300, 108);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    s_box = box;
    s_box_scrolled = false;
    lv_obj_add_event_cb(box, box_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, d->approval ? d->preview : a->draft);
    lv_obj_set_width(l, 300);
    lv_obj_set_style_text_font(l, f_body, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(l, 27 - lv_font_get_line_height(f_body), 0);

    /* hold target: disc r 50 `raise` with a 3 px `hair` ring; while held the screen fills, not the ring.
       The disc takes the touch; the ring (an lv_arc, which would turn presses into drags) is not clickable. */
    lv_obj_t *disc = lv_obj_create(s_take);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, 100, 100);
    lv_obj_set_pos(disc, 183, 240);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(COL_RAISE), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(disc, 6);
    lv_obj_t *arc = lv_arc_create(disc);                       /* child 0 */
    lv_obj_set_size(arc, 100, 100);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_HAIR), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *hl = lv_label_create(disc);                      /* child 1 */
    lv_label_set_text(hl, d->approval ? "HOLD TO\nAPPROVE" : "HOLD\nTO SEND");
    lv_obj_set_style_text_font(hl, f_meta13, 0);
    lv_obj_set_style_text_letter_space(hl, 2, 0);
    lv_obj_set_style_text_align(hl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(COL_INK), 0);
    lv_obj_center(hl);
    lv_obj_add_event_cb(disc, btn_cb, LV_EVENT_ALL, (void *)(intptr_t)T_CONFIRM);
    s_act_btn[s_nact_btn] = disc; s_act_label[s_nact_btn] = hl; s_act_tag[s_nact_btn] = T_CONFIRM; s_nact_btn++;

    /* EDIT is not available on the device, so it is removed (spec 5.6: never greyed). */
    if (d->approval) {
        int deny = -1;
        for (int i = 0; i < d->nact; i++) if (!strcmp(d->act[i].id, "deny")) deny = i;
        if (deny >= 0) {
            button("HOLD TO DENY", 103, 350, 120, 44, S_HOLD, T_DENY, f_meta13);
            button("CANCEL", 243, 350, 120, 44, S_GHOST, T_CANCEL, f_meta13);
            return;
        }
    }
    button("CANCEL", -1, 350, 120, 44, S_GHOST, T_CANCEL, f_meta13);
}

static void build_sent(void)
{
    clear_take();
    set_flood(false);
    text(s_sent_title, f_sent22, COL_INK, 83, 206, 300, 0, 1, 5);
    text(s_sent_words, f_body, COL_INK, 83, 248, 300, 26, 2, 0);
    char b[32]; snprintf(b, sizeof b, "BACK TO NOW IN %d S", (int)((s_sent_ms + 999) / 1000));
    lv_obj_t *back = text(b, f_whisper, COL_INK2, 60, 316, 346, 0, 1, 3);
    s_act_label[0] = back;     /* updated by the tick */
}

/* The Reply view (E105; proposal section 3.2 with the owner's decisions of Nov 22): the decision layout, peri.
   State and who at the top, the title, the body, the prompt, how to answer and until when, then the choices and SKIP.
   Voice is BOOT only (no mic pill): while this view is the top screen, a BOOT hold binds to it (decision_reply_bind). */
static void build_reply(const dec_t *d)
{
    clear_take();
    set_flood(false);
    snprintf(s_reply_id, sizeof s_reply_id, "%s", d->id);
    reply_layout_t L;
    reply_layout(&d->rp, d->nact, &L);
    char top[64];
    snprintf(top, sizeof top, "%s \xC2\xB7 %s", d->test ? "TEST" : "ASKED", d->agent[0] ? d->agent : s_agent);
    text(top, f_whisper, COL_PERI, 98, 100, 270, 0, 1, 2);
    text(d->title, f_head21, COL_INK, 83, 124, 300, 26, 2, 1);
    if (d->preview[0]) text(d->preview, f_preview, COL_INK, 83, 180, 300, 25, 2, 0);
    static char pr[REPLY_PROMPT_MAX * 4 + 8];                      /* static: the hooks build this on the httpd stack */
    if (d->rp.prompt[0]) dial_caps(pr, sizeof pr, d->rp.prompt);
    else snprintf(pr, sizeof pr, "%s", d->rp.mode == RM_VOICE ? "SAY YOUR ANSWER" : d->rp.mode == RM_CHOICES ? "PICK ONE" : "PICK ONE OR SAY IT");
    text(pr, f_lab17, COL_PERI, 63, 236, 340, 0, 1, 2);
    char meta[64] = "", until[24] = "";
    if (d->expires) { char t[16]; fmt_time(t, sizeof t, d->expires); snprintf(until, sizeof until, "UNTIL %s", t); }
    if (L.voice_hint) snprintf(meta, sizeof meta, "HOLD BOOT TO ANSWER%s%s", until[0] ? " \xC2\xB7 " : "", until);
    else snprintf(meta, sizeof meta, "%s", until);
    if (meta[0]) text(meta, f_meta13, COL_INK2, 60, 266, 346, 0, 1, 1);
    if (L.nchoice > 0) {
        const char *labels[3]; int tags[3]; bool holds[3];
        for (int i = 0; i < L.nchoice; i++) {
            int a = L.choice[i];
            labels[i] = d->act[a].label; tags[i] = T_ACT + a; holds[i] = d->act[a].hold;
        }
        action_row(labels, tags, holds, L.nchoice, false);
        if (L.skip) button("SKIP", -1, 384, 110, 40, S_GHOST, T_SKIP, f_meta13);
    } else {
        if (L.skip) button("SKIP", -1, 312, 134, 56, S_GHOST, T_SKIP, f_whisper);
        more_line(COL_INK2);
    }
}

/* TOO LATE: what he answered, and SEND TO PEGGY (filled) or DISCARD. */
static void build_late(void)
{
    clear_take();
    set_flood(false);
    text("TOO LATE", f_whisper, COL_AMBER, 98, 100, 270, 0, 1, 3);
    text(s_late.caps, f_head21, COL_INK, 83, 124, 300, 26, 2, 1);
    text("YOUR ANSWER", f_meta13, COL_INK2, 83, 184, 300, 0, 1, 2);
    char q[220]; snprintf(q, sizeof q, "\xE2\x80\x9C%s\xE2\x80\x9D", s_late.answer);
    text(q, f_preview, COL_INK, 83, 204, 300, 25, 2, 0);
    text("THE QUESTION HAD CLOSED", f_meta13, COL_INK2, 60, 266, 346, 0, 1, 1);
    const char *lab = s_late.st == L_SENDING ? "SENDING" : s_late.st == L_FAILED ? "TRY AGAIN" : "SEND TO PEGGY";
    button(lab, -1, 312, 200, 56, S_PERI, T_LATE_SEND, f_whisper);
    button("DISCARD", -1, 384, 110, 40, S_GHOST, T_LATE_DROP, f_meta13);
}

static void build_alert(const dec_t *d)
{
    if (reply_view(d)) { build_reply(d); return; }
    s_reply_id[0] = 0;
    if (is_leave(d)) build_leave(d);
    else if (d->weight == W_URGENT) build_urgent(d);
    else if (d->weight == W_NOTICE) build_notice(d);
    else build_decision(d);
}

static void reply_gone(void)
{
    if (!s_reply_id[0]) return;
    snprintf(s_gone_id, sizeof s_gone_id, "%s", s_reply_id);
    s_gone_at = now_ms();
    s_reply_id[0] = 0;
    s_reply_opened[0] = 0;
}

static void hide_take(void)
{
    reply_gone();
    s_mode = M_NONE;
    clear_take();
    lv_obj_add_flag(s_take, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lv_obj_get_child(lv_scr_act(), 0), LV_OBJ_FLAG_HIDDEN);   /* the face */
    lv_obj_invalidate(lv_scr_act());
    s_shown_id[0] = 0;
}

/* Show whatever should own the glass now. Display lock held. */
static void refresh(void)
{
    cue_kick();
    int needs = decision_needs_count();                             /* the Now caption follows the count */
    if (needs != s_needs_shown) { s_needs_shown = needs; dial_refresh_face(); }
    if (s_mode == M_SENT) return;                                   /* runs out on its own */
    if (s_mode == M_LATE) {                                         /* waits for SEND TO PEGGY, but never holds off an urgent */
        dec_t *u = current(NULL);
        if (!u || u->weight < W_URGENT) return;
        s_mode = M_ALERT;
        s_shown_id[0] = 0;
    }
    dec_t *d = current(&s_more);
    if (!d) { if (s_mode != M_NONE) hide_take(); return; }
    bool same = !strcmp(d->id, s_shown_id) && !strcmp(d->updated, s_shown_updated);
    if (same && s_mode != M_NONE) { lv_obj_invalidate(s_take); return; }   /* "N MORE" may have changed */
    if (strcmp(d->id, s_shown_id)) s_notice_lit_ms = 0;            /* a new notice gets its full 20 s */
    if (s_reply_id[0] && strcmp(d->id, s_reply_id)) reply_gone();  /* another item takes the glass */
    snprintf(s_shown_id, sizeof s_shown_id, "%s", d->id);
    snprintf(s_shown_updated, sizeof s_shown_updated, "%s", d->updated);
    s_mode = M_ALERT;
    s_drain_gone = s_notice_step = -1;
    earlier_drop_open(d->id);                                      /* it owns the glass again */
    build_alert(d);
    lv_obj_remove_flag(s_take, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(lv_scr_act(), 0), LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_take);
    if (d->weight >= W_DECISION) lv_display_trigger_activity(NULL);   /* a notice never wakes the glass */
    ESP_LOGI(TAG, "showing %s (weight %d, %d more)", d->id, d->weight, s_more);
}

/* ---- firing ---- */

static void test_result_cb(lv_timer_t *t);

static void fire(const dec_t *d, const dact_t *a)
{
    bool retry = s_act.active && s_act.failed && !strcmp(s_act.dec_id, d->id) && !strcmp(s_act.action_id, a->id);
    if (!retry || !s_act.key[0]) {
        uint32_t r1 = esp_random(), r2 = esp_random();
        snprintf(s_act.key, sizeof s_act.key, "compact-%08lx%08lx", (unsigned long)r1, (unsigned long)r2);
    }
    s_act.active = true;
    s_act.failed = false;
    s_act.at = now_ms();
    snprintf(s_act.object, sizeof s_act.object, "%s:%s", d->src == SRC_NOTICES ? "notices" : "events", d->id);
    snprintf(s_act.action_id, sizeof s_act.action_id, "%s", a->id);
    snprintf(s_act.label, sizeof s_act.label, "%s", a->label);
    snprintf(s_act.dec_id, sizeof s_act.dec_id, "%s", d->id);
    s_act.reply = is_reply(d);
    s_act.test = d->test;
    snprintf(s_act.title, sizeof s_act.title, "%s", d->rp.title);
    snprintf(s_act.caps, sizeof s_act.caps, "%s", d->title);
    if (d->test) {                                                 /* reply-test: answered locally, nothing sent */
        lv_timer_t *t = lv_timer_create(test_result_cb, 600, NULL);
        if (t) lv_timer_set_repeat_count(t, 1);
        return;
    }
    if (!intercom_send_action(s_act.object, a->id, a->draft[0] ? a->draft : NULL, s_act.key)) {
        s_act.failed = true;                                       /* not connected: TRY AGAIN at once */
        ESP_LOGW(TAG, "action not sent: socket down");
    }
}

static void fire_tag(int tag)
{
    dec_t *d = find(s_shown_id);
    if (!d) return;
    if (tag >= T_ACT && tag < T_ACT + 3 && tag - T_ACT < d->nact) fire(d, &d->act[tag - T_ACT]);
    else if (tag == T_CONFIRM) fire(d, &d->act[s_confirm_act]);
    else if (tag == T_DENY)
        for (int i = 0; i < d->nact; i++) if (!strcmp(d->act[i].id, "deny")) fire(d, &d->act[i]);
}

static bool tag_holds(int tag)
{
    dec_t *d = find(s_shown_id);
    if (tag == T_CONFIRM || tag == T_DENY) return true;
    return d && tag >= T_ACT && tag < T_ACT + 3 && tag - T_ACT < d->nact && d->act[tag - T_ACT].hold;
}

static void hold_reset(void)
{
    s_hold.from = 0;
    dial_hold_fill_step(s_take, &s_fill_h, 0);
}

static void late_send(void);
static void late_close(void);
static void reopen(const char *id, bool inform_first);

static void btn_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    int tag = (int)(intptr_t)lv_event_get_user_data(e);
    if (tag_holds(tag)) {
        if (c == LV_EVENT_PRESSED && !(s_act.active && !s_act.failed)) {
            s_hold.tag = tag;
            s_hold.from = now_ms();
        } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && s_hold.from) {
            hold_reset();                                          /* released early: nothing sent */
        }
        return;
    }
    if (c != LV_EVENT_CLICKED) return;
    if (compact_press_was_drag()) return;   /* LVGL 9.5 still clicks after a swipe or a drag */
    dec_t *d = find(s_shown_id);
    if (tag == T_LATER) {
        if (d) put_aside(d, "open");
        refresh();
    } else if (tag == T_GOTIT) {
        if (d && is_reply(d)) {
            put_aside(d, "open");              /* not answered: still awaiting, so no ack; it stays in the NEEDS YOU count */
        } else if (d) {
            put_aside(d, "cleared");
            if (d->src == SRC_NOTICES) intercom_ack_notice(d->id);
        }
        refresh();
    } else if (tag == T_REPLY && d && inform_stage(d)) {            /* the REPLY pill: open the Reply view */
        snprintf(s_reply_opened, sizeof s_reply_opened, "%s", d->id);
        s_mode = M_ALERT;
        build_alert(d);
    } else if (tag == T_SKIP) {                                    /* E105 contract 3.3: ends replies, no wake */
        if (d) {
            put_aside(d, "skipped");
            if (!d->test && !intercom_skip_notice(d->id)) ESP_LOGW(TAG, "skip for %s not queued", d->id);
            ESP_LOGI(TAG, "skip %s%s", d->id, d->test ? " (test)" : "");
        }
        refresh();
    } else if (tag == T_LATE_SEND && s_mode == M_LATE) {
        late_send();
    } else if (tag == T_LATE_DROP && s_mode == M_LATE) {
        ESP_LOGI(TAG, "late reply discarded");
        late_close();
    } else if (tag == T_REVIEW && d) {
        s_confirm_act = 0;
        if (d->approval) { for (int i = 0; i < d->nact; i++) if (!strcmp(d->act[i].id, "approve")) s_confirm_act = i; }
        else for (int i = 0; i < d->nact; i++) if (d->act[i].draft[0]) { s_confirm_act = i; break; }
        s_mode = M_CONFIRM;
        build_confirm(d, s_confirm_act);
    } else if (tag == T_CANCEL && d) {
        s_mode = M_ALERT;
        build_alert(d);
    } else if (tag >= T_ACT && tag < T_ACT + 3 && !(s_act.active && !s_act.failed)) {
        fire_tag(tag);
    }
    lv_obj_invalidate(s_take);
}

/* ---- the tick: hold progress, WAITING / TRY AGAIN, SENT countdown, notice and urgent drains, expiry ---- */

static void label_for(int tag, const char *t, uint32_t border)
{
    for (int i = 0; i < s_nact_btn; i++) if (s_act_tag[i] == tag) {
        if (strcmp(lv_label_get_text(s_act_label[i]), t)) {
            lv_label_set_text(s_act_label[i], t);
            if (tag != T_CONFIRM && label_w(t, f_whisper, 2) > lv_obj_get_width(s_act_btn[i]) - 16)
                lv_obj_set_style_text_font(s_act_label[i], f_meta13, 0);
        }
        if (border && tag != T_CONFIRM) lv_obj_set_style_border_color(s_act_btn[i], lv_color_hex(border), 0);
        if (border && tag != T_CONFIRM) lv_obj_set_style_border_width(s_act_btn[i], 2, 0);
    }
}

static int act_tag(void)
{
    if (s_mode == M_CONFIRM) {
        dec_t *d = find(s_shown_id);
        if (d && !strcmp(d->act[s_confirm_act].id, s_act.action_id)) return T_CONFIRM;
        return T_DENY;
    }
    dec_t *d = find(s_shown_id);
    for (int i = 0; d && i < d->nact; i++) if (!strcmp(d->act[i].id, s_act.action_id)) return T_ACT + i;
    return -1;
}

/* The notice's drain arc (r 150, 10 to 2 o'clock) and its icon: the only part that moves. */
static void invalidate_notice_arc(void)
{
    lv_area_t a = { 98, 78, 368, 166 };
    lv_obj_invalidate_area(s_take, &a);
}

static void notice_tick(int64_t dt)
{
    if (glass_lit() && !s_hold.from) s_notice_lit_ms += dt;          /* the clock runs only on lit glass */
    if (s_notice_lit_ms >= 2000) { dec_t *sd = find(s_shown_id); if (sd) mark_seen(sd); }   /* seen: the ring stops */
    if (s_notice_lit_ms >= NOTICE_MS) {                              /* cleared: it waits in Earlier (to come) */
        dec_t *d = find(s_shown_id);
        if (d) put_aside(d, d && is_reply(d) ? "open" : "cleared");   /* a question still waits: filed open, not cleared */
        refresh();
        return;
    }
    int step = (int)(notice_left() * 120);                           /* one step per degree of the arc */
    if (step != s_notice_step) { s_notice_step = step; invalidate_notice_arc(); }
    if (s_clears) {
        char b[24]; snprintf(b, sizeof b, "CLEARS IN %d S", (int)((NOTICE_MS - s_notice_lit_ms + 999) / 1000));
        if (strcmp(lv_label_get_text(s_clears), b)) lv_label_set_text(s_clears, b);
    }
}

static void tick_cb(lv_timer_t *t)
{
    int64_t now = now_ms();
    int64_t dt = s_last_tick ? now - s_last_tick : 0;
    s_last_tick = now;
    if (dt > 1000) dt = 0;                                           /* LVGL was paused (asleep) */
    if (s_hold.from) {
        int64_t ms = now - s_hold.from;
        dial_hold_fill_step(s_take, &s_fill_h, ms >= HOLD_MS ? 466 : (int)(ms * 466 / HOLD_MS));
        if (ms >= HOLD_MS) {
            int tag = s_hold.tag;
            hold_reset();                                          /* the answer shows on the button */
            fire_tag(tag);
        }
    }
    if (s_mode == M_LATE) {
        if (s_late.st == L_SENDING && now - s_late.sent_at >= GIVE_UP_MS) {
            s_late.st = L_FAILED;                                  /* no ack: the same id may be sent again */
            build_late(); lv_obj_invalidate(s_take);
        } else if (s_late.st != L_SENDING && now - s_late.at >= LATE_MS) {
            ESP_LOGI(TAG, "late reply: closed untouched");
            late_close();
        }
        return;
    }
    if (s_mode == M_SENT) {
        int left = (int)((s_sent_ms - (now - s_sent_at) + 999) / 1000);
        if (left <= 0) {
            hide_take();
            refresh();
        } else if (s_act_label[0]) {
            char b[32]; snprintf(b, sizeof b, "BACK TO NOW IN %d S", left);
            if (strcmp(lv_label_get_text(s_act_label[0]), b)) lv_label_set_text(s_act_label[0], b);
        }
        return;
    }
    if (s_mode == M_NONE) {
        /* a notice that arrived while the glass was dark shows once it lights up (checked twice a second) */
        static int64_t last_look;
        if (now - last_look >= 500) { last_look = now; if (s_ndec) refresh(); }
        return;
    }
    if (s_act.active && !strcmp(s_act.dec_id, s_shown_id)) {
        int tag = act_tag();
        if (s_act.failed || now - s_act.at >= GIVE_UP_MS) {
            s_act.failed = true;
            label_for(tag, tag == T_CONFIRM ? "TRY\nAGAIN" : "TRY AGAIN", COL_AMBER);
        } else if (now - s_act.at >= WAITING_MS) {
            label_for(tag, "WAITING", 0);
        }
    }
    dec_t *d = find(s_shown_id);
    if (!d || (d->expires && d->expires <= time(NULL))) { refresh(); return; }
    if (s_mode == M_ALERT && d->weight == W_NOTICE && !reply_view(d)) { notice_tick(dt); return; }
    if (s_mode == M_ALERT && is_leave(d) && leave_now(d) && d->leave_by > s_leave_built) {
        s_leave_built = d->leave_by;                               /* LEAVE 2:20 became LEAVE NOW */
        build_leave(d);
        lv_obj_invalidate(s_take);
    }
    if (s_mode == M_ALERT && d->weight == W_URGENT && s_drain_gone >= 0 &&
        dial_alert_dots_gone(d->arrival, d->expires) != s_drain_gone)
        lv_obj_invalidate(s_take);                                   /* one rim dot went out */
}

/* ---- public ---- */

void decision_init(void)
{
    s_dec = heap_caps_calloc(MAX_DEC, sizeof(dec_t), MALLOC_CAP_SPIRAM);
    bsp_display_lock(-1);
    s_take = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_take);
    lv_obj_set_size(s_take, 466, 466);
    lv_obj_center(s_take);
    dial_use_ground(s_take);
    lv_obj_set_style_bg_opa(s_take, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_take, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_take, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_take, take_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    s_tick = lv_timer_create(tick_cb, 40, NULL);
    bsp_display_unlock();
}

/* Replace this source's entries with the alerts in `items` (id -> item), newest first by updated_at. */
static void ingest(const cJSON *items, int src)
{
    int w = 0;
    for (int i = 0; i < s_ndec; i++) {
        if (s_dec[i].src == src && !s_dec[i].test) continue;      /* the reply-test item is the device's own */
        if (w != i) memcpy(&s_dec[w], &s_dec[i], sizeof(dec_t));
        w++;
    }
    s_ndec = w;
    time_t now = time(NULL);
    const cJSON *e;
    cJSON_ArrayForEach(e, items) {
        if (!e->string) continue;
        const char *st = str(e, "state");
        if (src == SRC_EVENTS && st && strcmp(st, "open")) {        /* ended (E100 tombstone): file it */
            int rw = raw_weight(e, false);
            if (rw >= W_NOTICE) {
                char title[84];
                dial_caps(title, sizeof title, str(e, "title") ? str(e, "title") : "");
                const char *ea = str(e, "ended_at") ? str(e, "ended_at") : str(e, "updated_at");
                earlier_note(e->string, title, rw, st, ea ? dial_parse_iso(ea) : now, src,
                             str(e, "expires_at") ? dial_parse_iso(str(e, "expires_at")) : 0);
                const cJSON *det = cJSON_GetObjectItem(e, "detail");
                const char *tb = str(e, "body") ? str(e, "body") : det ? str(det, "description") : NULL;
                if (!tb && det) tb = str(det, "location");
                char bb[160]; clean(bb, sizeof bb, tb ? tb : "");
                earlier_note_text(e->string, bb, NULL);
            }
            continue;
        }
        int wt = decision_weight(e, src == SRC_NOTICES);
        if (wt == W_NONE) continue;
        time_t exp = str(e, "expires_at") ? dial_parse_iso(str(e, "expires_at")) : 0;
        if (exp && exp <= now) continue;
        const char *up = str(e, "updated_at") ? str(e, "updated_at") : "";
        int at = s_ndec;
        while (at > 0 && strcmp(s_dec[at - 1].updated, up) < 0) at--;
        if (at >= MAX_DEC) continue;
        if (s_ndec == MAX_DEC) s_ndec--;
        memmove(&s_dec[at + 1], &s_dec[at], sizeof(dec_t) * (s_ndec - at));
        parse_one(&s_dec[at], e, e->string, src, wt);
        s_ndec++;
    }
}

static void after_ingest(void)
{
    /* the confirm screen gives way when its Moment ended or changed, or something weightier arrived */
    if (s_mode == M_CONFIRM) {
        dec_t *d = find(s_shown_id), *top = current(NULL);
        if (!d || strcmp(d->updated, s_shown_updated) || (top && top->weight > d->weight)) s_mode = M_ALERT, s_shown_id[0] = 0;
    }
    if (s_mode == M_CONFIRM) lv_obj_invalidate(s_take);
    else refresh();
}

void decision_on_events(const cJSON *events)
{
    if (!s_dec) return;
    bsp_display_lock(-1);
    /* learn the agent's name from any agent-published Event (approvals come from the bus) */
    const cJSON *e;
    cJSON_ArrayForEach(e, events) {
        const char *src = str(e, "source");
        if (src && !strncmp(src, "agent:", 6)) { agent_name(s_agent, sizeof s_agent, src + 6); break; }
    }
    ingest(events, SRC_EVENTS);
    earlier_settle(events, SRC_EVENTS);
    after_ingest();
    bsp_display_unlock();
}

void decision_on_notices(const cJSON *notices)
{
    if (!s_dec) return;
    bsp_display_lock(-1);
    ingest(notices, SRC_NOTICES);
    earlier_settle(notices, SRC_NOTICES);
    after_ingest();
    bsp_display_unlock();
}

static void show_sent(const char *title, const char *words, bool glyph, int64_t ms)
{
    snprintf(s_sent_title, sizeof s_sent_title, "%s", title);
    snprintf(s_sent_words, sizeof s_sent_words, "%s", words);
    s_sent_glyph = glyph;
    s_sent_ms = ms;
    s_mode = M_SENT;
    s_sent_at = now_ms();
    build_sent();
    lv_obj_remove_flag(s_take, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(lv_scr_act(), 0), LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_take);
}

/* TOO LATE for an answer the bus refused because the question had closed (contract 3.4). LVGL context. */
static void late_open(const char *title, const char *caps, const char *answer, bool test)
{
    memset(&s_late, 0, sizeof s_late);
    snprintf(s_late.title, sizeof s_late.title, "%s", title);
    snprintf(s_late.caps, sizeof s_late.caps, "%s", caps);
    snprintf(s_late.answer, sizeof s_late.answer, "%s", answer);
    s_late.test = test;
    s_late.at = now_ms();
    reply_gone();
    s_mode = M_LATE;
    build_late();
    lv_obj_remove_flag(s_take, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lv_obj_get_child(lv_scr_act(), 0), LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_take);
    lv_display_trigger_activity(NULL);
    ESP_LOGI(TAG, "too late: offering SEND TO PEGGY");
}

static void late_close(void)
{
    s_late.cmid[0] = 0;
    hide_take();
    refresh();
}

static void late_result_cb(lv_timer_t *t);

static void late_send(void)
{
    if (s_late.st == L_SENDING) return;
    if (!s_late.cmid[0] || s_late.st == L_FAILED) {                /* a fresh id, except to retry internal_error */
        if (!(s_late.st == L_FAILED && s_late.cmid[0]))
            snprintf(s_late.cmid, sizeof s_late.cmid, "late-%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    }
    s_late.st = L_SENDING;
    s_late.sent_at = s_late.at = now_ms();
    if (s_late.test) {
        lv_timer_t *t = lv_timer_create(late_result_cb, 600, NULL);
        if (t) lv_timer_set_repeat_count(t, 1);
    } else {
        char *body = heap_caps_malloc(REPLY_LATE_MAX + 1, MALLOC_CAP_SPIRAM);
        bool ok = body && reply_late_body(body, REPLY_LATE_MAX + 1, s_late.title, s_late.answer) > 0 && intercom_send_text(s_late.cmid, body);
        heap_caps_free(body);
        if (!ok) { s_late.st = L_FAILED; s_late.cmid[0] = 0; }
        ESP_LOGI(TAG, "late reply %s: %s", s_late.cmid, ok ? "queued" : "not sent");
    }
    build_late();
    lv_obj_invalidate(s_take);
}

/* The answer to SEND TO PEGGY (the `send` frame's ack). LVGL context. */
static void late_ack_lv(const char *status, const char *reason)
{
    if (!strcmp(status, "queued") || !strcmp(status, "command")) {
        show_sent("SENT", "Peggy has it.", true, SENT_MS);
        return;
    }
    s_late.st = L_FAILED;
    if (reply_outcome(reason) != OUT_RETRY) s_late.cmid[0] = 0;   /* only internal_error may reuse the id */
    ESP_LOGW(TAG, "late reply rejected: %s", reason ? reason : "?");
    build_late();
    lv_obj_invalidate(s_take);
}

static void late_result_cb(lv_timer_t *t)
{
    (void)t;
    if (s_mode == M_LATE && s_late.test && s_late.st == L_SENDING) late_ack_lv("queued", NULL);
}

static void action_result_lv(const char *key, const char *status, const char *reason);

static void test_result_cb(lv_timer_t *t)
{
    (void)t;
    if (s_act.active && s_act.test) action_result_lv(s_act.key, s_test_late ? "failed" : "done", s_test_late ? "not_found" : NULL);
}

void decision_on_action_result(const char *key, const char *status, const char *reason)
{
    bsp_display_lock(-1);
    action_result_lv(key, status, reason);
    bsp_display_unlock();
}

static void action_result_lv(const char *key, const char *status, const char *reason)
{
    if (!s_act.active || strcmp(key, s_act.key)) return;
    dec_t *d = find(s_act.dec_id);
    if (!strcmp(status, "done") || !strcmp(status, "already_done")) {
        s_act.active = false;
        s_act.key[0] = 0;
        if (d) put_aside(d, "answered");                         /* the bus ends it as answered next */
        char agent[24]; title_case(agent, sizeof agent, s_agent);
        char lab[25]; title_case(lab, sizeof lab, s_act.label);
        if (d && d->approval) snprintf(s_sent_words, sizeof s_sent_words, "%s", !strcmp(s_act.action_id, "deny") ? "Denied." : "Approved.");
        else if (d && s_mode == M_CONFIRM) snprintf(s_sent_words, sizeof s_sent_words, "%s is sending it.", agent);
        else snprintf(s_sent_words, sizeof s_sent_words, "%s has \xE2\x80\x9C%s\xE2\x80\x9D.", agent, lab);
        snprintf(s_sent_title, sizeof s_sent_title, "SENT");
        s_sent_glyph = true;
        s_sent_ms = SENT_MS;
        s_mode = M_SENT;
        s_sent_at = now_ms();
        reply_gone();
        build_sent();
        lv_obj_remove_flag(s_take, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_take);
    } else if (!strcmp(status, "expired")) {
        s_act.active = false;
        if (d) set_aside(d->id, d->updated, false);
        ESP_LOGW(TAG, "action expired (%s)", reason ? reason : "");
        s_mode = M_ALERT;
        s_shown_id[0] = 0;
        refresh();
    } else if (s_act.reply && reply_outcome(reason) == OUT_LATE) {   /* E105: the question closed under him */
        s_act.active = false;
        s_act.key[0] = 0;
        ESP_LOGW(TAG, "reply tap refused: %s", reason ? reason : "?");
        if (d) put_aside(d, "too late");
        late_open(s_act.title, s_act.caps, s_act.label, s_act.test);
    } else {
        s_act.failed = true;
        if (reason && strcmp(reason, "internal_error")) s_act.key[0] = 0;   /* only internal_error may reuse the key */
        ESP_LOGW(TAG, "action failed: %s", reason ? reason : status);
    }
}

void decision_set_aside(void)
{
    bsp_display_lock(-1);
    if (s_mode == M_SENT) {
        s_sent_at = now_ms() - s_sent_ms;                        /* let the tick close it */
    } else if (s_mode == M_LATE) {
        late_close();
    } else if (s_mode != M_NONE) {
        dec_t *d = find(s_shown_id);
        if (d) put_aside(d, "open");
        s_mode = M_ALERT;
        s_shown_id[0] = 0;
        refresh();
    }
    bsp_display_unlock();
}

/* Swipe down on a takeover (first a swipe up, now down): decision, urgent and confirm are set
   aside as LATER does; a notice clears as GOT IT does; SENT and TOO LATE close. A swipe up does nothing. The confirm
   screen's text box scrolls: a drag that starts on it scrolls it (the dismiss waits until it sits at its start, and a
   press that scrolled it never dismisses). Display lock held. */
void decision_swipe_down_lv(void)
{
    lv_obj_t *act = lv_indev_get_active_obj();
    if (s_box && act && (act == s_box || lv_obj_get_parent(act) == s_box) &&
        (s_box_scrolled || lv_obj_get_scroll_top(s_box) > 0)) return;
    if (s_mode == M_SENT) { s_sent_at = now_ms() - s_sent_ms; return; }
    if (s_mode == M_LATE) { late_close(); return; }
    if (s_mode == M_NONE) return;
    dec_t *d = find(s_shown_id);
    if (s_mode == M_ALERT && d && d->weight == W_NOTICE && !is_reply(d)) {
        put_aside(d, "cleared");
        if (d->src == SRC_NOTICES) intercom_ack_notice(d->id);
    } else if (d) put_aside(d, "open");   /* a question (Inform or Reply view) is filed open, never acked */
    hold_reset();
    s_mode = M_ALERT;
    s_shown_id[0] = 0;
    refresh();
}

bool decision_showing(void) { return s_mode != M_NONE; }
bool decision_flood_showing(void) { return s_mode != M_NONE && s_flood; }

/* Swipe up on Now: open the card of the pending notice, if any. Display lock held. */
bool decision_open_notice(void)
{
    if (s_mode != M_NONE) return false;
    dec_t *d = pending_notice();
    if (!d) return false;
    s_notice_lit_ms = 0;
    reopen(d->id, is_reply(d));                                    /* a question shows Inform with its REPLY pill first */
    return true;
}

void decision_reopen(const char *id) { reopen(id, false); }

/* The Now caption "N NEEDS YOU" (proposal 3.2 c): items still awaiting an answer, set aside ones included. LVGL context
   (display lock held). 0 with the flag off. */
int decision_needs_count(void)
{
    if (!reply_on() || !s_dec) return 0;
    time_t now = time(NULL);
    int n = 0;
    for (int i = 0; i < s_ndec; i++) {
        const dec_t *d = &s_dec[i];
        if (d->src == SRC_NOTICES && reply_needs_you(&d->rp, aside_done(d), d->expires && d->expires <= now)) n++;
    }
    return n;
}

/* A tap on the caption: open the oldest awaiting item, straight to its Reply view (it is closest to expiring; open
   question 3.3). Like a decision it takes the glass, and a swipe up files it open again. Display lock held. */
bool decision_open_needs(void)
{
    if (s_mode != M_NONE || !reply_on()) return false;
    time_t now = time(NULL);
    dec_t *best = NULL;
    for (int i = 0; i < s_ndec; i++) {
        dec_t *d = &s_dec[i];
        if (d->src != SRC_NOTICES || !reply_needs_you(&d->rp, aside_done(d), d->expires && d->expires <= now)) continue;
        if (!best || d->arrival < best->arrival) best = d;
    }
    if (!best) return false;
    reopen(best->id, false);
    return true;
}

static void reopen(const char *id, bool inform_first)
{
    if (inform_first) s_reply_opened[0] = 0;
    else { dec_t *d = find(id); if (d && is_reply(d)) snprintf(s_reply_opened, sizeof s_reply_opened, "%s", id); }
    for (int i = 0; i < s_naside; i++)
        if (!strcmp(s_aside[i].id, id)) {
            memmove(&s_aside[i], &s_aside[i + 1], sizeof s_aside[0] * (s_naside - i - 1));
            s_naside--;
            break;
        }
    snprintf(s_reopen_id, sizeof s_reopen_id, "%s", id);
    if (s_mode == M_SENT) return;
    s_mode = M_ALERT;
    s_shown_id[0] = 0;
    refresh();
}

/* Test hook (POST /debug/ui "notice-test"): inject one info notice as if the bus had pushed it. Replaces the
   notices resource until the bus next pushes it. Any task. */
void decision_debug_notice(void)
{
    char ts[32], ex[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm); strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    time_t e = now + 900; gmtime_r(&e, &tm); strftime(ex, sizeof ex, "%Y-%m-%dT%H:%M:%SZ", &tm);
    cJSON *root = cJSON_CreateObject(), *n = cJSON_AddObjectToObject(root, "ring-test-notice");
    cJSON_AddStringToObject(n, "title", "Notice test");
    cJSON_AddStringToObject(n, "level", "info");
    cJSON_AddStringToObject(n, "state", "active");
    cJSON_AddStringToObject(n, "created_at", ts);
    cJSON_AddStringToObject(n, "updated_at", ts);
    cJSON_AddStringToObject(n, "expires_at", ex);
    decision_on_notices(root);
    cJSON_Delete(root);
}

/* ---- notification replies (E105): push-to-talk binding, closing words, the late send's ack, test hook ---- */

/* Is a Reply view the top screen? Nothing of LVGL's top layer (power menu, flashlight, Charging) may cover it.
   Display lock held. */
static dec_t *reply_view_top(void)
{
    if (s_mode != M_ALERT || !s_reply_id[0] || strcmp(s_reply_id, s_shown_id)) return NULL;
    if (overlay_open() || overlay_flashlight_on() || overlay_charging_on()) return NULL;
    return find(s_reply_id);
}

reply_bind_t decision_reply_bind(char *id, size_t idn, char *title, size_t tn, char *raw, size_t rn)
{
    if (idn) id[0] = 0;
    if (tn) title[0] = 0;
    if (rn) raw[0] = 0;
    if (!reply_on()) return BIND_OFF;                              /* the flag first: no lock, nothing else runs */
    if (bsp_display_lock(1000) != ESP_OK) {                        /* bounded: unbound is the safe answer */
        ESP_LOGW(TAG, "reply bind: display busy, unbound");
        return BIND_NO_VIEW;
    }
    dec_t *d = s_dec ? reply_view_top() : NULL;
    reply_bind_ctx_t c = {
        .enabled = true, .view_top = d != NULL, .test = d && d->test,
        .expired = d && d->expires && d->expires <= time(NULL), .info = d ? &d->rp : NULL,
    };
    reply_bind_t b = reply_bind_decide(&c);
    if (d) { snprintf(title, tn, "%s", d->title); snprintf(raw, rn, "%s", d->rp.title); }
    if (b == BIND_YES) snprintf(id, idn, "%s", d->id);
    bsp_display_unlock();
    ESP_LOGI(TAG, "push-to-talk: %s%s%s", reply_bind_word(b), b == BIND_YES ? " to " : "", b == BIND_YES ? id : "");
    return b;
}

void decision_reply_handed(const char *id)
{
    if (!reply_on() || !id || !id[0]) return;
    if (bsp_display_lock(1000) != ESP_OK) return;                  /* the view stays; the bus update closes it */
    dec_t *d = find(id);
    if (d && s_mode == M_ALERT && !strcmp(s_shown_id, id)) {
        put_aside(d, "open");                                      /* the Understood screen shows; it stays openable */
        s_shown_id[0] = 0;
        refresh();
    }
    bsp_display_unlock();
}

/* v1 `notification` event (socket task). Resolved and expired items leave the `notices` resource with a delete, so
   their last word comes from here: DONE <resolution> or EXPIRED on a Reply view that was open, and the Earlier row. */
void decision_on_notification(const cJSON *n)
{
    if (!reply_on() || !s_dec || !n) return;
    const char *id = str(n, "id"), *st = str(n, "state"), *rs = str(n, "reply_state");
    const cJSON *rp = cJSON_GetObjectItem(n, "reply");
    const char *mode = cJSON_IsObject(rp) ? str(rp, "mode") : NULL;
    if (!id || !st || !mode || !strcmp(mode, "none")) return;      /* not a reply notification */
    bool resolved = !strcmp(st, "resolved"), expired = !strcmp(st, "expired");
    const char *row = resolved ? "done" : expired ? "expired" : rs && !strcmp(rs, "received") ? "replied" :
                      rs && !strcmp(rs, "skipped") ? "skipped" : NULL;
    if (!row) return;
    if (bsp_display_lock(1000) != ESP_OK) return;                  /* a missed closing word is harmless */
    static struct { char id[66], row[12]; } noted[8];               /* the bus repeats events (snapshots, replays) */
    static int noted_next;
    bool dup = false;
    for (int i = 0; i < 8; i++) if (!strcmp(noted[i].id, id) && !strcmp(noted[i].row, row)) dup = true;
    if (!dup) {
        snprintf(noted[noted_next].id, sizeof noted[0].id, "%s", id);
        snprintf(noted[noted_next].row, sizeof noted[0].row, "%s", row);
        noted_next = (noted_next + 1) % 8;
        char title[84];
        dial_caps(title, sizeof title, str(n, "title") ? str(n, "title") : "Notice");
        const char *ex = str(n, "expires_at");
        earlier_note(id, title, raw_weight(n, true), row, time(NULL), SRC_NOTICES, ex ? dial_parse_iso(ex) : 0);
        char bb[160], rr[REPLY_RESOLUTION_MAX * 4 + 1];
        clean(bb, sizeof bb, str(n, "body") ? str(n, "body") : "");
        clean(rr, sizeof rr, str(n, "resolution") ? str(n, "resolution") : "");
        earlier_note_text(id, bb, rr);
    }
    bool open_now = s_mode == M_ALERT && !strcmp(s_reply_id, id);
    bool just_gone = !strcmp(s_gone_id, id) && now_ms() - s_gone_at < GONE_MS && s_mode == M_NONE;
    if ((resolved || expired) && (open_now || just_gone)) {
        char words[96];
        const char *res = str(n, "resolution");
        if (resolved) clean(words, sizeof words, res && res[0] ? res : "Peggy closed the question.");
        else snprintf(words, sizeof words, "No answer was sent.");
        dec_t *d = find(id);
        if (d) set_aside(d->id, d->updated, true);
        s_reply_id[0] = 0;
        show_sent(resolved ? "DONE" : "EXPIRED", words, resolved, CLOSED_MS);
        ESP_LOGI(TAG, "reply %s %s", id, resolved ? "done" : "expired");
    }
    bsp_display_unlock();
}

void decision_on_ack(const char *cmid, const char *status, const char *reason, const char *interaction_id)
{
    (void)interaction_id;
    if (!cmid || !status || !reply_on()) return;
    if (bsp_display_lock(1000) != ESP_OK) return;
    if (s_mode == M_LATE && s_late.st == L_SENDING && !strcmp(cmid, s_late.cmid)) late_ack_lv(status, reason);
    bsp_display_unlock();
}

/* POST /debug/ui "reply on|off|status" and "reply test choices|voice|both|inform|late|done|expired|off". The test item is the
   device's own (id reply-test): taps, SKIP and SEND TO PEGGY are answered locally and nothing reaches the bus, and a
   BOOT hold on it is never bound. Any task. */
static void test_remove_lv(void)
{
    int w = 0;
    for (int i = 0; i < s_ndec; i++) {
        if (s_dec[i].test) continue;
        if (w != i) memcpy(&s_dec[w], &s_dec[i], sizeof(dec_t));
        w++;
    }
    s_ndec = w;
    for (int i = 0; i < s_naside; i++)
        if (!strcmp(s_aside[i].id, TEST_ID)) { memmove(&s_aside[i], &s_aside[i + 1], sizeof s_aside[0] * (s_naside - i - 1)); s_naside--; break; }
    earlier_drop_open(TEST_ID);
    if (!strcmp(s_reopen_id, TEST_ID)) s_reopen_id[0] = 0;
}

static bool test_inject_lv(const char *variant)
{
    const char *mode = !strcmp(variant, "voice") ? "voice" : !strcmp(variant, "choices") ? "choices" : "both";
    bool inform = !strcmp(variant, "inform");                      /* a notice-weight question: Inform with a REPLY pill */
    char ts[32], ex[32];
    time_t now = time(NULL), e = now + 900;
    struct tm tm;
    gmtime_r(&now, &tm); strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    gmtime_r(&e, &tm); strftime(ex, sizeof ex, "%Y-%m-%dT%H:%M:%SZ", &tm);
    cJSON *n = cJSON_CreateObject();
    if (!n) return false;
    cJSON_AddStringToObject(n, "title", "Dave asked to move the 1:1");
    cJSON_AddStringToObject(n, "body", "Thursday 2:00 or Friday 10:30? He is free both.");
    cJSON_AddStringToObject(n, "level", inform ? "info" : "attention");
    cJSON_AddStringToObject(n, "state", "active");
    cJSON_AddStringToObject(n, "source", "agent:peggy-pool-4");
    cJSON_AddStringToObject(n, "created_at", ts);
    cJSON_AddStringToObject(n, "updated_at", ts);
    cJSON_AddStringToObject(n, "expires_at", ex);
    cJSON *acts = cJSON_AddArrayToObject(n, "actions");
    const char *labels[2] = { "Thu 2:00", "Fri 10:30" };
    for (int i = 0; i < 2; i++) { cJSON *a = cJSON_CreateObject(); cJSON_AddStringToObject(a, "label", labels[i]); cJSON_AddItemToArray(acts, a); }
    cJSON *rp = cJSON_AddObjectToObject(n, "reply");
    cJSON_AddStringToObject(rp, "mode", mode);
    cJSON_AddStringToObject(rp, "prompt", !strcmp(mode, "voice") ? "Say a day and time" : "Tap one, or say a time");
    cJSON_AddStringToObject(n, "reply_state", "awaiting");
    test_remove_lv();
    if (s_ndec == MAX_DEC) s_ndec--;
    memmove(&s_dec[1], &s_dec[0], sizeof(dec_t) * s_ndec);
    parse_one(&s_dec[0], n, TEST_ID, SRC_NOTICES, inform ? W_NOTICE : W_DECISION);
    s_dec[0].test = true;
    s_ndec++;
    cJSON_Delete(n);
    s_test_late = !strcmp(variant, "late");
    reopen(TEST_ID, inform);                                       /* takes the glass now */
    lv_display_trigger_activity(NULL);
    return true;
}

const char *decision_reply_debug(const char *arg)
{
    char a[16] = "", b[16] = "";
    sscanf(arg ? arg : "", "%15s %15s", a, b);
    if (!strcmp(a, "on")) { prefs_set_notif_reply(true); return "notification replies on\n"; }
    if (!strcmp(a, "status") || !a[0]) return reply_on() ? "notification replies on\n" : "notification replies off\n";
    if (!strcmp(a, "off")) {
        prefs_set_notif_reply(false);
        if (s_dec && bsp_display_lock(2000) == ESP_OK) {
            test_remove_lv();
            if (s_mode == M_LATE) hide_take();
            s_reply_id[0] = 0;
            if (s_mode == M_ALERT) s_shown_id[0] = 0;              /* rebuild whatever shows in its usual look */
            refresh();
            bsp_display_unlock();
        }
        return "notification replies off\n";
    }
    if (strcmp(a, "test")) return "body: reply on|off|status | reply test choices|voice|both|inform|late|done|expired|off\n";
    if (!reply_on()) return "refused: notification replies are off (reply on)\n";
    if (!s_dec || bsp_display_lock(2000) != ESP_OK) return "display busy\n";
    const char *r = "ok\n";
    if (!strcmp(b, "off")) {
        test_remove_lv();
        if (s_mode == M_ALERT && !strcmp(s_shown_id, TEST_ID)) s_shown_id[0] = 0;
        refresh();
    } else if (!strcmp(b, "done") || !strcmp(b, "expired")) {      /* as the bus's last v1 event for it */
        if (!find(TEST_ID)) r = "no test item\n";
        else {
            bool done = !strcmp(b, "done");
            bool open_now = s_mode == M_ALERT && !strcmp(s_reply_id, TEST_ID);
            test_remove_lv();
            earlier_note(TEST_ID, "DAVE ASKED TO MOVE THE 1:1", W_DECISION, done ? "done" : "expired", time(NULL), SRC_NOTICES, 0);
            earlier_note_text(TEST_ID, "Thursday 2:00 or Friday 10:30? He is free both.", done ? "Booked Fri 10:30" : NULL);
            if (open_now) { s_reply_id[0] = 0; show_sent(done ? "DONE" : "EXPIRED", done ? "Booked Fri 10:30" : "No answer was sent.", done, CLOSED_MS); }
            else refresh();
        }
    } else if (!strcmp(b, "choices") || !strcmp(b, "voice") || !strcmp(b, "both") || !strcmp(b, "late") || !strcmp(b, "inform")) {
        if (!test_inject_lv(b)) r = "no memory\n";
    } else r = "body: reply test choices|voice|both|inform|late|done|expired|off\n";
    bsp_display_unlock();
    return r;
}

void decision_reply_info(char *out, size_t n)
{
    char view[66] = "";
    bool late = false;
    if (reply_on() && bsp_display_lock(200) == ESP_OK) {           /* flag off: no lock at all */                         /* bounded; /info shows no view when busy */
        snprintf(view, sizeof view, "%s", s_reply_id);
        late = s_mode == M_LATE;
        bsp_display_unlock();
    }
    snprintf(out, n, "{\"enabled\":%s,\"view\":%s%s%s,\"late\":%s}", reply_on() ? "true" : "false",
             view[0] ? "\"" : "", view[0] ? view : "null", view[0] ? "\"" : "", late ? "true" : "false");
}
