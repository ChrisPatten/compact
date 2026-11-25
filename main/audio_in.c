#include "audio_in.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

#define GAIN_DB   30.0f       /* ES7210 PGA; the esp_codec_dev default. Speech at desk distance peaks well above -30 dBFS */
#define MAX_CHUNK 320         /* 20 ms of mono per codec read */

static const char *TAG = "audio_in";
static esp_codec_dev_handle_t s_mic;
static bool s_open;

bool audio_in_open(void)
{
    if (s_open) return true;
    if (!s_mic) {
        s_mic = bsp_audio_codec_microphone_init();             /* I2S duplex + ES7210 (MIC1, MIC2), once */
        if (!s_mic) { ESP_LOGE(TAG, "microphone init failed"); return false; }
    }
    esp_codec_dev_sample_info_t fs = { .sample_rate = AUDIO_IN_RATE, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "open failed"); return false; }
    esp_codec_dev_set_in_gain(s_mic, GAIN_DB);
    s_open = true;
    return true;
}

int audio_in_read(int16_t *mono, int samples)
{
    if (!s_open) return -1;
    int16_t st[2 * MAX_CHUNK];
    int done = 0;
    while (done < samples) {
        int n = samples - done > MAX_CHUNK ? MAX_CHUNK : samples - done;
        if (esp_codec_dev_read(s_mic, st, n * 4) != ESP_CODEC_DEV_OK) return -1;
        for (int i = 0; i < n; i++) mono[done + i] = (int16_t)(((int32_t)st[2 * i] + st[2 * i + 1]) / 2);
        done += n;
    }
    return done;
}

void audio_in_close(void)
{
    if (!s_open) return;
    esp_codec_dev_close(s_mic);
    s_open = false;
}
