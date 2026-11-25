#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The owner's device preferences, set on the Settings screen and kept in NVS (namespace "prefs"). */
void prefs_load(void);                     /* once, after nvs_flash_init() */
int prefs_brightness(void);                /* 10-100 %, full-face brightness; dim and rest scale from it */
void prefs_set_brightness(int pct);
int prefs_sleep_min(void);                 /* minutes untouched before sleep on battery; 0 = never */
void prefs_set_sleep_min(int min);
int prefs_volume(void);                    /* 0-100 % speaker volume for spoken replies; 0 = muted */
void prefs_set_volume(int pct);
/* RIM GAP: minutes left empty behind the planet on the 12-hour rim (30, 60, 120 or 180; default 60). Settings sets it
   (saved when Settings closes); the bus widget `dial_config` sets it too (saved at once), whichever wrote last wins. */
int prefs_gap_min(void);
void prefs_set_gap_min(int min);
uint32_t prefs_gap_bus_at(void);           /* updated_at (epoch s) of the last dial_config applied, 0 = none */
void prefs_set_gap_from_bus(int min, uint32_t updated_at);   /* applied and saved to NVS at once */
void prefs_save(void);                     /* write changes (called when Settings closes) */
uint32_t prefs_earlier_cleared(void);          /* Earlier HOLD TO CLEAR time (epoch s), 0 = never */
void prefs_set_earlier_cleared(uint32_t t);     /* saved to NVS at once */
/* Notification replies (E105): the Reply view and push-to-talk binding. Off by default (NVS prefs/notif_reply = 0);
   with it off nothing of the feature runs. Set by POST /debug/ui "reply on|off"; saved to NVS at once. */
bool prefs_notif_reply(void);
void prefs_set_notif_reply(bool on);
