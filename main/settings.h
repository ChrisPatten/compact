#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char ssid[33];
    char pass[65];
    char token[96];
    char bus_host[64];
    char bus_ip[16];
    uint16_t bus_port;
    char ssid2[33];        /* optional secondary network (e.g. a phone hotspot) */
    char pass2[65];
    char remote_url[128];  /* optional wss://host/api/v1/intercom/ws; empty = LAN bus only */
    bool ble_link;         /* PHONE mode this boot (the phone link only, no Wi-Fi); else AUTO. settings_link_ble_next_boot() */
} settings_t;

/* Load settings from NVS. Non-empty compiled-in (Kconfig) values win and are saved to NVS,
   so later builds can ship with the secrets left out. nvs_flash_init() must have run. */
void settings_load(settings_t *s);

/* Save (or, when val is NULL or empty, erase) one string key in the "intercom" NVS namespace. */
bool settings_save_str(const char *key, const char *val);

/* Ask for PHONE mode (the phone link only, no Wi-Fi) on the next boot only. It is one-shot: settings_load clears
   it, so any later restart (PWR, a crash, the watchdog) comes back in AUTO. */
bool settings_link_ble_next_boot(void);
