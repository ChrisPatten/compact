#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Spoken replies: 16-bit mono PCM streamed by the bus (`speech_start`, binary frames, `speech_end` /
 * `speech_cancel`) played through the ES8311 codec and the NS4150B amp. The I2S port is shared with the
 * ES7210 microphones (duplex, same clock): playback and recording never run together, so a BOOT press
 * stops playback before the microphones open.
 *
 * Frames land in a PSRAM jitter buffer; playback starts once 150 ms is buffered (or the utterance has
 * ended), within ~200 ms of the first frame. An underrun plays silence until more arrives. The codec and
 * amp are powered only while playing.
 */
void audio_out_init(void);                                  /* once, after audio_in is usable */

/* socket task */
void audio_out_begin(const char *id, int rate);             /* speech_start: replaces anything playing */
void audio_out_data(const uint8_t *pcm, size_t n);          /* binary frame (any split; bytes in order) */
void audio_out_end(const char *id);                         /* speech_end: play out what is buffered */
void audio_out_cancel(const char *id);                      /* speech_cancel: stop, if `id` is current */

/* any task */
void audio_out_stop(void);                                  /* PWR, BOOT, sleep: silent at once, tells the bus */
bool audio_out_playing(void);                               /* speaking now (the speaker mark) */
void audio_out_tone(int hz, int ms);
void audio_out_chime(void);                                 /* a soft two-note bell (Pomodoro); silent at volume 0, while recording */                        /* a test or volume-preview tone */
void audio_out_set_volume(int pct);                         /* 0-100 (0 = muted), applied at once */
