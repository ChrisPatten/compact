/* Quiesce, then restart: see restart.h. */
#include "restart.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "display.h"
#include "intercom_ws.h"

static const char *TAG = "restart";
static volatile bool s_started;
static const char *s_why = "";
static bool s_hung;
static void (*s_final)(void);
static SemaphoreHandle_t s_ws_done;

static void hard_reset_cb(void *arg) { (void)arg; esp_rom_software_reset_system(); }

static void ws_stop_task(void *arg)
{
    (void)arg;
    intercom_stop_for_restart();                                     /* may take up to ~1 s: the client polls 1 s */
    if (s_ws_done) xSemaphoreGive(s_ws_done);
    vTaskDelete(NULL);                                               /* never reached in practice: the reset comes first */
}

static void restart_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    bool paused = false;
    if (!s_hung) paused = compact_display_quiesce(RESTART_LVGL_MS);  /* ring off, LVGL parked (bounded) */
    int64_t t1 = esp_timer_get_time();
    s_ws_done = xSemaphoreCreateBinary();
    bool ws = false;
    if (s_ws_done && xTaskCreate(ws_stop_task, "ws_stop", 3072, NULL, 9, NULL) == pdPASS)
        ws = xSemaphoreTake(s_ws_done, pdMS_TO_TICKS(RESTART_WS_MS)) == pdTRUE;
    ESP_LOGW(TAG, "%s: lvgl %s in %lld ms, websocket %s in %lld ms; restarting", s_why,
             s_hung ? "hung (not waited on)" : paused ? "paused" : "NOT paused", (t1 - t0) / 1000,
             ws ? "stopped" : "still stopping", (esp_timer_get_time() - t1) / 1000);
    if (s_final) s_final();
    esp_restart();
}

static void start(const char *why, bool hung, void (*final_step)(void))
{
    if (__atomic_exchange_n(&s_started, true, __ATOMIC_SEQ_CST)) return;   /* one restart at a time */
    s_why = why ? why : "";
    s_hung = hung;
    s_final = final_step;
    ESP_LOGW(TAG, "restart requested: %s", s_why);
    esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = hard_reset_cb, .name = "hard_reset" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, RESTART_HARD_MS * 1000LL);
    /* Internal stack, above LVGL (6) and the display watchdog (7): it must run while LVGL is busy or hung. The final
       step may map flash (the OTA rollback), which needs an internal stack. */
    if (xTaskCreate(restart_task, "restart", 4096, NULL, 10, NULL) != pdPASS) {
        if (final_step) final_step();
        esp_restart();
    }
}

void compact_restart(const char *why) { start(why, false, NULL); }
void compact_restart_hung(const char *why) { start(why, true, NULL); }
void compact_restart_with(const char *why, void (*final_step)(void)) { start(why, false, final_step); }
bool compact_restarting(void) { return s_started; }
