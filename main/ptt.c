#include "ptt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "psram_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_pm.h"
#include "bsp/esp-bsp.h"
#include "fonts.h"
#include "dial.h"
#include "audio_in.h"
#include "intercom_ws.h"
#include "thread.h"
#include "decision.h"
#include "reply_logic.h"

/* Plum Dusk tokens (spec 3.1) */
#define COL_HAIR     0x8B7098
#define COL_RIM      0x9E8AA3
#define COL_INK      0xF8ECE8
#define COL_INK2     0xCDB5C6
#define COL_ROSE     0xF5A99B
#define COL_PERI     0xA9B8FF
#define COL_PERI_INK 0x1D1A33
#define COL_AMBER    0xF7C95C

#define MAX_S        60                               /* spec: bounded utterance; the bus keeps the same limit */
#define MAX_SAMPLES  (MAX_S * AUDIO_IN_RATE)
#define READ_SAMPLES 320                              /* 20 ms per microphone read */
#define CHUNK        1600                             /* 100 ms per binary frame (3,200 bytes) */
#define BREATH_MS    1400                             /* spec 9: listening breathes over 1.4 s */
#define ORB_Y        150

static const char *TAG = "ptt";

lv_font_t *g_hanken22, *g_hanken23, *g_lexend28;

/* ---- state shared by the boot task, the recorder, the sender and LVGL ---- */

typedef enum { P_IDLE, P_ARMED, P_STREAM, P_FLUSH, P_CANCEL, P_DISARM, P_RETRY, P_PREP } phase_t;   /* P_PREP: a resend being set up */
static volatile phase_t s_phase = P_IDLE;
static int16_t *s_buf;                                /* PSRAM, MAX_SAMPLES */
static volatile size_t s_len;                         /* samples recorded */
static volatile bool s_rec_done = true, s_mic_failed;
static char s_cmid[48];                               /* the utterance being recorded or sent */
static char s_buf_cmid[48];                           /* whose audio s_buf holds (for TRY AGAIN) */
/* E105: the notification this utterance answers ("" = unbound), decided at the hold (decision_reply_bind) */
static char s_bind[66], s_bind_caps[84], s_bind_raw[REPLY_TITLE_MAX * 4 + 8];
static reply_bind_t s_bind_kind = BIND_OFF;
static char s_buf_bind[66];                           /* the binding of the kept audio (TRY AGAIN resends it bound) */
static TaskHandle_t s_rec_task, s_send_task;
static esp_pm_lock_handle_t s_pm;
static volatile int s_level;                          /* recent peak, 0-32767 */

/* ---- the Listening screen (LVGL top layer) ---- */

static lv_obj_t *s_root, *s_lab_state, *s_lab_text, *s_lab_time, *s_lab_hint, *s_lab_bind;
static lv_timer_t *s_timer;
static uint32_t s_hold_at;                            /* lv_tick at hold */
static int s_shown_s = -1;
static float s_breath = 1;
static char s_partial[200];
static bool s_partial_dirty;
static enum { H_SEND, H_OFFLINE, H_MIC, H_BUSY } s_hint_kind;

static void mk_fonts(void)
{
    extern const uint8_t hanken_600_ttf_start[] asm("_binary_hanken_600_ttf_start");
    extern const uint8_t hanken_600_ttf_end[] asm("_binary_hanken_600_ttf_end");
    extern const uint8_t lexend_200_ttf_start[] asm("_binary_lexend_200_ttf_start");
    extern const uint8_t lexend_200_ttf_end[] asm("_binary_lexend_200_ttf_end");
    g_hanken22 = lv_tiny_ttf_create_data_ex(hanken_600_ttf_start, hanken_600_ttf_end - hanken_600_ttf_start, 22, LV_FONT_KERNING_NORMAL, 192);
    g_hanken23 = lv_tiny_ttf_create_data_ex(hanken_600_ttf_start, hanken_600_ttf_end - hanken_600_ttf_start, 23, LV_FONT_KERNING_NORMAL, 192);
    g_lexend28 = lv_tiny_ttf_create_data_ex(lexend_200_ttf_start, lexend_200_ttf_end - lexend_200_ttf_start, 28, LV_FONT_KERNING_NORMAL, 16);
    if (!g_hanken22 || !g_hanken23 || !g_lexend28) ESP_LOGE(TAG, "talk fonts failed");
}

/* A filled diamond (◆ is not in the Lexend subset): the amber mark that always travels with a word. */
static void diamond(lv_layer_t *l, float cx, float cy, float r, uint32_t col)
{
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(col); d.opa = LV_OPA_COVER;
    d.p[0] = (lv_point_precise_t){ cx, cy - r }; d.p[1] = (lv_point_precise_t){ cx + r, cy }; d.p[2] = (lv_point_precise_t){ cx, cy + r };
    lv_draw_triangle(l, &d);
    d.p[1] = (lv_point_precise_t){ cx - r, cy };
    lv_draw_triangle(l, &d);
}

static void line(lv_layer_t *l, float x1, float y1, float x2, float y2, float w, uint32_t col)
{
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col); d.width = w; d.round_start = 1; d.round_end = 1;
    d.p1 = (lv_point_precise_t){ x1, y1 }; d.p2 = (lv_point_precise_t){ x2, y2 };
    lv_draw_line(l, &d);
}

static void ring_opa(lv_layer_t *l, float cx, float cy, float r, float w, uint32_t col, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.center.x = (int)cx; d.center.y = (int)cy; d.radius = (int)(r + w / 2); d.width = (int)w;
    d.start_angle = 0; d.end_angle = 360; d.color = lv_color_hex(col); d.opa = opa;
    lv_draw_arc(l, &d);
}

static void draw(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    time_t now = dial_now();
    dial_draw_crescent(l);
    dial_draw_rim_dots(l);                                /* the dotted 12-hour rim, r 214 */
    dial_draw_arc(l, 214, 8, 75, 105, COL_PERI);          /* rim tab where the finger is (2:30 to 3:30) */
    if (now > 1700000000) {                               /* now dot, moon parked at 12 o'clock (spec 5.2) */
        float a = dial_clock_deg(now) * 3.14159265f / 180, nx = 233 + 214 * sinf(a), ny = 233 - 214 * cosf(a);
        ring_opa(l, nx, ny, 13, 1, COL_HAIR, LV_OPA_60);
        dial_draw_disc(l, nx, ny - 13, 3, COL_ROSE);
        dial_draw_disc(l, nx, ny, 5, COL_INK);
    }
    /* the peri orb: disc r 36 with breathing rings r 52 (60%) and r 66 (30%) */
    ring_opa(l, 233, ORB_Y, 66, 2, COL_PERI, (lv_opa_t)(76 * s_breath));
    ring_opa(l, 233, ORB_Y, 52, 2, COL_PERI, (lv_opa_t)(153 * s_breath));
    dial_draw_disc(l, 233, ORB_Y, 36, COL_PERI);
    line(l, 233, ORB_Y - 16, 233, ORB_Y + 16, 3.5f, COL_PERI_INK);
    line(l, 221, ORB_Y - 9, 221, ORB_Y + 9, 3.5f, COL_PERI_INK);
    line(l, 245, ORB_Y - 9, 245, ORB_Y + 9, 3.5f, COL_PERI_INK);
    if (s_hint_kind != H_SEND && s_lab_hint) {            /* amber diamond before the warning */
        lv_area_t a; lv_obj_get_coords(s_lab_hint, &a);
        diamond(l, a.x1 - 12, (a.y1 + a.y2) / 2.0f, 5, COL_AMBER);
    }
}

static lv_obj_t *label(const char *t, const lv_font_t *f, uint32_t col, int y, int space)
{
    lv_obj_t *o = lv_label_create(s_root);
    lv_label_set_text(o, t);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_text_letter_space(o, space, 0);
    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, y);
    return o;
}

static void set_hint(void)
{
    if (!s_lab_hint) return;
    const char *t = s_hint_kind == H_OFFLINE ? "OFFLINE \xC2\xB7 NOT SENT" : s_hint_kind == H_MIC ? "MICROPHONE FAILED"
                  : s_hint_kind == H_BUSY ? "STILL SENDING" : "RELEASE TO SEND";
    lv_label_set_text(s_lab_hint, t);
    lv_obj_set_style_text_color(s_lab_hint, lv_color_hex(s_hint_kind == H_SEND ? COL_INK2 : COL_AMBER), 0);
    lv_obj_align(s_lab_hint, LV_ALIGN_TOP_MID, s_hint_kind == H_SEND ? 0 : 9, 366);
    lv_obj_invalidate(s_root);
}

/* The live transcript: one or two lines; when it runs longer, its tail with a leading ellipsis. */
static void show_partial(void)
{
    if (!s_lab_text) return;
    const char *t = s_partial;
    char buf[200];
    size_t n = strlen(t);
    if (n > 52) {
        const char *p = t + n - 50;
        while (*p && *p != ' ') p++;
        snprintf(buf, sizeof buf, "\xE2\x80\xA6%s", *p ? p : t + n - 50);
        t = buf;
    }
    lv_label_set_text(s_lab_text, t);
}

static void tick(lv_timer_t *t)
{
    if (!s_root) return;
    lv_display_trigger_activity(NULL);                    /* holding is not idle: no dim, no sleep */
    uint32_t ms = lv_tick_elaps(s_hold_at);
    float b = 0.675f - 0.325f * cosf(2 * 3.14159265f * (ms % BREATH_MS) / BREATH_MS);   /* 0.35 .. 1 */
    if (fabsf(b - s_breath) > 0.04f) {
        s_breath = b;
        lv_area_t a = { 233 - 70, ORB_Y - 70, 233 + 70, ORB_Y + 70 };
        lv_obj_invalidate_area(s_root, &a);
    }
    int sec = (int)((ms + 400) / 1000);                    /* the recording began at the press, ~0.4 s earlier */
    if (sec != s_shown_s) {
        s_shown_s = sec;
        char b2[16]; snprintf(b2, sizeof b2, "%d:%02d", sec / 60, sec % 60);
        lv_label_set_text(s_lab_time, b2);
        if (s_hint_kind == H_SEND && sec >= MAX_S - 5) {
            char h[24]; snprintf(h, sizeof h, "%d S LEFT", MAX_S - sec > 0 ? MAX_S - sec : 0);
            lv_label_set_text(s_lab_hint, h);
        }
    }
    if (s_partial_dirty) { s_partial_dirty = false; show_partial(); }
}

static void close_lv(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_root) { lv_obj_delete(s_root); s_root = NULL; }
    s_lab_state = s_lab_text = s_lab_time = s_lab_hint = s_lab_bind = NULL;
}

static void open_lv(void)
{
    close_lv();
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 466, 466);
    dial_use_ground(s_root);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);         /* swallow touches meant for what is below */
    lv_obj_add_event_cb(s_root, draw, LV_EVENT_DRAW_MAIN_END, NULL);
    s_lab_state = label(s_bind_kind == BIND_YES ? "ANSWERING" : "LISTENING", f_whisper, COL_PERI, 228, 3);
    if (s_bind_kind == BIND_YES || s_bind_kind == BIND_NOT_VOICE || s_bind_kind == BIND_TEST) {   /* E105 */
        char b[112];
        if (s_bind_kind == BIND_YES) snprintf(b, sizeof b, "%s", s_bind_caps);
        else snprintf(b, sizeof b, "%s", s_bind_kind == BIND_TEST ? "TEST ITEM \xC2\xB7 NOT BOUND" : "NOT A VOICE QUESTION");
        s_lab_bind = label(b, f_meta13, s_bind_kind == BIND_YES ? COL_INK2 : COL_AMBER, 398, 1);
        lv_obj_set_width(s_lab_bind, 280);
        lv_obj_set_style_text_align(s_lab_bind, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_lab_bind, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_lab_bind, lv_font_get_line_height(f_meta13));
        lv_obj_align(s_lab_bind, LV_ALIGN_TOP_MID, 0, 398);
    }
    s_lab_text = label("", g_hanken22 ? g_hanken22 : f_body, COL_INK, 258, 0);
    lv_obj_set_width(s_lab_text, 300);
    lv_obj_set_style_text_align(s_lab_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_lab_text, 29 - lv_font_get_line_height(lv_obj_get_style_text_font(s_lab_text, 0)), 0);
    lv_label_set_long_mode(s_lab_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_lab_text, 58);
    lv_obj_align(s_lab_text, LV_ALIGN_TOP_MID, 0, 258);
    s_lab_time = label("0:00", g_lexend28 ? g_lexend28 : f_num30, COL_INK, 330, 0);
    s_lab_hint = label("", f_meta13, COL_INK2, 366, 2);
    set_hint();
    s_hold_at = lv_tick_get();
    s_shown_s = -1;
    s_breath = 1;
    s_partial[0] = 0;
    s_partial_dirty = false;
    s_timer = lv_timer_create(tick, 66, NULL);              /* ~15 fps for the breathing rings */
    lv_display_trigger_activity(NULL);
}

/* Released (or 60 s reached): Listening closes, Understood takes over, the sender flushes. */
static void hand_over(void)
{
    char heard[200], id[48];
    bsp_display_lock(-1);
    strlcpy(heard, s_partial, sizeof heard);
    strlcpy(id, s_cmid, sizeof id);
    close_lv();
    bsp_display_unlock();
    if (s_bind[0]) {                                      /* a bound answer: the Reply view gives way to Understood */
        decision_reply_handed(s_bind);
        thread_talk_begin_reply(id, heard, s_bind, s_bind_raw);
    } else thread_talk_begin(id, heard);
    xTaskNotifyGive(s_send_task);
}

/* ---- recorder: microphone -> PSRAM, never blocks on the network ---- */

static void rec_task(void *arg)
{
    int16_t blk[READ_SAMPLES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_pm) esp_pm_lock_acquire(s_pm);              /* I2S DMA does not survive light sleep */
        s_mic_failed = !audio_in_open();
        int peak_decay = 0;
        while (!s_mic_failed && (s_phase == P_ARMED || s_phase == P_STREAM)) {
            int n = audio_in_read(blk, READ_SAMPLES);
            if (n <= 0) { s_mic_failed = true; break; }
            int pk = 0;
            for (int i = 0; i < n; i++) { int v = blk[i] < 0 ? -blk[i] : blk[i]; if (v > pk) pk = v; }
            peak_decay = pk > peak_decay ? pk : peak_decay * 7 / 8;
            s_level = peak_decay;
            size_t len = s_len, room = MAX_SAMPLES - len;
            size_t k = (size_t)n < room ? (size_t)n : room;
            memcpy(s_buf + len, blk, k * 2);
            s_len = len + k;
            if (s_len >= MAX_SAMPLES) {                    /* 60 s: send it as if released */
                if (__sync_bool_compare_and_swap(&s_phase, P_STREAM, P_FLUSH)) { ESP_LOGI(TAG, "60 s: sending"); hand_over(); }
                else __sync_bool_compare_and_swap(&s_phase, P_ARMED, P_DISARM);
                break;
            }
        }
        audio_in_close();
        if (s_pm) esp_pm_lock_release(s_pm);
        ESP_LOGI(TAG, "recorded %u ms%s", (unsigned)(s_len * 1000 / AUDIO_IN_RATE), s_mic_failed ? " (microphone failed)" : "");
        s_rec_done = true;
        xTaskNotifyGive(s_send_task);
    }
}

/* ---- sender: streams while held, then the rest and audio_end ---- */

static bool send_range(size_t *sent, size_t upto, bool all)
{
    while (*sent < upto && (all || upto - *sent >= CHUNK)) {
        size_t n = upto - *sent > CHUNK ? CHUNK : upto - *sent;
        if (!intercom_voice_chunk(s_buf + *sent, n)) return false;
        *sent += n;
    }
    return true;
}

static void send_task(void *arg)
{
    size_t sent = 0;
    bool started = false, ok = true;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        phase_t ph = s_phase;
        if (ph == P_STREAM && ok) {
            if (!started) { started = intercom_voice_start_bound(s_cmid, AUDIO_IN_RATE, s_bind); ok = started; sent = 0; }
            if (ok) ok = send_range(&sent, s_len, false);
        } else if (ph == P_FLUSH && s_rec_done) {
            if (!s_mic_failed && ok && !started) { started = intercom_voice_start_bound(s_cmid, AUDIO_IN_RATE, s_bind); ok = started; sent = 0; }
            if (!s_mic_failed && ok) ok = send_range(&sent, s_len, true) && intercom_voice_end(s_cmid);
            bool good = ok && !s_mic_failed;
            ESP_LOGI(TAG, "%s: %u ms %s", s_cmid, (unsigned)(s_len * 1000 / AUDIO_IN_RATE), good ? "sent" : "NOT sent");
            char id[48]; strlcpy(id, s_cmid, sizeof id);
            started = false; ok = true; sent = 0;
            s_phase = P_IDLE;
            thread_talk_net(id, good);
        } else if (ph == P_RETRY) {
            char id[48]; strlcpy(id, s_buf_cmid, sizeof id);
            size_t n = 0;
            bool good = intercom_voice_start_bound(id, AUDIO_IN_RATE, s_buf_bind) && send_range(&n, s_len, true) && intercom_voice_end(id);
            ESP_LOGI(TAG, "%s: retry %s", id, good ? "sent" : "NOT sent");
            s_phase = P_IDLE;
            thread_talk_net(id, good);
        } else if ((ph == P_CANCEL || ph == P_DISARM) && s_rec_done) {
            if (started) intercom_voice_cancel(s_cmid);
            started = false; ok = true; sent = 0;
            if (ph == P_DISARM) s_buf_cmid[0] = 0;
            s_phase = P_IDLE;
        }
    }
}

/* ---- public ---- */

void ptt_init(void)
{
    s_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) ESP_LOGE(TAG, "no PSRAM for the audio buffer");
#if CONFIG_PM_ENABLE
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ptt", &s_pm);
#endif
    bsp_display_lock(-1);
    mk_fonts();
    bsp_display_unlock();
    /* 764 bytes were left of 4 KB after one recording (codec init + logs). Stacks in PSRAM (psram_task.h). */
    task_create_psram(rec_task, "ptt_rec", 6144, NULL, 7, &s_rec_task);
    task_create_psram(send_task, "ptt_send", 4096, NULL, 5, &s_send_task);
}

void ptt_arm(void)
{
    if (!s_buf || s_phase != P_IDLE) return;              /* still sending the last one */
    s_len = 0;
    s_rec_done = false;
    s_mic_failed = false;
    snprintf(s_cmid, sizeof s_cmid, "ptt-%08lx-%lu", (unsigned long)esp_random(), (unsigned long)(time(NULL) % 100000));
    strlcpy(s_buf_cmid, s_cmid, sizeof s_buf_cmid);
    s_bind[0] = s_buf_bind[0] = 0;                        /* bound only if the hold finds a Reply view on top */
    s_bind_kind = BIND_OFF;
    s_phase = P_ARMED;
    xTaskNotifyGive(s_rec_task);
}

void ptt_disarm(void)
{
    __sync_bool_compare_and_swap(&s_phase, P_ARMED, P_DISARM);
}

void ptt_hold(void)
{
    /* E105: decided once, before streaming starts (the sender reads s_bind after it sees P_STREAM). With the flag off
       this returns BIND_OFF at once and nothing changes. */
    if (s_phase == P_ARMED) {
        s_bind_kind = decision_reply_bind(s_bind, sizeof s_bind, s_bind_caps, sizeof s_bind_caps, s_bind_raw, sizeof s_bind_raw);
        strlcpy(s_buf_bind, s_bind, sizeof s_buf_bind);
    }
    bool armed = __sync_bool_compare_and_swap(&s_phase, P_ARMED, P_STREAM);
    s_hint_kind = !armed ? H_BUSY : s_mic_failed ? H_MIC : intercom_connected() ? H_SEND : H_OFFLINE;
    ESP_LOGI(TAG, "listening (%s)", armed ? s_cmid : "busy");
    bsp_display_lock(-1);
    open_lv();
    bsp_display_unlock();
    if (armed) xTaskNotifyGive(s_send_task);
}

void ptt_release(void)
{
    if (__sync_bool_compare_and_swap(&s_phase, P_STREAM, P_FLUSH)) { hand_over(); return; }
    bsp_display_lock(-1);                                 /* cancelled, busy, or already sent at 60 s */
    close_lv();
    bsp_display_unlock();
}

void ptt_cancel(void)
{
    bool was = __sync_bool_compare_and_swap(&s_phase, P_STREAM, P_CANCEL) || __sync_bool_compare_and_swap(&s_phase, P_ARMED, P_CANCEL);
    bsp_display_lock(-1);
    bool open = s_root != NULL;
    close_lv();
    bsp_display_unlock();
    if (was || open) ESP_LOGI(TAG, "cancelled: nothing sent");
    if (was) { s_buf_cmid[0] = 0; xTaskNotifyGive(s_send_task); }
}

bool ptt_listening(void) { return s_phase == P_STREAM; }
bool ptt_recording(void) { return s_phase == P_ARMED || s_phase == P_STREAM || !s_rec_done; }

/* TOO LATE on a bound answer the bus dropped at audio_start (no transcript): send the kept audio again, unbound, as
   `cmid` (an ordinary push-to-talk to the followed conversation). */
bool ptt_resend_unbound(const char *cmid)
{
    if (!s_buf_cmid[0] || !s_len || !cmid || !cmid[0]) return false;
    if (!__sync_bool_compare_and_swap(&s_phase, P_IDLE, P_PREP)) return false;   /* holds off a new press meanwhile */
    strlcpy(s_buf_cmid, cmid, sizeof s_buf_cmid);
    s_buf_bind[0] = 0;
    s_phase = P_RETRY;
    xTaskNotifyGive(s_send_task);
    return true;
}

bool ptt_retry(const char *cmid)
{
    if (!s_buf_cmid[0] || strcmp(cmid, s_buf_cmid) || !s_len) return false;
    if (!__sync_bool_compare_and_swap(&s_phase, P_IDLE, P_RETRY)) return false;
    xTaskNotifyGive(s_send_task);
    return true;
}

void ptt_on_transcript(const char *cmid, bool final, const char *status, const char *text, const char *reason)
{
    if (final) { thread_on_transcript(cmid, status, text, reason); return; }
    bsp_display_lock(-1);
    if (s_root && !strcmp(cmid, s_cmid) && text && text[0]) {
        strlcpy(s_partial, text, sizeof s_partial);
        s_partial_dirty = true;
    }
    bsp_display_unlock();
}
