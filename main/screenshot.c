#include "screenshot.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "capture.h"
#include "power.h"
#include "ptt.h"
#include "display.h"
#include "panel_img.h"

#define W 466
#define H 466
#define LOCK_MS 2000          /* bounded waits for the display lock: never an endless wait */

static const char *TAG = "shot";
static uint8_t *s_img;                        /* W x H, 2 bytes per pixel, panel order: the bytes as sent */
static uint16_t s_rowpx[H];                   /* pixels LVGL sent per row during a grab */
static volatile bool s_grabbing, s_sending;
static volatile bool s_verify_want, s_verify_got;
static volatile int32_t s_vdiff = -1;
static volatile uint32_t s_vpx;
static int32_t s_step_diff = -1;
static uint32_t s_step_px;
static int s_count;
static int64_t s_taken_us;

void screenshot_record(int xs, int ys, int xe, int ye, const void *px, bool lvgl)
{
    if (!s_grabbing || !s_img) return;
    pimg_write(s_img, W, H, xs, ys, xe, ye, px, lvgl ? s_rowpx : NULL);   /* only LVGL's bands count towards "every row sent" */
}

bool screenshot_verify_wanted(void) { return s_grabbing && s_verify_want && !s_verify_got; }

uint32_t screenshot_diff(int xs, int ys, int xe, int ye, const void *px)
{
    return s_img ? pimg_diff(s_img, W, H, xs, ys, xe, ye, px) : 0;
}

void screenshot_verify_done(int32_t diff, uint32_t px)
{
    s_vdiff = diff;
    s_vpx = px;
    s_verify_got = true;
}

static bool all_rows(void)
{
    for (int y = 0; y < H; y++) if (s_rowpx[y] < W) return false;
    return true;
}

/* A thin white ring for 150 ms; added after the grab, so it is not in the picture. Drawn as a full-circle arc, not as
   an object with LV_RADIUS_CIRCLE: a 466 px rounded border makes LVGL build radius-233 and radius-228 anti-alias masks
   (circ_calc_aa4) on top of the crescent's radius 260 and the rim dots, which churns the 4-entry circle cache and
   allocates and frees large PSRAM blocks from both swdraw threads. An arc has no radius mask. */
static void flash_ring(void)
{
    if (bsp_display_lock(LOCK_MS) != ESP_OK) return;
    lv_obj_t *r = lv_arc_create(lv_layer_sys());
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, W, H);
    lv_obj_set_pos(r, 0, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_bg_angles(r, 0, 360);
    lv_obj_set_style_arc_width(r, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(r, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(r, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(r, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(r, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_opa(r, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_delete_delayed(r, 150);
    bsp_display_unlock();
}

/* Bounded throughout: each wait is a fixed number of 20 ms sleeps, the display lock is taken with a 2 s limit and never
   held while waiting, and it refuses from the LVGL task (which would wait on itself). */
bool screenshot_take(void)
{
    if (s_grabbing || s_sending) return false;
    if (compact_display_in_lvgl_task()) { ESP_LOGW(TAG, "refused from the LVGL task"); return false; }
    if (power_state() == PWR_SLEEP || capture_recording() || ptt_recording()) {
        ESP_LOGI(TAG, "refused (asleep, Capture or push-to-talk)");
        return false;
    }
    if (!s_img) s_img = heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM);
    if (!s_img) { ESP_LOGE(TAG, "no PSRAM for the frame"); return false; }
    if (bsp_display_lock(LOCK_MS) != ESP_OK) { ESP_LOGW(TAG, "display busy"); return false; }
    memset(s_rowpx, 0, sizeof s_rowpx);                      /* under the lock: no write is in progress */
    s_verify_want = false; s_verify_got = false; s_vdiff = -1; s_vpx = 0;
    s_grabbing = true;
    lv_obj_invalidate(lv_screen_active());                   /* redraw every strip, all layers */
    lv_obj_invalidate(lv_layer_top());                       /* a screen that hides the active one (Pomodoro) lives here */
    bsp_display_unlock();
    bool ok = false;
    for (int i = 0; i < 100 && !(ok = all_rows()); i++) vTaskDelay(pdMS_TO_TICKS(20));
    int32_t vd = -1;
    uint32_t vpx = 0;
    if (ok && compact_ring_active()) {                       /* the ring's step path, checked against the flushes */
        s_verify_want = true;
        for (int i = 0; i < 50 && !s_verify_got; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_verify_got) { vd = s_vdiff; vpx = s_vpx; }
    }
    if (bsp_display_lock(LOCK_MS) == ESP_OK) { s_grabbing = false; s_verify_want = false; bsp_display_unlock(); }
    else { s_grabbing = false; s_verify_want = false; }
    if (!ok) { ESP_LOGW(TAG, "frame incomplete: the image is partly new"); return false; }
    s_step_diff = vd; s_step_px = vpx;
    s_count++;
    s_taken_us = esp_timer_get_time();
    ESP_LOGI(TAG, "screenshot %d taken (ring step check: %ld of %lu px differ)", s_count, (long)vd, (unsigned long)vpx);
    flash_ring();
    return true;
}

size_t screenshot_info_json(char *out, size_t n)
{
    return (size_t)snprintf(out, n, "{\"count\":%d,\"age_s\":%lld,\"step_diff\":%ld,\"step_px\":%lu}", s_count,
                            s_count ? (long long)((esp_timer_get_time() - s_taken_us) / 1000000) : -1LL,
                            (long)s_step_diff, (unsigned long)s_step_px);
}

/* ---- PNG: 8-bit RGB, one stored deflate block per row, streamed ---- */

static uint32_t be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; return v; }

static esp_err_t chunk(httpd_req_t *req, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8], c[4];
    be32(h, len);
    memcpy(h + 4, type, 4);
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)type, 4);
    if (len) crc = esp_rom_crc32_le(crc, data, len);
    be32(c, crc);
    if (httpd_resp_send_chunk(req, (const char *)h, 8) != ESP_OK) return ESP_FAIL;
    if (len && httpd_resp_send_chunk(req, (const char *)data, len) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, (const char *)c, 4);
}

esp_err_t screenshot_send_png(httpd_req_t *req)
{
    if (!s_count || !s_img || s_grabbing) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no screenshot");
    s_sending = true;
    enum { ROW = 1 + W * 3, BLK = 5 + ROW };
    uint8_t *buf = heap_caps_malloc(BLK, MALLOC_CAP_SPIRAM);
    if (!buf) { s_sending = false; return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t ihdr[13];
    be32(ihdr, W); be32(ihdr + 4, H);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    esp_err_t r = httpd_resp_send_chunk(req, (const char *)sig, 8);
    if (r == ESP_OK) r = chunk(req, "IHDR", ihdr, 13);
    /* IDAT: zlib header, H stored blocks (BFINAL on the last), adler32. Length is known up front, so the CRC runs along. */
    uint32_t idat = 2 + (uint32_t)H * BLK + 4;
    uint8_t h[8];
    be32(h, idat); memcpy(h + 4, "IDAT", 4);
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)"IDAT", 4);
    uint8_t z[2] = { 0x78, 0x01 };
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)h, 8);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)z, 2);
    crc = esp_rom_crc32_le(crc, z, 2);
    uint32_t a = 1, b = 0;
    for (int y = 0; y < H && r == ESP_OK; y++) {
        uint8_t *p = buf;
        *p++ = y == H - 1;                                /* BFINAL, BTYPE 00 */
        *p++ = ROW & 0xFF; *p++ = ROW >> 8; *p++ = ~ROW & 0xFF; *p++ = (~ROW >> 8) & 0xFF;
        uint8_t *row = p;
        *p++ = 0;                                         /* PNG filter: none */
        pimg_row_rgb(s_img + (size_t)y * W * 2, W, p);       /* decoded as the panel reads the bytes */
        p += W * 3;
        for (int i = 0; i < ROW; i++) { a = (a + row[i]) % 65521; b = (b + a) % 65521; }
        crc = esp_rom_crc32_le(crc, buf, BLK);
        r = httpd_resp_send_chunk(req, (const char *)buf, BLK);
    }
    uint8_t ad[4], c[4];
    be32(ad, (b << 16) | a);
    crc = esp_rom_crc32_le(crc, ad, 4);
    be32(c, crc);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)ad, 4);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)c, 4);
    if (r == ESP_OK) r = chunk(req, "IEND", NULL, 0);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    s_sending = false;
    return r;
}
