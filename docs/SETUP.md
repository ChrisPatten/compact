# Setup, settings and connectivity

Detailed notes moved from the README: building and updating, device settings, the second Wi-Fi network, and the Bluetooth phone link. Back to the [reference index](REFERENCE.md).

## Contents

- [Build and update](#build-and-update)
- [Settings](#settings)
- [Second Wi-Fi network (phone hotspot)](#second-wi-fi-network-phone-hotspot)
- [Phone link](#phone-link)

## Build and update

    . ~/esp/esp-idf/export.sh
    idf.py set-target esp32s3   # first time only
    idf.py build
    scripts/ota.sh                     # over Wi-Fi to compact.local (normal path)
    idf.py -p /dev/cu.usbmodem1401 flash monitor   # over USB (first flash, recovery)

`scripts/ota.sh` posts the image to the device's `/ota` endpoint with the surface token
(`INTERCOM_COMPACT_TOKEN` from your AgentBus `.env` (or `INTERCOM_COMPACT_TOKEN` in the environment)). Two app slots (`ota_0`, `ota_1`);
a new image that does not pair with the bus within 120 s is rolled back by the bootloader.
`curl http://compact.local/info` shows the running version and slot.

## Settings

Wi-Fi, token and bus host live in the device's NVS (namespace `intercom`). Non-empty values in
`sdkconfig.local` (gitignored; see `sdkconfig.local.example`) are written to NVS on boot, so a
board that already has them can run builds without secrets. The bus is found by mDNS
(`agentbus.local`), with an optional fallback IP; the device re-resolves after 30 s without a link.

## Second Wi-Fi network (phone hotspot)

The device prefers the primary (home) network. A secondary network is optional; set it either way:

- Build time: `CONFIG_INTERCOM_WIFI2_SSID` / `CONFIG_INTERCOM_WIFI2_PASSWORD` in `sdkconfig.local`
  (see `sdkconfig.local.example`). Like the primary, a non-empty value is written to NVS on every boot,
  so it also overwrites anything set at runtime. Leave them empty if you use the script below.
- Runtime, over the LAN: put `WIFI2_SSID` and `WIFI2_PASSWORD` in `.envrc` (gitignored) and run
  `scripts/wifi2.sh`; `scripts/wifi2.sh --clear` removes it. This is `POST /wifi` with the surface
  token: `{"slot":2,"ssid":"...","password":"..."}` or `{"slot":2,"clear":true}`. Only slot 2 can be set
  remotely. The password is never echoed or logged.

Roaming: if the primary does not come up within ~18 s (at least two failed attempts), the device scans
for the secondary and joins it if visible; otherwise it retries with backoff (5, 10, 20, 30 s) and
never restarts just for being out of range (a 2 h stretch with no IP at all is the only exception).
While on the secondary it looks for the primary every 3 minutes (longer after a return that did not
stick) and moves back when it is seen at -78 dBm or better. After joining a different network the
bus link is re-resolved at once. `GET /info` and the Settings screen (WI-FI row) show the network in
use: `ssid`, `net_slot` (primary / secondary / none), `rssi`, `roam`, `bus_via` (lan / remote / none),
`remote_bus`.

iPhone hotspot notes: iPhone hotspots are 2.4 GHz only when "Maximize Compatibility" is on
(Settings > Personal Hotspot), and the ESP32-S3 has no 5 GHz radio, so turn it on. WPA2 and WPA3
(and the transition mode) both work. The hotspot sleeps when nothing is connected, so open
Settings > Personal Hotspot on the phone once; the device keeps trying with backoff.

Off the home LAN: `agentbus.local` and the LAN fallback IP are unreachable from a hotspot, and OTA
(`scripts/ota.sh`, `compact.local`) is LAN-only. Without a remote bus the device shows "Bus not found"
on a hotspot. Optionally give it a remote URL with `POST /bus` (Bearer):
`{"url":"wss://host/api/v1/intercom/ws"}` or `{"clear":true}` (wss only; TLS verified against the ESP
x509 certificate bundle; the same hello and token). It is empty by default and is used only when the
LAN bus cannot be found or reached (always tried first on the secondary network). Reaching the bus
from the internet is a deliberate decision: it needs a public, TLS-terminated route to the bus, and the
surface token is then the only gate.

## Phone link

The phone can carry the bus link over Bluetooth (BLE, Nordic UART Service) when Wi-Fi cannot. The contract,
shared with the iOS relay app, is [`phone-link-v2.md`](phone-link-v2.md); the framing is summarised in `main/ble_link.h`.

- **AUTO** (default, LINK row on Settings): Wi-Fi as above. After 20 s without a paired bus socket the Compact
  starts NimBLE (first time only, then kept) and advertises as "Compact". A relay that subscribes and sends
  `relay_open` becomes the link (the face reads PHONE). Wi-Fi keeps retrying in the background; when its socket
  pairs again the phone gets `relay_standby`, Bluetooth disconnects and advertising stops.
- **PHONE** (hold LINK, or `POST /link` with body `ble`): one boot, Bluetooth only, no Wi-Fi. Hold LINK again, or
  10 min without the bus, restarts in AUTO.
- Audio crosses the link as IMA ADPCM (16 kHz, 8 KB/s each way, `main/adpcm.c`); the relay converts to and from
  the bus's PCM. Check the codec against the shared test vector with
  `cc -std=c11 -Wall -O2 -o /tmp/adpcm_test tools/adpcm_test.c && /tmp/adpcm_test docs/adpcm-test-vector.json`.
- `GET /info` shows `link_mode` (auto / phone), `active_link` (wifi / phone / none), `ble` (off, advertising,
  connected with MTU and interval, or init failed) and `bus_via` (lan / remote / phone / none).
- `relay/index.html` is a text-only relay page for Bluefy (it ignores audio), useful for testing.
