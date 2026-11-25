# Debugging, performance and memory

Detailed notes moved from the README: frame timing, the display watchdog, restarts, crash post-mortem and the RAM budget. Back to the [reference index](REFERENCE.md).

## Contents

- [Performance](#performance)
- [Display watchdog](#display-watchdog)
- [Restarts (quiesce first)](#restarts-quiesce-first)
- [Crash post-mortem without a cable](#crash-post-mortem-without-a-cable)
- [RAM budget](#ram-budget)

## Performance

`/info` gives `frames`, `frame_avg_ms`, `frame_max_ms` since boot or the last `GET /info?reset`. `GET /debug/perf`
(Bearer) breaks the same window down: invalidations, flushed strips (`bands`) and pixels, time inside the Capture and
Now draw callbacks, and CPU ms per task since the previous `/debug/perf` call (FreeRTOS run-time stats). Lessons
from the Capture fix (recording frames ~190 ms -> ~5 ms, 21c4a30):
- A draw callback runs once for every 20-row strip of every dirty area. Measuring text there (tiny_ttf, ~30 us a
  character) costs ~10 ms a strip. Measure once and remember; skip anything that does not touch `layer->_clip_area`.
- LVGL renders the active screen under the top and sys layers even when an opaque sys-layer object covers it.
  Capture hides the screen while it is up (clear the flag bit by hand: `lv_obj_remove_flag(HIDDEN)` on a screen
  crashes in LVGL 9.5).
- Large rings: draw closed circles (a 0-359.9 degree arc adds an angle mask) and skip strips inside the hole. A
  full-screen ring redraw still costs ~25 ms, so the Capture rec ring is static (the owner's call); the timer
  invalidates only the digit cells that changed.
- The recorder, ADPCM and uploader are not the problem: with a tone recording both cores stay ~95% idle.

## Display watchdog

LVGL's task can stop for good (the display hangs: two tasks on esp_lcd's SPI IO, see [AGENTS.md](../AGENTS.md) "Panel ownership"). The task watchdog does not catch it (it only watches the idle tasks and does not reboot), so `displaywd.c` does.
- **Heartbeat:** an `lv_timer` (250 ms) in the LVGL task bumps a counter; it runs only when `lv_timer_handler()` completes a pass, so a stuck flush, a lock never released or a blocked task all stop it.
- **Monitor:** task `displaywd` (internal stack 3 KB, priority 7, above LVGL's 6) wakes once a second. Rules and numbers are in `main/wd_logic.h`: heartbeat frozen for **5 s** (15 s while Capture records), counted in capped samples (one late wake-up adds at most 1.5 s, so at least four consecutive frozen samples are needed). Never acts in the first 20 s after boot or before three beats have been seen.
- **Not a hang (held):** LVGL paused for panel sleep (`compact_display_lvgl_pause/resume`), an OTA upload, any `compact_display_wd_hold(true)` span. Staleness restarts from zero when the hold ends. The worst frame measured is about 500 ms, so 5 s is ten times that.
- **On a hang:** it writes a record to RTC memory (survives the restart, like `last_panic`) and calls `esp_restart()` (a 4 s esp_timer forces a ROM reset if the restart stalls). `/info` then shows `reset: "display-hang"` and `last_hang`: `uptime_s`, `stale_ms`, `power`, `lock` (free or held: a non-blocking try of the display lock, "held" usually means LVGL was inside a pass), `lvgl_state` (0 running, 1 ready, 2 blocked, 3 suspended), `last_op` / `last_op_task` / `last_op_age_ms` (last panel command from another task), `last_flush_age_ms`, internal and PSRAM heap. `/info` `display_wd` shows the live state (`on`, `held`, `off`, `loop-guard`), heartbeat and staleness. The same line is in `GET /log?prev`.
- **Off switches:** `POST /debug/ui` body `wd off` (stored in NVS, survives reboots) and `wd on` (also clears the loop count); `wd status`. Restart-loop guard: three hang restarts in a row, each within 5 minutes of boot, leave it off for the next boot (`display_wd.state` = `loop-guard`; it re-arms after ten healthy minutes, a power loss or `wd on`).
- **Tests (Bearer, `POST /debug/ui`):** `hang 10000` blocks the LVGL task 10 s with the lock held (the watchdog must restart it after about 5 s); `hammer <s>` flips brightness about 50 times a second against continuous redraws and must never hang; `hammer <s> nolock` does the same through the old unlocked path and is expected to hang LVGL, which proves the race and the recovery. Host test of the logic: `cc -O1 -g -std=c11 -fsanitize=address,undefined -Imain test/host/wd_logic_test.c -o /tmp/wd_logic_test && /tmp/wd_logic_test`.

## Restarts (quiesce first)

Every restart goes through `main/restart.c` (`compact_restart`), never straight to `esp_restart()`: OTA, the OTA
rollback, the power menu, Settings LINK, `POST /link`, the bus and phone-link watchdogs, the relay's restart and the display
watchdog. `esp_restart()` runs its shutdown handlers (`esp_wifi_stop`, the panicdiag cache writeback) while every other
task keeps running; twice the LVGL task, still drawing then, hit the PSRAM heap's "block already marked as free" assert
in lv_draw's task cleanup (05900ce at an OTA; 13a0f6c on 1 of 7 OTA restarts). The helper runs in a task of its
own: it arms a ROM system reset 1.5 s ahead, turns the ring off and parks LVGL (wait at most 400 ms; skipped for the display
watchdog, which must never wait on a hung LVGL), stops the bus websocket (wait at most 500 ms), then calls
`esp_restart()` (or the rollback). The log says how each step went (`restart:` lines, `GET /log?prev`). Test:
`POST /debug/ui restart`.

## Crash post-mortem without a cable

There is no coredump partition (changing the partition table needs USB). Instead `esp_panic_handler` is wrapped
(`main/panicdiag.c`): the PC, EXCCAUSE/EXCVADDR, the task, the abort message and 24 backtrace PCs go to RTC memory
and show as `last_panic` in `/info` after the reboot. Decode with
`xtensa-esp32s3-elf-addr2line -pfiaC -e build/intercom_firmware.elf <pcs>` using the ELF of the version that
crashed (keep a copy per OTA). The log ring is in no-init PSRAM, so `GET /log?prev` (Bearer) returns the previous
boot's last 16 KB. `/info` `stack_free_min` lists each long-lived task's least free stack in bytes.

## RAM budget

Internal RAM (about 340 KB of SRAM shared by IRAM code, .data/.bss and the internal heap) is the scarce one; PSRAM
(8 MB) is not. Wi-Fi DMA buffers, the LVGL draw buffers, task control blocks, ISR-reachable data and the Bluetooth
controller must be internal; almost everything else can live in PSRAM. Measured (ed67464 before, 2bd2067
after), on Wi-Fi with Capture idle:

| Consumer (internal RAM)                         | Before   | After    | How |
|-------------------------------------------------|----------|----------|-----|
| .bss: app statics (dial, earlier, thread, power) | 34.5 KB  | 0        | `main/linker.lf` (extram_bss) |
| .bss: net80211, lwip, pp, wpa_supplicant         | 14 KB    | 0        | `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` |
| App task stacks (power 12, boot_btn, speech, cap_up, bus 6, roam 5, ptt_send 4) | 46 KB | 0 | `psram_task.h` |
| Wi-Fi static TX buffers                          | 16 x 1.7 KB | 8 x 1.7 KB | the 4-segment TCP send window never filled 16 |
| mDNS task stack                                  | 4 KB     | 0        | `CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM` |
| LVGL draw buffers (2 x 466 x 20 x 2)             | 37.3 KB  | 37.3 KB  | kept: PSRAM buffers cost a 46 KB bounce buffer and frame time |
| Wi-Fi static RX buffers                          | 12.8 KB  | 12.8 KB  | kept |
| IDF/component stacks (httpd 8, websocket 8, lvgl 8, swdraw 2 x 8, wifi 6.5, sys_evt, esp_timer, tiT ...) | 66 KB | 68 KB | kept (sys_evt 2.3 -> 4 KB: it had 108 bytes left) |
| LVGL tiny_ttf cache mutexes (90 x 92 B)          | 8.3 KB   | 8.3 KB   | kept (FreeRTOS objects must be internal) |
| IRAM code                                        | 98 KB    | 100 KB   | kept |
| **Internal free, steady state**                  | **38-44 KB** (largest 24-32 KB) | **149 KB** (largest 76 KB) | |
| Internal free while asleep (CPU retention now fits, 13 KB) | (retention failed) | 135 KB | |
| Internal free with Bluetooth started as well     | (start crashed at ~30 KB) | 100 KB | |

Starting Bluetooth costs 34.5 KB of internal DMA RAM (largest piece 9.5 KB, plus 4 KB stacks for `btController`
and `nimble_host`). The controller does not report a shortfall: its buffer allocation fails later in its own task
and dereferences NULL (the `btController` panic). `ble_link_start` therefore refuses below 52 KB free / 16 KB
largest and the link stays on Wi-Fi, retrying in 5 minutes.

Rules that keep it this way:
- New tasks: `task_create_psram` (main/psram_task.h) unless the task maps flash (`esp_partition_mmap`, any
  `esp_ota_*` that reads otadata) or is an IDF/LVGL render task. Flash reads, writes and erases are fine from a
  PSRAM stack on this XIP build; mmap freezes the cache and asserts. A PSRAM task that ends uses
  `vTaskDeleteWithCaps(NULL)`.
- The light-sleep DFS minimum stays at 80 MHz: dropping to the 40 MHz XTAL clock retimes MSPI with the cache frozen
  on whichever task released the last PM lock, which may have a PSRAM stack.
- Never put a static TCB (`StaticTask_t`) in PSRAM. Large buffers: `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
- Check after a change: `GET /debug/heap` (Bearer) gives the heaps, every task's stack size, location and least
  free, and the largest internal blocks with their owner. `/debug/ui mem-hog <kb>` holds internal RAM for 15 s and
  `/debug/ui ble-start` starts NimBLE on Wi-Fi (no advertising), for testing. For attribution by call site, build
  with heap tracing: `idf.py -B build-trace -D SDKCONFIG=build-trace/sdkconfig -D INTERCOM_TRACE=1 build`, OTA it
  with `BIN=build-trace/intercom_firmware.bin scripts/ota.sh`, and read `GET /debug/heap?trace` (size, address and
  callers of each live internal allocation; resolve with `xtensa-esp32s3-elf-addr2line -fC -e <elf>`).
- Static use: `idf.py size` (DIRAM line) and `idf.py size-components`.
