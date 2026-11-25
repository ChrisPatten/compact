#include "pill_list.h"
#include "src/misc/lv_area_private.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "dial.h"
#include "fonts.h"

#define COL_RAISE  0x4A3752
#define COL_LINE   0x6B5575
#define COL_HAIR   0x8B7098
#define COL_INK    0xF8ECE8
#define COL_INK2   0xCDB5C6
#define COL_SHADED 0x24182A          /* glass under the 50% daylight shade (day ground only) */
#define FADE       40
#define PL_MAX     32

struct pill_list {
    pill_list_cfg_t c;
    lv_obj_t *list;
    int n;
    lv_obj_t *row[PL_MAX];
    pill_row_t meta[PL_MAX];         /* style, flags; the strings live in the labels */
};

static const int PAD_TOP = 4, PAD_BOT = 8;

static void list_deleted(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void scroll_cb(lv_event_t *e)
{
    pill_list_t *p = lv_event_get_user_data(e);
    lv_area_t a = { p->c.x, p->c.y, p->c.x + p->c.w, p->c.y + p->c.h };   /* fades and icons follow the scroll */
    lv_obj_invalidate_area(lv_obj_get_parent(p->list), &a);
}

static void row_cb(lv_event_t *e)
{
    if (compact_press_was_drag()) return;                          /* LVGL 9.5 sends CLICKED after a swipe too */
    pill_list_t *p = lv_obj_get_user_data(lv_event_get_target(e));
    if (!p || !p->c.tap) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    p->c.tap(i, p->c.ud);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, int sp)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_letter_space(o, sp, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(o, lv_font_get_line_height(f));
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static int row_width(const pill_list_t *p, int i)
{
    int w = p->c.row_w;
    if (p->c.width == PILL_W_CHORD) {
        float top = p->c.y + PAD_TOP + i * (p->c.row_h + p->c.gap), bot = top + p->c.row_h;
        float d = fmaxf(fabsf(top - 233.0f), fabsf(bot - 233.0f));
        float chord = d < 233 ? 2 * sqrtf(233.0f * 233.0f - d * d) - 16 : 0;
        if (chord < w) w = chord < 140 ? 140 : (int)chord;
    }
    return w;
}

pill_list_t *pill_list_create(lv_obj_t *parent, const pill_list_cfg_t *cfg)
{
    pill_list_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->c = *cfg;
    lv_obj_t *o = p->list = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, cfg->x, cfg->y);
    lv_obj_set_size(o, cfg->w, cfg->h);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(o, PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(o, PAD_BOT, 0);
    lv_obj_set_style_pad_row(o, cfg->gap, 0);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ELASTIC);           /* no bounce past the ends */
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(o, scroll_cb, LV_EVENT_SCROLL, p);
    lv_obj_add_event_cb(o, list_deleted, LV_EVENT_DELETE, p);
    return p;
}

void pill_list_set(pill_list_t *p, int i, const pill_row_t *s)
{
    if (!p || i < 0 || i > p->n || i >= PL_MAX) return;
    lv_obj_t *r;
    bool fresh = i == p->n;
    int tx = 22 + (p->c.icon ? p->c.icon + 14 : 0);
    int w = row_width(p, i);
    if (fresh) {
        r = p->row[i] = lv_obj_create(p->list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, w, p->c.row_h);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(COL_RAISE), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r, 1, 0);                  /* 1.5 px `line` */
        lv_obj_set_style_border_color(r, lv_color_hex(COL_LINE), 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_user_data(r, p);
        label(r, f_whisper, 1);                                  /* child 0: title */
        label(r, f_detail15, 0);                                 /* child 1: sub */
        label(r, f_whisper, 1);                                  /* child 2: value */
        p->n++;
    } else {
        r = p->row[i];
    }
    p->meta[i] = *s;
    p->meta[i].title = p->meta[i].sub = p->meta[i].value = NULL;
    lv_obj_t *t = lv_obj_get_child(r, 0), *d = lv_obj_get_child(r, 1), *v = lv_obj_get_child(r, 2);
    int vw = 0;
    if (s->value && s->value[0]) {
        lv_point_t pt; lv_text_get_size(&pt, s->value, f_whisper, 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        vw = pt.x + 1;
        lv_label_set_text(v, s->value);
        lv_obj_set_width(v, vw);
        lv_obj_set_style_text_color(v, lv_color_hex(COL_INK2), 0);
        lv_obj_set_pos(v, w - 22 - vw, (p->c.row_h - lv_font_get_line_height(f_whisper)) / 2 - 1);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
    }
    int tw = w - tx - 22 - (vw ? vw + 10 : 0);
    bool two = s->sub && s->sub[0];
    int th = lv_font_get_line_height(f_whisper), dh = lv_font_get_line_height(f_detail15);
    int ty = two ? (p->c.row_h - (th + 2 + dh)) / 2 - 1 : (p->c.row_h - th) / 2 - 1;
    lv_label_set_text(t, s->title ? s->title : "");
    lv_obj_set_style_text_color(t, lv_color_hex(s->title_col ? s->title_col : COL_INK), 0);
    lv_obj_set_pos(t, tx, ty);
    lv_obj_set_width(t, tw);
    if (two) {
        lv_label_set_text(d, s->sub);
        lv_obj_set_style_text_color(d, lv_color_hex(s->sub_col ? s->sub_col : COL_INK2), 0);
        lv_obj_set_pos(d, tx, ty + th + 2);
        lv_obj_set_width(d, tw);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    if (s->tappable && p->c.tap) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(r, 8);                         /* hit area 82 high (spec 5.8) */
        lv_obj_set_style_bg_color(r, lv_color_hex(COL_HAIR), LV_STATE_PRESSED);
        if (fresh) lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    } else {
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_invalidate(r);
}

void pill_list_trim(pill_list_t *p, int count)
{
    if (!p || count < 0) return;
    while (p->n > count) lv_obj_delete(p->row[--p->n]);
}

int pill_list_count(const pill_list_t *p) { return p ? p->n : 0; }
int pill_list_scroll_y(const pill_list_t *p) { return p ? (int)lv_obj_get_scroll_top(p->list) : 0; }
bool pill_list_overflows(const pill_list_t *p)
{
    return p && (lv_obj_get_scroll_top(p->list) > 0 || lv_obj_get_scroll_bottom(p->list) > 0);
}

lv_obj_t *pill_list_row_obj(const pill_list_t *p, int i) { return p && i >= 0 && i < p->n ? p->row[i] : NULL; }

bool pill_list_contains(const pill_list_t *p, const lv_obj_t *obj)
{
    for (; p && obj; obj = lv_obj_get_parent(obj)) if (obj == p->list) return true;
    return false;
}

void pill_list_scroll_to(pill_list_t *p, int y)
{
    if (!p) return;
    lv_obj_update_layout(p->list);
    lv_obj_scroll_to_y(p->list, y, LV_ANIM_OFF);
}

void pill_list_scroll_by(pill_list_t *p, int dy)
{
    if (!p) return;                                              /* dy > 0 reveals lower rows; clamped to the ends */
    int top = (int)lv_obj_get_scroll_top(p->list), bot = (int)lv_obj_get_scroll_bottom(p->list);
    if (dy > bot) dy = bot;
    if (dy < -top) dy = -top;
    lv_obj_scroll_by(p->list, 0, -dy, LV_ANIM_ON);
}

static void icon_box(const pill_list_t *p, int i, lv_area_t *a)
{
    lv_area_t r; lv_obj_get_coords(p->row[i], &r);
    int x = r.x1 + 22, y = r.y1 + (p->c.row_h - p->c.icon) / 2;
    a->x1 = x - 2; a->y1 = y - 2; a->x2 = x + p->c.icon + 2; a->y2 = y + p->c.icon + 2;
}

void pill_list_tick(pill_list_t *p)
{
    if (!p || !p->c.icon_fn) return;
    for (int i = 0; i < p->n; i++) {
        if (!p->meta[i].animated) continue;
        lv_area_t a; icon_box(p, i, &a);
        lv_obj_invalidate_area(lv_obj_get_parent(p->list), &a);
    }
}

/* One row of ground colour at `opa` over [x1, x2], split where the daylight shade circle (r 260) begins. */
static void ground_row(lv_layer_t *l, int y, int x1, int x2, lv_opa_t opa, float cx)
{
    lv_area_t row = { x1, y, x2, y };
    if (!lv_area_is_on(&row, &l->_clip_area)) return;
    float dy = y - 233.0f, h2 = 260 * 260 - dy * dy, half = h2 > 0 ? sqrtf(h2) : -1;
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

/* 40 px fades into the ground at the top and/or bottom of a scroll viewport (spec 7.5), drawn over the rows. */
void pill_fades_draw(lv_layer_t *l, const lv_area_t *vp, bool top, bool bottom)
{
    float cx = dial_crescent_cx();
    for (int i = 0; i < FADE; i++) {
        lv_opa_t opa = (lv_opa_t)(255 * (FADE - i) / FADE);
        if (top) ground_row(l, vp->y1 + i, vp->x1, vp->x2, opa, cx);
        if (bottom) ground_row(l, vp->y2 - i, vp->x1, vp->x2, opa, cx);
    }
}

void pill_list_draw_post(pill_list_t *p, lv_layer_t *l)
{
    if (!p || lv_obj_has_flag(p->list, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t vp; lv_obj_get_coords(p->list, &vp);
    if (p->c.icon_fn) {
        lv_area_t saved = l->_clip_area, clip;
        if (lv_area_intersect(&clip, &saved, &vp)) {
            l->_clip_area = clip;
            for (int i = 0; i < p->n; i++) {
                lv_area_t r; lv_obj_get_coords(p->row[i], &r);
                if (r.y2 < vp.y1 || r.y1 > vp.y2) continue;
                p->c.icon_fn(l, r.x1 + 22, r.y1 + (p->c.row_h - p->c.icon) / 2, p->meta[i].style, p->c.ud);
            }
        }
        l->_clip_area = saved;
    }
    pill_fades_draw(l, &vp, lv_obj_get_scroll_top(p->list) > 0, lv_obj_get_scroll_bottom(p->list) > 0);
}
