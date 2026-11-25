#include "displaywd.h"
#include "restart.h"
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "capture.h"
#include "display.h"
#include "power.h"
#include "wd_logic.h"

#define HD_MAGIC 0x484E4731u   /* "HNG1" */

static const char *TAG = "displaywd";

typedef struct {
    uint32_t magic;
    uint32_t count;            /* hang restarts since power-on */
    uint32_t recent;           /* consecutive hang restarts that came under WD_LOOP_UPTIME_S after boot */
    uint32_t fresh;            /* 1 from the restart until the next boot has read it */
    uint32_t uptime_s;
    uint32_t stale_ms, timeout_ms;
    uint32_t hb, flush_n, flush_age_ms, op_age_ms;
    int32_t power, paused, hold, lock_free, lvgl_state, lvgl_stack_min;
    uint32_t free_int, min_int, largest_int, free_psram;
    int32_t recording;
    char op[16], op_task[16];
    uint32_t sum;
} hd_t;

static RTC_NOINIT_ATTR hd_t s_hd;
static bool s_valid, s_fresh, s_tripped, s_off_nvs, s_off_rt;
static volatile int64_t s_stale_ms;
static volatile bool s_running;
static bool s_cleared_recent;

static uint32_t hd_sum(const hd_t *p)
{
    const uint32_t *w = (const uint32_t *)p;
    uint32_t s = 0x9E3779B9u;
    for (size_t i = 0; i < offsetof(hd_t, sum) / 4; i++) s = (s ^ w[i]) * 16777619u;
    return s;
}
static void hd_seal(void) { s_hd.sum = hd_sum(&s_hd); }

void displaywd_init(void)
{
    s_valid = s_hd.magic == HD_MAGIC && s_hd.sum == hd_sum(&s_hd);
    if (!s_valid) { memset(&s_hd, 0, sizeof s_hd); return; }
    s_fresh = s_hd.fresh && esp_reset_reason() == ESP_RST_SW;
    s_hd.fresh = 0;
    s_tripped = wd_loop_tripped(s_hd.recent);        /* stays until ten healthy minutes, a power loss or "wd on" */
    hd_seal();
    if (s_tripped) ESP_LOGE(TAG, "restart-loop guard: %lu hang restarts in a row, watchdog off for this boot", (unsigned long)s_hd.recent);
}

bool displaywd_reset_was_hang(void) { return s_fresh; }

static void nvs_flag(int set /* -1 read, 0 clear, 1 set */)
{
    nvs_handle_t h;
    if (nvs_open("dispwd", set < 0 ? NVS_READONLY : NVS_READWRITE, &h) != ESP_OK) return;
    if (set < 0) { uint8_t v = 0; if (nvs_get_u8(h, "off", &v) == ESP_OK) s_off_nvs = v != 0; }
    else { nvs_set_u8(h, "off", (uint8_t)set); nvs_commit(h); s_off_nvs = set != 0; }
    nvs_close(h);
}

static bool enabled(void) { return !s_off_nvs && !s_off_rt && !s_tripped; }

static void reset_fallback_cb(void *arg) { (void)arg; esp_rom_software_reset_system(); }

static void hang_now(const compact_display_diag_t *d, int64_t stale_ms, int64_t timeout_ms)
{
    uint32_t prev_recent = s_valid ? s_hd.recent : 0, prev_count = s_valid ? s_hd.count : 0;
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    memset(&s_hd, 0, sizeof s_hd);
    s_hd.count = prev_count + 1;
    s_hd.recent = wd_loop_next(prev_recent, up);
    s_hd.fresh = 1;
    s_hd.uptime_s = up;
    s_hd.stale_ms = (uint32_t)stale_ms;
    s_hd.timeout_ms = (uint32_t)timeout_ms;
    s_hd.hb = d->hb; s_hd.flush_n = d->flush_n; s_hd.flush_age_ms = d->flush_age_ms; s_hd.op_age_ms = d->op_age_ms;
    s_hd.power = (int32_t)power_state();
    s_hd.paused = d->paused; s_hd.hold = d->hold; s_hd.lock_free = d->lock_free;
    s_hd.lvgl_state = d->lvgl_state; s_hd.lvgl_stack_min = d->lvgl_stack_min;
    s_hd.free_int = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_hd.min_int = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s_hd.largest_int = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s_hd.free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_hd.recording = capture_recording();
    strlcpy(s_hd.op, d->op ? d->op : "", sizeof s_hd.op);
    strlcpy(s_hd.op_task, d->op_task, sizeof s_hd.op_task);
    s_hd.magic = HD_MAGIC;
    hd_seal();
    ESP_LOGE(TAG, "DISPLAY HANG: heartbeat %lu frozen %lld ms, power %d, lock %s, lvgl task state %d, last panel op %s by %s %lu ms ago, "
             "last flush %lu ms ago (#%lu), internal free %lu; restarting",
             (unsigned long)d->hb, (long long)stale_ms, (int)power_state(), d->lock_free ? "free" : "held", d->lvgl_state,
             d->op ? d->op : "none", d->op_task, (unsigned long)d->op_age_ms, (unsigned long)d->flush_age_ms,
             (unsigned long)d->flush_n, (unsigned long)s_hd.free_int);
    /* If esp_restart() stalls in a shutdown handler, a one-shot esp_timer forces the reset 4 s later. */
    esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = reset_fallback_cb, .name = "wd_reset" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 4 * 1000 * 1000);
    compact_restart_hung("display hang");      /* never waits on the hung LVGL task; ROM reset after 1.5 s at most */
    for (;;) vTaskDelay(portMAX_DELAY);         /* no second hang record while the restart runs */
}

static void wd_task(void *arg)
{
    (void)arg;
    wd_t w;
    wd_init(&w);
    for (;;) {                                      /* runs for the life of the device; sleeps 1 s per pass */
        vTaskDelay(pdMS_TO_TICKS(WD_SAMPLE_MS));
        int64_t now = esp_timer_get_time() / 1000;
        if (!s_cleared_recent && now > 600000) {     /* ten healthy minutes: the restart-loop count starts again */
            s_cleared_recent = true;
            if (s_valid && s_hd.recent) { s_hd.recent = 0; hd_seal(); }
        }
        compact_display_diag_t d;
        compact_display_diag(&d);
        bool on = enabled();
        int64_t timeout = capture_recording() ? WD_TIMEOUT_REC_MS : WD_TIMEOUT_MS;
        wd_result_t r = wd_step(&w, now, d.hb, d.held || !on, timeout);
        s_stale_ms = w.stale_ms;
        if (r == WD_HANG) hang_now(&d, w.stale_ms, timeout);
        else if (r == WD_STALE && w.stale_ms >= 2000)
            ESP_LOGW(TAG, "LVGL heartbeat frozen for %lld ms (limit %lld)", (long long)w.stale_ms, (long long)timeout);
    }
}

void displaywd_start(void)
{
    nvs_flag(-1);
    if (s_off_nvs) ESP_LOGW(TAG, "watchdog disabled by the NVS flag (POST /debug/ui \"wd on\" re-enables it)");
    s_running = xTaskCreate(wd_task, "displaywd", 3072, NULL, 7, NULL) == pdPASS;
    if (!s_running) ESP_LOGE(TAG, "watchdog task not started");
}

static void esc(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 2 < n; in++) {
        char c = *in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if ((unsigned char)c < 0x20) out[o++] = ' ';
        else out[o++] = c;
    }
    out[o] = 0;
}

size_t displaywd_last_json(char *out, size_t n)
{
    if (!s_valid || !s_hd.count) return (size_t)snprintf(out, n, "null");
    char op[40], task[40];
    esc(s_hd.op, op, sizeof op);
    esc(s_hd.op_task, task, sizeof task);
    return (size_t)snprintf(out, n,
        "{\"count\":%lu,\"recent\":%lu,\"uptime_s\":%lu,\"stale_ms\":%lu,\"timeout_ms\":%lu,\"power\":\"%s\",\"hb\":%lu,"
        "\"lock\":\"%s\",\"lvgl_state\":%ld,\"lvgl_stack_min\":%ld,\"last_op\":\"%s\",\"last_op_task\":\"%s\",\"last_op_age_ms\":%lu,"
        "\"last_flush_age_ms\":%lu,\"flushes\":%lu,\"recording\":%s,\"internal_free\":%lu,\"internal_min\":%lu,"
        "\"internal_largest\":%lu,\"psram_free\":%lu}",
        (unsigned long)s_hd.count, (unsigned long)s_hd.recent, (unsigned long)s_hd.uptime_s, (unsigned long)s_hd.stale_ms,
        (unsigned long)s_hd.timeout_ms,
        (const char *[]){"awake", "dim", "rest", "sleep"}[(uint32_t)s_hd.power & 3], (unsigned long)s_hd.hb,
        s_hd.lock_free ? "free" : "held", (long)s_hd.lvgl_state, (long)s_hd.lvgl_stack_min, op, task,
        (unsigned long)s_hd.op_age_ms, (unsigned long)s_hd.flush_age_ms, (unsigned long)s_hd.flush_n,
        s_hd.recording ? "true" : "false", (unsigned long)s_hd.free_int, (unsigned long)s_hd.min_int,
        (unsigned long)s_hd.largest_int, (unsigned long)s_hd.free_psram);
}

size_t displaywd_state_json(char *out, size_t n)
{
    compact_display_diag_t d;
    compact_display_diag(&d);
    const char *st = !s_running ? "not-running" : s_off_nvs ? "off" : s_off_rt ? "off-session" : s_tripped ? "loop-guard" : d.held ? "held" : "on";
    return (size_t)snprintf(out, n, "{\"state\":\"%s\",\"hb\":%lu,\"stale_ms\":%lld,\"hold\":%d,\"paused\":%s,\"timeout_ms\":%d}",
                            st, (unsigned long)d.hb, (long long)s_stale_ms, d.hold, d.paused ? "true" : "false", WD_TIMEOUT_MS);
}

const char *displaywd_command(const char *arg)
{
    if (!strcmp(arg, "off")) { nvs_flag(1); return "watchdog off (NVS), stays off until \"wd on\"\n"; }
    if (!strcmp(arg, "on")) {
        nvs_flag(0); s_off_rt = false; s_tripped = false;
        if (s_valid) { s_hd.recent = 0; hd_seal(); }
        return "watchdog on, restart-loop count cleared\n";
    }
    if (!strcmp(arg, "status") || !arg[0]) return s_off_nvs ? "off\n" : s_tripped ? "loop-guard\n" : "on\n";
    return NULL;
}
