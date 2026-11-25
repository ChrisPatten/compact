#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * The two microphones (ES7210 on I2S, through the BSP and esp_codec_dev), for push-to-talk.
 * 16 kHz, 16-bit; the two channels are averaged to mono. Open only while recording: the ADC and the
 * I2S clocks cost power. One task uses it at a time.
 */
#define AUDIO_IN_RATE 16000

bool audio_in_open(void);                       /* power the mics and start the clocks; false on failure */
int audio_in_read(int16_t *mono, int samples);  /* blocks until `samples` mono samples are read; returns samples or -1 */
void audio_in_close(void);
