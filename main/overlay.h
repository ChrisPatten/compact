#pragma once
#include <stdbool.h>

/*
 * Screens that cover the face (spec 8.1, buttons set Nov 8 2026). They sit on LVGL's top
 * layer, so the face underneath keeps its state. Every function takes the display lock itself.
 */
void overlay_power_menu(void);        /* PWR held 3 s: RESTART, HOLD TO POWER OFF, CANCEL */
void overlay_capture_soon(void);      /* BOOT double-press: placeholder notice until Capture exists */
void overlay_close_all(void);         /* back to Now */
bool overlay_open(void);
void overlay_battery_changed(void);   /* any task: USB plugged or unplugged; the power menu refreshes now */
void overlay_flashlight_lv(void);     /* LVGL context: all white, full brightness, until PWR or BOOT */
bool overlay_flashlight_on(void);

/* Charging screen (Nov 11): battery ring, time, bolt, "FULL IN h:mm". Takes the display lock itself.
   CHG_PLUG: just plugged in, normal brightness, closes itself after 10 s (back to Now).
   CHG_TAP: the bolt on Now was tapped; closes after 60 s untouched like a side face.
   CHG_SLEEP: asleep on USB at <= 95%: black ground, small drift each minute, no auto close.
   PWR and swipe down close it (back to Now); the power module closes it with overlay_close_all(). */
typedef enum { CHG_PLUG, CHG_TAP, CHG_SLEEP } chg_mode_t;
void overlay_charging(chg_mode_t mode);
bool overlay_charging_on(void);
