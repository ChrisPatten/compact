#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * Pomodoro mode: a full-screen analog countdown on the Compact. The whole dial is one thick ring: a
 * focus round starts full at 12 and drains clockwise; a break fills back up. Big MM:SS numerals, a sweeping second-hand
 * moon on the ring's head, four dots for the rounds done. Focus is warm peach, breaks are cool violet. The last minute
 * brightens the ring and pulses the numerals once a second; a phase end gives a soft chime (never in quiet hours or at
 * volume 0) and a ring pulse.
 *
 * Cycle (pomo_logic.h): 25 min focus, 5 min break, three times, then a fourth focus and a 15 min long break, then it
 * finishes. Durations are set at start. The timer runs on the device from the monotonic clock, so it survives a lost
 * Wi-Fi link, and keeps counting if the panel sleeps (a remote sleep): outside quiet hours a phase end wakes it.
 *
 * On the glass: tap = pause or resume, hold 0.8 s = cancel, any swipe = skip to the next phase; PWR click = cancel.
 * While it runs the device never rests or sleeps by itself (it dims at 30 s on battery). The screen lives on the top
 * layer under the power menu and push-to-talk; a decision or Thread covers it until they close. Remote control:
 * POST /pomodoro (Bearer), see pomodoro_command(); state in /info "pomodoro".
 */
void pomodoro_init(void);                       /* once, after fonts_init() */
bool pomodoro_active(void);                     /* a timer is running or paused (the glass is kept awake) */
bool pomodoro_showing(void);                    /* active and its screen is the one in front */
bool pomodoro_start(double focus_min, double break_min, double long_min);   /* any task; restarts a running one; <= 0 = default */
void pomodoro_stop(void);                       /* any task: cancel, back to the clock face */
bool pomodoro_pause(int mode);                  /* any task: 0 resume, 1 pause, 2 toggle; false if nothing runs */
bool pomodoro_skip(void);                       /* any task: next phase now */
void pomodoro_wake_lv(void);                    /* LVGL context, display lock held: the panel woke, repaint */
size_t pomodoro_info_json(char *out, size_t n);
/* POST /pomodoro body: "start [focus_min [break_min [long_min]]]" | "stop" | "pause" | "resume" | "toggle" | "skip" |
   "status", or JSON {"action":"start","focus":50,"break":10,"long":20}. Minutes may be fractional (0.5 = 30 s, for
   tests). Writes the resulting state as JSON to out; false for a bad command (out holds the reason). */
bool pomodoro_command(const char *body, char *out, size_t n);
