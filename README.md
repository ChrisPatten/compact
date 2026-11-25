# The Compact (intercom-firmware)

The Compact is a small round desk device that shows your day at a glance. Its 466 px AMOLED screen is a clock face with a rim around it: appointments ("Moments") sit on the rim, and a dot (the "planet") circles it like the tip of a clock hand. It also lights a soft breathing ring when something is waiting for you, shows what your AI agents are doing, runs a Pomodoro timer, takes push-to-talk questions, records long audio (Capture), and talks to an AgentBus server over Wi-Fi, or over Bluetooth through a phone relay when Wi-Fi is out of reach.

This repository is its firmware: C on ESP-IDF v5.5.5 and LVGL 9.5. It speaks the AgentBus Intercom WebSocket protocol v1 and draws the Plum Dusk Day Dial design. The device draws only what the bus sends it.

## What it does

- Shows today's Moments on a 12-hour rim (12 at the top), with a countdown to the next one and a leave-by gauge.
- Lists the day, scrolling, with gaps ("2 H FREE") and overlaps marked.
- Breathes a ring on the rim when a notice, decision or urgent item waits: plum, amber or rose by weight.
- Shows agent jobs and lets you open a job's conversation.
- Asks you decisions and takes replies, with hold-to-confirm for anything that sends something.
- Runs a full-screen Pomodoro timer on the device, with no network needed.
- Records long audio (Capture) and uploads it to the bus in chunks, saving to flash when offline.
- Speaks replies and plays chimes; takes push-to-talk.
- Updates itself over Wi-Fi (OTA), with automatic rollback if a new image cannot reach the bus.
- Takes a screenshot of exactly what the panel shows, without a camera.

## Hardware

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C.
- Screen: 466 x 466 round AMOLED with capacitive touch.
- Power: AXP2101 chip, 500 mAh battery, USB-C. 2.4 GHz Wi-Fi only (the ESP32-S3 has no 5 GHz radio), plus Bluetooth LE.
- Memory: about 340 KB internal RAM (the scarce kind) and 8 MB PSRAM.
- Buttons: BOOT and PWR (see "Tour" below).

## Quick start

You need ESP-IDF v5.5.5 installed at `~/esp/esp-idf`.

    . ~/esp/esp-idf/export.sh
    idf.py set-target esp32s3   # first time only
    idf.py build
    scripts/ota.sh                                  # over Wi-Fi to compact.local (the normal path)
    idf.py -p /dev/cu.usbmodem1401 flash monitor    # over USB (first flash, recovery)

- **First flash** must go over USB. After that, use OTA. The device has two app slots; a new image that does not pair with the bus within 120 s is rolled back.
- **OTA** posts the image to the device's `/ota` endpoint with the surface token (`INTERCOM_COMPACT_TOKEN`, read from your AgentBus `.env` (or `INTERCOM_COMPACT_TOKEN` in the environment) by the script). OTA works on the home LAN only.
- **Check it:** `curl -4 http://compact.local/info` shows the running version, slot and link state.
- **First-time settings:** Wi-Fi, token and bus host live in the device's NVS (flash settings). Put them in `sdkconfig.local` (gitignored; copy `sdkconfig.local.example`) and the device writes them to NVS on boot. The bus is found by mDNS (`agentbus.local`), with an optional fallback IP.
- **Never** commit tokens or Wi-Fi passwords. `sdkconfig` is generated; change defaults in `sdkconfig.defaults`.

More: [docs/SETUP.md](docs/SETUP.md).

## Tour

**Faces.** The faces sit in a row, left to right: Day, Now, Agents, Thread. Now is home. Swipe right on Now to open Day; swipe left on Now to open Agents, and left again to reach Thread. Swipe the other way to go back.

- **Now:** the Day Dial. Clock, countdown to the next Moment, the rim, the agent count ring at 9 o'clock.
- **Day:** a scrolling list of today's Moments. Drag up and down to scroll; tap a row for its detail card.
- **Agents:** a list of jobs, or the top job's question with its buttons. A row opens that conversation (**Thread**).
- **Settings:** swipe down on Now. Six rows: SLEEP AFTER, QUIET HOURS, RIM GAP, FLASHLIGHT, WI-FI and LINK. Tap for list values; hold 0.8 s for FLASHLIGHT, WI-FI and LINK. Swipe up from above or below the list, press PWR, or wait 60 s to close.
- **Earlier:** swipe up on Now. A tray of notices and decisions that were set aside or ended. Tap a row to reopen it or read how it ended; hold to clear.
- **Detail cards:** a Moment, a notice, a decision or an urgent alert covers the face. Swipe down to dismiss. With a notice pending, swipe up on Now opens its card instead of Earlier.

**Buttons.** Touch also wakes a dimmed or resting screen, and that tap does nothing else.

- **PWR click:** from sleep, wakes to Now. Dimmed or resting, goes to full Now. On full Now with nothing covering it, sleeps. Anything else returns to Now. **Hold 3 s:** power menu. **Hold 8 s:** forced power-off.
- **BOOT click:** opens Thread on the followed conversation (waking the device if asleep). **Double-click:** Capture. **Hold:** push-to-talk, asleep or awake. BOOT never sleeps the device.
- **BOOT + PWR together** (each pressed within 150 ms of the other, awake only): takes a screenshot. Neither button's own action runs, and it is refused during Capture and push-to-talk.

Gestures and tap rules are owned by `main/earlier.c`; see [docs/DISPLAY.md](docs/DISPLAY.md).

## Connectivity

- **Wi-Fi first.** The device prefers the home network and finds the bus by mDNS.
- **Hotspot fallback.** You can set a second network (for example a phone hotspot) with `scripts/wifi2.sh` or `sdkconfig.local`. If home Wi-Fi does not come up, the device scans for it, and moves back home when the home network is seen again. iPhone hotspots must have "Maximize Compatibility" on. Off the home LAN, an optional remote bus URL (`POST /bus`, `wss` only) is needed.
- **BLE phone relay.** In AUTO (the default LINK mode), if the bus has not paired for 20 s the Compact advertises over Bluetooth as "Compact". An iPhone relay app (or the test page `relay/index.html`) carries the bus link, including push-to-talk and spoken replies as compressed audio. When Wi-Fi returns, the phone steps back. Hold LINK in Settings for PHONE-only mode.
- `GET /info` shows the link state (`link_mode`, `active_link`, `bus_via`, `net_slot`, `rssi`).

Details: [docs/SETUP.md](docs/SETUP.md) and the shared contract [docs/phone-link-v2.md](docs/phone-link-v2.md).

## Commands

The device has one registry of named operations ("ops"): `pomodoro`, `power`, `face`, `chime`, `info` and `help`. The same ops can be run three ways: as a `command` frame from the bus (over Wi-Fi or the BLE phone link, the answer returns the same way), as `POST /cmd` over HTTP with the Bearer token, and through thin wrappers such as `POST /pomodoro` and `POST /power`. Adding a feature is one `command_register` line in `main/commands.c`; the bus never needs to know about it.

Commands never run on the receive path: they queue (8 slots), expire after their `ttl_s`, and repeats are answered from a small memory of recent ids. Destructive ops (restart, Wi-Fi, bus URL, link mode, OTA) and screenshots are deliberately not available as commands. Contract and details: [docs/FEATURES.md](docs/FEATURES.md#commands-maincommandsc).

## Repository map

- `main/`: the firmware. Faces (`dial.c`, `dayview.c`, `agents.c`, `thread.c`, `earlier.c`, `setscreen.c`, `decision.c`, `overlay.c`), the bus link (`intercom_ws.c`, `ble_link.c`), power and buttons (`power.c`), display and panel (`display.c`), Capture, Pomodoro, commands, OTA, and the pure logic files that have host tests.
- `docs/`: deeper notes (see below) and the phone-link contract and ADPCM test vector.
- `test/host/`: host-side unit tests for the pure logic. Run them all with `tools/host_tests.sh` (it runs rim12, mom_logic, ring_px, wd_logic, touch_logic, pomo_logic, cmd_logic, gap_text and reply_logic under ASan/UBSan).
- `scripts/`: `ota.sh` (update over Wi-Fi) and `wifi2.sh` (set or clear the second network).
- `tools/`: `check_panel_owner.sh` (run at configure time), `fw_version.cmake`, `host_tests.sh`, `adpcm_test.c` and `adpcm_ref.py`.
- `relay/`: `index.html`, a text-only Bluetooth relay page for Bluefy, for testing.
- `fonts/`: Lexend Exa and Hanken Grotesk subsets (OFL).
- `partitions.csv`, `sdkconfig.defaults`, `sdkconfig.local.example`, `sdkconfig.trace`: build configuration.

## Deeper docs

- [AGENTS.md](AGENTS.md): working agreement for people and agents (workflow, panel ownership rule, design rules, secrets).
- [docs/REFERENCE.md](docs/REFERENCE.md): index of everything below.
- [docs/SETUP.md](docs/SETUP.md): build and update, settings, second Wi-Fi and roaming, the phone link.
- [docs/FEATURES.md](docs/FEATURES.md): Pomodoro, Capture, Screenshot, Commands.
- [docs/DISPLAY.md](docs/DISPLAY.md): source layout, the 12-hour rim, breathing ring, FREE Moments, Day list, gesture router, pill list.
- [docs/DEBUGGING.md](docs/DEBUGGING.md): performance, display watchdog, restarts, crash post-mortem, RAM budget.
- [docs/phone-link-v2.md](docs/phone-link-v2.md): BLE framing, audio and failover contract.

## License

MIT, see [LICENSE](LICENSE). The bundled fonts (Lexend Exa, Hanken Grotesk, and Fraunces on the website) are under the SIL Open Font License; their licences sit beside the font files.
