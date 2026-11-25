#pragma once
#include <stdbool.h>
#include "cJSON.h"

/*
 * Power states and rules:
 *   AWAKE -> DIM (40% of the setting) after 30 s untouched, on battery or USB.
 *   On battery: REST face (50%) after 60 s; SLEEP after the SLEEP AFTER setting (0 = never).
 *   On USB: no rest and no automatic sleep; sleep only by PWR on full Now.
 *   SLEEP = panel off, LVGL paused, CPU light sleep. Exception: asleep on USB at 0-95% shows the
 *   Charging screen at 20% of the setting (panel on, LVGL running, no light sleep, Wi-Fi kept);
 *   the state is still SLEEP. Unplugging or passing 95% there turns the panel off.
 *   Quiet hours (22:00-06:30, or flipped in Settings) only change notification delivery:
 *   a decision does not wake the device, an urgent Moment always does, notices never do.
 *   Plugging in wakes the device and shows the Charging screen for 10 s (a decision is left alone).
 *   Buttons:
 *     PWR click: asleep -> Now; dimmed or resting -> full Now; full Now with nothing covering it -> sleep;
 *       anything else -> Now. PWR hold 3 s: power menu. PWR hold 8 s: AXP2101 forced power-off.
 *     BOOT click: Thread on the followed conversation (wakes from sleep straight into it; no-op if open).
 *       BOOT double-click: Capture. BOOT hold: push-to-talk, asleep or awake. BOOT never sleeps the device.
 *     BOOT + PWR together (each pressed within 150 ms of the other, awake only): screenshot (screenshot.h); neither button's own
 *       action runs. Not during Capture or push-to-talk.
 *   Touch wakes DIM/REST to the full face and the tap does nothing else; touch never wakes SLEEP.
 *   An urgent Moment wakes from anything.
 *   < 10% on battery: Wi-Fi off while asleep. < 5% on battery: "CHARGE ME", then power off.
 */
typedef enum { PWR_AWAKE, PWR_DIM, PWR_REST, PWR_SLEEP } power_state_t;

void power_start(void);                      /* after display + LVGL are up */
void power_on_moments(const cJSON *events);  /* wakes on a new urgent (or, outside quiet hours, decision) Moment */
void power_on_notices(const cJSON *notices); /* the same for E89 notices */
void power_wake(const char *why);
void power_now(const char *why);             /* back to Now, waking if needed */
void power_off_now(void);                    /* AXP2101 software power-off */
void power_sleep(const char *why);
power_state_t power_state(void);
int power_battery_pct(void);                 /* -1 when no battery */
bool power_on_usb(void);
typedef enum { PWR_CHG_BATTERY, PWR_CHG_CHARGING, PWR_CHG_FULL, PWR_CHG_USB } power_charge_t;   /* USB = plugged, not charging */
power_charge_t power_charge_state(void);
void power_apply_brightness(void);           /* Settings changed the brightness: apply it now */
void power_battery_refresh(void);            /* read the AXP2101 now (the power task reads every 30 s) */
void power_wake_timing(int *render_ms, int *total_ms);   /* last wake from SLEEP; -1 before the first */

/* Nov 11 batch (shared interfaces). */
bool power_quiet(void);                      /* quiet hours on: the 22:00-06:30 schedule, or a manual flip from Settings */
void power_quiet_toggle(void);               /* Settings: flip it; holds until the next 22:00 / 06:30 change */
int power_full_eta_min(void);                /* minutes until 100% while charging; -1 when unknown or not charging */
