/* Host check of the firmware's ADPCM codec (main/adpcm.c) against docs/adpcm-test-vector.json (made by
 * tools/adpcm_ref.py). Build and run on the Mac from the repo root:
 *
 *   cc -std=c11 -Wall -Wextra -O2 -o /tmp/adpcm_test tools/adpcm_test.c && /tmp/adpcm_test docs/adpcm-test-vector.json
 *
 * Exit status 0 when every check passes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/adpcm.c"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

/* The integer array after "key": [ ... ]. Returns the count, or -1. */
static int int_array(const char *json, const char *key, int *out, int max)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p || !(p = strchr(p, '['))) return -1;
    p++;
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p == ']') return n;
        char *e;
        long v = strtol(p, &e, 10);
        if (e == p || n >= max) return -1;
        out[n++] = (int)v;
        p = e;
    }
}

/* The hex string after "key": "...", as bytes. Returns the byte count, or -1. */
static int hex_string(const char *json, const char *key, uint8_t *out, int max)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p || !(p = strchr(p + strlen(pat), '"'))) return -1;
    p++;
    int n = 0;
    while (p[0] && p[0] != '"') {
        unsigned v;
        if (!p[1] || sscanf(p, "%2x", &v) != 1 || n >= max) return -1;
        out[n++] = (uint8_t)v;
        p += 2;
    }
    return n;
}

static int fails;
static void check(int ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "docs/adpcm-test-vector.json";
    char *json = slurp(path);
    if (!json) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    int pcm_i[512], dec_i[512];
    uint8_t want[512];
    int np = int_array(json, "pcm", pcm_i, 512), nd = int_array(json, "decoded", dec_i, 512);
    int nb = hex_string(json, "block_hex", want, 512);
    if (np != 256 || nd != 256 || nb <= 0) { fprintf(stderr, "bad vector: pcm %d, decoded %d, block %d\n", np, nd, nb); return 2; }
    int16_t pcm[256], dec_want[256];
    for (int i = 0; i < 256; i++) { pcm[i] = (int16_t)pcm_i[i]; dec_want[i] = (int16_t)dec_i[i]; }
    printf("vector %s: %d samples, block %d bytes\n", path, np, nb);

    /* 1. one 256-sample block from a cold encoder reproduces block_hex byte for byte */
    adpcm_state_t st;
    adpcm_reset(&st);
    uint8_t blk[ADPCM_BLOCK_BYTES(256)];
    size_t len = adpcm_encode_block(&st, pcm, 256, blk);
    int first_diff = -1;
    for (int i = 0; i < nb && i < (int)len; i++) if (blk[i] != want[i]) { first_diff = i; break; }
    char msg[160];
    snprintf(msg, sizeof msg, "encode: %u bytes, equals block_hex%s", (unsigned)len,
             first_diff >= 0 ? " (first difference shown below)" : "");
    check(len == (size_t)nb && first_diff < 0, msg);
    if (first_diff >= 0) printf("      byte %d: got %02x want %02x\n", first_diff, blk[first_diff], want[first_diff]);

    /* 2. decoding block_hex gives `decoded` exactly */
    int16_t out[256];
    int k = adpcm_decode_block(want, (size_t)nb, out, 256);
    int dd = -1;
    for (int i = 0; i < 256 && k == 256; i++) if (out[i] != dec_want[i]) { dd = i; break; }
    check(k == 256 && dd < 0, "decode block_hex: 256 samples, equals decoded");
    if (dd >= 0) printf("      sample %d: got %d want %d\n", dd, out[dd], dec_want[dd]);

    int maxerr = 0;
    for (int i = 0; i < 256; i++) { int e = abs(out[i] - pcm[i]); if (e > maxerr) maxerr = e; }
    printf("      max |pcm - decoded| = %d\n", maxerr);

    /* 3. the same stream in blocks of 100, 100 and 56 (encoder state carried over, each block decoded on its own)
          decodes to the same samples as the single block */
    adpcm_reset(&st);
    int16_t joined[256];
    size_t at = 0, sizes[3] = { 100, 100, 56 };
    int ok3 = 1;
    for (int b = 0; b < 3; b++) {
        uint8_t bb[ADPCM_BLOCK_BYTES(256)];
        size_t bl = adpcm_encode_block(&st, pcm + at, sizes[b], bb);
        int got = adpcm_decode_block(bb, bl, joined + at, 256 - at);
        if (bl != ADPCM_BLOCK_BYTES(sizes[b]) || got != (int)sizes[b]) ok3 = 0;
        at += sizes[b];
    }
    for (int i = 0; i < 256 && ok3; i++) if (joined[i] != dec_want[i]) ok3 = 0;
    check(ok3, "three blocks (100+100+56) with carried state decode to the same samples");

    /* 4. an odd partial block (255 samples, count byte 255) decodes to the first 255 samples */
    adpcm_reset(&st);
    len = adpcm_encode_block(&st, pcm, 255, blk);
    k = adpcm_decode_block(blk, len, out, 256);
    int ok4 = len == ADPCM_BLOCK_BYTES(255) && blk[4] == 255 && k == 255 && (blk[len - 1] >> 4) == 0;
    for (int i = 0; i < 255 && ok4; i++) if (out[i] != dec_want[i]) ok4 = 0;
    check(ok4, "255-sample block: 133 bytes, count 255, high nibble of the last byte 0, same samples");

    /* 5. malformed blocks are refused */
    uint8_t bad[ADPCM_BLOCK_BYTES(256)];
    memcpy(bad, want, (size_t)nb);
    bad[0] = 0x02;
    int r1 = adpcm_decode_block(bad, (size_t)nb, out, 256);
    memcpy(bad, want, (size_t)nb);
    bad[3] = 89;
    int r2 = adpcm_decode_block(bad, (size_t)nb, out, 256);
    int r3 = adpcm_decode_block(want, (size_t)nb - 1, out, 256);
    int r4 = adpcm_decode_block(want, (size_t)nb, out, 128);
    check(r1 < 0 && r2 < 0 && r3 < 0 && r4 < 0, "refuses a wrong codec, index 89, a short block and too many samples");

    free(json);
    printf("%s\n", fails ? "ADPCM HOST TEST FAILED" : "ADPCM HOST TEST OK");
    return fails ? 1 : 0;
}
