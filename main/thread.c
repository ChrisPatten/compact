#include "thread.h"
#include "display.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"
#include "fonts.h"
#include "lvgl_private.h"   /* lv_text_get_next_line */
#include "dial.h"
#include "intercom_ws.h"
#include "ptt.h"
#include "audio_out.h"
#include "decision.h"
#include "earlier.h"
#include "setscreen.h"
#include "dayview.h"
#include "agents.h"
#include "reply_logic.h"

/* Plum Dusk tokens (spec 3.1) */
#define COL_SHADED   0x24182A          /* glass under the 50% daylight shade (day ground only) */
#define COL_HAIR     0x8B7098
#define COL_INK      0xF8ECE8
#define COL_INK2     0xCDB5C6
#define COL_ROSE     0xF5A99B
#define COL_ROSE_INK 0x2A1A2C
#define COL_AMBER    0xF7C95C

#define AGENT        "Peggy"           /* the agent behind the followed conversation */
#define MAX_MSG      20                /* messages held and shown */
#define FETCH_N      20
#define IDLE_MS      60000             /* spec 8.3: Thread untouched 60 s -> Now */
#define NOACK_MS     3000              /* spec 7.3: no answer in 3 s -> WAITING */
#define GIVEUP_MS    25000             /* no transcript by then -> TRY AGAIN */
#define FADE         40                /* spec 7.5: 40 px fades top and bottom */
#define LIST_X       90
#define LIST_Y       92
#define LIST_W       290
#define LIST_H_BTNS  206               /* down to y 298, the capsule at y 322 */
#define LIST_H_FULL  250               /* down to y 342 without buttons */
#define CAP_Y        322
#define THUMB_X      406
#define THUMB_Y      140
#define THUMB_H      120

static const char *TAG = "thread";

/* ---- the conversation ---- */

typedef struct { int index; char label[28]; } btn_t;
typedef struct {
    char id[48];
    bool agent;
    char *text;                        /* PSRAM, sanitised */
    time_t at;
    int nbtn;
    btn_t btn[3];
} msg_t;

static msg_t s_msg[MAX_MSG];
static int s_nmsg;
static char s_conv[65];                /* a job's conversation opened from Agents; "" = the followed one */
static enum { LOAD_NONE, LOAD_BUSY, LOAD_OK, LOAD_ERR } s_load;

/* ---- the push-to-talk utterance (Understood) ---- */

typedef enum { T_SENDING, T_NOT_SENT, T_HEARD, T_OK, T_EMPTY, T_FAILED, T_OFF, T_REJECTED, T_LATE } talk_t;   /* T_LATE: E105 */
static struct {
    bool active;
    char cmid[48], iid[48];
    talk_t st;
    char heard[400];
    char answer[1300];
    bool retry;                        /* TRY AGAIN can resend the kept audio */
    int64_t at_ms, changed_ms;
    time_t sent_at;
    char bound[66];                    /* E105: the notification this answers ("" = an ordinary push-to-talk) */
    char btitle[REPLY_TITLE_MAX * 4 + 8];   /* its title as sent, for a late reply */
} s_talk;
static char s_net_cmid[48]; static int s_net_ok = -1;   /* a transport result that beat thread_talk_begin */
/* E105: a bound press refused at audio_start gets its failed transcript while BOOT is still held, before Understood */
static char s_early_cmid[48], s_early_reason[32];
static char s_activity[96];
static bool s_running;

/* ---- the screen ---- */

typedef enum { M_CLOSED, M_THREAD, M_TALK } tmode_t;
static tmode_t s_mode = M_CLOSED;
static lv_obj_t *s_scr, *s_list, *s_cap;
static lv_obj_t *s_t_status, *s_t_heard, *s_t_who, *s_t_answer;
static bool s_dirty;
static char s_act_key[40]; static int s_act_seg = -1; static enum { A_NONE, A_SENDING, A_SENT, A_FAILED } s_act;
static char s_top[48];
static bool s_speaking;                /* the speaker mark beside PEGGY (spec 7.5) while a reply plays */

static int64_t now_ms(void) { return lv_tick_get(); }

/* The conversation on show: a job's (opened from Agents), else the followed one. */
static const char *cur_conv(void) { return s_conv[0] ? s_conv : intercom_conversation_id(); }

/* ---- text helpers ---- */

/* Keep what the Hanken subset can draw (Latin, Latin-1, Latin Extended-A, curly quotes, dashes, bullet,
   ellipsis, arrow); drop emoji and other scripts; strip Markdown emphasis; collapse blank-line runs. */
void thread_sanitize(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    int nl = 0;
    bool line_start = true;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < n;) {
        uint32_t cp; int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0x10000; len = 4; }
        else { p++; continue; }
        bool keep = (cp >= 0x20 && cp < 0x7F) || cp == '\n' || (cp >= 0xA0 && cp <= 0x17F) ||
                    cp == 0x2013 || cp == 0x2014 || cp == 0x2018 || cp == 0x2019 || cp == 0x201C ||
                    cp == 0x201D || cp == 0x2022 || cp == 0x2026 || cp == 0x2192;
        if (cp == '*' || cp == '`' || cp == '_' || (cp == '#' && line_start)) keep = false;
        if (cp == '\n') {
            if (++nl > 2 || o == 0) { p += len; continue; }
            line_start = true;
        } else if (keep) { nl = 0; if (cp != ' ') line_start = false; }
        if (keep) { memcpy(dst + o, p, len); o += len; }
        p += len;
    }
    while (o && (dst[o - 1] == '\n' || dst[o - 1] == ' ')) o--;
    dst[o] = 0;
}

static void hm(char *b, size_t n, time_t t)
{
    struct tm lt; localtime_r(&t, &lt);
    snprintf(b, n, "%d:%02d", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min);
}

static const msg_t *latest_agent(void)
{
    for (int i = s_nmsg - 1; i >= 0; i--) if (s_msg[i].agent) return &s_msg[i];
    return NULL;
}

/* ---- the message store ---- */

static void free_msg(msg_t *m) { free(m->text); m->text = NULL; }

static int by_time(const void *a, const void *b)
{
    const msg_t *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

/* Merge one message (page or live append): replace by id (buttons close once answered), keep time order. */
static void put_msg(const cJSON *m)
{
    const cJSON *id = cJSON_GetObjectItem(m, "id"), *role = cJSON_GetObjectItem(m, "role");
    const cJSON *text = cJSON_GetObjectItem(m, "text"), *at = cJSON_GetObjectItem(m, "created_at");
    if (!cJSON_IsString(id) || !cJSON_IsString(text)) return;
    msg_t *dst = NULL;
    for (int i = 0; i < s_nmsg; i++) if (!strcmp(s_msg[i].id, id->valuestring)) { dst = &s_msg[i]; free_msg(dst); break; }
    if (!dst) {
        if (s_nmsg == MAX_MSG) { free_msg(&s_msg[0]); memmove(&s_msg[0], &s_msg[1], sizeof(msg_t) * (MAX_MSG - 1)); s_nmsg--; }
        dst = &s_msg[s_nmsg++];
    }
    memset(dst, 0, sizeof *dst);
    strlcpy(dst->id, id->valuestring, sizeof dst->id);
    dst->agent = cJSON_IsString(role) && !strcmp(role->valuestring, "agent");
    dst->at = cJSON_IsString(at) ? dial_parse_iso(at->valuestring) : time(NULL);
    size_t n = strlen(text->valuestring) + 1;
    dst->text = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (dst->text) thread_sanitize(dst->text, n, text->valuestring);
    const cJSON *b;
    cJSON_ArrayForEach(b, cJSON_GetObjectItem(m, "buttons")) {
        if (dst->nbtn == 3) break;
        const cJSON *ix = cJSON_GetObjectItem(b, "index"), *lab = cJSON_GetObjectItem(b, "label");
        if (!cJSON_IsNumber(ix) || !cJSON_IsString(lab)) continue;
        dst->btn[dst->nbtn].index = ix->valueint;
        char caps[64]; dial_caps(caps, sizeof caps, lab->valuestring);
        if (strlen(caps) > 10) { ESP_LOGW(TAG, "button label cut: %s", caps); caps[10] = 0; }   /* spec 10: no ellipsis */
        strlcpy(dst->btn[dst->nbtn].label, caps, sizeof dst->btn[0].label);
        dst->nbtn++;
    }
    qsort(s_msg, s_nmsg, sizeof(msg_t), by_time);
}

/* The buttons on show: the newest message's, when it is the agent's. */
static const msg_t *button_msg(void)
{
    if (!s_nmsg) return NULL;
    const msg_t *m = &s_msg[s_nmsg - 1];
    return m->agent && m->nbtn ? m : NULL;
}

/* ---- drawing ---- */

static bool on_strip(lv_layer_t *l, const lv_area_t *a) { return lv_area_is_on(a, &l->_clip_area); }

/* One row of ground colour at `opa` over [x1, x2], split where the daylight shade circle (r 260) begins. At night
   the ground is plain black (no shade circle), so the whole row is black. */
static void ground_row(lv_layer_t *l, int y, int x1, int x2, lv_opa_t opa, float cx)
{
    lv_area_t row = { x1, y, x2, y };
    if (!on_strip(l, &row)) return;
    float dy = y - 233.0f, half = 260 * 260 - dy * dy > 0 ? sqrtf(260 * 260 - dy * dy) : -1;
    int s1 = half < 0 ? x2 + 1 : (int)lroundf(cx - half), s2 = half < 0 ? x2 : (int)lroundf(cx + half);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_opa = opa;
    bool night = dial_is_night();
    int cuts[4] = { x1, s1 < x1 ? x1 : s1 > x2 + 1 ? x2 + 1 : s1, s2 + 1 < x1 ? x1 : s2 + 1 > x2 + 1 ? x2 + 1 : s2 + 1, x2 + 1 };
    for (int k = 0; k < 3; k++) {
        if (cuts[k + 1] <= cuts[k]) continue;
        d.bg_color = lv_color_hex(k == 1 && !night ? COL_SHADED : dial_ground());
        lv_area_t a = { cuts[k], y, cuts[k + 1] - 1, y };
        lv_draw_rect(l, &d, &a);
    }
}

static void fades(lv_layer_t *l)
{
    if (!s_list || lv_obj_has_flag(s_list, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(s_list, &a);
    float cx = dial_crescent_cx();
    bool top = lv_obj_get_scroll_top(s_list) > 0, bottom = lv_obj_get_scroll_bottom(s_list) > 0;
    for (int i = 0; i < FADE; i++) {
        lv_opa_t opa = (lv_opa_t)(255 * (FADE - i) / FADE);
        if (top) ground_row(l, a.y1 + i, a.x1, a.x2, opa, cx);
        if (bottom) ground_row(l, a.y2 - i, a.x1, a.x2, opa, cx);
    }
    /* scroll thumb on the right rim */
    int st = lv_obj_get_scroll_top(s_list), sb = lv_obj_get_scroll_bottom(s_list);
    if (st + sb <= 0) return;
    int vis = lv_area_get_height(&a), total = vis + st + sb;
    int th = THUMB_H * vis / total; if (th < 18) th = 18;
    int ty = THUMB_Y + (THUMB_H - th) * st / (st + sb);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = 2; d.bg_color = lv_color_hex(COL_HAIR); d.bg_opa = 115;
    lv_area_t tr = { THUMB_X, THUMB_Y, THUMB_X + 3, THUMB_Y + THUMB_H - 1 };
    if (on_strip(l, &tr)) lv_draw_rect(l, &d, &tr);
    d.bg_color = lv_color_hex(COL_INK); d.bg_opa = LV_OPA_COVER;
    lv_area_t tb = { THUMB_X, ty, THUMB_X + 3, ty + th - 1 };
    if (on_strip(l, &tb)) lv_draw_rect(l, &d, &tb);
}

static void stroke(lv_layer_t *l, float x1, float y1, float x2, float y2, float w, uint32_t col, lv_opa_t opa)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col); d.width = w; d.opa = opa; d.round_start = 1; d.round_end = 1;
    d.p1 = (lv_point_precise_t){ x1, y1 }; d.p2 = (lv_point_precise_t){ x2, y2 };
    lv_draw_line(l, &d);
}

static void diamond(lv_layer_t *l, float cx, float cy, float r, uint32_t col)
{
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(col); d.opa = LV_OPA_COVER;
    d.p[0] = (lv_point_precise_t){ cx, cy - r }; d.p[1] = (lv_point_precise_t){ cx + r, cy }; d.p[2] = (lv_point_precise_t){ cx, cy + r };
    lv_draw_triangle(l, &d);
    d.p[1] = (lv_point_precise_t){ cx - r, cy };
    lv_draw_triangle(l, &d);
}

static bool talk_failed(void) { return s_talk.st == T_NOT_SENT || s_talk.st == T_FAILED || s_talk.st == T_OFF || s_talk.st == T_REJECTED || s_talk.st == T_LATE; }

static void draw_main(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    dial_draw_base(l, COL_ROSE);
    if (s_mode == M_THREAD) dial_draw_top_caption(l, s_top, COL_ROSE);
    if (s_mode == M_TALK) stroke(l, 178, 176, 288, 176, 1.5f, COL_HAIR, 179);   /* divider between heard and answer */
}

static void draw_post(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    if (s_mode == M_THREAD) fades(l);
    if (s_mode == M_TALK && s_t_status) {
        lv_area_t a; lv_obj_get_coords(s_t_status, &a);
        float y = (a.y1 + a.y2) / 2.0f;
        if (s_talk.st == T_OK || s_talk.st == T_HEARD) {             /* the check before UNDERSTOOD */
            float x = a.x1 - 24;
            stroke(l, x, y, x + 6, y + 6, 2.5f, COL_INK2, LV_OPA_COVER);
            stroke(l, x + 6, y + 6, x + 17, y - 6, 2.5f, COL_INK2, LV_OPA_COVER);
        } else if (talk_failed()) {
            diamond(l, a.x1 - 13, y, 5, COL_AMBER);
        }
    }
    if (s_mode == M_TALK && s_t_who && s_speaking) {                 /* speaker mark: board path M3 9v6h4l5 4V5L7 9z, M16 8.5a5 5 0 0 1 0 7 */
        lv_area_t a; lv_obj_get_coords(s_t_who, &a);
        float ox = a.x1 - 34, oy = (a.y1 + a.y2) / 2.0f - 12;
        static const float body[] = { 3, 9, 3, 15, 7, 15, 12, 19, 12, 5, 7, 9 };
        for (int i = 0; i < 6; i++) {
            int j = (i + 1) % 6;
            stroke(l, ox + body[2 * i], oy + body[2 * i + 1], ox + body[2 * j], oy + body[2 * j + 1], 2, COL_ROSE, LV_OPA_COVER);
        }
        lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
        d.color = lv_color_hex(COL_ROSE); d.width = 2; d.rounded = 1;
        d.center.x = (int)(ox + 12.4f); d.center.y = (int)(oy + 12); d.radius = 6; d.start_angle = 316; d.end_angle = 44;
        lv_draw_arc(l, &d);
    }
    if (s_mode == M_THREAD && s_act == A_FAILED && s_cap) {
        lv_area_t a; lv_obj_get_coords(s_cap, &a);
        diamond(l, a.x1 - 14, (a.y1 + a.y2) / 2.0f, 5, COL_AMBER);
    }
}

/* ---- building ---- */

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *f, uint32_t col, int space)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, t);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(o, space, 0);
    return o;
}

static int text_w(const char *t, const lv_font_t *f, int space)
{
    lv_point_t p; lv_text_get_size(&p, t, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

/* The action capsule (spec 5.6): one rounded capsule, segments fit their labels, the first one primary
   (rose) unless `ghost`, dividers 2 px `hair`, border 2 px (`amber` when `warn`). */
static lv_obj_t *capsule(const char *const *labels, int n, bool ghost, bool warn, lv_event_cb_t cb)
{
    int w[3], total = 0;
    for (int i = 0; i < n; i++) { w[i] = text_w(labels[i], f_label15, 1) + 48; if (w[i] < 94) w[i] = 94; total += w[i]; }
    if (n == 1 && total < 220) { w[0] = 220; total = 220; }
    if (total > 296) { for (int i = 0; i < n; i++) w[i] = w[i] * 296 / total; total = 296; }
    lv_obj_t *c = lv_obj_create(s_scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, total, n == 1 ? 56 : 60);
    lv_obj_set_pos(c, 233 - total / 2, n == 1 ? CAP_Y + 2 : CAP_Y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(warn ? COL_AMBER : COL_HAIR), 0);
    lv_obj_set_style_border_post(c, true, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    int x = 0;
    for (int i = 0; i < n; i++) {
        bool primary = i == 0 && !ghost;
        lv_obj_t *s = lv_obj_create(c);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, w[i], LV_PCT(100));
        lv_obj_set_pos(s, x, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(primary ? COL_ROSE : COL_INK), 0);
        lv_obj_set_style_bg_opa(s, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(COL_INK), LV_STATE_PRESSED);   /* press inverts within 100 ms */
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_STATE_PRESSED);
        if (i) { lv_obj_set_style_border_side(s, LV_BORDER_SIDE_LEFT, 0); lv_obj_set_style_border_width(s, 2, 0); lv_obj_set_style_border_color(s, lv_color_hex(COL_HAIR), 0); }
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s, 8);                                      /* hit target >= 76 px (spec 11.3) */
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *t = label(s, labels[i], f_label15, primary ? COL_ROSE_INK : COL_INK, 1);
        lv_obj_set_style_text_color(t, lv_color_hex(COL_ROSE_INK), LV_STATE_PRESSED);
        lv_obj_add_flag(t, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_center(t);
        x += w[i];
    }
    return c;
}

static void thread_btn_cb(lv_event_t *e);
static void talk_btn_cb(lv_event_t *e);
static bool was_swipe(void);
static void list_scroll_cb(lv_event_t *e)
{
    lv_area_t a = { LIST_X, LIST_Y, THUMB_X + 4, LIST_Y + LIST_H_FULL };   /* fades and thumb follow the scroll */
    lv_obj_invalidate_area(s_scr, &a);
}

/* A message body as one label per wrapped line (Hanken 500 20 / 27), in a column with no gaps. One
   wrapped label per message made every 20-row draw strip re-measure the message from its first letter,
   and the Thread scrolled at about 1 frame a second; single lines are measured once, here. */
static lv_obj_t *body_lines(const char *txt, uint32_t col)
{
    lv_obj_t *box = lv_obj_create(s_list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LIST_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_text_attributes_t at = { .letter_space = 0, .line_space = 0, .max_width = LIST_W, .text_flags = LV_TEXT_FLAG_NONE };
    uint32_t len = strlen(txt), pos = 0;
    char line[256];
    do {
        uint32_t n = lv_text_get_next_line(txt + pos, len - pos, f_body, NULL, &at);
        if (!n) break;
        uint32_t k = n < sizeof line ? n : sizeof line - 1;
        memcpy(line, txt + pos, k); line[k] = 0;
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r' || line[k - 1] == ' ')) line[--k] = 0;
        lv_obj_t *o = label(box, line, f_body, col, 0);
        lv_obj_set_height(o, 27);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        pos += n;
    } while (pos < len);
    return box;
}

static void build_thread(void)
{
    bool at_bottom = !s_list || lv_obj_get_scroll_bottom(s_list) <= 4;
    int keep_top = s_list ? lv_obj_get_scroll_top(s_list) : 0;
    lv_obj_clean(s_scr);
    s_list = s_cap = NULL;
    s_t_status = s_t_heard = s_t_who = s_t_answer = NULL;
    const msg_t *la = latest_agent();
    if (la) { char t[16]; hm(t, sizeof t, la->at); snprintf(s_top, sizeof s_top, "%s \xC2\xB7 %s", AGENT, t); }
    else snprintf(s_top, sizeof s_top, "THREAD");
    if (!s_nmsg) {                                            /* nothing to show yet */
        const char *w = s_load == LOAD_BUSY || s_load == LOAD_NONE ? "LOADING" : s_load == LOAD_ERR ? "COULD NOT LOAD" : "NO MESSAGES YET";
        lv_obj_t *o = label(s_scr, w, f_whisper, COL_INK2, 3);
        lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 210);
        lv_obj_t *h = label(s_scr, "Hold BOOT and ask", f_body, COL_INK2, 0);
        lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 240);
        lv_obj_invalidate(s_scr);
        return;
    }
    const msg_t *bm = button_msg();
    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, LIST_X, LIST_Y);
    lv_obj_set_size(s_list, LIST_W, bm ? LIST_H_BTNS : LIST_H_FULL);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(s_list, 8, 0);
    lv_obj_set_style_pad_bottom(s_list, 12, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_t *last = NULL;
    for (int i = 0; i < s_nmsg; i++) {
        const msg_t *m = &s_msg[i];
        char head[40], t[16]; hm(t, sizeof t, m->at);
        snprintf(head, sizeof head, "%s \xC2\xB7 %s", m->agent ? "PEGGY" : "YOU", t);
        lv_obj_t *h = label(s_list, head, f_meta13, m->agent ? COL_ROSE : COL_INK2, 2);
        if (i) lv_obj_set_style_margin_top(h, 14, 0);
        last = body_lines(m->text ? m->text : "", m->agent ? COL_INK : COL_INK2);
    }
    lv_obj_update_layout(s_list);
    if (at_bottom && last) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
    else lv_obj_scroll_to_y(s_list, keep_top, LV_ANIM_OFF);
    if (bm) {
        const char *labs[3];
        for (int i = 0; i < bm->nbtn; i++) labs[i] = bm->btn[i].label;
        if (s_act == A_SENT) {
            lv_obj_t *o = label(s_scr, "SENT", f_label15, COL_ROSE, 3);
            lv_obj_align(o, LV_ALIGN_TOP_MID, 0, CAP_Y + 20);
        } else if (s_act == A_FAILED) {
            const char *again[1] = { "TRY AGAIN" };
            s_cap = capsule(again, 1, true, true, thread_btn_cb);
        } else {
            s_cap = capsule(labs, bm->nbtn, false, false, thread_btn_cb);
            if (s_act == A_SENDING && s_act_seg >= 0 && s_act_seg < bm->nbtn) {
                lv_obj_t *seg = lv_obj_get_child(s_cap, s_act_seg);
                lv_label_set_text(lv_obj_get_child(seg, 0), "\xE2\x80\xA6");
            }
        }
    }
    lv_obj_invalidate(s_scr);
}

static const char *talk_word(void)
{
    int64_t age = now_ms() - s_talk.at_ms;
    switch (s_talk.st) {
    case T_SENDING: return age < NOACK_MS ? "SENDING" : "WAITING";
    case T_HEARD:   return age < NOACK_MS ? "SENDING" : "WAITING";
    case T_OK:      return s_talk.bound[0] ? "REPLY SENT" : "UNDERSTOOD";
    case T_LATE:    return "TOO LATE";
    case T_EMPTY:   return "DIDN\xE2\x80\x99T CATCH THAT";
    case T_OFF:     return "VOICE IS OFF";
    case T_REJECTED:return "NOT SENT";
    case T_NOT_SENT:
    case T_FAILED:  return "TRY AGAIN";
    }
    return "";
}

static void build_talk(void)
{
    lv_obj_clean(s_scr);
    s_list = s_cap = NULL;
    bool fail = talk_failed();
    s_t_status = label(s_scr, talk_word(), f_whisper, fail ? COL_AMBER : COL_INK2, 3);
    bool mark = fail || s_talk.st == T_OK || s_talk.st == T_HEARD;
    lv_obj_align(s_t_status, LV_ALIGN_TOP_MID, mark ? 14 : 0, 94);
    /* what was heard: italic on the board (no italic subset ships), quoted, two lines at most */
    char q[420];
    if (s_talk.heard[0]) snprintf(q, sizeof q, "\xE2\x80\x9C%s\xE2\x80\x9D", s_talk.heard);
    else snprintf(q, sizeof q, "%s", s_talk.st == T_EMPTY ? "Hold BOOT and speak" : s_talk.st == T_OFF ? "No speech-to-text on the bus" :
                  s_talk.st == T_LATE ? "The question had closed" : "");
    s_t_heard = label(s_scr, q, f_preview, COL_INK2, 0);
    lv_obj_set_width(s_t_heard, 280);
    lv_obj_set_style_text_align(s_t_heard, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_t_heard, 25 - lv_font_get_line_height(f_preview), 0);
    lv_label_set_long_mode(s_t_heard, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_t_heard, 50);
    lv_obj_align(s_t_heard, LV_ALIGN_TOP_MID, 0, 122);
    if (s_talk.st == T_OK || s_talk.answer[0]) {
        s_t_who = label(s_scr, "PEGGY", f_whisper, COL_ROSE, 3);
        lv_obj_align(s_t_who, LV_ALIGN_TOP_MID, s_speaking ? 18 : 0, 190);   /* board: the mark takes 36 px on the left */
        const lv_font_t *af = g_hanken23 ? g_hanken23 : f_headline;
        bool waiting = !s_talk.answer[0];
        const char *ans = waiting ? (s_running && s_activity[0] ? s_activity : "\xE2\x80\xA6") : s_talk.answer;
        s_t_answer = label(s_scr, ans, waiting ? f_preview : af, waiting ? COL_INK2 : COL_INK, 0);
        lv_obj_set_width(s_t_answer, 300);
        lv_obj_set_style_text_align(s_t_answer, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(s_t_answer, 30 - lv_font_get_line_height(af), 0);
        lv_label_set_long_mode(s_t_answer, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_t_answer, 90);
        lv_obj_align(s_t_answer, LV_ALIGN_TOP_MID, 0, 218);
    }
    if (s_talk.st == T_LATE) {                                      /* E105 contract 3.4 */
        const char *labs[2] = { "SEND TO PEGGY", "OPEN" };
        s_cap = capsule(labs, 2, true, true, talk_btn_cb);
    } else if (s_talk.retry && (s_talk.st == T_NOT_SENT || s_talk.st == T_FAILED)) {
        const char *labs[2] = { "TRY AGAIN", "OPEN" };
        s_cap = capsule(labs, 2, true, true, talk_btn_cb);
    } else if (s_talk.st != T_SENDING && s_talk.st != T_HEARD) {
        const char *labs[1] = { "OPEN" };
        s_cap = capsule(labs, 1, true, false, talk_btn_cb);
    }
    lv_obj_invalidate(s_scr);
}

static void rebuild(void)
{
    s_dirty = false;
    if (s_mode == M_THREAD) build_thread();
    else if (s_mode == M_TALK) build_talk();
}

/* ---- open / close (LVGL context) ---- */

static void conv_view(bool on)
{
    const char *id = cur_conv();
    if (!on || !id[0]) { intercom_set_view(NULL); return; }
    char v[96]; snprintf(v, sizeof v, "conversation:%s", id);
    intercom_set_view(v);
}

static void show(tmode_t m)
{
    s_mode = m;
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    conv_view(true);
    rebuild();
    lv_display_trigger_activity(NULL);
}

static void clear_msgs(void)
{
    for (int i = 0; i < s_nmsg; i++) free_msg(&s_msg[i]);
    s_nmsg = 0;
    s_load = LOAD_NONE;
}

static void try_fetch(void)
{
    if (s_load != LOAD_BUSY && intercom_fetch_conversation_id(cur_conv(), FETCH_N) && !s_nmsg) s_load = LOAD_BUSY;
}

/* Open the Thread on a conversation: a job's (from Agents) or, with NULL, the followed one. */
static void open_conv_lv(const char *conv)
{
    const char *want = conv && strcmp(conv, intercom_conversation_id()) ? conv : "";
    if (strcmp(want, s_conv)) { strlcpy(s_conv, want, sizeof s_conv); clear_msgs(); }   /* the held page is another chat's */
    s_act = A_NONE;
    try_fetch();
    if (s_mode != M_THREAD && s_list) { lv_obj_delete(s_list); s_list = NULL; }   /* land at the newest */
    show(M_THREAD);
    ESP_LOGI(TAG, "thread open (%d held%s)", s_nmsg, s_conv[0] ? ", job" : "");
}
static void open_thread_lv(void) { open_conv_lv(NULL); }
void thread_open_conv_lv(const char *conv_id) { open_conv_lv(conv_id); }
void thread_open_lv(void) { open_conv_lv(NULL); }

static void close_lv(void)
{
    if (s_mode == M_CLOSED) return;
    s_mode = M_CLOSED;
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_scr);
    s_list = s_cap = NULL;
    s_t_status = s_t_heard = s_t_who = s_t_answer = NULL;
    conv_view(false);
    if (s_conv[0]) { s_conv[0] = 0; clear_msgs(); }              /* the followed conversation is the default again */
    lv_obj_invalidate(lv_scr_act());
}

/* ---- input ---- */

static void thread_btn_cb(lv_event_t *e)
{
    if (was_swipe()) return;
    int seg = (int)(intptr_t)lv_event_get_user_data(e);
    const msg_t *bm = button_msg();
    if (!bm) return;
    if (s_act == A_FAILED) { s_act = A_NONE; rebuild(); return; }   /* TRY AGAIN: the buttons come back */
    if (seg >= bm->nbtn || s_act == A_SENDING) return;
    snprintf(s_act_key, sizeof s_act_key, "thr-%08lx", (unsigned long)esp_random());
    s_act_seg = seg;
    s_act = A_SENDING;
    ESP_LOGI(TAG, "button %s on %s", bm->btn[seg].label, bm->id);
    if (!intercom_send_conv_action(cur_conv(), bm->btn[seg].index, bm->id, s_act_key)) s_act = A_FAILED;
    rebuild();
}

/* TOO LATE: send what he said to the followed conversation instead. With the transcript, an ordinary `send` with
   (late reply to "<title>") in front (contract 3.4); a recording the bus dropped at audio_start has no transcript, so
   the kept audio goes again as an ordinary push-to-talk. Either way the Understood screen follows the new id. */
static void late_send_lv(void)
{
    char id[48];
    bool ok;
    if (s_talk.heard[0]) {
        snprintf(id, sizeof id, "late-%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
        char *body = heap_caps_malloc(REPLY_LATE_MAX + 1, MALLOC_CAP_SPIRAM);
        ok = body && reply_late_body(body, REPLY_LATE_MAX + 1, s_talk.btitle, s_talk.heard) > 0 && intercom_send_text(id, body);
        heap_caps_free(body);
    } else {
        snprintf(id, sizeof id, "ptt-%08lx-%lu", (unsigned long)esp_random(), (unsigned long)(time(NULL) % 100000));
        ok = ptt_resend_unbound(id);
    }
    ESP_LOGI(TAG, "%s: late reply as %s: %s", s_talk.cmid, id, ok ? "sending" : "not sent");
    if (!ok) { s_talk.st = T_NOT_SENT; s_talk.retry = false; s_talk.changed_ms = now_ms(); rebuild(); return; }
    strlcpy(s_talk.cmid, id, sizeof s_talk.cmid);
    s_talk.bound[0] = 0;
    s_talk.iid[0] = 0;
    s_talk.st = T_SENDING;
    s_talk.retry = !s_talk.heard[0];                                 /* resent audio may be tried again */
    s_talk.at_ms = s_talk.changed_ms = now_ms();
    s_talk.sent_at = time(NULL);
    rebuild();
}

static void talk_btn_cb(lv_event_t *e)
{
    if (was_swipe()) return;
    int seg = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_talk.st == T_LATE && seg == 0) { late_send_lv(); return; }
    bool retry = s_talk.retry && (s_talk.st == T_NOT_SENT || s_talk.st == T_FAILED);
    if (retry && seg == 0) {
        if (ptt_retry(s_talk.cmid)) {
            s_talk.st = T_SENDING; s_talk.at_ms = s_talk.changed_ms = now_ms();
            ESP_LOGI(TAG, "%s: try again", s_talk.cmid);
        } else {
            s_talk.retry = false;                                    /* the recording is gone */
        }
        rebuild();
        return;
    }
    open_thread_lv();                                                /* OPEN */
}

/* Called by the gesture router (earlier.c) after agents_swipe_lv. Spec 8.1: faces in a row, Day · Now ·
   Agents · Thread. Agents opens Thread on a swipe left; from here swipe right goes back to Agents (or Now
   from Understood), and there is nothing further right. */
bool thread_swipe_lv(lv_dir_t dir)
{
    if (s_mode == M_CLOSED) return false;                            /* Now and Agents are not ours */
    if (dir == LV_DIR_RIGHT) {
        bool back_to_agents = s_mode == M_THREAD;
        close_lv();
        if (back_to_agents) agents_open_lv();
    } else if (s_mode == M_TALK) {
        open_thread_lv();                                            /* Understood -> Thread */
    }
    return true;
}

/* LVGL 9.5 sends CLICKED after a swipe too: a tap handler ignores a press that was a gesture. */
static bool was_swipe(void) { return compact_press_was_drag(); }

static void tick_cb(lv_timer_t *t)
{
    if (s_mode == M_CLOSED) return;
    int64_t now = now_ms();
    bool sp = audio_out_playing();
    if (sp != s_speaking) { s_speaking = sp; if (s_mode == M_TALK) { s_dirty = true; s_talk.changed_ms = now; } }
    if (s_mode == M_TALK) {
        if ((s_talk.st == T_SENDING || s_talk.st == T_HEARD) && now - s_talk.at_ms >= GIVEUP_MS) {
            ESP_LOGW(TAG, "%s: no answer from the bus", s_talk.cmid);
            s_talk.st = T_FAILED; s_talk.changed_ms = now; s_dirty = true;
        }
        static int64_t last_word;                                    /* SENDING turns WAITING at 3 s */
        if ((s_talk.st == T_SENDING || s_talk.st == T_HEARD) && now - s_talk.at_ms >= NOACK_MS && last_word < s_talk.at_ms + NOACK_MS) {
            last_word = now; s_dirty = true;
        }
    }
    /* 60 s untouched -> Now; Understood stays while its answer is still coming */
    int64_t quiet = now - (s_mode == M_TALK ? s_talk.changed_ms : 0);
    if (lv_display_get_inactive_time(NULL) >= IDLE_MS && quiet >= IDLE_MS) { close_lv(); return; }
    static int64_t last_try;                                         /* a fetch refused (socket not up, two busy): try again */
    if (s_mode == M_THREAD && s_load == LOAD_NONE && !s_nmsg && now - last_try >= 1000) { last_try = now; try_fetch(); }
    if (s_dirty) rebuild();
}

/* ---- public ---- */

void thread_init(void)
{
    bsp_display_lock(-1);
    s_scr = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 466, 466);
    lv_obj_center(s_scr);
    dial_use_ground(s_scr);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_scr, draw_main, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(s_scr, draw_post, LV_EVENT_DRAW_POST_END, NULL);
    lv_timer_create(tick_cb, 100, NULL);
    bsp_display_unlock();
}

void thread_close(void)
{
    bsp_display_lock(-1);
    close_lv();
    bsp_display_unlock();
}

bool thread_is_open(void) { return s_mode != M_CLOSED; }

static void talk_begin(const char *cmid, const char *heard, const char *bound, const char *btitle);

void thread_talk_begin(const char *cmid, const char *heard) { talk_begin(cmid, heard, NULL, NULL); }

void thread_talk_begin_reply(const char *cmid, const char *heard, const char *notification_id, const char *title)
{
    talk_begin(cmid, heard, notification_id, title);
}

static void talk_begin(const char *cmid, const char *heard, const char *bound, const char *btitle)
{
    earlier_close();                                                 /* Understood takes the glass */
    setscreen_close();
    dayview_close();
    agents_close();
    bsp_display_lock(-1);
    if (s_conv[0]) { s_conv[0] = 0; clear_msgs(); }                  /* push-to-talk speaks to the followed chat */
    memset(&s_talk, 0, sizeof s_talk);
    s_talk.active = true;
    strlcpy(s_talk.cmid, cmid, sizeof s_talk.cmid);
    if (heard) thread_sanitize(s_talk.heard, sizeof s_talk.heard, heard);
    s_talk.st = T_SENDING;
    s_talk.retry = true;
    s_talk.at_ms = s_talk.changed_ms = now_ms();
    s_talk.sent_at = time(NULL);
    if (bound) strlcpy(s_talk.bound, bound, sizeof s_talk.bound);
    if (btitle) strlcpy(s_talk.btitle, btitle, sizeof s_talk.btitle);
    if (bound && s_early_cmid[0] && !strcmp(s_early_cmid, cmid)) {   /* refused while he was still speaking */
        reply_outcome_t o = reply_outcome(s_early_reason);
        s_talk.st = o == OUT_LATE ? T_LATE : T_REJECTED;
        s_talk.retry = false;
        ESP_LOGW(TAG, "%s: reply refused at audio_start (%s)", cmid, s_early_reason);
    }
    s_early_cmid[0] = 0;
    if (s_net_ok >= 0 && !strcmp(s_net_cmid, cmid) && !s_net_ok) s_talk.st = T_NOT_SENT;   /* it failed before we got here */
    s_net_ok = -1;
    show(M_TALK);
    bsp_display_unlock();
}

void thread_talk_net(const char *cmid, bool ok)
{
    bsp_display_lock(-1);
    if (!s_talk.active || strcmp(cmid, s_talk.cmid)) {                /* thread_talk_begin is still on its way */
        strlcpy(s_net_cmid, cmid, sizeof s_net_cmid); s_net_ok = ok;
    } else if (!ok && s_talk.st == T_SENDING) {
        s_talk.st = T_NOT_SENT; s_talk.changed_ms = now_ms();
        if (s_mode == M_TALK) s_dirty = true;
    }
    bsp_display_unlock();
}

void thread_on_transcript(const char *cmid, const char *status, const char *text, const char *reason)
{
    bsp_display_lock(-1);
    if (!(s_talk.active && !strcmp(cmid, s_talk.cmid)) && strcmp(status, "ok") && strcmp(status, "empty")) {
        reply_outcome_t o = reply_outcome(reason);
        if (o == OUT_LATE || o == OUT_NOT_ALLOWED) {               /* only E105 codes; kept for talk_begin */
            strlcpy(s_early_cmid, cmid, sizeof s_early_cmid);
            strlcpy(s_early_reason, reason, sizeof s_early_reason);
        }
    }
    if (s_talk.active && !strcmp(cmid, s_talk.cmid)) {
        if (!strcmp(status, "ok")) {
            thread_sanitize(s_talk.heard, sizeof s_talk.heard, text);
            if (s_talk.st == T_SENDING) s_talk.st = T_HEARD;
            s_talk.retry = false;                                     /* the bus has it; the send is idempotent anyway */
        } else if (!strcmp(status, "empty")) {
            s_talk.st = T_EMPTY; s_talk.heard[0] = 0; s_talk.retry = false;
        } else if (s_talk.bound[0] && reply_outcome(reason) == OUT_LATE) {   /* E105: refused at audio_start */
            s_talk.st = T_LATE; s_talk.heard[0] = 0; s_talk.retry = false;
            ESP_LOGW(TAG, "%s: reply refused (%s)", cmid, reason);
        } else if (s_talk.bound[0] && reply_outcome(reason) == OUT_NOT_ALLOWED) {
            s_talk.st = T_REJECTED; s_talk.retry = false;
            ESP_LOGW(TAG, "%s: reply refused (%s)", cmid, reason);
        } else {
            s_talk.st = reason && !strcmp(reason, "unavailable") ? T_OFF : T_FAILED;
            s_talk.retry = s_talk.st == T_FAILED;
        }
        s_talk.changed_ms = now_ms();
        if (s_mode == M_TALK) s_dirty = true;
    }
    bsp_display_unlock();
}

void thread_on_ack(const char *cmid, const char *status, const char *reason, const char *interaction_id)
{
    bsp_display_lock(-1);
    if (s_talk.active && !strcmp(cmid, s_talk.cmid)) {
        if (!strcmp(status, "queued") || !strcmp(status, "command")) {
            s_talk.st = T_OK;
            if (interaction_id) strlcpy(s_talk.iid, interaction_id, sizeof s_talk.iid);
        } else if (s_talk.bound[0] && reply_outcome(reason) == OUT_LATE) {   /* E105: closed while he spoke */
            s_talk.st = T_LATE;
            ESP_LOGW(TAG, "%s: reply refused at send (%s)", cmid, reason);
        } else {
            s_talk.st = T_REJECTED;
            ESP_LOGW(TAG, "%s rejected: %s", cmid, reason ? reason : "?");
        }
        s_talk.retry = false;
        s_talk.changed_ms = now_ms();
        if (s_mode == M_TALK) s_dirty = true;
    }
    bsp_display_unlock();
}

/* The agent's answer in this interaction (v1 `reply`): the body when short, else its glance line. */
void thread_on_reply(const cJSON *d)
{
    const cJSON *iid = cJSON_GetObjectItem(d, "interaction_id"), *body = cJSON_GetObjectItem(d, "body");
    const cJSON *glance = cJSON_GetObjectItem(d, "glance"), *at = cJSON_GetObjectItem(d, "created_at");
    bsp_display_lock(-1);
    bool mine = s_talk.active && s_talk.st == T_OK && cJSON_IsString(iid) && !strcmp(iid->valuestring, s_talk.iid);
    time_t t = cJSON_IsString(at) ? dial_parse_iso(at->valuestring) : 0;
    if (mine && t && t + 10 < s_talk.sent_at) mine = false;          /* a replay of an older answer */
    if (mine) {
        const char *b = cJSON_IsString(body) ? body->valuestring : "", *g = cJSON_IsString(glance) ? glance->valuestring : "";
        thread_sanitize(s_talk.answer, sizeof s_talk.answer, strlen(b) <= 120 || !g[0] ? b : g);
        s_talk.changed_ms = now_ms();
        if (s_mode == M_TALK) s_dirty = true;
        lv_display_trigger_activity(NULL);                           /* brighten from dim to show it */
    }
    bsp_display_unlock();
}

void thread_on_conv_append(const char *conv_id, const cJSON *message)
{
    if (strcmp(conv_id, cur_conv())) return;
    bsp_display_lock(-1);
    put_msg(message);
    if (s_act == A_SENT || s_act == A_FAILED) s_act = A_NONE;        /* a new message: fresh buttons */
    if (s_mode == M_THREAD) { s_dirty = true; lv_display_trigger_activity(NULL); }
    bsp_display_unlock();
}

void thread_on_conversation(const char *conv_id, const cJSON *page, int http_status)
{
    bsp_display_lock(-1);
    if (!strcmp(conv_id, cur_conv())) {                               /* a page for a chat we left is dropped */
        if (page) {
            const cJSON *m;
            cJSON_ArrayForEach(m, cJSON_GetObjectItem(page, "messages")) put_msg(m);
            s_load = LOAD_OK;
        } else {
            s_load = s_nmsg ? LOAD_OK : LOAD_ERR;
            ESP_LOGW(TAG, "conversation fetch failed (HTTP %d)", http_status);
        }
        if (s_mode == M_THREAD) s_dirty = true;
    }
    bsp_display_unlock();
}

void thread_on_action_result(const char *key, const char *status, const char *reason)
{
    bsp_display_lock(-1);
    if (s_act == A_SENDING && !strcmp(key, s_act_key)) {
        bool ok = !strcmp(status, "done") || !strcmp(status, "already_done");
        s_act = ok ? A_SENT : A_FAILED;
        if (!ok) ESP_LOGW(TAG, "button failed: %s %s", status, reason ? reason : "");
        if (s_mode == M_THREAD) s_dirty = true;
    }
    bsp_display_unlock();
}

void thread_on_activity(const char *state, const char *status_text)
{
    bsp_display_lock(-1);
    s_running = state && strcmp(state, "idle");
    if (status_text) thread_sanitize(s_activity, sizeof s_activity, status_text); else s_activity[0] = 0;
    if (s_mode == M_TALK && s_talk.st == T_OK && !s_talk.answer[0]) { s_talk.changed_ms = now_ms(); s_dirty = true; }
    bsp_display_unlock();
}

/* Testing (POST /debug/ui): open the Thread, or scroll it by dy px with the usual animation. */
bool thread_debug_lv(const char *what, int dy)
{
    if (!strcmp(what, "thread")) { open_thread_lv(); return true; }
    if (!strcmp(what, "now")) { close_lv(); agents_close_lv(); return true; }
    if (!strcmp(what, "agents")) { close_lv(); agents_open_lv(); return true; }
    if (!strcmp(what, "scroll") && s_mode == M_THREAD && s_list) { lv_obj_scroll_by(s_list, 0, dy, LV_ANIM_ON); return true; }
    return false;
}
