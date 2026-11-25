# Display and interface

Detailed notes moved from the README: source layout, the 12-hour rim, the breathing ring, FREE Moments, Day list scrolling, the gesture router (Earlier and Settings entry) and the pill list. Back to the [reference index](REFERENCE.md).

## Contents

- [Layout](#layout)
- [The 12-hour rim](#the-12-hour-rim)
- [Breathing ring](#breathing-ring)
- [FREE Moments](#free-moments)
- [Day list scrolling (`main/dayview.c`)](#day-list-scrolling-maindayviewc)
- [Earlier and Settings entry (`main/earlier.c`, the gesture router)](#earlier-and-settings-entry-mainearlierc-the-gesture-router)
- [Pill list (`main/pill_list.h`)](#pill-list-mainpill_listh)

## Layout

- `main/display.c`: panel, touch and LVGL bring-up with internal-RAM draw buffers (the BSP's
  PSRAM buffers force a 46 KB internal DMA bounce buffer per flush).
- `main/fonts.c`: Lexend Exa / Hanken Grotesk subsets (`fonts/`, OFL) via tiny_ttf.
- `main/dial.c`: the Day Dial face (Now), including the agent count ring at 9 o'clock.
- `main/pill_list.c/.h`: the reusable scrolling pill list (spec 5.8 rows). Agents and Settings use it; `pill_fades_draw()` is the
  shared 40 px fade, also used by the Day list.
  See "Pill list" below.
- `main/agents.c`: the Agents face (spec 7.4), from the bus `sessions` resource: nothing running, a list of
  up to 3 jobs, or the top job's question with its buttons. Faces run Day · Now · Agents · Thread; the
  gesture router is in `main/earlier.c`. A row opens that job's conversation in `main/thread.c`.
- `main/intercom_ws.c`: Wi-Fi, mDNS, the bus socket and frame reassembly. Keeps the `events`, `notices` and
  `sessions` resources and fetches conversation pages (`intercom_fetch_conversation_id`, two at a time).
- `main/ota.c`: update server and rollback guard. `main/settings.c`: NVS settings.
- `main/capture.c`: Capture (see [FEATURES.md](FEATURES.md#capture-recording)). `main/panicdiag.c`: panic post-mortem (see [DEBUGGING.md](DEBUGGING.md#crash-post-mortem-without-a-cable)).
- `main/pomodoro.c` + `main/pomo_logic.c`: Pomodoro mode (see [FEATURES.md](FEATURES.md#pomodoro); the cycle logic is pure and host tested by `test/host/pomo_logic_test.c`).

Other source files in `main/` (one line each, from the headers and code):

- `main.c`: boot and task wiring.
- `power.c/.h`: power states (awake, dim, rest, sleep), battery and charging, quiet hours, the PWR and BOOT buttons and the screenshot chord.
- `decision.c/.h`: alert Moments that take over the face (decisions, urgent, confirm and Reply views, SENT and TOO LATE) and the cue query for the breathing ring.
- `dayview.c/.h`: the Day face (scrolling list) and Event detail cards.
- `earlier.c/.h`: the Earlier tray and the gesture router.
- `setscreen.c/.h`: the Settings screen (brightness and volume sliders plus the pill-list rows).
- `thread.c/.h`: the Thread face (a conversation) and the "Understood" view.
- `overlay.c/.h`: screens that cover the face (power menu, notices, Charging and similar), on LVGL's top layer.
- `now_layout.c`: the Now face as a const slot table of components and gauges; edit it to change what the face shows. `dial_priv.h` and `dial.h` hold the shared dial definitions.
- `commands.c/.h`: the device command registry (see [FEATURES.md](FEATURES.md#commands-maincommandsc)); `cmd_logic.c/.h` is its pure part.
- `ptt.c/.h`: push-to-talk. `audio_in.c/.h`: the two microphones (16 kHz, mono). `audio_out.c/.h`: spoken replies and the chime.
- `ble_link.c/.h`: the Bluetooth phone link (Nordic UART Service). `adpcm.c/.h`: IMA ADPCM codec for audio over that link and for Capture.
- `cue.c/.h`: the breathing ring's colour, opacity and shape. `ring_px.c/.h`: the ring's pixel maths. `panel_img.c/.h`: the model of panel memory used by screenshots. `screenshot.c/.h`: the screenshot grab and PNG.
- `rim12.c/.h`: the 12-hour rim maths. `mom_logic.c/.h`: what FREE Moments change. `gap_text.c/.h`: Day list gap wording. `reply_logic.c/.h`: notification replies (pure). `touch_logic.c/.h`: tap, hold and swipe classification. `pomo_logic.c/.h`: Pomodoro cycle logic. All of these are host tested.
- `displaywd.c/.h` and `wd_logic.h`: the display watchdog. `restart.c/.h`: the quiesce-then-restart helper.
- `prefs.c/.h`: device preferences in NVS (brightness, volume, sleep, rim gap and others). `logbuf.c/.h`: recent log output in PSRAM for `GET /log`. `memdiag.c/.h`: `GET /debug/heap` and RAM test hooks.
- `lv_mem_psram.c`: routes the LVGL heap to PSRAM. `psram_task.h`: task stacks in PSRAM. `linker.lf`: app .bss in PSRAM. `fonts.h`, `display.h`: font and panel APIs.

## The 12-hour rim

The rim is a fixed clock face (spec 4.2; proposal `2026-11-22-twelve-hour-dial-breathing-ring-notification-replies.md`
section 1): 12 at the top, one hour is 30 degrees, theta = (local wall-clock hours mod 12) / 12 x 360, and the planet
(the now dot) circles it like the tip of a clock hand. It shows the Moments from now to the leading edge, 12 h minus the
gap ahead on the wall clock; the gap behind the planet stays empty (its rim dots at 35%). The gap is the Settings row
RIM GAP (0:30, 1:00, 2:00, 3:00; default 1:00; NVS `prefs`/`gap_min`) or the bus widget `dial_config`
(`{"rim_gap_min": 30|60|120|180}`, applied when its `updated_at` is newer than the last one applied; the last writer
wins). Past Moments vanish at once; a Moment fades in over the first 15 min after the edge reaches it (a point grows
r 2 to 4); a span still cut by the edge ends in a 50% tip of 7.5 degrees; spans sit on lane 1 (r 204, rose) or lane 2
(r 196, amber) by start order over the Moments ending after now - 12 h and starting before now + 24 h; a third overlap
is not drawn. A 2 px tick at the top marks local midnight when it is inside the window. Hour dots r 2.0, quarter dots
r 2.8. On a daylight-saving day the window is measured on the wall clock, so the gap stays its size and the planet
jumps 30 degrees at the change. The Now numeral, countdown, NEXT label (with TOMORROW past midnight), running Moment and
the bottom title follow the window; N LEFT TODAY and the Day list stay today-based; Event detail draws the selected
Moment at its own place, unwindowed. Decision expiry arcs run from the planet to the expiry, clamped to the edge.

Pure maths in `main/rim12.c`, host tested (angles, gap values, reveal, past vanish, lanes, midnight, both DST changes,
a fuzz of the drawing invariants):
`cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain test/host/rim12_test.c main/rim12.c -lm -o /tmp/rim12_test && /tmp/rim12_test`.
`dial.c` caches one `rim_t` per minute (and per Moments or gap change) instead of looping the Moments on every strip.

Debug (`POST /debug/ui`, Bearer): `clock HH:MM [+days]` sets the drawing clock (faces, crescent and night ground only;
SNTP, power, quiet hours and the bus keep real time), `clock off`, `clock` (status); `testday rel` (Moments around the
drawing clock) and `testday abs` (fixed times on its date: 7:30, 9:00-10:00 with an overlap pair and a third overlap,
noon, 17:00-19:00, 17:30, 18:30, 22:00-23:00, tomorrow 0:30 and 7:00-8:00) replace the live Moments on Now and Day only;
`testday free` (busy and FREE Moments mixed, see FREE Moments below), `testday detail` (nine Moments for the Event detail title band, with long titles, place, note and a cleared caption; ids t-d01..t-d09, with `clock 09:00`) and `detail [id]` open a card; `testday off` returns to them (the bus link reconnects for a fresh reset); `gap <min>` does what the Settings row does;
`widgets {json}` runs the bus widgets path by hand (it also replaces the leave_by and weather gauges until the next
bus update).

## Breathing ring

A thin ring on the rim breathes while something waits that does not own the glass. Colour is the weight of the top open item
(`decision.c` `decision_cue_query`): plum notice (6 s breath, 6 px band), amber decision (4 s, 8 px), rose urgent
(2.4 s, 8 px), each with a soft 5 px inner band at a third of the opacity, left out 22 degrees either side of the
planet. Two or more at the top weight breathe twice (heartbeat). 20 levels, spaced evenly in perceived brightness
(square root), sampled every 40 ms; a tick that changes nothing sends nothing. Hidden for Capture, push-to-talk,
Settings, the power menu, Charging, the flashlight and sleep; quiet hours: notice off, decision at 60% of its range.
Notices are only a ring cue now: swipe up on Now opens the pending notice's card (with none pending it opens Earlier); swipe down dismisses it.

How it is drawn: `cue.c` decides colour, opacity and shape; `display.c` draws it and is the only code that sends pixels.
LVGL never renders the ring. Its flushes reach the panel through the adapter's `custom_draw_bitmap` hook, which (while
a ring is active) copies the face under the ring (the push set: 7 strips, 12 rects, 58,480 px) into a PSRAM snapshot and
blends the ring in, in panel byte order, then calls `panel_write`. A breath step composes snapshot + ring into LVGL's
two idle draw buffers and sends the 12 rects through the same `panel_write` (`esp_lcd_panel_draw_bitmap`, so the
CO5300's 6-column gap is applied by the driver), then a NOP drains the SPI queue. Pixel maths: `main/ring_px.c`, host
tested with `test/host/ring_px_test.c` (ASan/UBSan; simulated panel with the gap, both byte orders, the screenshot model):
`cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain test/host/ring_px_test.c main/ring_px.c main/panel_img.c -lm -o /tmp/ring_px_test && /tmp/ring_px_test`.
A step over 50 ms three times running, or a failed write, sends later steps through LVGL invalidations for the rest of
the boot (`/info` `cue.comp.direct_off`).

Debug (`POST /debug/ui`, Bearer): `cue plum|amber|rose [phase 0-100] [count] [quiet]` pins a ring (phase 0 is the bottom
of the breath, 50 the top), `cue off` releases it; `cue tune <mode> <levels> <tick_ms> <inner> <nodraw> <rect_cost>`
(mode 1: every step through an LVGL redraw); `cue bench` (cost of a step's parts in us, `/info` `cue.comp.bench`:
drain, compose all, zero-fill all, a 2 x 2 write, copy all); `notice-test`, `notice-open`. `/info` `cue` shows level,
count, opacity and the compositor's counters (`comp`: steps, steps_lvgl, verifies, step_us_max, hook_us_max, last_us
compose/io/drain). `GET /debug/perf` adds `ring_hook_ms` and `ring_step_ms`.

## FREE Moments

An `events` item may carry `"free": true` (boolean; absent means false, so old data behaves exactly as before). A free Moment
is something happening that is not busy time: a focus block, office hours, a "tentative" hold. One parser,
`dial_moment_is_free()` (`dial.c`), is used by both `dial.c` (`set_moments_locked`) and `dayview.c` (`parse_one`); the logic
that follows is the pure `main/mom_logic.c` plus the `free` flag in `rim12.c`, host tested by `test/host/mom_logic_test.c`.
- **Busy state (Now).** A free Moment never makes the device read busy: no BUSY UNTIL, and it is not the "running" Moment;
  `mom_scan` keeps a busy `running` apart from a `free_running`. It does not start or end a free stretch (`gap_from` counts busy
  ends only) and the upper arc counts to the next busy Moment (`busy_next`), not to a free one.
- **No countdown.** The numeral pair shows the countdown only when the window's next Moment is busy; for a free next Moment the
  heading reads `NEXT 7:28 · FREE` in ink-2 (rose stays the busy countdown's colour), the clock is centred alone, and the bottom
  title names it. The numeral tap does nothing then. A free Moment running with nothing next shows `ON NOW · UNTIL 9:00 · FREE`
  and its title. The Event detail caption has no countdown either: `FREE · 7:28-8:28` (a point: `FREE · 9:30`; once ended:
  `FREE · ENDED 8:28 AM`).
- **Rim.** Still drawn, quieter: a free span is a 2 px rose arc at 65% on its own radius (`DIAL_FREE_R` 208, between lane 1 and
  the rim dots), a free point is a hollow ring (r 4, 2 px) instead of a disc; same window, reveal, tip and past rules. Free
  Moments take no lane, so one never pushes a busy Moment to the amber lane and is never "hidden". Event detail draws a
  selected free span at r 208 (3 px) and a free point as an outline diamond.
- **Day list.** Same time and title row, in ink-2, with a small `FREE` whisper under the title (on the overlap note's line).
  Rose (the next Moment) goes to the next busy Moment, never to a free one. A free row does not interrupt a free gap: the
  `N MIN FREE` gap row is measured from the latest busy end and shown before the next busy row.
- **Overlaps.** A free Moment overlapping anything is not an overlap: no `OVERLAPS` note on the list or the card, no amber.
- Detail card: the time range is ink-2 instead of rose.
- Not changed: the 12-hour rim, the leave-by gauge, gestures and tap rules, the Rest face line (it names the next Moment
  whatever it is).
Test: `POST /debug/ui` `testday free` (twelve Moments at fixed times on the drawing clock's date, ids t-f01..t-f12: free
7:28-8:28 overlapping busy 8:00-9:00, a free point 9:30, free 11:30-12:30, free 2:30-3:30 over busy 3:00-4:00, a free point
5:00, a free 8:00-9:00 PM); with `clock 07:00`, `07:45`, `08:15`, `12:00`, `20:30` and `detail t-f01`; restore with `testday off`,
`clock off`, `now`.

## Day list scrolling (`main/dayview.c`)

The Day list is a vertical scrolling list of the same rows (time and title, not pills), "OVERLAPS n MIN" notes and "N MIN FREE"
gap rows; the old "+N LATER TODAY" line is gone. Viewport x 83..383, y 110..380 (an LVGL scroll object, drag only,
no bounce, `DAYL_*` constants); the fades are `pill_fades_draw()` from the root's `LV_EVENT_DRAW_POST_END`, only on a side with
more beyond. Gap rows draw their own dotted rules so they scroll and clip with the list. It opens at the top (next or running
Moment; ended Moments are not listed, as before). The position (`s_day_scroll`) survives rebuilds and a detail visit; the
per-minute and new-data rebuilds wait while a finger is down, within 400 ms of one, or while the list is still coasting.
Gestures: a vertical drag on Day is only list scrolling (a list that fits simply does not move); Day never opens Earlier.
Row taps (the 10 px dead zone, `TOUCH_DEADZONE_PX`): a tap must not move; travel of 10 px or more on either axis makes the
press a scroll or swipe for good (LVGL's scroll limit and `compact_press_was_drag()` use the same constant), so it cannot
open a row, even if the finger comes back or lifts over a row. The list has momentum (LVGL's default throw, no bounce): a
press that lands while it is still coasting only stops it (`display.c` latches `s_press_stop` at the press edge from
`lv_indev_get_scroll_obj`; `compact_press_was_drag()` is then true, so no row, Settings or Agents row opens). That latch is
global, so it also covers the Settings and Agents lists. Gap rows and OVERLAPS notes are not clickable: a press on them
scrolls the list. Gap wording is the pure `gap_text.c` ("2 H FREE", "1 H 15 MIN FREE", `test/host/gap_text_test.c`).
Test: `POST /debug/ui` `clock 09:00`, `testday abs`, `day`, then `day-scroll <dy>` (animated, clamped); restore with
`testday off`, `clock off`.

## Earlier and Settings entry (`main/earlier.c`, the gesture router)

Swipe up: on Now, the pending notice's card if there is one, else the Earlier tray; Settings closes; Day, Agents and Thread
ignore it (Day scrolls, Agents scrolls). On a detail card or takeover it does nothing. Earlier opens ONLY from Now (spec 8.1).
Swipe down (it used to be swipe up): dismisses a detail or takeover card: Event detail (back to the Day list or Now),
the notice / Inform card, decision, urgent, confirm and Reply views (set aside open or cleared as before), SENT and TOO LATE,
and the Charging screen. Otherwise Now opens Settings; Earlier closes (also PWR or 60 s idle); elsewhere nothing. The card
takes the swipe first: `route_vertical` tries the detail dismiss before Settings, and a takeover is handled in
`gesture_cb` before the router, so a swipe down on a card never opens Settings behind it. The one scrollable card content is
the confirm screen's text box: a press that starts on it scrolls it, and the swipe dismisses only when it sits at its start
and the press did not scroll it (`decision_swipe_down_lv`); the swipe itself already needs SWIPE_MIN_PX of travel, far past
the 10 px dead zone, and a press that left the dead zone never taps or holds a button. There is no on-screen hint text for
dismissal.

Earlier rows: an open row (2 px `peri` edge) reopens its Moment as before (straight to the Reply view for a question).
An ended row (cleared, expired, answered, withdrawn, done, skipped, too late: anything `is_open_row` rejects) is tappable too and
opens a read-only detail inside the tray (`build_detail`), laid out like the notice card: the row's glyph, the title (17 caps, 2
lines), "STATE · 6:42 PM" in rose, the body (20 px, 3 lines, 2 when a resolution follows) only if the item had one, then the reply
resolution (14 px, 2 lines) only if it has one; a title-only item shows no empty body area. The detail has its own HOLD TO CLEAR pill
(same 0.8 s hold and rose wipe as the list's, so the convention is unchanged) that removes just that entry; swipe down in the detail
returns to the list (`route_vertical`), swipe down on the list closes the tray. A tap uses the 10 px dead zone
(`compact_press_was_drag`); a held clear also cancels once the press leaves the dead zone. The list's HOLD TO CLEAR still clears all
ended rows. Rows keep the item's `body` (or `detail.description`, else `detail.location`) and the notification's `resolution`:
`entry_t` grew by `body[160]` and `resolution[96]` (about 6 KB across the 24 entries, in PSRAM through `main/linker.lf`; no internal
RAM). The text arrives with `earlier_note_text` right after `earlier_note` (set aside from a card, the `events` tombstones, the v1
`notification` event; an open row's text carries over to how it ended). A row cleared singly is remembered by id and time (`s_gone`,
RAM only), so a repeated bus tombstone stays gone until a restart; the list-wide clear persists through `prefs`.
Debug (`POST /debug/ui`, each counts as a touch so the device does not rest or close the tray under the test): `earlier-test` seeds three
ended rows (`etest-*`: EXPIRED with a body, ANSWERED with a body and a resolution, CLEARED with no body; it also resets the in-RAM clear
marks) and `earlier-test off` removes them; `earlier-row <n>` does what a tap on ended row n does (0 = newest, tray must be open);
`earlier-clear` does what the held clear on the open detail does. A rest device must be woken after the hook (`POST /power` `wake`), since
the power task rests it again at once if the display has been idle.
`POST /debug/ui` `detail` opens Event detail for the first listed Moment (no touch); `swipe up|down` runs the router's vertical branch without a touch (it does not test the finger path).

## Pill list (`main/pill_list.h`)

A vertical list of spec 5.8 pills (286 x 66 capsule, `raise` fill, 1.5 px `line` edge) that scrolls by vertical drag with
40 px fades (spec 7.5). API, all in LVGL context (display lock held):

- `pill_list_create(parent, &cfg)`: `cfg` has the viewport `x, y, w, h`, `row_w`, `row_h`, `gap`, `icon` size, `width`
  policy, `icon_fn` and `tap` callbacks and `ud`. The list is freed with its object (`lv_obj_clean(parent)`).
- `pill_list_set(p, i, &row)`: add row `i` (== count) or update it in place. `pill_row_t`: `title`, `sub` (detail line,
  optional), `value` (right-aligned, optional, for Settings-style rows), `title_col` / `sub_col` (0 = ink / ink-2),
  `style` (passed to `icon_fn`, e.g. a job state), `tappable`, `animated` (icon redrawn by `pill_list_tick`).
- `pill_list_trim`, `_count`, `_scroll_y`, `_scroll_to` (keep position across a rebuild), `_scroll_by`, `_overflows`.
- Width policy: `PILL_W_FIXED` (every row `row_w`, the default) or `PILL_W_CHORD` (`row_w` capped by the circle's
  chord at the row's resting position; not recomputed while scrolling).
- The owner's screen calls `pill_list_draw_post(p, layer)` from its `LV_EVENT_DRAW_POST_END` handler (draws icons via
  `icon_fn`, clipped to the viewport, then the fades; fades show only on a side that has more rows) and
  `pill_list_tick(p)` from a timer when rows are `animated`.
- Gestures: the list scrolls vertically only and only when rows overflow; the list and rows bubble gestures, so
  horizontal face swipes still reach `earlier.c`'s router. Agents takes no vertical swipes (Earlier and Settings are Now-only), so a vertical drag there is a scroll; Settings closes on a swipe up, see below. A tap is ignored when the press
  was a drag (`compact_press_was_drag`).
- Agents geometry: viewport x 83..383, y 132..360, rows 270 wide (all the same), starting below the curved top caption
  (which sweeps down to about y 125 at its ends). Three rows fit without scrolling. The old "+N WORKING" line is gone: the
  rest are reached by scrolling. Test: `POST /debug/ui` `agents-test <n>` (n fake jobs, 0 = back to the bus),
  `agents-scroll <dy>` (clamped), after `agents`.
- Getters: `pill_list_row_obj(p, i)` (the row's LVGL object, for raw press/release handlers) and
  `pill_list_contains(p, obj)` (obj is the list, a row or a row's child). `pill_list_set` clears CLICKABLE on an update
  of a non-tappable row, so re-add it after each update of a row that has its own handler.
- Settings (`setscreen.c`): six rows, all 270 x 52 at a 60 px pitch (gap 8) in a viewport x 98..368, y 92..364 (inside
  r 190, clear of the slider touch bands, the sun and speaker marks and the hint line at y 404). Title is the label,
  value right-aligned. SLEEP AFTER, QUIET HOURS and RIM GAP are list taps; FLASHLIGHT, WI-FI and LINK keep their 0.8 s
  hold-to-confirm through `pill_list_row_obj` handlers (a drag that scrolls sends PRESS_LOST and cancels the hold). Four
  rows show; the rest are reached by dragging up. Values refresh in place (QUIET HOURS every tick, WI-FI every 500 ms).
  Closing: the router's swipe up calls `setscreen_swipe_up_lv()`, which ignores a swipe that starts on the list while it
  can scroll (it is a scroll), so Settings closes by a swipe up that starts above or below the list (y < 92 or > 364),
  by the 60 s idle timeout, or by PWR. Swipe down inside Settings does nothing. Test: `POST /debug/ui` `settings`, then
  `settings-scroll <dy>`.
