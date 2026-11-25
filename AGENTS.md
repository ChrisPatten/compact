# AGENTS.md: the Compact firmware

Firmware for the Compact, a round desk device: Waveshare ESP32-S3-Touch-AMOLED-1.75C (466 x 466 AMOLED, capacitive touch, AXP2101 power chip, 500 mAh battery, 2.4 GHz Wi-Fi only). ESP-IDF v5.5.5, LVGL 9.5, C. It connects over Wi-Fi to AgentBus as surface `compact` (Intercom WebSocket protocol v1) and draws only what the bus sends it. `README.md` is the short front door; `docs/REFERENCE.md` indexes the detailed notes (build, settings, Wi-Fi roaming and the rest); this file is the working agreement for contributors and coding agents.

## Related projects

- The design spec (Plum Dusk) and the Intercom protocol contract are maintained alongside the AgentBus project and are not part of this repository. The spec beats any board or screenshot; where it is silent, match the existing faces.
- The rim is a fixed 12-hour clock face (12 at the top) showing the next 12 hours minus a gap behind the planet.

## Layout

- `main/main.c`: boot and task wiring.
- `main/display.c`: panel, touch, LVGL bring-up. Draw buffers live in internal RAM on purpose (BSP PSRAM buffers force a 46 KB DMA bounce buffer per flush). `lv_mem_psram.c` routes LVGL heap to PSRAM.
- `main/fonts.c` + `fonts/`: Lexend Exa / Hanken Grotesk subsets (OFL) through tiny_ttf.
- Faces: `dial.c` (Now, the Day Dial: rim, Moments, drawing primitives; the 12-hour rim maths is the pure `rim12.c`, host tested by `test/host/rim12_test.c`; what FREE Moments change on Now and Day is the pure `mom_logic.c`, `test/host/mom_logic_test.c`) with `now_layout.c` (the Now face as a const slot table of components and gauges; edit it to change what the face shows) and `dial_priv.h`, `dayview.c` (Day), `agents.c` (Agents), `thread.c` (Thread/conversation), `earlier.c` (Earlier and the **gesture router**), `setscreen.c` (Settings), `decision.c` (decisions), `overlay.c` (notices, overlays).
- `main/intercom_ws.c`: Wi-Fi, mDNS, the bus socket, frame reassembly, and the `events`, `notices`, `sessions` and `widgets` resources. Widgets feed the Now face: gauges `leave_by` and `weather`, and the crown line `sports`; debug hooks `gauge ...` and `sports ...` via `POST /debug/ui`.
- `main/displaywd.c` + `wd_logic.h`: the display watchdog (restarts and records a hang; `last_hang` in `/info`).
- `main/power.c`: power states (awake, dim, rest, sleep), battery, charging, quiet hours, PWR/BOOT buttons. Rules are in `power.h`.
- `main/ptt.c`, `audio_in.c`, `audio_out.c`: push-to-talk and speech playback.
- `main/capture.c`: Capture (BOOT double-press recording): ADPCM chunks, PSRAM ring, upload with acks, flash spill on the `storage` partition, the Capture screens. `main/panicdiag.c`: panic record in RTC memory (`last_panic` in `/info`).
- `main/pomodoro.c` (+ pure `pomo_logic.c`): Pomodoro mode, an on-device timer with its own full-screen layer; `POST /pomodoro` drives it (`docs/FEATURES.md` "Pomodoro").
- `main/commands.c` (+ pure `cmd_logic.c`, host test `test/host/cmd_logic_test.c`): the device command registry. Bus `command` frames (Wi-Fi and BLE) and `POST /cmd` run the same ops; `/pomodoro` and `/power` wrap it. Add a feature with one `command_register` line; never register destructive ops (`docs/FEATURES.md` "Commands").
- `main/ota.c`: OTA server and rollback guard. `settings.c` (NVS settings) and `prefs.c`.
- `main/memdiag.c`: `GET /debug/heap` and RAM test hooks. `main/psram_task.h`: task stacks in PSRAM. `main/linker.lf`: app .bss in PSRAM.
- `scripts/ota.sh`, `scripts/wifi2.sh`: update and second-network helpers.

## Build and ship

```bash
cd compact && . ~/esp/esp-idf/export.sh
idf.py set-target esp32s3                    # first time only
idf.py build 2>&1 | tail -5                 # must end "Project build complete"
COMPACT_HOST=compact.local scripts/ota.sh   # normal path, over Wi-Fi
curl -4 -s http://compact.local/info        # verify
```

- `/info` `version` comes from `git describe` at build time (`tools/fw_version.cmake`); commit before building if you want a clean version.
- **OTA only after "Project build complete".** A failed build leaves the old binary in `build/`; a stale image was once pushed as new.
- After OTA, `/info` must show the new `version` (no `-dirty` unless intended), the other `partition`, and `link` still `Paired` past the 120 s rollback window.
- Use `compact.local` with `curl -4`. The DHCP address moves; resolve with `dscacheutil -q host -a name compact.local` if the name fails. A failed upload (curl 56) can leave it briefly unreachable: wait, check `/info`, retry.
- USB (first flash, recovery): `idf.py -p /dev/cu.usbmodem* flash monitor`. Needs a data cable; if no `/dev/cu.usbmodem*` appears, suspect the cable or a charge-only port. USB boots `ota_0` with no rollback.
- Pure logic has host tests: `tools/host_tests.sh [outdir]` builds each `test/host/*_test.c` with ASan/UBSan and runs it (rim12, mom_logic, ring_px, wd_logic, touch_logic, pomo_logic, cmd_logic, gap_text, reply_logic; cJSON comes from ESP-IDF) and stops at the first failure. There are no on-device automated tests. Verification of the firmware itself is a clean build, `/info`, and looking at the device: take a screenshot (next section), and glance at the real glass where the look matters. The vivid violet in phone photos is the camera, not the panel.

## Screenshots (see the screen yourself)

Trigger one with `POST /debug/ui` body `screenshot` (Bearer), then `GET /screenshot` (Bearer) returns the PNG. Screenshots can show personal data (calendar titles); do not post them without checking.
- `/info` `screenshot` shows `{count, age_s, step_diff, step_px}`.
- It is built from the bytes actually sent to the panel (`docs/FEATURES.md` "Screenshot"), ring steps included; with a ring breathing, `step_diff` 0 means the ring's step path and LVGL's flush path agree byte for byte.
- Physical: pressing BOOT and PWR together while awake takes one (a 150 ms white ring confirms). It refuses while asleep, recording a Capture or holding push-to-talk. Agents cannot press buttons, so use the debug hook.
- The device must be awake: `POST /power` body `wake` first if `/info` says otherwise. Nothing is uploaded automatically; only the last shot is kept, in PSRAM.
- Screenshots are the way to check layout questions such as centring, clipping and text fit. Test strings: `POST /debug/ui` `sports <text>`, `gauge ...`; always clear them afterwards (`sports`, `gauge clear`).
- After a crash (`/info` `reset: "panic"`), decode `last_panic` with the ELF of the version that crashed (keep a copy of each flashed ELF).

## Workflow

- Work on a branch, ideally in its own worktree (`git worktree add ../compact-wt-<name> -b <name>`); run `git worktree list` first and never OTA over someone else's test without saying so.
- Build, OTA, check the result on the device, then merge to `main`.
- Commit messages: plain, modern, say what and why.
- Do not restart or reconfigure the AgentBus server from here.

## Secrets and safety

- Never print, log or commit tokens or Wi-Fi passwords. `sdkconfig.local` and `.envrc` are gitignored; scripts read `INTERCOM_COMPACT_TOKEN` from the environment or from the AgentBus `.env` named by `AGENTBUS_ENV`.
- `sdkconfig` is gitignored and generated; change defaults in `sdkconfig.defaults`.
- Remote endpoints (`/ota`, `/power`, `/wifi`, `/bus`) need the surface Bearer token. Keep it that way. Exposing the bus to the internet is a deliberate decision, not a side effect.
- Never stop or reconfigure the device in ways that could strand it (bad Wi-Fi slot, wrong bus URL) without a recovery path; remember OTA is LAN-only.

## Panel ownership (display lock rule)

- **`main/display.c` is the only code that talks to the panel or its SPI IO.** No other file may call `esp_lcd_*`, panel IO, the BSP's panel or brightness functions, or `esp_lv_adapter_pause/resume/refresh_now`. `tools/check_panel_owner.sh` enforces it and CMake runs it at configure time (the build fails on a violation).
- **One pixel writer.** Inside `display.c`, `panel_write()` is the only code that sends pixels, through `esp_lcd_panel_draw_bitmap` (the driver adds the CO5300's 6-column gap and builds the window). LVGL's flushes reach it through the adapter's `custom_draw_bitmap` hook, the breathing ring's steps from the compositor; the screenshot records every write there. Never build a CASET/RASET/RAMWR by hand, queue raw SPI transfers or touch esp_lcd's private structs: the check script fails the build on any of these and on a second `draw_bitmap` call. (The Nov 22 colour fault was a second writer that forgot the gap.)
- The whole panel API is in `display.h`: `compact_display_brightness()`, `_sleep()`, `_sleep_out()`, `_on()`, `_lvgl_pause()/_resume()`, `_refresh_now()`. Each takes the recursive display lock (the one `bsp_display_lock` takes) with a 2 s bounded wait, so it is safe from any task and from inside LVGL callbacks. Never add a second path to the panel; add a function in `display.c` that takes the lock.
- Why: esp_lcd's SPI panel IO keeps one shared count of queued transfers and every polling command first collects all of them with an endless wait. Two tasks at once can take the same result and one waits for ever (the Nov 22 hangs). LVGL's flush runs inside `lv_timer_handler()` under the display lock, so holding the lock is what serialises the panel.
- Never block while holding the display lock for more than a frame. The display watchdog (`docs/DEBUGGING.md` "Display watchdog") restarts the device if LVGL makes no progress for 5 s; a new feature that legitimately stalls LVGL longer must wrap itself in `compact_display_wd_hold(true/false)`.

## Design rules that bite

- **Readability first.** The screen is read at arm's length: body text at least ~26 px, primary text 34 px+, few words per screen. Do not shrink text to fit more.
- Weight sets loudness: `ambient` never interrupts; a notice never wakes the device; a decision wakes it except in quiet hours (22:00-06:30); urgent always wakes it.
- Hold-to-confirm for any action that sends something.
- Faces run Day, Now, Agents, Thread, with Now as home; Earlier is swipe up from Now only (Day scrolls its list), Settings swipe down from Now only; a detail or takeover card is dismissed by swipe down (swipe up does nothing there). Change gestures only through `earlier.c`.
- Mind RAM: internal RAM is the scarce kind (`docs/DEBUGGING.md` "RAM budget"; about 149 KB free at steady state since Nov 19, and Bluetooth needs 35 KB of it to start). New tasks go through `task_create_psram`, large buffers in PSRAM except LVGL draw buffers; check `GET /debug/heap` after adding allocations.
- Only the power task feeds the time-to-full history; do not add other writers.
- Do not say something is on the screen until the publish or flash succeeded and, where the look matters, someone has glanced at it. The device may be asleep.
