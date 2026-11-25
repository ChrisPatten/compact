#include "capture.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "psram_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "mbedtls/base64.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"
#include "src/core/lv_obj_private.h"
#include "fonts.h"
#include "dial.h"
#include "audio_in.h"
#include "adpcm.h"
#include "intercom_ws.h"
#include "power.h"
#include "ptt.h"
#include "display.h"

/* Plum Dusk tokens (spec 3.1); `rec` only here */
#define COL_INK   0xF8ECE8
#define COL_INK2  0xCDB5C6
#define COL_HAIR  0x8B7098
#define COL_ROSE  0xF5A99B
#define COL_REC   0xFF6B81
#define COL_AMBER 0xF7C95C

#define RATE          AUDIO_IN_RATE
#define BLK           256                                  /* samples per ADPCM block */
#define BLKS          125                                  /* blocks per chunk: 32,000 samples = 2 s */
#define CHUNK_SAMPLES (BLK * BLKS)
#define CHUNK_MAX     (BLKS * ADPCM_BLOCK_BYTES(BLK))      /* 16,625 bytes */
#define RING_N        48                                   /* 96 s in PSRAM (~800 KB) */
#define MAX_CHUNKS    3600                                 /* 2 h */
#define WINDOW        4                                    /* chunks in flight before an ack */
#define RESEND_US     (15LL * 1000000)
#define START_RETRY_US (5LL * 1000000)
#define END_RETRY_US  (10LL * 1000000)
#define SPILL_AFTER_US (8LL * 1000000)                     /* offline this long: chunks go to flash */
#define TAB_N         4                                    /* recordings waiting to upload */
#define ID_LEN        24
#define FL_SLOT       20480                                /* 5 flash sectors per chunk */
#define FL_HDR        64
#define FL_MAGIC      0x4B504143u                          /* "CAPK" */
#define HOLD_MS       800
#define IDLE_LOOK_MS  60000                                /* spec 7.6: the hour-in look after 60 s untouched */
#define SAVED_MS      5000

static const char *TAG = "capture";
enum { R_STOPPED, R_MAX, R_BATTERY, R_FULL, R_INTERRUPTED };
static const char *REASON[] = { "stopped", "max", "battery", "storage_full", "interrupted" };

/* ---- the recordings table (NVS "capture"/"tab"): what still has to reach the bus ---- */
typedef struct { char id[ID_LEN]; uint32_t started; uint32_t chunks; uint32_t dur_ms; uint8_t used, ended, reason, pad; } tab_t;
static tab_t s_tab[TAB_N];
typedef struct { bool started; int64_t start_sent, end_sent; uint32_t acked; } up_t;
static up_t s_up[TAB_N];
static volatile bool s_tab_dirty;

/* ---- chunks: PSRAM ring and flash slots ---- */
enum { S_FREE, S_FILL, S_READY, S_FLIGHT, S_SPILL, S_FREEME };
typedef struct { int8_t cap; uint8_t st; uint16_t nbytes; uint32_t seq; int64_t sent_at; uint8_t *data; } rslot_t;
typedef struct { int8_t cap; uint8_t st; uint16_t nbytes; uint32_t seq; int64_t sent_at; } fslot_t;
static rslot_t s_ring[RING_N];
static fslot_t *s_fl;                                       /* PSRAM, s_fl_n */
static int s_fl_n, s_fl_next;
static const esp_partition_t *s_part;
typedef struct __attribute__((packed)) { uint32_t magic, seq, samples, nbytes, crc, started; char id[ID_LEN]; uint8_t pad[FL_HDR - 24 - ID_LEN]; } fl_hdr_t;
_Static_assert(sizeof(fl_hdr_t) == FL_HDR, "flash header");

static SemaphoreHandle_t s_mu;
static TaskHandle_t s_rec_task, s_up_task;
static esp_pm_lock_handle_t s_pm;
static char *s_json;                                        /* PSRAM: one capture_chunk frame */
static uint8_t *s_flbuf;                                    /* PSRAM: one flash slot (header + payload) */
#define JSON_MAX (CHUNK_MAX * 4 / 3 + 256)

/* ---- recording state ---- */
static volatile bool s_rec, s_stop_req, s_tone, s_mic_failed, s_flush_req;
static volatile int s_rec_cap = -1, s_stop_reason;
static volatile uint32_t s_rec_samples;
static volatile int64_t s_rec_t0, s_debug_stop_at;
static volatile bool s_flash_full, s_online;
static volatile int64_t s_offline_until;                    /* test hook: act as if the bus were unreachable */
static volatile int s_alerts;
static char s_last[64] = "none";                            /* last result, for /info */

static void L(void) { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void U(void) { xSemaphoreGive(s_mu); }

/* ---------------------------------------------------------------- table ---- */

static void tab_save(void)
{
    nvs_handle_t h;
    tab_t copy[TAB_N];
    L(); memcpy(copy, s_tab, sizeof copy); U();
    if (nvs_open("capture", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "tab", copy, sizeof copy);
    nvs_commit(h);
    nvs_close(h);
}

static void tab_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof s_tab;
    memset(s_tab, 0, sizeof s_tab);
    if (nvs_open("capture", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "tab", s_tab, &n) != ESP_OK || n != sizeof s_tab) memset(s_tab, 0, sizeof s_tab);
    nvs_close(h);
    for (int i = 0; i < TAB_N; i++) s_tab[i].id[ID_LEN - 1] = 0;
}

static int tab_find(const char *id)
{
    for (int i = 0; i < TAB_N; i++) if (s_tab[i].used && !strcmp(s_tab[i].id, id)) return i;
    return -1;
}

static int tab_add(const char *id, uint32_t started)
{
    for (int i = 0; i < TAB_N; i++) {
        if (s_tab[i].used) continue;
        memset(&s_tab[i], 0, sizeof s_tab[i]);
        strlcpy(s_tab[i].id, id, ID_LEN);
        s_tab[i].started = started;
        s_tab[i].used = 1;
        memset(&s_up[i], 0, sizeof s_up[i]);
        s_tab_dirty = true;
        return i;
    }
    return -1;
}

/* The recording to upload next: the oldest one in the table. */
static int tab_current(void)
{
    int best = -1;
    for (int i = 0; i < TAB_N; i++)
        if (s_tab[i].used && (best < 0 || s_tab[i].started < s_tab[best].started)) best = i;
    return best;
}

/* -------------------------------------------------------------- chunks ---- */

static int ring_used(void)
{
    int n = 0;
    for (int i = 0; i < RING_N; i++) n += s_ring[i].st != S_FREE;
    return n;
}

static int held(int c)                                     /* chunks of recording c still on the device */
{
    int n = 0;
    for (int i = 0; i < RING_N; i++) n += s_ring[i].st != S_FREE && s_ring[i].cap == c;
    for (int i = 0; i < s_fl_n; i++) n += s_fl[i].st != S_FREE && s_fl[i].st != S_FREEME && s_fl[i].cap == c;
    return n;
}

static void free_chunk(int c, uint32_t seq)
{
    for (int i = 0; i < RING_N; i++)
        if (s_ring[i].cap == c && s_ring[i].seq == seq && (s_ring[i].st == S_READY || s_ring[i].st == S_FLIGHT)) s_ring[i].st = S_FREE;
    for (int i = 0; i < s_fl_n; i++)
        if (s_fl[i].cap == c && s_fl[i].seq == seq && (s_fl[i].st == S_READY || s_fl[i].st == S_FLIGHT)) s_fl[i].st = S_FREEME;
}

static void forget(int c, const char *why)
{
    for (int i = 0; i < RING_N; i++) if (s_ring[i].cap == c && s_ring[i].st != S_FILL && s_ring[i].st != S_SPILL) s_ring[i].st = S_FREE;
    for (int i = 0; i < s_fl_n; i++) if (s_fl[i].cap == c && s_fl[i].st != S_FREE) s_fl[i].st = S_FREEME;
    snprintf(s_last, sizeof s_last, "%s %s", s_tab[c].id, why);
    ESP_LOGI(TAG, "%s: %s", s_tab[c].id, why);
    s_tab[c].used = 0;
    memset(&s_up[c], 0, sizeof s_up[c]);
    s_tab_dirty = true;
}

/* --------------------------------------------------------------- flash ---- */

static void flash_scan(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!s_part) { ESP_LOGW(TAG, "no storage partition: no offline spill"); return; }
    s_fl_n = (int)(s_part->size / FL_SLOT);
    s_fl = heap_caps_calloc(s_fl_n, sizeof *s_fl, MALLOC_CAP_SPIRAM);
    if (!s_fl) { s_fl_n = 0; return; }
    int found = 0;
    for (int i = 0; i < s_fl_n; i++) {
        fl_hdr_t h;
        s_fl[i].st = S_FREE;
        if (esp_partition_read(s_part, (size_t)i * FL_SLOT, &h, sizeof h) != ESP_OK) continue;
        if (h.magic != FL_MAGIC || h.nbytes == 0 || h.nbytes > CHUNK_MAX || h.seq >= MAX_CHUNKS + 8) {
            if (h.magic != 0xFFFFFFFFu && h.magic != 0) s_fl[i].st = S_FREEME;   /* torn write: clear it */
            continue;
        }
        h.id[ID_LEN - 1] = 0;
        int c = tab_find(h.id);
        if (c < 0 && (c = tab_add(h.id, h.started)) >= 0) {           /* the table lost it: finish it as interrupted */
            s_tab[c].ended = 1;
            s_tab[c].reason = R_INTERRUPTED;
        }
        if (c < 0) { s_fl[i].st = S_FREEME; continue; }
        s_fl[i] = (fslot_t){ .cap = (int8_t)c, .st = S_READY, .nbytes = (uint16_t)h.nbytes, .seq = h.seq };
        if (h.seq + 1 > s_tab[c].chunks && s_tab[c].reason == R_INTERRUPTED) s_tab[c].chunks = h.seq + 1;
        found++;
    }
    ESP_LOGI(TAG, "storage: %d slots of %d KB, %d chunks waiting", s_fl_n, FL_SLOT / 1024, found);
}

/* Flash reads into PSRAM in 1 KB pieces: esp_flash bounces a PSRAM destination through an internal buffer the size of
   the read (up to 16 KB), which took internal free RAM down to 8 KB during uploads from flash. */
static esp_err_t fl_read(size_t off, uint8_t *dst, size_t len)
{
    for (size_t done = 0; done < len; done += 1024) {
        size_t n = len - done < 1024 ? len - done : 1024;
        esp_err_t e = esp_partition_read(s_part, off + done, dst + done, n);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

static esp_err_t fl_write(size_t off, const uint8_t *src, size_t len)
{
    for (size_t done = 0; done < len; done += 1024) {
        size_t n = len - done < 1024 ? len - done : 1024;
        esp_err_t e = esp_partition_write(s_part, off + done, src + done, n);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

/* Clear slots whose chunk was acked (write 0 over the magic; no erase needed). Uploader task. */
static void flash_reap(void)
{
    for (int i = 0; i < s_fl_n; i++) {
        L();
        bool reap = s_fl[i].st == S_FREEME;
        U();
        if (!reap) continue;
        uint32_t zero = 0;
        esp_partition_write(s_part, (size_t)i * FL_SLOT, &zero, sizeof zero);
        L(); if (s_fl[i].st == S_FREEME) s_fl[i].st = S_FREE; U();
    }
}

/* Move the oldest unsent ring chunk to flash. Uploader task. False when there was nothing to move or no room. */
static bool spill_one(void)
{
    L();
    int r = -1, f = -1;
    for (int i = 0; i < RING_N; i++) {
        rslot_t *s = &s_ring[i];
        if (s->st != S_READY) continue;
        if (r < 0 || s_tab[s->cap].started < s_tab[s_ring[r].cap].started ||
            (s->cap == s_ring[r].cap && s->seq < s_ring[r].seq)) r = i;
    }
    if (r >= 0) {
        for (int k = 0; k < s_fl_n; k++) {
            int i = (s_fl_next + k) % s_fl_n;
            if (s_fl[i].st == S_FREE) { f = i; break; }
        }
        s_flash_full = f < 0;
    }
    if (r < 0 || f < 0) { U(); return false; }
    rslot_t snap = s_ring[r];
    s_ring[r].st = S_SPILL;
    s_fl[f].st = S_SPILL;
    s_fl_next = (f + 1) % s_fl_n;
    fl_hdr_t *h = (fl_hdr_t *)s_flbuf;
    memset(h, 0xFF, sizeof *h);
    h->magic = FL_MAGIC; h->seq = snap.seq; h->samples = CHUNK_SAMPLES; h->nbytes = snap.nbytes; h->started = s_tab[snap.cap].started;
    memcpy(h->id, s_tab[snap.cap].id, ID_LEN);
    U();
    memcpy(s_flbuf + FL_HDR, snap.data, snap.nbytes);
    h->crc = esp_rom_crc32_le(0, snap.data, snap.nbytes);
    size_t off = (size_t)f * FL_SLOT;
    bool ok = esp_partition_erase_range(s_part, off, FL_SLOT) == ESP_OK &&
              fl_write(off, s_flbuf, FL_HDR + snap.nbytes) == ESP_OK;
    L();
    if (ok) {
        s_fl[f] = (fslot_t){ .cap = snap.cap, .st = S_READY, .nbytes = snap.nbytes, .seq = snap.seq };
        s_ring[r].st = S_FREE;
    } else {
        s_fl[f].st = S_FREE;
        s_ring[r].st = S_READY;
        ESP_LOGE(TAG, "spill to flash failed");
    }
    U();
    return ok;
}

/* -------------------------------------------------------------- upload ---- */

static size_t json_head(char *out, size_t n, const char *type, int c)
{
    return (size_t)snprintf(out, n, "{\"type\":\"%s\",\"id\":\"%s\"", type, s_tab[c].id);
}

static bool send_start(int c)
{
    char buf[256];
    size_t n = json_head(buf, sizeof buf, "capture_start", c);
    snprintf(buf + n, sizeof buf - n, ",\"rate\":%d,\"codec\":\"ima_adpcm\",\"chunk_samples\":%d,\"started_at\":%llu}",
             RATE, CHUNK_SAMPLES, (unsigned long long)s_tab[c].started * 1000ULL);
    return intercom_capture_send(buf, strlen(buf));
}

static bool send_end(int c)
{
    char buf[256];
    size_t n = json_head(buf, sizeof buf, "capture_end", c);
    snprintf(buf + n, sizeof buf - n, ",\"chunks\":%lu,\"duration_ms\":%lu,\"reason\":\"%s\"}",
             (unsigned long)s_tab[c].chunks, (unsigned long)s_tab[c].dur_ms, REASON[s_tab[c].reason % 5]);
    return intercom_capture_send(buf, strlen(buf));
}

static bool send_chunk(int c, uint32_t seq, const uint8_t *data, size_t len)
{
    size_t n = json_head(s_json, JSON_MAX, "capture_chunk", c);
    n += snprintf(s_json + n, JSON_MAX - n, ",\"seq\":%lu,\"data\":\"", (unsigned long)seq);
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)s_json + n, JSON_MAX - n - 4, &olen, data, len) != 0) return false;
    n += olen;
    s_json[n++] = '"'; s_json[n++] = '}'; s_json[n] = 0;
    return intercom_capture_send(s_json, n);
}

/* One step of the upload of the current recording. Uploader task, lock not held. */
static void upload_step(int64_t now)
{
    L();
    int c = tab_current();
    if (c < 0) { U(); return; }
    up_t *u = &s_up[c];
    if (!u->started) {
        bool go = now - u->start_sent > START_RETRY_US;
        if (go) u->start_sent = now;
        U();
        if (go && !send_start(c)) ESP_LOGW(TAG, "capture_start not sent");
        return;
    }
    int flying = 0;
    for (int i = 0; i < RING_N; i++) {
        rslot_t *s = &s_ring[i];
        if (s->cap != c || s->st != S_FLIGHT) continue;
        if (now - s->sent_at > RESEND_US) s->st = S_READY; else flying++;
    }
    for (int i = 0; i < s_fl_n; i++) {
        fslot_t *s = &s_fl[i];
        if (s->cap != c || s->st != S_FLIGHT) continue;
        if (now - s->sent_at > RESEND_US) s->st = S_READY; else flying++;
    }
    while (flying < WINDOW) {
        int ri = -1, fi = -1;
        uint32_t best = UINT32_MAX;
        for (int i = 0; i < RING_N; i++)
            if (s_ring[i].cap == c && s_ring[i].st == S_READY && s_ring[i].seq < best) { best = s_ring[i].seq; ri = i; }
        for (int i = 0; i < s_fl_n; i++)
            if (s_fl[i].cap == c && s_fl[i].st == S_READY && s_fl[i].seq < best) { best = s_fl[i].seq; fi = i; ri = -1; }
        if (ri < 0 && fi < 0) break;
        bool ok;
        if (ri >= 0) {
            s_ring[ri].st = S_FLIGHT; s_ring[ri].sent_at = now;
            const uint8_t *data = s_ring[ri].data;
            size_t len = s_ring[ri].nbytes;
            U();
            ok = send_chunk(c, best, data, len);
            L();
            if (!ok && s_ring[ri].st == S_FLIGHT) s_ring[ri].st = S_READY;
        } else {
            s_fl[fi].st = S_FLIGHT; s_fl[fi].sent_at = now;
            size_t len = s_fl[fi].nbytes;
            U();
            const fl_hdr_t *h = (const fl_hdr_t *)s_flbuf;
            ok = fl_read((size_t)fi * FL_SLOT, s_flbuf, FL_HDR + len) == ESP_OK;
            bool good = ok && h->magic == FL_MAGIC && h->seq == best && esp_rom_crc32_le(0, s_flbuf + FL_HDR, len) == h->crc;
            if (!good) {
                ESP_LOGW(TAG, "%s chunk %lu: bad in flash, skipped", s_tab[c].id, (unsigned long)best);
                L(); s_fl[fi].st = S_FREEME; continue;
            }
            ok = send_chunk(c, best, s_flbuf + FL_HDR, len);
            L();
            if (!ok && s_fl[fi].st == S_FLIGHT) s_fl[fi].st = S_READY;
        }
        if (!ok) { U(); return; }
        flying++;
    }
    bool end = s_tab[c].ended && !held(c) && now - u->end_sent > END_RETRY_US;
    if (end) u->end_sent = now;
    U();
    if (end && !send_end(c)) ESP_LOGW(TAG, "capture_end not sent");
}

static void up_task(void *arg)
{
    uint32_t gen = 0;
    bool was_ready = false;
    int64_t offline_since = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        if (s_tab_dirty) { s_tab_dirty = false; tab_save(); }
        if (s_part) flash_reap();
        bool ready = intercom_capture_ready() && now >= s_offline_until;
        uint32_t g = intercom_capture_gen();
        if (ready && (!was_ready || g != gen)) {                 /* a new socket: say capture_start again, resend */
            L();
            for (int c = 0; c < TAB_N; c++) { s_up[c].started = false; s_up[c].start_sent = 0; s_up[c].end_sent = 0; }
            for (int i = 0; i < RING_N; i++) if (s_ring[i].st == S_FLIGHT) s_ring[i].st = S_READY;
            for (int i = 0; i < s_fl_n; i++) if (s_fl[i].st == S_FLIGHT) s_fl[i].st = S_READY;
            U();
            gen = g;
        }
        was_ready = ready;
        s_online = ready;
        offline_since = ready ? 0 : (offline_since ? offline_since : now);
        /* spill: offline for a while, the ring three-quarters full, or the battery is going */
        if (s_part) {
            for (int k = 0; k < 4; k++) {
                L();
                int used = ring_used();
                bool ended_offline = !ready && s_rec_cap < 0 && used;
                U();
                bool want = s_flush_req || ended_offline || (!ready && now - offline_since > SPILL_AFTER_US) || used > RING_N * 3 / 4;
                if (!want || !spill_one()) break;
            }
        }
        if (ready) upload_step(now);
    }
}

/* --------------------------------------------------------------- acks ---- */

void capture_on_ack(const cJSON *j)
{
    if (!s_up_task) return;
    const cJSON *id = cJSON_GetObjectItem(j, "id"), *seq = cJSON_GetObjectItem(j, "seq"), *acked = cJSON_GetObjectItem(j, "acked");
    const cJSON *err = cJSON_GetObjectItem(j, "error");
    if (!cJSON_IsString(id) || !s_mu) return;
    L();
    int c = tab_find(id->valuestring);
    if (c < 0) { U(); return; }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "end"))) {
        if (c == s_rec_cap) { U(); return; }                       /* still recording (a stale id clash): ignore */
        forget(c, "saved");
    } else if (cJSON_IsString(err)) {
        bool drop = cJSON_IsTrue(cJSON_GetObjectItem(j, "drop"));
        ESP_LOGW(TAG, "%s: bus says %s%s", s_tab[c].id, err->valuestring, drop ? " (dropped)" : "");
        if (drop && cJSON_IsNumber(seq)) { free_chunk(c, (uint32_t)seq->valuedouble); s_up[c].acked++; }
        else if (drop && c != s_rec_cap) forget(c, "refused by the bus");
        else if (!strcmp(err->valuestring, "not_started")) { s_up[c].started = false; s_up[c].start_sent = 0; }
    } else if (cJSON_IsNumber(seq)) {
        free_chunk(c, (uint32_t)seq->valuedouble);
        s_up[c].acked++;
    } else if (cJSON_IsNumber(acked)) {                            /* the answer to capture_start */
        uint32_t a = (uint32_t)acked->valuedouble;
        s_up[c].started = true;
        for (int i = 0; i < RING_N; i++)                         /* the bus already holds everything below `a` */
            if (s_ring[i].cap == c && s_ring[i].seq < a && (s_ring[i].st == S_READY || s_ring[i].st == S_FLIGHT)) s_ring[i].st = S_FREE;
        for (int i = 0; i < s_fl_n; i++)
            if (s_fl[i].cap == c && s_fl[i].seq < a && (s_fl[i].st == S_READY || s_fl[i].st == S_FLIGHT)) s_fl[i].st = S_FREEME;
        if (a > s_up[c].acked) s_up[c].acked = a;
    }
    U();
    if (s_up_task) xTaskNotifyGive(s_up_task);
}

/* ------------------------------------------------------------- recorder ---- */

static int tone_fill(int16_t *pcm, int n)
{
    static float ph;
    for (int i = 0; i < n; i++) { pcm[i] = (int16_t)(6000 * sinf(ph)); ph += 2 * 3.14159265f * 440 / RATE; if (ph > 6.2831853f) ph -= 6.2831853f; }
    vTaskDelay(pdMS_TO_TICKS(16));                                   /* about real time */
    return n;
}

static int ring_take(int c, uint32_t seq)
{
    for (int i = 0; i < RING_N; i++) {
        if (s_ring[i].st != S_FREE) continue;
        s_ring[i].st = S_FILL; s_ring[i].cap = (int8_t)c; s_ring[i].seq = seq; s_ring[i].nbytes = 0;
        return i;
    }
    return -1;
}

static void ui_saved(int c, int reason);

static void rec_task(void *arg)
{
    int16_t pcm[BLK];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int c = s_rec_cap;
        for (int i = 0; i < 100 && ptt_recording(); i++) vTaskDelay(pdMS_TO_TICKS(10));   /* the first press opened PTT's mics */
        if (s_pm) esp_pm_lock_acquire(s_pm);
        bool tone = s_tone;
        s_mic_failed = !tone && !audio_in_open();
        uint32_t seq = 0, samples = 0;
        int reason = R_STOPPED, slot = -1, blocks = 0;
        size_t off = 0;
        adpcm_state_t enc;
        adpcm_reset(&enc);
        s_rec_t0 = esp_timer_get_time();
        ESP_LOGI(TAG, "%s: recording%s", s_tab[c].id, tone ? " (test tone)" : s_mic_failed ? ": MICROPHONE FAILED" : "");
        while (!s_mic_failed) {
            if (slot < 0) {
                L(); slot = ring_take(c, seq); U();
                if (slot < 0) { reason = R_FULL; ESP_LOGW(TAG, "storage full: stopping"); break; }
                off = 0; blocks = 0;
            }
            int n = tone ? tone_fill(pcm, BLK) : audio_in_read(pcm, BLK);
            if (n != BLK) { s_mic_failed = true; ESP_LOGE(TAG, "microphone read failed: stopping"); break; }
            off += adpcm_encode_block(&enc, pcm, BLK, s_ring[slot].data + off);
            blocks++;
            samples += BLK;
            s_rec_samples = samples;
            if (blocks == BLKS) {
                L(); s_ring[slot].nbytes = (uint16_t)off; s_ring[slot].st = S_READY; U();
                slot = -1; seq++;
                xTaskNotifyGive(s_up_task);
                if (seq >= MAX_CHUNKS) { reason = R_MAX; break; }
                int b = power_battery_pct();
                if (!power_on_usb() && b >= 0 && b <= 5) { reason = R_BATTERY; break; }
                if (s_flash_full && ring_used() >= RING_N - 1) { reason = R_FULL; break; }
            }
            if (s_stop_req) { reason = s_stop_reason; break; }
            if (s_debug_stop_at && esp_timer_get_time() >= s_debug_stop_at) break;
        }
        L();
        if (slot >= 0) {
            if (blocks) { s_ring[slot].nbytes = (uint16_t)off; s_ring[slot].st = S_READY; seq++; }
            else s_ring[slot].st = S_FREE;
        }
        U();
        if (!tone) audio_in_close();
        if (s_pm) esp_pm_lock_release(s_pm);
        L();
        s_tab[c].ended = 1;
        s_tab[c].chunks = seq;
        s_tab[c].dur_ms = (uint32_t)((uint64_t)samples * 1000 / RATE);
        s_tab[c].reason = (uint8_t)reason;
        s_tab_dirty = true;
        s_rec_cap = -1;
        s_rec = false;
        s_stop_req = false;
        s_debug_stop_at = 0;
        s_tone = false;
        U();
        ESP_LOGI(TAG, "%s: stopped (%s), %lu chunks, %lu ms", s_tab[c].id, REASON[reason], (unsigned long)seq, (unsigned long)s_tab[c].dur_ms);
        snprintf(s_last, sizeof s_last, "%s recorded %lus (%s)", s_tab[c].id, (unsigned long)(s_tab[c].dur_ms / 1000), REASON[reason]);
        ui_saved(c, reason);
        xTaskNotifyGive(s_up_task);
    }
}

/* ------------------------------------------------------------------- UI ---- */

static lv_obj_t *s_root;
static lv_timer_t *s_timer;
static lv_font_t *f_timer96, *f_timer52, *f_rec20;
enum { UI_OFF, UI_REC, UI_SAVED };
static int s_ui = UI_OFF;
static uint32_t s_touch_at, s_saved_at, s_hold_from;
static int s_fill_h, s_saved_cap = -1, s_saved_reason;
static uint32_t s_saved_ms, s_saved_chunks;
static char s_saved_id[ID_LEN];
static bool s_full = true;
static int s_drift_min = -1, s_dx, s_dy;
static float s_prog = -1;
static int s_shown_s = -1;
static char s_shown_timer[12];
static lv_obj_t *s_covered;                                  /* the screen hidden under the capture layer */
static bool s_shown_online = true;
static int s_shown_alerts;

/*
 * Performance (Nov 19): the draw callback runs once for every 20-row strip LVGL renders, and a tiny_ttf text
 * measurement costs ~30 us a character (three cache lookups, PSRAM). Measuring every string on every strip
 * made the callback ~10 ms a strip and a recording frame ~190 ms. So: sizes are measured once and remembered,
 * and nothing is measured or queued for a strip it does not touch.
 */
static inline bool on_clip(lv_layer_t *l, const lv_area_t *a) { return lv_area_is_on(a, &l->_clip_area); }

/* Does the strip touch a ring of outer radius r_out and width w round the centre? LVGL's ring still runs its
   circle masks over a strip that lies wholly inside the hole (a timer digit), so skip those. */
static bool ring_on_clip(lv_layer_t *l, float r_out, float w)
{
    const lv_area_t *c = &l->_clip_area;
    float ri = r_out - w - 1, dmax = 0;
    float xs[2] = { c->x1 - 233.0f, c->x2 - 233.0f }, ys[2] = { c->y1 - 233.0f, c->y2 + 0.0f - 233 };
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) { float d = xs[i] * xs[i] + ys[j] * ys[j]; if (d > dmax) dmax = d; }
    return dmax >= ri * ri;
}

#define TW_N 24
static struct { const lv_font_t *f; int space; char s[40]; int32_t w; } s_tw[TW_N];
static int s_tw_next;
static int32_t text_w(const char *s, const lv_font_t *f, int space)
{
    for (int i = 0; i < TW_N; i++) if (s_tw[i].f == f && s_tw[i].space == space && !strcmp(s_tw[i].s, s)) return s_tw[i].w;
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (strlen(s) < sizeof s_tw[0].s) {
        int i = s_tw_next; s_tw_next = (s_tw_next + 1) % TW_N;
        s_tw[i].f = f; s_tw[i].space = space; strcpy(s_tw[i].s, s); s_tw[i].w = sz.x;
    }
    return sz.x;
}

static void text_at(lv_layer_t *l, const char *s, const lv_font_t *f, int space, uint32_t col, int cx, int y)
{
    int32_t lh = lv_font_get_line_height(f);
    if (y > l->_clip_area.y2 || y + lh < l->_clip_area.y1) return;      /* before measuring */
    int32_t w = text_w(s, f, space);
    lv_area_t a = { cx - w / 2, y, cx - w / 2 + w + 2, y + lh };
    if (!on_clip(l, &a)) return;
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = s; d.font = f; d.color = lv_color_hex(col); d.letter_space = space; d.text_local = 1;
    lv_draw_label(l, &d, &a);
}

/* Tabular figures: each digit in a cell as wide as the widest digit, so the timer never jitters. Cell widths are
   measured once per font. */
typedef struct { const lv_font_t *f; int dw, cw; int16_t gw[11]; } cells_t;
static cells_t s_cells[3];
static const cells_t *cells(const lv_font_t *f)
{
    for (int i = 0; i < 3; i++) if (s_cells[i].f == f) return &s_cells[i];
    static int next;
    cells_t *c = &s_cells[next]; next = (next + 1) % 3;
    c->f = f; c->dw = 0;
    for (int k = 0; k < 10; k++) { c->gw[k] = (int16_t)lv_font_get_glyph_width(f, (uint32_t)('0' + k), 0); if (c->gw[k] > c->dw) c->dw = c->gw[k]; }
    c->gw[10] = (int16_t)lv_font_get_glyph_width(f, ':', 0);
    c->cw = c->gw[10];
    return c;
}

/* Cell i of timer string s (laid out as timer_at does): its box. */
static lv_area_t timer_cell(const char *s, int i, const lv_font_t *f, int cx, int y)
{
    const cells_t *c = cells(f);
    int total = 0;
    for (const char *p = s; *p; p++) total += *p == ':' ? c->cw : c->dw;
    int x = cx - total / 2;
    for (int k = 0; k < i && s[k]; k++) x += s[k] == ':' ? c->cw : c->dw;
    int w = s[i] == ':' ? c->cw : c->dw;
    return (lv_area_t){ x - 4, y, x + w + 6, y + lv_font_get_line_height(f) };
}

static void timer_at(lv_layer_t *l, const char *s, const lv_font_t *f, uint32_t col, int cx, int y)
{
    int32_t lh = lv_font_get_line_height(f);
    if (y > l->_clip_area.y2 || y + lh < l->_clip_area.y1) return;
    const cells_t *c = cells(f);
    int total = 0;
    for (const char *p = s; *p; p++) total += *p == ':' ? c->cw : c->dw;
    int x = cx - total / 2;
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = f; d.color = lv_color_hex(col); d.text_local = 1;
    char one[2] = { 0, 0 };
    for (const char *p = s; *p; p++) {
        int w = *p == ':' ? c->cw : c->dw;
        int gw = *p == ':' ? c->gw[10] : *p >= '0' && *p <= '9' ? c->gw[*p - '0'] : c->dw;
        one[0] = *p;
        d.text = one;
        lv_area_t a = { x + (w - gw) / 2, y, x + (w - gw) / 2 + gw + 2, y + lh };
        if (on_clip(l, &a)) lv_draw_label(l, &d, &a);
        x += w;
    }
}

static void line(lv_layer_t *l, float x1, float y1, float x2, float y2, float w, uint32_t col)
{
    lv_area_t a = { (int)fminf(x1, x2) - (int)w, (int)fminf(y1, y2) - (int)w, (int)fmaxf(x1, x2) + (int)w, (int)fmaxf(y1, y2) + (int)w };
    if (!on_clip(l, &a)) return;
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(col); d.width = w; d.round_start = 1; d.round_end = 1;
    d.p1 = (lv_point_precise_t){ x1, y1 }; d.p2 = (lv_point_precise_t){ x2, y2 };
    lv_draw_line(l, &d);
}

static void diamond(lv_layer_t *l, float cx, float cy, float r, uint32_t col)
{
    lv_area_t a = { (int)(cx - r) - 1, (int)(cy - r) - 1, (int)(cx + r) + 1, (int)(cy + r) + 1 };
    if (!on_clip(l, &a)) return;
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(col); d.opa = LV_OPA_COVER;
    d.p[0] = (lv_point_precise_t){ cx, cy - r }; d.p[1] = (lv_point_precise_t){ cx + r, cy }; d.p[2] = (lv_point_precise_t){ cx, cy + r };
    lv_draw_triangle(l, &d);
    d.p[1] = (lv_point_precise_t){ cx - r, cy };
    lv_draw_triangle(l, &d);
}

static int64_t s_demo_until, s_demo_t0;                      /* /debug/ui capture-ui: the recording look, no recording */
static uint32_t elapsed_s(void) { return s_demo_until ? (uint32_t)((esp_timer_get_time() - s_demo_t0) / 1000000) : s_rec_samples / RATE; }

static void fmt_timer(char *b, size_t n, uint32_t s, bool hours)
{
    if (hours || s >= 3600) snprintf(b, n, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* Upload progress of the saved recording, 0-1 (1 once the bus has it all). */
static float saved_progress(void)
{
    if (s_saved_cap < 0) return 1;
    L();
    float p;
    if (!s_tab[s_saved_cap].used || strcmp(s_tab[s_saved_cap].id, s_saved_id)) p = 1;
    else p = s_saved_chunks ? (float)s_up[s_saved_cap].acked / (float)(s_saved_chunks + 1) : 0;
    U();
    return p > 1 ? 1 : p;
}

static void draw_body(lv_event_t *e);
static void draw(lv_event_t *e)
{
    int64_t t0 = esp_timer_get_time();
    draw_body(e);
    compact_perf_draw_us(PERF_DRAW_CAPTURE, (uint32_t)(esp_timer_get_time() - t0));
}

static void draw_body(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    char b[40];
    if (s_ui == UI_REC && s_full) {
        /* A static rec ring (static, no breathing). A closed ring, not a 0-359.9
           degree arc: a part arc adds LVGL's angle mask to every pixel (same geometry: outer radius = r + w / 2). */
        if (ring_on_clip(l, 232, 16)) dial_draw_ring(l, 233, 233, 232, 16, COL_REC);
        int32_t rw = text_w("REC", f_rec20, 4);
        int gx = 233 - (14 + 10 + rw) / 2;
        dial_draw_disc(l, gx + 7, 98 + lv_font_get_line_height(f_rec20) / 2.0f, 7, COL_REC);
        text_at(l, "REC", f_rec20, 4, COL_REC, gx + 24 + rw / 2, 98);
        uint32_t s = elapsed_s();
        fmt_timer(b, sizeof b, s, false);
        timer_at(l, b, s >= 3600 ? f_num64 : f_timer96, COL_INK, 233, s >= 3600 ? 146 : 132);
        /* No CONVERSATION / "SAVING TO PEGGY AS YOU GO" lines; only a warning shows here. */
        if (!s_online || s_flash_full) {
            const char *st = s_flash_full ? "STORAGE NEARLY FULL" : "SAVING ON THE COMPACT";
            text_at(l, st, f_whisper, 3, COL_AMBER, 233 + 9, 256);
            diamond(l, 233 + 9 - text_w(st, f_whisper, 3) / 2.0f - 12, 256 + lv_font_get_line_height(f_whisper) / 2.0f, 5, COL_AMBER);
        }
        lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
        r.bg_opa = LV_OPA_TRANSP; r.border_color = lv_color_hex(COL_REC); r.border_width = 2; r.border_opa = LV_OPA_COVER; r.radius = 30;
        lv_area_t pa = { 123, 312, 123 + 219, 312 + 59 };
        if (on_clip(l, &pa)) lv_draw_rect(l, &r, &pa);
        text_at(l, "HOLD TO STOP", f_label15, 3, COL_INK, 233, 312 + 30 - lv_font_get_line_height(f_label15) / 2);
        if (s_alerts) { snprintf(b, sizeof b, "%d ALERT%s", s_alerts, s_alerts > 1 ? "S" : ""); text_at(l, b, f_whisper, 3, COL_ROSE, 233, 390); }
        dial_draw_hold_fill(l, s_fill_h, COL_ROSE, 90);
    } else if (s_ui == UI_REC) {                                     /* the hour-in look: a thin static rim at 90% */
        if (ring_on_clip(l, 229, 5)) dial_draw_ring_opa(l, 233, 233, 229, 5, COL_REC, LV_OPA_90);
        int dx = s_dx, dy = s_dy;
        int32_t rw = text_w("REC", f_cap_bot, 3);
        int gx = 233 + dx - (10 + 8 + rw) / 2;
        dial_draw_disc(l, gx + 5, 108 + dy + lv_font_get_line_height(f_cap_bot) / 2.0f, 5, COL_REC);
        text_at(l, "REC", f_cap_bot, 3, COL_REC, gx + 18 + rw / 2, 108 + dy);
        fmt_timer(b, sizeof b, elapsed_s(), true);
        timer_at(l, b, f_timer52, COL_INK, 233 + dx, 140 + dy);
        text_at(l, "TOUCH TO WAKE", f_whisper, 3, COL_INK2, 233 + dx, 230 + dy);
        if (s_alerts) { snprintf(b, sizeof b, "%d ALERT%s", s_alerts, s_alerts > 1 ? "S" : ""); text_at(l, b, f_whisper, 3, COL_ROSE, 233 + dx, 290 + dy); }
    } else if (s_ui == UI_SAVED) {
        float p = s_prog < 0 ? 0 : s_prog;
        if (ring_on_clip(l, 226, 3)) dial_draw_ring_opa(l, 233, 233, 226, 3, COL_HAIR, LV_OPA_30);
        if (p > 0.003f) dial_draw_arc(l, 224, 3, 0, p * 359.9f, COL_INK);
        dial_draw_ring(l, 233, 130, 40, 3.5f, COL_ROSE);
        line(l, 216, 131, 228, 143, 4, COL_ROSE);
        line(l, 228, 143, 251, 118, 4, COL_ROSE);
        bool bad = s_saved_reason == R_FULL || s_saved_reason == R_BATTERY;
        text_at(l, "SAVED", f_head21, 3, COL_INK, 233, 190);
        uint32_t s = s_saved_ms / 1000;
        if (s >= 3600) snprintf(b, sizeof b, "%lu H %lu MIN", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
        else snprintf(b, sizeof b, "%lu MIN %lu S", (unsigned long)(s / 60), (unsigned long)(s % 60));
        text_at(l, b, f_num_m, 0, COL_ROSE, 233, 226);
        const char *line1 = p >= 1 ? "Sent for transcription" : s_online ? "Sending for transcription\xE2\x80\xA6" : "Sends when Wi-Fi is back";
        text_at(l, line1, f_body, 0, COL_INK2, 233, 284);
        if (bad) {
            const char *w = s_saved_reason == R_FULL ? "STORAGE FULL" : "BATTERY LOW";
            text_at(l, w, f_whisper, 3, COL_AMBER, 242, 322);
            diamond(l, 242 - text_w(w, f_whisper, 3) / 2.0f - 12, 322 + lv_font_get_line_height(f_whisper) / 2.0f, 5, COL_AMBER);
        }
        uint32_t left = s_saved_at ? (SAVED_MS - (lv_tick_elaps(s_saved_at) > SAVED_MS ? SAVED_MS : lv_tick_elaps(s_saved_at)) + 999) / 1000 : 5;
        snprintf(b, sizeof b, "BACK TO NOW IN %lu S", (unsigned long)left);
        text_at(l, b, f_whisper, 3, COL_INK2, 233, 358);
    }
}

/* Invalidate the band r_in..r_out only (16 boxes round the circle), so the Saved upload ring does not redraw the middle. */
static void ring_invalidate(int r_in, int r_out)
{
    for (int k = 0; k < 16; k++) {
        float a0 = k * 22.5f * 3.14159265f / 180, a1 = (k + 1) * 22.5f * 3.14159265f / 180;
        float xs[4] = { 233 + r_in * sinf(a0), 233 + r_out * sinf(a0), 233 + r_in * sinf(a1), 233 + r_out * sinf(a1) };
        float ys[4] = { 233 - r_in * cosf(a0), 233 - r_out * cosf(a0), 233 - r_in * cosf(a1), 233 - r_out * cosf(a1) };
        float x1 = xs[0], x2 = xs[0], y1 = ys[0], y2 = ys[0];
        for (int i = 1; i < 4; i++) { x1 = fminf(x1, xs[i]); x2 = fmaxf(x2, xs[i]); y1 = fminf(y1, ys[i]); y2 = fmaxf(y2, ys[i]); }
        /* the arc bulges past its chord: pad by the sagitta of 22.5 degrees (~2% of r) */
        lv_area_t a = { (int)x1 - 6, (int)y1 - 6, (int)x2 + 6, (int)y2 + 6 };
        lv_obj_invalidate_area(s_root, &a);
    }
}

static bool in_pill(int x, int y) { return x >= 123 - 8 && x <= 343 + 8 && y >= 312 - 8 && y <= 372 + 8; }

static void close_lv(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_root) { lv_obj_delete(s_root); s_root = NULL; }
    /* not lv_obj_remove_flag: on a screen it marks the NULL parent's layout dirty (LVGL 9.5, LoadProhibited) */
    if (s_covered) { s_covered->flags &= ~LV_OBJ_FLAG_HIDDEN; lv_obj_invalidate(s_covered); s_covered = NULL; }
    s_ui = UI_OFF;
    s_fill_h = 0;
    s_hold_from = 0;
}

static void on_input(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (code == LV_EVENT_PRESSED) {
        bool was_full = s_full;
        s_touch_at = lv_tick_get();
        if (s_ui == UI_REC && !was_full) { s_full = true; lv_obj_invalidate(s_root); return; }   /* touch to wake */
        if (s_ui == UI_REC && in_pill(p.x, p.y)) s_hold_from = lv_tick_get() ? lv_tick_get() : 1;
        if (s_ui == UI_SAVED) { close_lv(); power_now("capture saved: tap"); }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_hold_from) { s_hold_from = 0; if (s_root) dial_hold_fill_step(s_root, &s_fill_h, 0); }
    }
}

static void tick(lv_timer_t *t)
{
    if (!s_root) return;
    uint32_t now = lv_tick_get();
    if (s_ui == UI_REC) {
        bool full = lv_tick_diff(now, s_touch_at) < IDLE_LOOK_MS;
        if (full != s_full) { s_full = full; s_shown_s = -1; lv_obj_invalidate(s_root); }
        /* the rim is static: nothing to animate; only the timer, the minute shift and the fill move */
        if (!s_full) {
            int m = (int)(time(NULL) / 60);
            if (m != s_drift_min) { s_drift_min = m; s_dx = (m * 7) % 9 - 4; s_dy = (m * 13) % 9 - 4; s_shown_s = -1; lv_obj_invalidate(s_root); }
        }
        int sec = (int)elapsed_s();
        if (sec != s_shown_s || s_online != s_shown_online || s_alerts != s_shown_alerts) {
            /* a new second alone: only the digit cells that changed (usually one, 60 x 120 px) */
            char t[12];
            fmt_timer(t, sizeof t, (uint32_t)sec, !s_full);
            const lv_font_t *f = !s_full ? f_timer52 : sec >= 3600 ? f_num64 : f_timer96;
            int cx = s_full ? 233 : 233 + s_dx, y = !s_full ? 140 + s_dy : sec >= 3600 ? 146 : 132;
            bool cells_only = s_shown_s >= 0 && s_online == s_shown_online && s_alerts == s_shown_alerts &&
                              strlen(t) == strlen(s_shown_timer);
            if (cells_only) {
                for (int i = 0; t[i]; i++) {
                    if (t[i] == s_shown_timer[i]) continue;
                    lv_area_t a = timer_cell(t, i, f, cx, y);
                    lv_obj_invalidate_area(s_root, &a);
                }
            } else {
                lv_area_t a = { 40, 90, 426, 300 };
                lv_obj_invalidate_area(s_root, &a);
                if (s_alerts) { lv_area_t b = { 60, 280, 406, 420 }; lv_obj_invalidate_area(s_root, &b); }
            }
            s_shown_s = sec; s_shown_online = s_online; s_shown_alerts = s_alerts;
            strlcpy(s_shown_timer, t, sizeof s_shown_timer);
        }
        if (s_hold_from) {
            uint32_t ms = lv_tick_elaps(s_hold_from);
            dial_hold_fill_step(s_root, &s_fill_h, ms >= HOLD_MS ? 466 : (int)(ms * 466 / HOLD_MS));
            if (ms >= HOLD_MS) { s_hold_from = 0; ESP_LOGI(TAG, "HOLD TO STOP"); s_stop_reason = R_STOPPED; s_stop_req = true; }
        }
        if (s_demo_until && esp_timer_get_time() >= s_demo_until) { s_demo_until = 0; close_lv(); power_now("capture-ui done"); return; }
    } else if (s_ui == UI_SAVED) {
        float p = saved_progress();
        if (fabsf(p - s_prog) >= 0.01f) { s_prog = p; ring_invalidate(214, 233); }
        static uint32_t shown;
        uint32_t left = lv_tick_elaps(s_saved_at) / 1000;
        if (left != shown) { shown = left; lv_area_t a = { 40, 270, 426, 390 }; lv_obj_invalidate_area(s_root, &a); }
        if (lv_tick_elaps(s_saved_at) >= SAVED_MS) { close_lv(); power_now("capture saved"); }
    }
}

static void open_lv(int ui)
{
    if (!s_root) {
        s_root = lv_obj_create(lv_layer_sys());                       /* above alerts, overlays and push-to-talk */
        lv_obj_remove_style_all(s_root);
        lv_obj_set_size(s_root, 466, 466);
        lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);   /* a true-black screen (spec 5.2, 5.3) */
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_root, draw, LV_EVENT_DRAW_MAIN_END, NULL);
        lv_obj_add_event_cb(s_root, on_input, LV_EVENT_ALL, NULL);
        s_timer = lv_timer_create(tick, 50, NULL);
        /* The capture layer is opaque and covers the glass, but LVGL still renders the active screen under it in
           every strip (the face cost ~2 ms a strip, and its own timers kept invalidating). Hide the screen while
           Capture is up; close_lv shows it again. There is only one screen (faces are its children). */
        lv_obj_t *scr = lv_scr_act();
        if (scr && !lv_obj_has_flag(scr, LV_OBJ_FLAG_HIDDEN)) { lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN); s_covered = scr; }
    }
    s_ui = ui;
    s_full = true;
    s_touch_at = lv_tick_get();
    s_prog = -1; s_shown_s = -1; s_shown_timer[0] = 0; s_drift_min = -1;
    s_fill_h = 0; s_hold_from = 0;
    lv_obj_invalidate(s_root);
    lv_display_trigger_activity(NULL);
}

static void ui_saved(int c, int reason)
{
    bsp_display_lock(-1);
    s_saved_cap = c;
    s_saved_reason = reason;
    s_saved_ms = s_tab[c].dur_ms;
    s_saved_chunks = s_tab[c].chunks;
    strlcpy(s_saved_id, s_tab[c].id, sizeof s_saved_id);
    s_saved_at = lv_tick_get() ? lv_tick_get() : 1;
    open_lv(UI_SAVED);
    bsp_display_unlock();
    power_wake("capture saved");
}

/* ---------------------------------------------------------------- public ---- */

bool capture_recording(void) { return s_rec; }
bool capture_busy(void) { return s_rec || s_ui != UI_OFF; }

static void notice(const char *line1, const char *line2);

static void start(bool tone, int seconds)
{
    if (s_rec || !s_ring[0].data) return;
    int b = power_battery_pct();
    if (!tone && !power_on_usb() && b >= 0 && b < 10) { ESP_LOGW(TAG, "battery %d%%: not recording", b); notice("BATTERY LOW", "Charge to record"); return; }
    char id[ID_LEN];
    time_t now = time(NULL);
    snprintf(id, sizeof id, "%s%08lx-%04x", tone ? "test-" : "cap-", (unsigned long)now, (unsigned)(esp_random() & 0xFFFF));
    L();
    int c = tab_add(id, (uint32_t)(now > 1700000000 ? now : 0));
    if (c >= 0) { s_rec_cap = c; s_rec = true; s_stop_req = false; s_rec_samples = 0; s_alerts = 0; s_tone = tone; }
    U();
    if (c < 0) { notice("UPLOADS WAITING", "Connect to Wi-Fi first"); return; }
    s_debug_stop_at = seconds > 0 ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
    xTaskNotifyGive(s_up_task);                                   /* saves the table */
    xTaskNotifyGive(s_rec_task);
    bsp_display_lock(-1);
    open_lv(UI_REC);
    bsp_display_unlock();
}

void capture_toggle(void)
{
    if (!s_up_task) return;
    if (s_rec) { ESP_LOGI(TAG, "BOOT double-press: stop"); s_stop_reason = R_STOPPED; s_stop_req = true; return; }
    if (s_ui == UI_SAVED) { bsp_display_lock(-1); close_lv(); bsp_display_unlock(); }
    start(false, 0);
}

void capture_touch_lv(void)
{
    s_touch_at = lv_tick_get();
    if (s_root && !s_full) { s_full = true; lv_obj_invalidate(s_root); }
}

void capture_dismiss(void)
{
    bsp_display_lock(-1);
    if (s_ui == UI_SAVED) close_lv();
    bsp_display_unlock();
}

void capture_touch(void)
{
    bsp_display_lock(-1);
    s_touch_at = lv_tick_get();
    if (s_root && !s_full) { s_full = true; lv_obj_invalidate(s_root); }
    lv_display_trigger_activity(NULL);
    bsp_display_unlock();
}

void capture_note_alert(void)
{
    if (!s_rec) return;
    s_alerts++;
    ESP_LOGI(TAG, "urgent Moment while recording: waits (%d)", s_alerts);
}

bool capture_debug_start(int seconds, bool tone)
{
    if (!s_up_task) return false;
    if (s_rec || seconds < 1 || seconds > 600) return false;
    start(tone, seconds);
    return s_rec;
}

bool capture_debug_ui(int seconds)
{
    if (s_rec || s_ui != UI_OFF || seconds < 1 || seconds > 600) return false;
    bsp_display_lock(-1);
    s_demo_t0 = esp_timer_get_time();
    s_demo_until = s_demo_t0 + (int64_t)seconds * 1000000;
    open_lv(UI_REC);
    bsp_display_unlock();
    return true;
}

void capture_debug_offline(int seconds)
{
    s_offline_until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGW(TAG, "test: uploads paused for %d s", seconds);
}

void capture_flush_now(void)
{
    if (!s_up_task) return;
    if (s_rec) { s_stop_reason = R_BATTERY; s_stop_req = true; }
    for (int i = 0; i < 200 && s_rec; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_flush_req = true;
    xTaskNotifyGive(s_up_task);
    for (int i = 0; i < 150; i++) {                                   /* up to ~15 s: the ring to flash */
        L(); int used = ring_used(); U();
        if (!used || !s_part || s_flash_full) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

size_t capture_info_json(char *out, size_t n)
{
    if (!s_mu) return (size_t)snprintf(out, n, "null");
    L();
    int ring = ring_used(), fl = 0, pend = 0;
    for (int i = 0; i < s_fl_n; i++) fl += s_fl[i].st != S_FREE && s_fl[i].st != S_FREEME;
    for (int i = 0; i < TAB_N; i++) pend += s_tab[i].used;
    int c = s_rec_cap >= 0 ? s_rec_cap : tab_current();
    char cur[160] = "null";
    if (c >= 0) snprintf(cur, sizeof cur, "{\"id\":\"%s\",\"ended\":%s,\"chunks\":%lu,\"acked\":%lu,\"started\":%s}",
                         s_tab[c].id, s_tab[c].ended ? "true" : "false", (unsigned long)(s_tab[c].ended ? s_tab[c].chunks : s_rec_samples / CHUNK_SAMPLES),
                         (unsigned long)s_up[c].acked, s_up[c].started ? "true" : "false");
    size_t r = (size_t)snprintf(out, n,
        "{\"state\":\"%s\",\"elapsed_s\":%lu,\"alerts\":%d,\"online\":%s,\"queued_ram\":%d,\"queued_flash\":%d,\"flash_slots\":%d,"
        "\"flash_full\":%s,\"pending\":%d,\"current\":%s,\"last\":\"%s\"}",
        s_rec ? "recording" : s_ui == UI_SAVED ? "saved" : pend ? "uploading" : "idle", (unsigned long)(s_rec ? elapsed_s() : 0),
        s_alerts, s_online ? "true" : "false", ring, fl, s_fl_n, s_flash_full ? "true" : "false", pend, cur, s_last);
    U();
    return r;
}

/* A short notice on the capture layer (refusals): two lines for 2.5 s. */
static lv_obj_t *s_note;
static void note_done(lv_timer_t *t) { if (s_note) { lv_obj_delete(s_note); s_note = NULL; } }
static void notice(const char *line1, const char *line2)
{
    bsp_display_lock(-1);
    if (s_note) lv_obj_delete(s_note);
    s_note = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_note);
    lv_obj_set_size(s_note, 466, 466);
    lv_obj_set_style_bg_color(s_note, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_note, LV_OPA_COVER, 0);
    lv_obj_t *a = lv_label_create(s_note);
    lv_label_set_text(a, line1);
    lv_obj_set_style_text_font(a, f_label, 0);
    lv_obj_set_style_text_letter_space(a, 3, 0);
    lv_obj_set_style_text_color(a, lv_color_hex(COL_AMBER), 0);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_t *b = lv_label_create(s_note);
    lv_label_set_text(b, line2);
    lv_obj_set_style_text_font(b, f_headline, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_INK), 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 222);
    lv_timer_t *t = lv_timer_create(note_done, 2500, NULL);
    lv_timer_set_repeat_count(t, 1);
    lv_display_trigger_activity(NULL);
    bsp_display_unlock();
}

void capture_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    uint8_t *mem = heap_caps_malloc((size_t)RING_N * CHUNK_MAX, MALLOC_CAP_SPIRAM);
    s_json = heap_caps_malloc(JSON_MAX, MALLOC_CAP_SPIRAM);
    s_flbuf = heap_caps_malloc(FL_HDR + CHUNK_MAX, MALLOC_CAP_SPIRAM);
    if (!mem || !s_json || !s_flbuf) { ESP_LOGE(TAG, "no PSRAM for Capture: disabled"); return; }
    for (int i = 0; i < RING_N; i++) s_ring[i] = (rslot_t){ .cap = -1, .st = S_FREE, .data = mem + (size_t)i * CHUNK_MAX };
    tab_load();
    flash_scan();
    for (int i = 0; i < TAB_N; i++) {
        if (!s_tab[i].used) continue;
        if (!s_tab[i].ended) {                                        /* restarted mid-recording: what reached flash or the bus */
            s_tab[i].ended = 1;
            s_tab[i].reason = R_INTERRUPTED;
            s_tab_dirty = true;
        }
        ESP_LOGI(TAG, "%s waits to upload (%lu chunks, %s)", s_tab[i].id, (unsigned long)s_tab[i].chunks, REASON[s_tab[i].reason % 5]);
    }
#if CONFIG_PM_ENABLE
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "capture", &s_pm);
#endif
    extern const uint8_t lexend_200_ttf_start[] asm("_binary_lexend_200_ttf_start");
    extern const uint8_t lexend_200_ttf_end[] asm("_binary_lexend_200_ttf_end");
    extern const uint8_t lexend_700_ttf_start[] asm("_binary_lexend_700_ttf_start");
    extern const uint8_t lexend_700_ttf_end[] asm("_binary_lexend_700_ttf_end");
    bsp_display_lock(-1);
    f_timer96 = lv_tiny_ttf_create_data_ex(lexend_200_ttf_start, lexend_200_ttf_end - lexend_200_ttf_start, 96, LV_FONT_KERNING_NONE, 12);
    f_timer52 = lv_tiny_ttf_create_data_ex(lexend_200_ttf_start, lexend_200_ttf_end - lexend_200_ttf_start, 52, LV_FONT_KERNING_NONE, 12);
    f_rec20 = lv_tiny_ttf_create_data_ex(lexend_700_ttf_start, lexend_700_ttf_end - lexend_700_ttf_start, 20, LV_FONT_KERNING_NORMAL, 8);
    bsp_display_unlock();
    if (!f_timer96) f_timer96 = f_num_xl;
    if (!f_timer52) f_timer52 = f_num_m;
    if (!f_rec20) f_rec20 = f_label;
    /* Both stacks in PSRAM (psram_task.h): the first mic open runs the codec init and its logs on the recorder; the
       uploader erases, writes and reads the spill partition, which never disables the cache on this XIP build. */
    task_create_psram(rec_task, "cap_rec", 6144, NULL, 7, &s_rec_task);
    task_create_psram(up_task, "cap_up", 6144, NULL, 4, &s_up_task);
}
