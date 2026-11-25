#pragma once
#include <stddef.h>
#include <stdbool.h>

/*
 * Display watchdog. A small task (not the LVGL task) samples the LVGL heartbeat once a second (see wd_logic.h for the
 * rules and thresholds). If LVGL has not completed a timer pass for 5 s while the display is meant to be running
 * (not paused for sleep, no OTA upload, not in the first 20 s after boot; 15 s while Capture records), it records what
 * it knows in RTC memory (survives the restart, like panicdiag) and restarts the device. GET /info shows it as
 * `last_hang` and `display_wd`, and `reset` reads "display-hang" after such a restart.
 *
 * Off switches: POST /debug/ui "wd off" (kept in NVS until "wd on") and an automatic restart-loop guard (three hang
 * restarts in a row, each within 5 minutes of boot, leave the watchdog off for the next boot).
 */
void displaywd_init(void);                  /* early in app_main: validate the RTC record (after panicdiag_init) */
void displaywd_start(void);                 /* after the display is up and NVS is ready: start the task */
bool displaywd_reset_was_hang(void);        /* this boot followed a watchdog restart */
size_t displaywd_last_json(char *out, size_t n);   /* the last_hang object, or "null" */
size_t displaywd_state_json(char *out, size_t n);  /* the display_wd object */
const char *displaywd_command(const char *arg);    /* /debug/ui "wd on|off|status": a one-line answer */
