#include "adpcm.h"

/* The standard IMA tables; the arithmetic matches tools/adpcm_ref.py step for step. */
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const int8_t INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

/* Apply one 4-bit code to (pred, idx): the decoder step, which the encoder also runs to track the decoder. */
static inline void step_once(int32_t *pred, int32_t *idx, unsigned code)
{
    int32_t s = STEP[*idx];
    int32_t diff = s >> 3;
    if (code & 4) diff += s;
    if (code & 2) diff += s >> 1;
    if (code & 1) diff += s >> 2;
    int32_t p = (code & 8) ? *pred - diff : *pred + diff;
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    int32_t i = *idx + INDEX[code & 15];
    if (i < 0) i = 0;
    if (i > 88) i = 88;
    *pred = p;
    *idx = i;
}

void adpcm_reset(adpcm_state_t *s)
{
    s->pred = 0;
    s->index = 0;
}

size_t adpcm_encode_block(adpcm_state_t *s, const int16_t *pcm, size_t n, uint8_t *out)
{
    if (n < 1 || n > ADPCM_MAX_SAMPLES) return 0;
    int32_t pred = s->pred, idx = s->index > 88 ? 88 : s->index;
    out[0] = ADPCM_CODEC_IMA;
    out[1] = (uint8_t)((uint16_t)pred & 0xFF);
    out[2] = (uint8_t)((uint16_t)pred >> 8);
    out[3] = (uint8_t)idx;
    out[4] = (uint8_t)(n & 0xFF);
    uint8_t *body = out + ADPCM_HDR;
    for (size_t i = 0; i < n; i++) {
        int32_t st = STEP[idx];
        int32_t d = (int32_t)pcm[i] - pred;
        unsigned code = 0;
        if (d < 0) { code = 8; d = -d; }
        if (d >= st) { code |= 4; d -= st; }
        if (d >= (st >> 1)) { code |= 2; d -= st >> 1; }
        if (d >= (st >> 2)) code |= 1;
        step_once(&pred, &idx, code);
        if (i & 1) body[i >> 1] |= (uint8_t)(code << 4);
        else body[i >> 1] = (uint8_t)code;               /* low nibble first; a lone last code leaves 0 above it */
    }
    s->pred = (int16_t)pred;
    s->index = (uint8_t)idx;
    return ADPCM_BLOCK_BYTES(n);
}

int adpcm_decode_block(const uint8_t *blk, size_t len, int16_t *out, size_t max)
{
    if (len < ADPCM_HDR || blk[0] != ADPCM_CODEC_IMA || blk[3] > 88) return -1;
    size_t n = blk[4] ? blk[4] : 256;
    if (n > max || len < ADPCM_BLOCK_BYTES(n)) return -1;
    int32_t pred = (int16_t)((uint16_t)blk[1] | ((uint16_t)blk[2] << 8)), idx = blk[3];
    const uint8_t *body = blk + ADPCM_HDR;
    for (size_t i = 0; i < n; i++) {
        unsigned code = (i & 1) ? body[i >> 1] >> 4 : body[i >> 1] & 0x0F;
        step_once(&pred, &idx, code);
        out[i] = (int16_t)pred;
    }
    return (int)n;
}
