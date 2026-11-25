#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * IMA ADPCM block codec for the phone link (docs/phone-link-v2.md, section 3). No ESP-IDF dependencies, so the host
 * test (tools/adpcm_test.c) compiles it as is.
 *
 * Block layout (the payload of one BINARY chunk, after the flags byte):
 *   byte 0     codec, ADPCM_CODEC_IMA (0x01)
 *   bytes 1-2  int16 LE predictor: the encoder state before the first sample (the decoder's initial state)
 *   byte 3     step index, 0-88
 *   byte 4     sample count n (1-255; 0 means 256)
 *   bytes 5..  ceil(n/2) bytes of 4-bit codes, low nibble first
 * Each block decodes on its own. The encoder carries its state across the blocks of one stream; reset it at the
 * start of each stream (predictor 0, index 0).
 */

#define ADPCM_CODEC_IMA     0x01
#define ADPCM_HDR           5
#define ADPCM_MAX_SAMPLES   256
#define ADPCM_BLOCK_BYTES(n) (ADPCM_HDR + ((size_t)(n) + 1) / 2)

typedef struct {
    int16_t pred;
    uint8_t index;
} adpcm_state_t;

void adpcm_reset(adpcm_state_t *s);

/* Encode n samples (1..256) into out (at least ADPCM_BLOCK_BYTES(n) bytes), advancing s. Returns the block length,
   or 0 when n is out of range. */
size_t adpcm_encode_block(adpcm_state_t *s, const int16_t *pcm, size_t n, uint8_t *out);

/* Decode one block into out (room for max samples). Returns the sample count, or -1 for a malformed block (wrong
   codec, index over 88, too short, or more samples than max). */
int adpcm_decode_block(const uint8_t *blk, size_t len, int16_t *out, size_t max);
