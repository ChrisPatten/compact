#include "prefs.h"
#include <stdbool.h>
#include "esp_log.h"
#include "nvs.h"

static int s_bright = 100, s_sleep = 10, s_volume = 60;
static uint32_t s_earlier_clr;
static int s_gap = 60;                 /* RIM GAP, minutes */
static uint32_t s_gap_bus_at;
static bool s_notif_reply;            /* E105 Reply view, default off */

static bool gap_ok(int m) { return m == 30 || m == 60 || m == 120 || m == 180; }
static bool s_dirty;

void prefs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("prefs", NVS_READONLY, &h) != ESP_OK) return;     /* nothing saved yet: defaults */
    uint8_t b; uint16_t m;
    if (nvs_get_u8(h, "bright", &b) == ESP_OK && b >= 10 && b <= 100) s_bright = b;
    if (nvs_get_u16(h, "sleep_min", &m) == ESP_OK && m <= 240) s_sleep = m;
    uint8_t v;
    if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100) s_volume = v;
    uint32_t c;
    if (nvs_get_u32(h, "earlier_clr", &c) == ESP_OK) s_earlier_clr = c;
    if (nvs_get_u16(h, "gap_min", &m) == ESP_OK && gap_ok(m)) s_gap = m;
    if (nvs_get_u32(h, "gap_bus_at", &c) == ESP_OK) s_gap_bus_at = c;
    if (nvs_get_u8(h, "notif_reply", &v) == ESP_OK) s_notif_reply = v == 1;
    nvs_close(h);
    ESP_LOGI("prefs", "brightness %d%%, sleep after %d min, volume %d%%, rim gap %d min, notification replies %s", s_bright, s_sleep, s_volume, s_gap,
             s_notif_reply ? "on" : "off");
}

int prefs_brightness(void) { return s_bright; }
int prefs_sleep_min(void) { return s_sleep; }

void prefs_set_brightness(int pct)
{
    pct = pct < 10 ? 10 : pct > 100 ? 100 : pct;
    if (pct != s_bright) { s_bright = pct; s_dirty = true; }
}

int prefs_volume(void) { return s_volume; }

void prefs_set_volume(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (pct != s_volume) { s_volume = pct; s_dirty = true; }
}

void prefs_set_sleep_min(int min)
{
    if (min != s_sleep) { s_sleep = min; s_dirty = true; }
}

void prefs_save(void)
{
    if (!s_dirty) return;
    nvs_handle_t h;
    if (nvs_open("prefs", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bright", (uint8_t)s_bright);
    nvs_set_u16(h, "sleep_min", (uint16_t)s_sleep);
    nvs_set_u8(h, "volume", (uint8_t)s_volume);
    nvs_set_u16(h, "gap_min", (uint16_t)s_gap);
    nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
    ESP_LOGI("prefs", "saved: brightness %d%%, sleep after %d min, volume %d%%, rim gap %d min", s_bright, s_sleep, s_volume, s_gap);
}

int prefs_gap_min(void) { return s_gap; }

void prefs_set_gap_min(int min)
{
    if (gap_ok(min) && min != s_gap) { s_gap = min; s_dirty = true; }
}

uint32_t prefs_gap_bus_at(void) { return s_gap_bus_at; }

void prefs_set_gap_from_bus(int min, uint32_t updated_at)
{
    if (!gap_ok(min)) return;
    s_gap = min;
    s_gap_bus_at = updated_at;
    nvs_handle_t h;
    if (nvs_open("prefs", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "gap_min", (uint16_t)min);
    nvs_set_u32(h, "gap_bus_at", updated_at);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI("prefs", "rim gap %d min from the bus", min);
}

uint32_t prefs_earlier_cleared(void) { return s_earlier_clr; }

/* Written at once: the bus re-sends ended Moments for 24 h on every reconnect, so a clear that lived
   only in RAM brought them all back after each restart. */
void prefs_set_earlier_cleared(uint32_t t)
{
    s_earlier_clr = t;
    nvs_handle_t h;
    if (nvs_open("prefs", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "earlier_clr", t);
    nvs_commit(h);
    nvs_close(h);
}

bool prefs_notif_reply(void) { return s_notif_reply; }

void prefs_set_notif_reply(bool on)
{
    s_notif_reply = on;
    nvs_handle_t h;
    if (nvs_open("prefs", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "notif_reply", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI("prefs", "notification replies %s", on ? "on" : "off");
}
