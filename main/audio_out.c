#include "audio_out.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "psram_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "intercom_ws.h"
#include "prefs.h"
#include "ptt.h"
#include "capture.h"

#define RING_BYTES   (1024 * 1024)   /* ~32 s at 16 kHz; the bus sends at most 20 s */
#define PREROLL_MS   150
#define BLOCK_MS     20
#define TAIL_MS      40              /* silence before the amp goes off: no click */

static const char *TAG = "audio_out";

typedef enum { S_IDLE, S_BUFFER, S_PLAY } state_t;
static volatile state_t s_state = S_IDLE;
static uint8_t *s_ring;
static volatile size_t s_head, s_tail;       /* write and read positions (bytes, mod RING_BYTES) */
static volatile bool s_ended, s_stop;
static volatile int s_rate = 16000;
static char s_id[24];
static esp_codec_dev_handle_t s_spk;
static bool s_open;
static int s_open_rate;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_idle;             /* given when the task has closed the codec */
static esp_pm_lock_handle_t s_pm;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_tone_hz, s_tone_ms;

static size_t avail(void) { return (s_head - s_tail + RING_BYTES) % RING_BYTES; }

static bool codec_open(int rate)
{
    if (s_open && s_open_rate == rate) return true;
    if (s_open) { esp_codec_dev_close(s_spk); s_open = false; }
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();           /* shares the duplex I2S with the microphones */
        if (!s_spk) { ESP_LOGE(TAG, "speaker init failed"); return false; }
        /* The library's default curve runs -50..0 dB, so mid-slider was ~27 dB down and nearly silent
           on this small speaker. 1 % = -30 dB up to 100 % = 0 dB; 0 never plays (muted). */
        esp_codec_dev_vol_map_t map[2] = { { .vol = 1, .db_value = -30.0f }, { .vol = 100, .db_value = 0.0f } };
        esp_codec_dev_vol_curve_t curve = { .vol_map = map, .count = 2 };
        esp_codec_dev_set_vol_curve(s_spk, &curve);
    }
    esp_codec_dev_sample_info_t fs = { .sample_rate = rate, .channel = 1, .bits_per_sample = 16 };
    int r = esp_codec_dev_open(s_spk, &fs);
    if (r != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "open failed (%d)", r); return false; }
    r = esp_codec_dev_set_out_vol(s_spk, prefs_volume());
    ESP_LOGI(TAG, "codec open at %d Hz, volume %d (%d)", rate, prefs_volume(), r);
    s_open = true;
    s_open_rate = rate;
    return true;
}

static void codec_close(void)
{
    if (!s_open) return;
    esp_codec_dev_close(s_spk);
    s_open = false;
}

static void write_silence(int ms)
{
    static int16_t z[320];
    int n = s_open_rate * ms / 1000;
    while (n > 0 && !s_stop) {
        int k = n > 320 ? 320 : n;
        esp_codec_dev_write(s_spk, z, k * 2);
        n -= k;
    }
}

static void play_tone(int hz, int ms)
{
    if (!codec_open(16000)) return;
    int16_t blk[320];
    int total = 16000 * ms / 1000, done = 0;
    while (done < total && !s_stop) {
        int k = total - done > 320 ? 320 : total - done;
        for (int i = 0; i < k; i++) {
            float env = fminf(1.0f, fminf((done + i) / 160.0f, (total - done - i) / 160.0f));   /* 10 ms ramps */
            blk[i] = (int16_t)(16000 * env * sinf(2 * 3.14159265f * hz * (done + i) / 16000.0f));
        }
        int w = esp_codec_dev_write(s_spk, blk, k * 2);
        if (w != ESP_CODEC_DEV_OK && done == 0) ESP_LOGE(TAG, "tone write failed (%d)", w);
        done += k;
    }
    write_silence(TAIL_MS);
}

/* A soft two-note bell: G5 then D6, each a decaying sine with a quiet octave partial. */
static void play_chime(void)
{
    if (!codec_open(16000)) return;
    int16_t blk[320];
    const int total = 16000 * 1600 / 1000;
    for (int done = 0; done < total && !s_stop;) {
        int k = total - done > 320 ? 320 : total - done;
        for (int i = 0; i < k; i++) {
            float t = (done + i) / 16000.0f, v = 0;
            v += expf(-t / 0.30f) * (sinf(6.2831853f * 784 * t) + 0.22f * sinf(6.2831853f * 1568 * t));
            if (t > 0.24f) { float u = t - 0.24f; v += expf(-u / 0.42f) * (sinf(6.2831853f * 1175 * u) + 0.18f * sinf(6.2831853f * 2350 * u)); }
            float att = fminf(1.0f, t / 0.008f), rel = fminf(1.0f, (total - done - i) / 1600.0f);
            blk[i] = (int16_t)(5200 * v * att * rel);
        }
        esp_codec_dev_write(s_spk, blk, k * 2);
        done += k;
    }
    write_silence(TAIL_MS);
}

static void task(void *arg)
{
    uint8_t blk[2 * 24000 * BLOCK_MS / 1000];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_pm) esp_pm_lock_acquire(s_pm);
        if (s_tone_ms) {
            int hz = s_tone_hz, ms = s_tone_ms;
            s_tone_ms = 0;
            ESP_LOGI(TAG, "tone %d Hz %d ms, volume %d", hz, ms, prefs_volume());
            if (prefs_volume() > 0) { if (hz < 0) play_chime(); else play_tone(hz, ms); }
            s_state = S_IDLE;
        }
        while (s_state != S_IDLE && !s_stop) {
            int rate = s_rate;
            size_t pre = (size_t)rate * 2 * PREROLL_MS / 1000;
            if (s_state == S_BUFFER) {
                if (avail() < pre && !s_ended) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
                if (!codec_open(rate)) { s_state = S_IDLE; break; }
                ESP_LOGI(TAG, "%s: playing (%u ms buffered)", s_id, (unsigned)(avail() * 1000 / 2 / rate));
                s_state = S_PLAY;
            }
            if (!s_open) { s_state = S_IDLE; break; }
            size_t want = (size_t)rate * 2 * BLOCK_MS / 1000, n = avail() & ~(size_t)1;
            if (n == 0) {
                if (s_ended) { write_silence(TAIL_MS); s_state = S_IDLE; break; }
                write_silence(BLOCK_MS);                      /* underrun: keep the clock running */
                continue;
            }
            if (n > want) n = want;
            size_t t = s_tail, first = RING_BYTES - t < n ? RING_BYTES - t : n;
            memcpy(blk, s_ring + t, first);
            if (n > first) memcpy(blk + first, s_ring, n - first);
            s_tail = (t + n) % RING_BYTES;
            esp_codec_dev_write(s_spk, blk, n);
        }
        codec_close();
        s_state = S_IDLE;
        if (s_pm) esp_pm_lock_release(s_pm);
        xSemaphoreGive(s_idle);
    }
}

void audio_out_init(void)
{
    s_ring = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_ring) { ESP_LOGE(TAG, "no PSRAM for the jitter buffer"); return; }
    s_idle = xSemaphoreCreateBinary();
#if CONFIG_PM_ENABLE
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "speech", &s_pm);
#endif
    task_create_psram(task, "speech", 6144, NULL, 6, &s_task);
}

void audio_out_begin(const char *id, int rate)
{
    if (!s_ring || prefs_volume() == 0 || ptt_recording() || capture_recording()) {    /* muted (the bus should not have sent it), or the mics hold the I2S */
        if (s_ring) intercom_speech_stop();
        return;
    }
    if (rate != 16000 && rate != 22050 && rate != 24000) { ESP_LOGW(TAG, "rate %d not supported", rate); return; }
    if (s_state != S_IDLE) audio_out_stop();                  /* a new utterance replaces the old */
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_id, id, sizeof s_id);
    s_head = s_tail = 0;
    s_ended = false;
    s_stop = false;
    s_rate = rate;
    s_state = S_BUFFER;
    taskEXIT_CRITICAL(&s_mux);
    xSemaphoreTake(s_idle, 0);                                /* clear a stale "idle" */
    xTaskNotifyGive(s_task);
}

void audio_out_data(const uint8_t *pcm, size_t n)
{
    if (s_state == S_IDLE || s_ended || !s_ring) return;
    size_t room = RING_BYTES - 1 - avail();
    if (n > room) n = room;                                   /* full: drop the rest (the bus caps length) */
    size_t h = s_head, first = RING_BYTES - h < n ? RING_BYTES - h : n;
    memcpy(s_ring + h, pcm, first);
    if (n > first) memcpy(s_ring, pcm + first, n - first);
    s_head = (h + n) % RING_BYTES;
}

void audio_out_end(const char *id)
{
    if (s_state != S_IDLE && !strcmp(id, s_id)) s_ended = true;
}

void audio_out_cancel(const char *id)
{
    if (s_state == S_IDLE || strcmp(id, s_id)) return;        /* an older utterance's cancel: ignore */
    ESP_LOGI(TAG, "%s: cancelled by the bus", id);
    s_stop = true;
}

void audio_out_stop(void)
{
    if (!s_ring || s_state == S_IDLE) return;
    bool from_bus = s_ended || s_tone_ms;
    s_stop = true;
    xSemaphoreTake(s_idle, pdMS_TO_TICKS(150));              /* codec closed: the microphones may open */
    if (!from_bus) intercom_speech_stop();                    /* the bus need not send the rest */
    ESP_LOGI(TAG, "stopped");
}

bool audio_out_playing(void) { return s_state != S_IDLE; }

void audio_out_tone(int hz, int ms)
{
    if (!s_ring || s_state != S_IDLE) return;
    s_stop = false;
    s_ended = true;
    s_head = s_tail = 0;
    s_tone_hz = hz;
    s_tone_ms = ms > 3000 ? 3000 : ms;
    s_state = S_PLAY;                                         /* counts as playing: a BOOT press waits for it */
    xSemaphoreTake(s_idle, 0);
    xTaskNotifyGive(s_task);
}

void audio_out_chime(void)
{
    if (!s_ring || s_state != S_IDLE || prefs_volume() == 0 || ptt_recording() || capture_recording()) return;
    s_stop = false;
    s_ended = true;
    s_head = s_tail = 0;
    s_tone_hz = -1;
    s_tone_ms = 1;
    s_state = S_PLAY;
    xSemaphoreTake(s_idle, 0);
    xTaskNotifyGive(s_task);
}

void audio_out_set_volume(int pct)
{
    if (s_open) esp_codec_dev_set_out_vol(s_spk, pct);
}
