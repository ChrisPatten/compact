#include "commands.h"
#include "cmd_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "fw_version.h"
#include "intercom_ws.h"
#include "pomodoro.h"
#include "power.h"
#include "audio_out.h"
#include "thread.h"
#include "dayview.h"
#include "setscreen.h"
#include "psram_task.h"

static const char *TAG = "cmd";

#define MAX_OPS 16
#define OP_MAX 24
#define ARGS_MAX 200
#define OUT_MAX 1400                    /* handler output; a result frame is at most OUT_MAX + ~100 */
#define QUEUE_N 8

typedef struct { char op[OP_MAX]; const char *usage; command_fn fn; } entry_t;
static entry_t s_ops[MAX_OPS];
static int s_nops;
static SemaphoreHandle_t s_run_mu;      /* one handler at a time, whoever calls */

bool command_register(const char *op, const char *usage, command_fn fn)
{
    if (s_nops >= MAX_OPS || !op || !*op || strlen(op) >= OP_MAX || !fn) return false;
    for (int i = 0; i < s_nops; i++) if (!strcmp(s_ops[i].op, op)) return false;
    strlcpy(s_ops[s_nops].op, op, OP_MAX);
    s_ops[s_nops].usage = usage ? usage : "";
    s_ops[s_nops].fn = fn;
    s_nops++;
    return true;
}

const char *commands_reason(cmd_status_t s)
{
    switch (s) {
    case CMD_BAD_ARGS: return "bad_args";
    case CMD_BUSY: return "busy";
    case CMD_FAILED: return "failed";
    case CMD_UNKNOWN_OP: return "unknown_op";
    default: return NULL;
    }
}

size_t commands_ops_json(char *out, size_t n)
{
    size_t o = 0;
    if (n < 16) { if (n) out[0] = 0; return 0; }
    o += (size_t)snprintf(out, n, "\"ops\":[");
    for (int i = 0; i < s_nops; i++) {
        char u[2 * 130];
        cmd_json_escape(s_ops[i].usage, u, sizeof u);
        int w = snprintf(out + o, n - o, "%s{\"op\":\"%s\",\"usage\":\"%s\"}", i ? "," : "", s_ops[i].op, u);
        if (w < 0 || o + (size_t)w + 2 >= n) break;
        o += (size_t)w;
    }
    o += (size_t)snprintf(out + o, n - o, "]");
    return o;
}

cmd_status_t commands_run(const char *op, const char *args, char *out, size_t out_n)
{
    out[0] = 0;
    if (!s_run_mu) { snprintf(out, out_n, "commands not ready"); return CMD_BUSY; }
    for (int i = 0; i < s_nops; i++) {
        if (strcmp(s_ops[i].op, op)) continue;
        xSemaphoreTake(s_run_mu, portMAX_DELAY);
        cmd_status_t st = s_ops[i].fn(args ? args : "", out, out_n);
        xSemaphoreGive(s_run_mu);
        return st;
    }
    snprintf(out, out_n, "unknown op \"%.24s\"; try help", op);
    return CMD_UNKNOWN_OP;
}

/* ---- the queue: bus frames in, results out on the same link ---- */

typedef struct {
    int link;
    int64_t queued_ms;
    int ttl_s;
    char id[CMD_ID_MAX];
    char op[OP_MAX];
    char *args;                         /* heap (PSRAM), freed by the task */
    const char *pre_reason;             /* decided on the receive path: answer without running */
    const char *pre_hint;
} item_t;
static QueueHandle_t s_q;
static cmd_dedupe_t s_dedupe;
static char *s_out, *s_frame;           /* task buffers (PSRAM) */

static void send_frame(int link, const char *frame, size_t len)
{
    if (!intercom_send_on(link, frame, len, pdMS_TO_TICKS(2000)))
        ESP_LOGW(TAG, "result not sent (%s link)", link == INTERCOM_LINK_PHONE ? "phone" : "wifi");
}

static void answer(const item_t *it, bool ok, const char *reason, const char *data)
{
    int n = cmd_result_frame(s_frame, OUT_MAX + 160, it->id, ok, reason, data);
    if (n < 0) n = cmd_result_frame(s_frame, OUT_MAX + 160, it->id, false, "failed", "result too large");
    if (n < 0) return;
    cmd_dedupe_store(&s_dedupe, it->id, s_frame);
    send_frame(it->link, s_frame, (size_t)n);
}

static void process(item_t *it)
{
    const char *prev = cmd_dedupe_find(&s_dedupe, it->id);
    if (prev) {                                                 /* a repeat: the stored answer, no second run */
        ESP_LOGI(TAG, "%s repeat: stored result", it->id);
        send_frame(it->link, prev, strlen(prev));
        return;
    }
    if (it->pre_reason) { answer(it, false, it->pre_reason, it->pre_hint); return; }
    if (cmd_expired(it->queued_ms, esp_timer_get_time() / 1000, it->ttl_s)) {
        ESP_LOGW(TAG, "%s %s expired in the queue", it->id, it->op);
        answer(it, false, "expired", "waited longer than ttl_s");
        return;
    }
    cmd_status_t st = commands_run(it->op, it->args ? it->args : "", s_out, OUT_MAX);
    ESP_LOGI(TAG, "%s %s -> %s", it->id, it->op, st == CMD_OK ? "ok" : commands_reason(st));
    if (st == CMD_OK) answer(it, true, NULL, s_out);
    else answer(it, false, commands_reason(st), s_out);
}

static void cmd_task(void *arg)
{
    item_t it;
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;
        process(&it);
        free(it.args);
    }
}

void commands_on_frame(const struct cJSON *frame, int link)
{
    if (!s_q) return;
    const cJSON *id = cJSON_GetObjectItem(frame, "id"), *op = cJSON_GetObjectItem(frame, "op");
    const cJSON *args = cJSON_GetObjectItem(frame, "args"), *ttl = cJSON_GetObjectItem(frame, "ttl_s");
    if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) >= CMD_ID_MAX) {
        ESP_LOGW(TAG, "command frame without a usable id: ignored");
        return;
    }
    item_t it = { .link = link, .queued_ms = esp_timer_get_time() / 1000 };
    strlcpy(it.id, id->valuestring, sizeof it.id);
    it.ttl_s = cmd_clamp_ttl(cJSON_IsNumber(ttl) ? ttl->valuedouble : 0, cJSON_IsNumber(ttl));
    if (!cJSON_IsString(op) || strlen(op->valuestring) >= OP_MAX) {
        it.pre_reason = "unknown_op"; it.pre_hint = "missing or too long op; try help";
    } else {
        strlcpy(it.op, op->valuestring, sizeof it.op);
        char *a = NULL;
        if (cJSON_IsString(args)) a = strdup(args->valuestring);
        else if (args && !cJSON_IsNull(args)) a = cJSON_PrintUnformatted(args);
        if (a && strlen(a) >= ARGS_MAX) { free(a); a = NULL; it.pre_reason = "bad_args"; it.pre_hint = "args too long"; }
        it.args = a;
    }
    if (xQueueSend(s_q, &it, 0) != pdTRUE) {                    /* full: answer busy now, short wait at most */
        ESP_LOGW(TAG, "queue full: %s busy", it.id);
        free(it.args);
        char fr[200];
        int n = cmd_result_frame(fr, sizeof fr, it.id, false, "busy", "command queue full");
        if (n > 0) intercom_send_on(link, fr, (size_t)n, pdMS_TO_TICKS(50));
    }
}

bool commands_parse_http(const char *body, char *op, size_t op_n, char *args, size_t args_n)
{
    while (*body == ' ' || *body == '\n' || *body == '\r') body++;
    if (*body == '{') {
        cJSON *j = cJSON_Parse(body);
        if (!j) return false;
        const cJSON *o = cJSON_GetObjectItem(j, "op"), *a = cJSON_GetObjectItem(j, "args");
        bool ok = cJSON_IsString(o) && strlen(o->valuestring) < op_n;
        if (ok) {
            strlcpy(op, o->valuestring, op_n);
            args[0] = 0;
            if (cJSON_IsString(a)) strlcpy(args, a->valuestring, args_n);
            else if (a && !cJSON_IsNull(a)) { char *p = cJSON_PrintUnformatted(a); if (p) { strlcpy(args, p, args_n); free(p); } }
        }
        cJSON_Delete(j);
        return ok;
    }
    const char *a = cmd_split(body, op, op_n);
    strlcpy(args, a, args_n);
    size_t l = strlen(args);
    while (l && (args[l - 1] == '\n' || args[l - 1] == '\r' || args[l - 1] == ' ')) args[--l] = 0;
    return op[0] != 0;
}

/* ---- phase 1 ops ---- */

#define USAGE_POMODORO "start [focus [short [long]]] | pause | resume | skip | stop | status"

static cmd_status_t op_pomodoro(const char *args, char *out, size_t n)
{
    char tmp[360];
    if (pomodoro_command(args, tmp, sizeof tmp)) { snprintf(out, n, "%s", tmp); return CMD_OK; }
    /* tmp is {"error":"..."}: hand the text on as the hint */
    const char *e = strstr(tmp, "\"error\":\"");
    char hint[160] = USAGE_POMODORO;
    if (e) {
        strlcpy(hint, e + 9, sizeof hint);
        char *q = strrchr(hint, '"');
        if (q) *q = 0;
    }
    snprintf(out, n, "%s", hint);
    return strstr(hint, "no timer") ? CMD_FAILED : CMD_BAD_ARGS;
}

static cmd_status_t op_power(const char *args, char *out, size_t n)
{
    bool sleep = !strncmp(args, "sleep", 5), wake = !strncmp(args, "wake", 4);
    if (!sleep && !wake) { snprintf(out, n, "sleep | wake"); return CMD_BAD_ARGS; }
    if (sleep) power_sleep("command"); else power_wake("command");
    snprintf(out, n, "{\"requested\":\"%s\"}", sleep ? "sleep" : "wake");
    return CMD_OK;
}

static cmd_status_t op_face(const char *args, char *out, size_t n)
{
    char f[12];
    cmd_split(args, f, sizeof f);
    static const char *faces[] = { "thread", "now", "agents", "day", "settings" };
    int k = -1;
    for (int i = 0; i < 5; i++) if (!strcmp(f, faces[i])) k = i;
    if (k < 0) { snprintf(out, n, "thread | now | agents | day | settings"); return CMD_BAD_ARGS; }
    /* the same moves as POST /debug/ui: close the side faces first, then go through the gesture code under the lock */
    dayview_close();
    setscreen_close();
    if (bsp_display_lock(2000) != ESP_OK) { snprintf(out, n, "display busy"); return CMD_BUSY; }
    if (k == 3 || k == 4) {
        thread_debug_lv("now", 0);
        if (k == 3) dayview_swipe_lv(LV_DIR_RIGHT); else setscreen_open_lv();
    } else {
        thread_debug_lv(faces[k], 0);
    }
    bsp_display_unlock();
    snprintf(out, n, "{\"face\":\"%s\",\"power\":\"%s\"}", faces[k], (const char *[]){"awake", "dim", "rest", "sleep"}[power_state()]);
    return CMD_OK;
}

static cmd_status_t op_chime(const char *args, char *out, size_t n)
{
    audio_out_chime();                                           /* silent at volume 0 or while recording */
    snprintf(out, n, "{}");
    return CMD_OK;
}

static cmd_status_t op_info(const char *args, char *out, size_t n)
{
    char pom[320], link[128], elink[300], eact[420];
    pomodoro_info_json(pom, sizeof pom);
    intercom_link_info(link, sizeof link);
    cmd_json_escape(link, elink, sizeof elink);
    cmd_json_escape(intercom_last_action(), eact, sizeof eact);
    const esp_partition_t *run = esp_ota_get_running_partition();
    snprintf(out, n, "{\"version\":\"%s\",\"partition\":\"%s\",\"power\":\"%s\",\"battery\":%d,\"usb\":%s,\"charge\":\"%s\","
             "\"link\":\"%s\",\"active_link\":\"%s\",\"pomodoro\":%s,\"last_action\":\"%s\"}",
             FW_VERSION, run ? run->label : "?", (const char *[]){"awake", "dim", "rest", "sleep"}[power_state()],
             power_battery_pct(), power_on_usb() ? "true" : "false",
             (const char *[]){"battery", "charging", "full", "usb"}[power_charge_state()], elink, intercom_active_link(), pom, eact);
    return CMD_OK;
}

static cmd_status_t op_help(const char *args, char *out, size_t n)
{
    char o[OP_MAX];
    cmd_split(args, o, sizeof o);
    if (o[0]) {
        for (int i = 0; i < s_nops; i++) {
            if (strcmp(s_ops[i].op, o)) continue;
            char u[2 * 130];
            cmd_json_escape(s_ops[i].usage, u, sizeof u);
            snprintf(out, n, "{\"op\":\"%s\",\"usage\":\"%s\"}", s_ops[i].op, u);
            return CMD_OK;
        }
        snprintf(out, n, "unknown op \"%.24s\"", o);
        return CMD_BAD_ARGS;
    }
    out[0] = '{';
    size_t l = commands_ops_json(out + 1, n - 2);
    out[1 + l] = '}';
    out[2 + l] = 0;
    return CMD_OK;
}

void commands_init(void)
{
    s_run_mu = xSemaphoreCreateMutex();
    s_out = heap_caps_malloc(OUT_MAX, MALLOC_CAP_SPIRAM);
    s_frame = heap_caps_malloc(OUT_MAX + 160, MALLOC_CAP_SPIRAM);
    command_register("pomodoro", USAGE_POMODORO, op_pomodoro);
    command_register("power", "sleep | wake", op_power);
    command_register("face", "thread | now | agents | day | settings", op_face);
    command_register("chime", "no args: the soft two-note bell (silent at volume 0)", op_chime);
    command_register("info", "no args: version, partition, power, battery, usb, charge, link, pomodoro, last_action", op_info);
    command_register("help", "[op]: list the ops, or one op's usage", op_help);
    /* screenshot: not registered; the PNG only goes out by GET /screenshot (no upload path to the bus yet) */
    s_q = xQueueCreate(QUEUE_N, sizeof(item_t));
    TaskHandle_t t;
    if (!s_out || !s_frame || !s_q || task_create_psram(cmd_task, "cmd", 6144, NULL, 4, &t) != pdPASS)
    {
        ESP_LOGE(TAG, "command task not started: bus commands off (HTTP /cmd still works)");
        s_q = NULL;
    }
    ESP_LOGI(TAG, "%d ops registered", s_nops);
}
