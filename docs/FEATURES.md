# Features

Detailed notes moved from the README: Pomodoro, Capture (recording), Screenshot and the device command registry. Back to the [reference index](REFERENCE.md).

## Contents

- [Pomodoro](#pomodoro)
- [Capture (recording)](#capture-recording)
- [Screenshot](#screenshot)
- [Commands (`main/commands.c`)](#commands-maincommandsc)

## Pomodoro

A full-screen focus timer (requested by the owner). The whole dial is one thick ring (outer r 232, 40 px): a focus round starts full at
12 and drains clockwise; a break fills back up from 12. Big `MM:SS` numerals (tabular, `f_num_l`), a caption (FOCUS, BREAK,
LONG BREAK, PAUSED), four dots under the numerals for the rounds done (filled = done, hollow = the focus round in progress), and the
sweeping second-hand moon orbiting a dot on the ring's head. Focus is warm peach (`0xF6B08C`), breaks cool violet (`0xB5A0F5`).
The last minute (phases of 2 min or more) brightens the ring and flashes the numerals in the phase colour for the first 0.3 s of
every second. Paused: ring at about 35%, numerals grey, amber PAUSED, "TAP TO RESUME / HOLD TO CANCEL". The ring and its head move
in whole degrees (LVGL arc angles are integers; `LV_USE_FLOAT` is off), about every 4 s on a 25 min round.

Cycle: focus, break, focus, break, focus, break, focus, long break, then it finishes and the clock face returns. Defaults 25 / 5 / 15
minutes, set at start (0.1 to 99 minutes each; fractions work: 0.5 = 30 s). A skipped focus round counts as done. Phase ends
carry their overshoot, so nothing drifts. The timer lives on the device (`esp_timer` 200 ms tick, monotonic clock): it needs
no Wi-Fi and keeps running while the panel sleeps.

On the glass: tap = pause or resume, hold 0.8 s = cancel (a rose fill rises from the bottom from 0.3 s), any swipe = skip to
the next phase, PWR click = cancel (a dimmed screen wakes first). While it runs the device never rests or sleeps by itself; on
battery it dims at 30 s (a tap wakes it without pausing). A remote sleep (`POST /power sleep`) darkens the panel and the timer
carries on; outside quiet hours a phase end wakes it. The screen is on the LVGL top layer (under the power menu, Charging and
push-to-talk); a decision, Thread or Capture covers it until they close.

Phase end: a ring pulse (1.5 s) always, a soft two-note chime (`audio_out_chime`) unless quiet hours are on or the volume is 0.
Skipping pulses but does not chime. Finishing chimes once.

Remote control, Bearer token like the other endpoints:

```bash
T=$(grep '^INTERCOM_COMPACT_TOKEN=' $AGENTBUS_ENV | cut -d= -f2)
curl -4 -s -H "Authorization: Bearer $T" -X POST -d "start" http://compact.local/pomodoro            # 25 / 5 / 15
curl -4 -s -H "Authorization: Bearer $T" -X POST -d "start 50 10 20" http://compact.local/pomodoro   # focus, break, long break (minutes)
curl -4 -s -H "Authorization: Bearer $T" -X POST -d "pause" http://compact.local/pomodoro            # resume | toggle | skip | stop | status
curl -4 -s -H "Authorization: Bearer $T" http://compact.local/pomodoro                               # GET: the state
```

The body may also be JSON: `{"action":"start","focus":50,"break":10,"long":20}`. A restart replaces a running timer. Answer: 200 with
`{"state":"running|paused|idle","phase":"focus|break|long","remaining_s":..,"elapsed_s":..,"phase_s":..,"rounds_done":..,"focus_min":..,
"break_min":..,"long_min":..,"showing":bool}`, 400 with `{"error":..}` for a bad command, bad minutes, or pause/resume/skip with
nothing running. `/info` carries the same object as `pomodoro`. `POST /debug/ui chime` plays the chime (to hear it). Screenshots work
(`screenshot_take` also invalidates the top layer, because the active screen is hidden while the timer is up).

## Capture (recording)

BOOT double-press records (spec 7.6). 16 kHz mono, IMA ADPCM (`main/adpcm.c`), 2 s chunks numbered from 0 in a
PSRAM ring of 48. An uploader sends `capture_start`, then `capture_chunk` (base64 in JSON), then `capture_end` over
the Wi-Fi socket, up to 4 chunks in flight, and deletes each chunk when the bus acks it (agentbus
`docs/SURFACE_API.md`, "Capture: long recordings"). With no bus for 8 s (or the ring three-quarters full) chunks
spill to the raw `storage` partition: 358 slots of 20 KB (one chunk each, header with id, seq and crc). A table
of up to 4 recordings in NVS (`capture`/`tab`) plus a scan of the slots at boot resumes uploads after a restart; a
recording cut off by a restart ends as `interrupted`. Uploads use Wi-Fi only; on the phone link chunks wait in flash.
Stops: HOLD TO STOP 0.8 s, BOOT double-press, 2 h, battery 5% (on battery), storage full. Refused under 10%.

`/info` `capture`: `state` (idle, recording, saved, uploading), `elapsed_s`, `queued_ram`, `queued_flash`,
`pending`, `current` (id, chunks, acked), `last`. Tests (Bearer, `POST /debug/ui`): `capture-tone <s>` records a
440 Hz tone (id `test-…`; the agent only acknowledges it), `capture <s>` records the microphones, `capture-offline
<s>` pauses uploads to exercise the spill, `capture-ui <s>` shows the recording screen without recording anything
(no message to the bus; use it for look and frame-time tests). While spilling, flash erases stall the UI for up to
~0.5 s.

## Screenshot

BOOT and PWR pressed together (each within 150 ms of the other; awake only) takes a screenshot and neither button's own
action runs. `power.c` `chord_check` polls the AXP2101 key IRQ every 10 ms for 150 ms after a BOOT press (the power task
polls every 200 ms), and the power task swallows the chord's PWR click and hold. Refused during Capture and push-to-talk.
The picture is built from what was actually sent to the panel (`main/screenshot.c`, `main/panel_img.c`): `display.c`'s
single pixel writer (`panel_write`) records every write, LVGL bands and ring breath steps alike, into one 466 x 466 PSRAM
image (434 KB, allocated on first use; no internal RAM) in panel byte order at the coordinates handed to the driver. A
grab invalidates the whole screen so every row is sent at least once; while a ring breathes it then asks the compositor
for one direct step at the blend already on the glass and compares it byte for byte with what the flushes sent
(`/info` `screenshot.step_diff` must be 0; `step_px` is how many pixels were compared; -1 means no ring or no direct
step). The PNG decodes each pixel as the panel does (first byte high). Only the last shot is kept; nothing uploads it.
Feedback is a 150 ms white ring on the sys layer, added after the grab. `GET /screenshot` (Bearer) streams a PNG (stored
deflate, no compression, ~650 KB); `POST /debug/ui screenshot` takes one without the buttons. All waits are bounded (2 s
for the frame, 1 s for the step check, 2 s for the display lock) and a grab refuses from the LVGL task.

## Commands (`main/commands.c`)

One registry of named device ops, reachable over any transport. A new feature is one `command_register(op, usage, handler)` line in `commands_init()`; the bus treats `op` and `args` as opaque.

- **Bus frame** (Wi-Fi socket or BLE phone link, same code): `{"type":"command","id":"<uuid>","op":"pomodoro","args":"start 25 5 15","ttl_s":120}`.
  `args` is a string or any JSON value (non-strings are handed to the handler as compact JSON text, max 199 chars). The answer goes back on the link it
  came in on: `{"type":"command_result","id":..,"ok":true,"data":{..}}` or `{"ok":false,"reason":"unknown_op|bad_args|expired|busy|failed","data":"<hint>"}`.
- **Never on the receive path:** `handle_frame` only copies the frame into an 8-slot queue; the `cmd` task (PSRAM stack) runs the handler, one at a
  time. A full queue answers `busy` at once. A command that waited longer than `ttl_s` (default 120, max 3600) answers `expired` and does not run.
  The last 16 ids are remembered with their result frame: a repeat id re-sends that frame and never runs again.
- **Hello:** every hello (each pairing, both links) carries `"ops":[{"op":..,"usage":..},..]`.
- **HTTP debug path:** `POST /cmd` (Bearer), body `<op> <args>` or `{"op":..,"args":..}`; answers `{"ok":true,"data":..}` (200) or
  `{"ok":false,"reason":..,"data":"hint"}` (400). Same registry, serialised with the task. `POST /pomodoro` and `POST /power` are thin wrappers on the
  `pomodoro` and `power` ops (same answers as before).
- **Ops:** `pomodoro` (`start [focus [short [long]]] | pause | resume | skip | stop | status`), `power` (`sleep | wake`), `face`
  (`thread | now | agents | day | settings`), `chime`, `info` (version, partition, power, battery, usb, charge, link, pomodoro, last_action),
  `help` (`[op]`). Not registered: `screenshot` (the PNG only leaves by `GET /screenshot`; there is no upload path to the bus), and anything destructive
  (restart, Wi-Fi, bus URL, link mode, OTA): never add those.
- Handlers must not block long, touch the panel (only through the `display.h` API) or restart directly (`compact_restart`). Pure parts (JSON escape,
  dedupe, TTL, result frame) are `main/cmd_logic.c`, host tested: `cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain test/host/cmd_logic_test.c main/cmd_logic.c -o /tmp/cmd_logic_test && /tmp/cmd_logic_test`.
- RAM: queue about 1 KB and the task control block internal; stack, two 1.5 KB buffers and the dedupe frames in PSRAM.
