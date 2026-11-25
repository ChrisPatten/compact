#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "display.h"
#include "fonts.h"
#include "dial.h"
#include "settings.h"
#include "ota.h"
#include "logbuf.h"
#include "panicdiag.h"
#include "displaywd.h"
#include "memdiag.h"
#include "intercom_ws.h"
#include "pomodoro.h"
#include "commands.h"
#include "power.h"
#include "decision.h"
#include "earlier.h"
#include "prefs.h"
#include "setscreen.h"
#include "dayview.h"
#include "ptt.h"
#include "capture.h"
#include "audio_out.h"
#include "thread.h"
#include "agents.h"

static settings_t s_cfg;

static void on_moments(const cJSON *events)
{
    if (!dial_testday_active()) {      /* POST /debug/ui "testday": test Moments stand in on Now and Day */
        dial_set_moments(events);
        dayview_on_events(events);
    }
    decision_on_events(events);
    power_on_moments(events);
}

static void on_notices(const cJSON *notices)
{
    decision_on_notices(notices);
    power_on_notices(notices);
}

static void on_action_result(const char *key, const char *status, const char *reason)
{
    decision_on_action_result(key, status, reason);   /* each ignores keys it did not send */
    dayview_on_action_result(key, status, reason);
    thread_on_action_result(key, status, reason);
    agents_on_action_result(key, status, reason);
}

static void on_ack(const char *cmid, const char *status, const char *reason, const char *interaction_id)
{
    thread_on_ack(cmid, status, reason, interaction_id);
    decision_on_ack(cmid, status, reason, interaction_id);    /* a late reply's SEND TO PEGGY (E105); off by default */
}

static void on_conversation(const char *conv_id, const cJSON *page, int http_status)
{
    thread_on_conversation(conv_id, page, http_status);          /* each keeps only the conversation it asked for */
    agents_on_conversation(conv_id, page, http_status);
}

static void on_activity(const char *state, const char *status_text)
{
    dial_set_activity(state, status_text);
    thread_on_activity(state, status_text);
}

static void on_speech(const char *kind, const char *id, int rate)
{
    if (!strcmp(kind, "start")) audio_out_begin(id, rate);
    else if (!strcmp(kind, "end")) audio_out_end(id);
    else if (!strcmp(kind, "cancel")) audio_out_cancel(id);
}

static void on_paired(void)
{
    ota_mark_good();
    intercom_send_state(power_state() == PWR_SLEEP, power_battery_pct());
}

static void on_net_up(void)
{
    dial_net_up();
    ota_server_start(s_cfg.token);
}

void app_main(void)
{
    logbuf_init();
    memdiag_trace_start();             /* heap-tracing debug builds only */
    panicdiag_init();
    displaywd_init();                  /* validate the last display-hang record (RTC memory) */
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    ota_boot_check();
    settings_load(&s_cfg);
    prefs_load();

    compact_display_start();
    displaywd_start();                 /* display watchdog: restarts the device if LVGL stops for 5 s */
    bsp_display_lock(-1);
    fonts_init();
    dial_create();
    bsp_display_unlock();
    dayview_init();                    /* Day list / Event detail: above the face, under Earlier */
    ptt_init();
    capture_init();                    /* Capture: PSRAM ring, flash spill, recorder and uploader tasks */
    pomodoro_init();                   /* Pomodoro: the on-device timer (starts only on request) */
    commands_init();                   /* device commands: registry and task; before the bus hello lists the ops */
    audio_out_init();                  /* spoken replies: ES8311 + amp, PSRAM jitter buffer */                        /* audio buffer, talk fonts, recorder and sender tasks */
    thread_init();                     /* a face: above Now, below Earlier, Settings and the alerts */
    agents_init();                     /* the face left of Thread */
    earlier_init();                    /* below the alert takeover: created first */
    setscreen_init();
    decision_init();
    ESP_LOGI("heap", "internal free=%u largest=%u psram_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const intercom_cbs_t cbs = {
        .link = dial_set_link,
        .net_up = on_net_up,
        .paired = on_paired,
        .activity = on_activity,
        .moments = on_moments,
        .notices = on_notices,
        .action_result = on_action_result,
        .ack = on_ack,
        .transcript = ptt_on_transcript,
        .reply = thread_on_reply,
        .conv_append = thread_on_conv_append,
        .conversation = on_conversation,
        .sessions = agents_on_sessions,
        .widgets = dial_set_widgets,
        .speech = on_speech,
        .speech_data = audio_out_data,
        .capture_ack = capture_on_ack,
        .notification = decision_on_notification,          /* E105 closing words; returns at once with the flag off */
    };
    intercom_start(&s_cfg, &cbs);
    power_start();
}
