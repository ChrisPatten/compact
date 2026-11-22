#include "ota.h"
#include "agents.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "fw_version.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "power.h"
#include "esp_random.h"
#include "intercom_ws.h"
#include "display.h"
#include "restart.h"
#include "logbuf.h"
#include "panicdiag.h"
#include "displaywd.h"
#include "memdiag.h"
#include "ptt.h"
#include "capture.h"
#include "pomodoro.h"
#include "commands.h"
#include "screenshot.h"
#include "decision.h"
#include "cue.h"
#include "thread.h"
#include "dial.h"
#include "dayview.h"
#include "earlier.h"
#include "setscreen.h"
#include "audio_out.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ROLLBACK_AFTER_US (120LL * 1000 * 1000)

static const char *TAG = "ota";
static esp_timer_handle_t s_rollback, s_reboot;
static char s_token[96];
static httpd_handle_t s_httpd;

static void rollback_now(void) { esp_ota_mark_app_invalid_rollback_and_reboot(); }   /* maps otadata: restart task */
static void rollback_cb(void *arg)
{
    ESP_LOGE(TAG, "new image never reached the bus: rolling back");
    compact_restart_with("rollback", rollback_now);
}

static void reboot_cb(void *arg) { compact_restart("ota"); }

void ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    ESP_LOGI(TAG, "running %s, version %s (app desc %s)", run->label, FW_VERSION, esp_app_get_description()->version);
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "image pending verification; rollback in 120 s unless the bus pairs");
        const esp_timer_create_args_t a = { .callback = rollback_cb, .name = "rollback" };
        esp_timer_create(&a, &s_rollback);
        esp_timer_start_once(s_rollback, ROLLBACK_AFTER_US);
    }
}

/* esp_ota_mark_app_valid_cancel_rollback maps otadata (flash mmap freezes the cache), so it runs on the esp_timer
   task, whose stack is internal: the caller (the websocket task or the phone link's ble_rx) may have a PSRAM stack. */
static void mark_good_cb(void *arg)
{
    if (!s_rollback) return;
    esp_timer_stop(s_rollback);
    esp_timer_delete(s_rollback);
    s_rollback = NULL;
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "image marked good");
}

void ota_mark_good(void)
{
    static esp_timer_handle_t t;
    if (!s_rollback) return;
    const esp_timer_create_args_t a = { .callback = mark_good_cb, .name = "mark_good" };
    if (!t && esp_timer_create(&a, &t) != ESP_OK) return;     /* the next pairing tries again */
    if (!esp_timer_is_active(t)) esp_timer_start_once(t, 0);
}

static void json_escape(const char *in, char *out, size_t n);

static bool authorised(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) != ESP_OK) return false;
    if (strncmp(hdr, "Bearer ", 7) || !s_token[0]) return false;
    const char *a = hdr + 7, *b = s_token;
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = la ^ lb;
    for (size_t i = 0; i < lb; i++) diff |= (unsigned)(b[i] ^ (i < la ? a[i] : 0));
    return diff == 0;
}

static esp_err_t ota_post_inner(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!dst || esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, dst->label);
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    int left = req->content_len, retries = 0;
    esp_err_t err = ESP_OK;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < 4096 ? left : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 5) continue;
        if (n <= 0) { err = ESP_FAIL; break; }
        if ((err = esp_ota_write(h, buf, n)) != ESP_OK) break;
        left -= n;
    }
    free(buf);
    if (err != ESP_OK) { esp_ota_abort(h); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed"); }
    if (esp_ota_end(h) != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image invalid");
    if (esp_ota_set_boot_partition(dst) != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
    httpd_resp_sendstr(req, "ok, rebooting\n");
    ESP_LOGI(TAG, "update written to %s; rebooting", dst->label);
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
    esp_timer_create(&a, &s_reboot);
    esp_timer_start_once(s_reboot, 1000 * 1000);
    return ESP_OK;
}

/* The display watchdog is held for the whole upload (flash writes and mapping can stall LVGL briefly). */
static esp_err_t ota_post(httpd_req_t *req)
{
    compact_display_wd_hold(true);
    esp_err_t r = ota_post_inner(req);
    compact_display_wd_hold(false);
    return r;
}

/* POST /power with body "sleep" or "wake" (Bearer): the same as a PWR click, for testing and agents. A wrapper on the
   `power` command (commands.c). */
static esp_err_t power_post(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[16] = {0}, out[64];
    int n = httpd_req_recv(req, body, sizeof body - 1);
    if (n <= 0 || commands_run("power", body, out, sizeof out) != CMD_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: sleep | wake");
    return httpd_resp_sendstr(req, "ok\n");
}

/* GET /pomodoro (Bearer): the timer's state as JSON. POST /pomodoro (Bearer): "start [focus_min [break_min [long_min]]]" |
   "stop" | "pause" | "resume" | "toggle" | "skip" | "status" (or the same as JSON); answers the new state. See pomodoro.h.
   POST runs the `pomodoro` command (commands.c); a failure is {"error":<hint>} with 400, as before. */
static esp_err_t pomodoro_handler(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[120] = {0}, out[360], err[420];
    bool ok = true;
    if (req->method == HTTP_POST) {
        int n = req->content_len > 0 ? httpd_req_recv(req, body, sizeof body - 1) : 0;
        if (n < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        ok = commands_run("pomodoro", body, out, sizeof out) == CMD_OK;
        if (!ok) {
            char h[360];
            json_escape(out, h, sizeof h);
            snprintf(err, sizeof err, "{\"error\":\"%s\"}", h);
        }
    } else {
        pomodoro_info_json(out, sizeof out);
    }
    httpd_resp_set_type(req, "application/json");
    if (!ok) httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, ok ? out : err);
}

/* POST /cmd (Bearer): the debug path into the device command registry. Body "<op> <args>" or {"op":..,"args":..};
   answers {"ok":true,"data":..} or {"ok":false,"reason":..,"data":"hint"}, the command_result frame without type and id. */
static esp_err_t cmd_post(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[260] = {0}, op[24], args[200];
    int n = req->content_len > 0 ? httpd_req_recv(req, body, sizeof body - 1) : 0;
    if (n <= 0 || !commands_parse_http(body, op, sizeof op, args, sizeof args))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: <op> [args] | {\"op\":..,\"args\":..}");
    char *out = heap_caps_malloc(1400 + 300, MALLOC_CAP_SPIRAM);
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    cmd_status_t st = commands_run(op, args, out, 1400);
    char *res = out + 1400;
    if (st == CMD_OK) {
        snprintf(res, 300, "{\"ok\":true,\"data\":");
    } else {
        snprintf(res, 300, "{\"ok\":false,\"reason\":\"%s\",\"data\":\"", commands_reason(st));
    }
    httpd_resp_set_type(req, "application/json");
    if (st != CMD_OK) httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send_chunk(req, res, HTTPD_RESP_USE_STRLEN);
    if (st == CMD_OK) {
        httpd_resp_send_chunk(req, out, HTTPD_RESP_USE_STRLEN);
    } else {
        char *h = heap_caps_malloc(2 * 400, MALLOC_CAP_SPIRAM);
        if (h) { out[399] = 0; json_escape(out, h, 2 * 400); httpd_resp_send_chunk(req, h, HTTPD_RESP_USE_STRLEN); free(h); }
        httpd_resp_send_chunk(req, "\"", 1);
    }
    httpd_resp_send_chunk(req, "}\n", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    free(out);
    return ESP_OK;
}

/* POST /debug/action, body "<object> <action_id>" (Bearer): fire a button as a tap would, for testing. */
static esp_err_t debug_action_post(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[200] = {0}, object[160] = {0}, action[40] = {0}, key[32];
    if (httpd_req_recv(req, body, sizeof body - 1) <= 0 || sscanf(body, "%159s %39s", object, action) != 2)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: <object> <action_id>");
    snprintf(key, sizeof key, "debug-%08lx", (unsigned long)esp_random());
    bool ok = intercom_send_action(object, action, NULL, key);
    char out[64]; snprintf(out, sizeof out, "%s key %s\n", ok ? "sent" : "not sent", key);
    return httpd_resp_sendstr(req, out);
}

/* POST /debug/ui, body "thread" | "now" | "agents" | "day" | "settings" | "scroll <dy>" (Bearer): drive the faces for
   frame-time and look tests; "night on|off|auto" forces the night / day ground and reports two flushed pixels. */
static esp_err_t debug_ui_post(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[200] = {0}, what[16] = {0};
    int dy = 0;
    if (httpd_req_recv(req, body, sizeof body - 1) <= 0 || sscanf(body, "%15s %d", what, &dy) < 1)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: thread | now | agents | day | settings | restart | night on|off|auto | gauge ... | sports [text] | clock HH:MM [+d] | clock off | testday rel|abs|detail|free|off | widgets {..} | gap <min> | scroll <dy> | agents-test <n> | agents-scroll <dy> | settings-scroll <dy> | day-scroll <dy> | swipe up|down | detail | tone <hz> | chime | ptt <ms> | capture[-tone] <s> | capture-ui <s> | capture-offline <s> | ble-start | mem-hog <kb> | hang <ms> | hammer <s> [nolock] | wd on|off|status | screenshot | cue plum|amber|rose|off [phase] [count] [quiet] | cue tune ... | cue bench | notice-test | notice-open | earlier-test [off] | earlier-row <n> | earlier-clear | reply on|off|status | reply test choices|voice|both|inform|late|done|expired|off");
    if (!strcmp(what, "swipe")) {                              /* "swipe up|down": the router's vertical branch, no touch */
        char dir[8] = {0};
        sscanf(body, "%*s %7s", dir);
        if (strcmp(dir, "up") && strcmp(dir, "down")) return httpd_resp_sendstr(req, "body: swipe up|down\n");
        return httpd_resp_sendstr(req, earlier_debug_swipe(!strcmp(dir, "up")) ? "ok\n" : "display busy\n");
    }
    if (!strcmp(what, "chime")) { audio_out_chime(); return httpd_resp_sendstr(req, "ok\n"); }   /* the Pomodoro phase chime (silent at volume 0) */
    if (!strcmp(what, "detail")) return httpd_resp_sendstr(req, dayview_debug_detail(body[6] == ' ' ? body + 7 : NULL) ? "ok\n" : "no\n");
    if (!strcmp(what, "day-scroll")) return httpd_resp_sendstr(req, dayview_debug_scroll(dy) ? "ok\n" : "no\n");
    if (!strcmp(what, "settings-scroll")) return httpd_resp_sendstr(req, setscreen_debug_scroll(dy) ? "ok\n" : "no\n");
    if (!strncmp(what, "agents-", 7)) return httpd_resp_sendstr(req, agents_debug(what, dy) ? "ok\n" : "no\n");
    if (!strcmp(what, "gauge")) {                              /* "gauge leave <min>" ...: see now_layout.c */
        char r[96]; dial_gauge_debug(body + 5, r, sizeof r);
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "clock")) {                              /* "clock HH:MM [+days]" | "clock off": the drawing clock */
        char r[128]; dial_clock_debug(body + 5, r, sizeof r);
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "testday")) {                            /* "testday rel|abs|off": test Moments on Now and Day */
        char r[128]; dial_testday_debug(body + 7, r, sizeof r);
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "widgets")) {                            /* "widgets {json}": as the bus widgets resource */
        char r[96]; dial_widgets_debug(body + 7, r, sizeof r);
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "gap")) {                                /* "gap 30|60|120|180": as the Settings RIM GAP row */
        char r[48]; dial_gap_debug(dy, r, sizeof r);
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "restart")) {                            /* "restart": the quiesced restart path, as the power menu */
        compact_restart("debug");
        return httpd_resp_sendstr(req, "ok, restarting\n");
    }
    if (!strcmp(what, "sports")) {                             /* "sports <text>" sets the crown line, "sports" clears it */
        char r[96]; dial_gauge_debug(body, r, sizeof r);
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "ptt")) {                                /* "ptt <ms>": a BOOT hold of <ms>, then PWR-cancel (nothing sent) */
        int ms = dy > 0 && dy < 20000 ? dy : 2000;
        audio_out_stop();
        ptt_arm();
        vTaskDelay(pdMS_TO_TICKS(450));
        ptt_hold();
        vTaskDelay(pdMS_TO_TICKS(ms));
        ptt_cancel();
        ptt_release();
        return httpd_resp_sendstr(req, "ok, cancelled\n");
    }
    if (!strcmp(what, "night")) {                              /* "night on|off|auto": force the night / day ground */
        char arg[8] = {0};
        sscanf(body, "%*s %7s", arg);
        int m = !strcmp(arg, "on") ? 1 : !strcmp(arg, "off") ? 0 : !strcmp(arg, "auto") ? -1 : -2;
        uint32_t p[2];
        compact_display_probe(p, true);
        if (m != -2) dial_set_night_override(m);
        vTaskDelay(pdMS_TO_TICKS(600));                        /* let a frame or two flush */
        compact_display_probe(p, false);
        char r[128];
        snprintf(r, sizeof r, "override %s, ground %s (%06lx), probe corner %s%04lx inner %s%04lx\n",
                 dial_night_override() < 0 ? "auto" : dial_night_override() ? "night" : "day",
                 dial_is_night() ? "night" : "day", (unsigned long)dial_ground(),
                 p[0] & 0x10000 ? "" : "stale ", (unsigned long)(p[0] & 0xFFFF),
                 p[1] & 0x10000 ? "" : "stale ", (unsigned long)(p[1] & 0xFFFF));
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "day") || !strcmp(what, "settings")) {   /* open the Day list or Settings from Now */
        dayview_close(); setscreen_close();
        bsp_display_lock(-1);
        thread_debug_lv("now", 0);
        if (!strcmp(what, "day")) dayview_swipe_lv(LV_DIR_RIGHT); else setscreen_open_lv();
        bsp_display_unlock();
        return httpd_resp_sendstr(req, "ok\n");
    }
    if (!strcmp(what, "now")) { dayview_close(); setscreen_close(); }
    if (!strcmp(what, "mem-hog")) {
        char r[48]; snprintf(r, sizeof r, "holding %d KB for 15 s\n", memdiag_hog(dy));
        return httpd_resp_sendstr(req, r);
    }
    if (!strcmp(what, "hang")) {                               /* "hang <ms>": block the LVGL task (watchdog test) */
        return httpd_resp_sendstr(req, compact_display_debug_hang(dy > 0 ? (uint32_t)dy : 10000) ? "ok, LVGL task will block\n" : "refused (100-60000 ms)\n");
    }
    if (!strcmp(what, "hammer")) {                             /* "hammer <s> [nolock]": brightness flips against redraws */
        char arg[8] = {0};
        sscanf(body, "%*s %*s %7s", arg);
        return httpd_resp_sendstr(req, compact_display_debug_hammer(dy > 0 ? dy : 30, !strcmp(arg, "nolock")) ? "ok\n" : "refused (1-300 s)\n");
    }
    if (!strcmp(what, "wd")) {                                 /* "wd on|off|status": display watchdog switch */
        char arg[8] = {0};
        sscanf(body, "%*s %7s", arg);
        const char *r = displaywd_command(arg);
        return httpd_resp_sendstr(req, r ? r : "body: wd on|off|status\n");
    }
    if (!strcmp(what, "screenshot")) return httpd_resp_sendstr(req, screenshot_take() ? "ok, taken (GET /screenshot)\n" : "not now\n");
    if (!strcmp(what, "cue") && strstr(body, "readback")) return httpd_resp_sendstr(req, compact_ring_readback());
    if (!strcmp(what, "cue")) { cue_debug(body + 3); return httpd_resp_sendstr(req, "ok\n"); }   /* cue plum|amber|rose [phase] [count] [quiet] | cue off | cue tune ... | cue bench */
    if (!strncmp(what, "earlier-", 8)) return httpd_resp_sendstr(req, earlier_debug(body + 8));   /* the tray's ended-row detail */
    if (!strcmp(what, "notice-test")) { decision_debug_notice(); return httpd_resp_sendstr(req, "ok\n"); }
    if (!strcmp(what, "reply")) return httpd_resp_sendstr(req, decision_reply_debug(body + 5));   /* E105 Reply view: on|off|status|test ... */
    if (!strcmp(what, "notice-open")) {                        /* what a swipe up on Now does */
        if (bsp_display_lock(2000) != ESP_OK) return httpd_resp_sendstr(req, "display busy\n");
        bool o = decision_open_notice();
        bsp_display_unlock();
        return httpd_resp_sendstr(req, o ? "ok, opened\n" : "none pending\n");
    }
    if (!strcmp(what, "ble-start")) return httpd_resp_sendstr(req, intercom_debug_ble_start() ? "ok, NimBLE up\n" : "failed\n");
    if (!strcmp(what, "capture-ui")) return httpd_resp_sendstr(req, capture_debug_ui(dy > 0 ? dy : 20) ? "ok, showing\n" : "not now\n");
    if (!strcmp(what, "capture-offline")) { capture_debug_offline(dy > 0 ? dy : 0); return httpd_resp_sendstr(req, "ok\n"); }
    if (!strcmp(what, "capture") || !strcmp(what, "capture-tone")) {   /* "capture-tone <s>": a test recording of a tone */
        bool ok = capture_debug_start(dy > 0 ? dy : 10, !strcmp(what, "capture-tone"));
        return httpd_resp_sendstr(req, ok ? "ok, recording\n" : "not now\n");
    }
    if (!strcmp(what, "tone")) {                               /* "tone <hz>": one second on the speaker */
        audio_out_tone(dy > 0 ? dy : 440, 1000);
        return httpd_resp_sendstr(req, audio_out_playing() ? "ok\n" : "not now\n");
    }
    bsp_display_lock(-1);
    bool ok = thread_debug_lv(what, dy);
    bsp_display_unlock();
    return httpd_resp_sendstr(req, ok ? "ok\n" : "not now\n");
}

/* GET /log (Bearer): the last 16 KB of the device log; GET /log?prev: the previous boot's (kept across a panic). */
static esp_err_t log_get(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char *b = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    if (!b) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    char q[16] = {0};
    bool prev = httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "prev");
    size_t n = prev ? logbuf_copy_prev(b, 16 * 1024) : logbuf_copy(b, 16 * 1024);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

/* GET /debug/heap (Bearer): heaps, task stacks and the largest internal blocks; ?trace: live internal allocations
   with callers (heap-tracing builds only). */
static esp_err_t heap_get(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char q[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "trace")) return memdiag_send_trace(req);
    return memdiag_send_heap(req);
}

/* GET /screenshot (Bearer): the last screenshot as a PNG (BOOT + PWR together, or POST /debug/ui screenshot). */
static esp_err_t screenshot_get(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    return screenshot_send_png(req);
}

/* GET /debug/perf (Bearer): see memdiag.h. */
static esp_err_t perf_get(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    return memdiag_send_perf(req);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return displaywd_reset_was_hang() ? "display-hang" : "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt-watchdog";
    case ESP_RST_TASK_WDT: return "task-watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    default: return "other";
    }
}

/* Read a small request body (at most cap-1 bytes) into buf; false on an empty, oversized or short body. */
static bool read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= cap) return false;
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return false;
        got += n;
    }
    buf[got] = 0;
    return true;
}

/* POST /wifi (Bearer): {"slot":2,"ssid":"..","password":".."} or {"slot":2,"clear":true}. Never echoes the password. */
static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[300];
    if (!read_body(req, body, sizeof body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    cJSON *j = cJSON_Parse(body);
    memset(body, 0, sizeof body);
    const cJSON *slot = j ? cJSON_GetObjectItem(j, "slot") : NULL, *ssid = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    const cJSON *pw = j ? cJSON_GetObjectItem(j, "password") : NULL, *clr = j ? cJSON_GetObjectItem(j, "clear") : NULL;
    bool clear = cJSON_IsTrue(clr), ok = false;
    if (cJSON_IsNumber(slot) && slot->valueint == 2 && (clear || (cJSON_IsString(ssid) && (!pw || cJSON_IsString(pw))))) {
        const char *p = cJSON_IsString(pw) ? pw->valuestring : "";
        /* WPA2/WPA3 passphrases are 8-63 characters (64 = raw hex PSK); an empty one means an open network */
        size_t pl = strlen(p);
        if (clear || (pl == 0 || (pl >= 8 && pl <= 64)))
            ok = intercom_wifi_set(2, clear ? NULL : ssid->valuestring, p, clear);
    }
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot must be 2; ssid 1-32 chars; password 8-64 chars or empty");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, clear ? "{\"ok\":true,\"slot\":2,\"cleared\":true}\n" : "{\"ok\":true,\"slot\":2,\"saved\":true}\n");
}

/* POST /bus (Bearer): {"url":"wss://host/api/v1/intercom/ws"} or {"clear":true}. The remote bus is used only
   when the LAN bus cannot be reached. */
static esp_err_t bus_post(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[260];
    if (!read_body(req, body, sizeof body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    cJSON *j = cJSON_Parse(body);
    const cJSON *url = j ? cJSON_GetObjectItem(j, "url") : NULL, *clr = j ? cJSON_GetObjectItem(j, "clear") : NULL;
    bool clear = cJSON_IsTrue(clr);
    bool ok = (clear || cJSON_IsString(url)) && intercom_bus_set(clear ? NULL : url->valuestring, clear);
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url must be wss://host[:port]/path (under 128 chars), or clear");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, clear ? "{\"ok\":true,\"remote_bus\":false}\n" : "{\"ok\":true,\"remote_bus\":true}\n");
}

/* POST /link (Bearer), body "ble": restart into PHONE mode (the phone link only, no Wi-Fi) for one boot. This
   server is then gone until the next restart (LINK held on Settings, PWR, or the 10-min no-bus rule) comes back
   in AUTO. AUTO needs no request: it is every other boot. */
static void restart_soon(void *arg) { vTaskDelay(pdMS_TO_TICKS(500)); compact_restart("link: phone next boot"); vTaskDelete(NULL); }
static esp_err_t link_post(httpd_req_t *req)
{
    if (!authorised(req)) { httpd_resp_set_status(req, "401 Unauthorized"); return httpd_resp_sendstr(req, "unauthorized\n"); }
    char body[16] = {0};
    int n = httpd_req_recv(req, body, sizeof body - 1);
    if (n <= 0 || strncmp(body, "ble", 3)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: ble");
    if (!settings_link_ble_next_boot()) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    httpd_resp_sendstr(req, "ok: restarting on the phone link\n");
    xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* JSON string body (no quotes) of an SSID: escapes quote, backslash and control bytes. */
static void json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, n - o, "\\u%04x", c);
        else out[o++] = c;
    }
    out[o] = 0;
}

/* Least free stack each long-lived task has had (bytes), for crash hunting: "boot_btn:812,ptt_rec:1400,...". */
static void stacks_info(char *out, size_t n)
{
    static const char *names[] = { "boot_btn", "ptt_rec", "ptt_send", "cap_rec", "cap_up", "speech", "power", "bus", "bus_tx",
                                   "websocket_task", "roam", "phone_link", "ble_rx" };
    size_t o = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0] && o + 24 < n; i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (t) o += snprintf(out + o, n - o, "%s%s:%u", o ? "," : "", names[i], (unsigned)uxTaskGetStackHighWaterMark(t));
    }
}

static esp_err_t info_get(httpd_req_t *req)
{
    /* the buffers live in PSRAM: on the httpd stack they left it 156 bytes free (GET /debug/heap, Nov 19) */
    enum { OUT = 5900, PAN = 800, CAP = 600, STK = 300, HNG = 900, SHT = 160, CUE = 520, RPL = 140, POM = 320 };
    char *out = heap_caps_malloc(OUT, MALLOC_CAP_SPIRAM), *mem = heap_caps_malloc(PAN + CAP + STK + HNG + SHT + CUE + RPL + POM, MALLOC_CAP_SPIRAM);
    if (!out || !mem) { free(out); free(mem); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    char *pan = mem, *cap = pan + PAN, *stk = cap + CAP, *hng = stk + STK, *sht = hng + HNG, *cue = sht + SHT, *rpl = cue + CUE, *pom = rpl + RPL;
    char wds[160];
    char link[128], ssid[80], ble[96];
    intercom_net_t net;
    int fr_n, fr_avg, fr_max;
    char q[16] = {0};
    bool reset = httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "reset");
    compact_display_frames(&fr_n, &fr_avg, &fr_max, reset);
    intercom_link_info(link, sizeof link);
    intercom_net_info(&net);
    intercom_ble_info(ble, sizeof ble);
    json_escape(net.ssid, ssid, sizeof ssid);
    int wake_render, wake_total;
    power_wake_timing(&wake_render, &wake_total);
    const esp_partition_t *run = esp_ota_get_running_partition();
    panicdiag_json(pan, PAN);
    capture_info_json(cap, CAP);
    stacks_info(stk, STK);
    displaywd_last_json(hng, HNG);
    displaywd_state_json(wds, sizeof wds);
    screenshot_info_json(sht, SHT);
    cue_info_json(cue, CUE);
    decision_reply_info(rpl, RPL);
    pomodoro_info_json(pom, POM);
    snprintf(out, OUT,
             "{\"version\":\"%s\",\"partition\":\"%s\",\"uptime_s\":%lld,\"power\":\"%s\",\"battery\":%d,\"usb\":%s,"
             "\"reset\":\"%s\",\"internal_free\":%u,\"internal_min_free\":%u,\"internal_largest\":%u,"
             "\"dma_largest\":%u,\"psram_free\":%u,\"wake_render_ms\":%d,\"wake_total_ms\":%d,\"last_action\":\"%s\",\"link\":\"%s\",\"charge\":\"%s\",\"frames\":%d,\"frame_avg_ms\":%d,\"frame_max_ms\":%d,"
             "\"ssid\":\"%s\",\"net_slot\":\"%s\",\"rssi\":%d,\"roam\":\"%s\",\"bus_via\":\"%s\",\"remote_bus\":%s,\"net_pinned\":%s,"
             "\"link_mode\":\"%s\",\"active_link\":\"%s\",\"ble\":\"%s\",\"last_panic\":%s,\"last_hang\":%s,\"display_wd\":%s,\"capture\":%s,\"stack_free_min\":\"%s\",\"screenshot\":%s,\"cue\":%s,\"reply\":%s,\"pomodoro\":%s}\n",
             FW_VERSION, run->label, esp_timer_get_time() / 1000000,
             (const char *[]){"awake", "dim", "rest", "sleep"}[power_state()], power_battery_pct(), power_on_usb() ? "true" : "false", reset_reason(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), wake_render, wake_total, intercom_last_action(), link,
             (const char *[]){"battery", "charging", "full", "usb"}[power_charge_state()], fr_n, fr_avg, fr_max,
             ssid, net.slot == 1 ? "primary" : net.slot == 2 ? "secondary" : "none", net.online ? net.rssi : 0, net.roam, net.bus_via,
             net.remote_set ? "true" : "false", net.pinned ? "true" : "false",
             intercom_link_mode(), intercom_active_link(), ble, pan, hng, wds, cap, stk, sht, cue, rpl, pom);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, out);
    free(out);
    free(mem);
    return r;
}

void ota_server_start(const char *token)
{
    if (s_httpd) return;
    strlcpy(s_token, token, sizeof s_token);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 20;
    cfg.recv_wait_timeout = 15;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    const httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    const httpd_uri_t info = { .uri = "/info", .method = HTTP_GET, .handler = info_get };
    httpd_register_uri_handler(s_httpd, &ota);
    httpd_register_uri_handler(s_httpd, &info);
    const httpd_uri_t pwr = { .uri = "/power", .method = HTTP_POST, .handler = power_post };
    const httpd_uri_t dbg = { .uri = "/debug/action", .method = HTTP_POST, .handler = debug_action_post };
    httpd_register_uri_handler(s_httpd, &pwr);
    httpd_register_uri_handler(s_httpd, &dbg);
    const httpd_uri_t ui = { .uri = "/debug/ui", .method = HTTP_POST, .handler = debug_ui_post };
    httpd_register_uri_handler(s_httpd, &ui);
    const httpd_uri_t lg = { .uri = "/log", .method = HTTP_GET, .handler = log_get };
    httpd_register_uri_handler(s_httpd, &lg);
    const httpd_uri_t wf = { .uri = "/wifi", .method = HTTP_POST, .handler = wifi_post };
    const httpd_uri_t bs = { .uri = "/bus", .method = HTTP_POST, .handler = bus_post };
    httpd_register_uri_handler(s_httpd, &wf);
    httpd_register_uri_handler(s_httpd, &bs);
    const httpd_uri_t lk = { .uri = "/link", .method = HTTP_POST, .handler = link_post };
    httpd_register_uri_handler(s_httpd, &lk);
    const httpd_uri_t hp = { .uri = "/debug/heap", .method = HTTP_GET, .handler = heap_get };
    httpd_register_uri_handler(s_httpd, &hp);
    const httpd_uri_t pf = { .uri = "/debug/perf", .method = HTTP_GET, .handler = perf_get };
    httpd_register_uri_handler(s_httpd, &pf);
    const httpd_uri_t ss = { .uri = "/screenshot", .method = HTTP_GET, .handler = screenshot_get };
    httpd_register_uri_handler(s_httpd, &ss);
    const httpd_uri_t pg = { .uri = "/pomodoro", .method = HTTP_GET, .handler = pomodoro_handler };
    const httpd_uri_t pp = { .uri = "/pomodoro", .method = HTTP_POST, .handler = pomodoro_handler };
    httpd_register_uri_handler(s_httpd, &pg);
    httpd_register_uri_handler(s_httpd, &pp);
    const httpd_uri_t cm = { .uri = "/cmd", .method = HTTP_POST, .handler = cmd_post };
    httpd_register_uri_handler(s_httpd, &cm);
    ESP_LOGI(TAG, "update server on :80");
}
