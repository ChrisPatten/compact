# Phone link v2: automatic failover and audio over Bluetooth

_Nov 17, 2026. The contract shared by the Compact firmware (`ble-link` branch) and the iOS relay app. Both sides must implement exactly this, and the bus does not change. v1 is described in `main/ble_link.h` and is a strict subset of this._

## 1. Transport (unchanged from v1 unless noted)
- The GATT service is the Nordic UART Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E. The phone writes to RX (6E400002), and the Compact notifies on TX (6E400003).
- Every write or notification is one chunk: `[flags][payload]`.
  - bit0 `0x01` FIRST: the first chunk of a frame.
  - bit1 `0x02` LAST: the last chunk of a frame.
  - **bit2 `0x04` BINARY (new):** the frame is binary audio (section 3), not JSON text. A binary frame is always a single chunk (`0x07`) and is never split. Any other flag bits mean the chunk is dropped.
- Text frames are UTF-8 JSON and may span chunks (FIRST … LAST), as in v1.
- Order: all frames in one direction are delivered in the order they were sent. Text and binary frames share one queue on each side, so they never overtake each other. A `speech_end` must arrive after the last speech audio, and an `audio_end` after the last mic audio.
- Writes from the phone:
  - Text frames use write-with-response, as now.
  - Binary audio frames use write-without-response, paced by `canSendWriteWithoutResponse` / `peripheralIsReady(toSendWriteWithoutResponse:)`. They are still taken from the same FIFO queue as the text frames.
- Connection parameters: after connecting, the Compact requests a connection interval of 15–30 ms, latency 0 and supervision timeout 4 s (`ble_gap_update_params`), for audio throughput. iOS may adjust these values. Both sides should log the parameters they actually get.

## 2. Control frames (JSON, `type` field)
Phone to Compact:
- `relay_open` `{"now": <epoch ms>}`: the phone's socket to the bus is open. The Compact replies with its `hello` (unchanged).
- `relay_closed`: the phone's socket to the bus dropped.
- `relay_restart`: the Compact restarts.

Compact to phone:
- `relay_ping`: a wake-up, never forwarded to the bus. It is sent every 15 s while a phone is subscribed. **New:** it is sent every 1 s in "audio window" mode. That mode starts at `audio_start` (push-to-talk) or `speech_start` and lasts until 30 s after the later of the last `audio_end`, `transcript` final, `reply` or `speech_end` frame. This keeps a suspended iOS app awake while a spoken reply is due.
- `relay_reconnect`: reopen the bus socket (unchanged).
- **New: `relay_standby`.** The Compact's own Wi-Fi link to the bus is back, so the phone should close its bus socket and stay quiet. The Compact then stops advertising and disconnects (section 4). The phone keeps a pending reconnect, which iOS holds indefinitely, so it relinks automatically when the Compact advertises again. The app shows "Standby (Compact on Wi-Fi)".

## 3. Audio frames (binary)
- The bus wire format is unchanged: raw PCM s16le mono in WebSocket binary frames.
  - Mic audio (push-to-talk) goes up between the `audio_start` (with `rate`) and `audio_end` JSON frames.
  - Spoken replies come down between `speech_start` (with `rate`) and `speech_end` / `speech_cancel`.
- Over Bluetooth, audio is compressed with **IMA ADPCM** (4 bits per sample, 4:1). At 16 kHz that is 8 KB/s each way. The relay app converts between the two:
  - **Compact to bus:** decode each ADPCM block to PCM and send it as a WebSocket binary frame.
  - **Bus to Compact:** buffer PCM, encode it into ADPCM blocks of 256 samples and send each as one binary frame. Before forwarding `speech_end` / `speech_cancel`, flush the partial block, padded with zero samples so the decoder output stays well-formed. That adds at most 15 ms of silence.
- Binary frame payload, after the flags byte:
  - byte 0: codec, `0x01` = IMA ADPCM mono.
  - bytes 1–2: int16 LE predictor (the first sample's predicted value, i.e. the decoder's initial state).
  - byte 3: step index, 0–88.
  - byte 4: sample count `n` (1–255; 0 means 256).
  - bytes 5…: `ceil(n/2)` bytes of 4-bit codes, **low nibble first**.
  - Each block is independent: the decoder state is reset from the header. The *encoder* carries its predictor and step index across the blocks of one stream, and the header records that state. It starts each stream (each `audio_start` or `speech_start`) at predictor 0, index 0. The test vector's large error is only the first few samples, while a cold encoder ramps up.
  - A full 256-sample block is 5 + 128 = 133 bytes, or 134 with the flags byte. It fits in one ATT packet at any MTU of 137 or more. With a smaller negotiated MTU, both sides use blocks of `2*(MTU-3-1-5)` samples instead.
- Standard IMA ADPCM step table (89 entries) and index table (−1,−1,−1,−1,2,4,6,8). The encoder and decoder follow the usual IMA reference.
  - Firmware: the encoder buffers mic PCM from `intercom_voice_chunk`; the decoder feeds `cbs.speech_data` with PCM bytes.
  - App: Swift encoder and decoder.
  - Both sides need a small test vector so the two implementations provably agree: a 440 Hz sine, 256 samples at 16 kHz, peak 8000, with the expected encoded bytes. Generate it with one implementation and check the other against it. The firmware repo holds the vector at `docs/adpcm-test-vector.json`, and the app has a unit test or debug assert against the same file.
- Over the phone link the Compact reports its real `volume` in `state`, so the bus speaks again. Push-to-talk works again: `intercom_voice_*` no longer refuse in phone mode.

## 4. Automatic failover (firmware)
`LINK` setting values:
- **AUTO (default):** Wi-Fi is preferred, the phone is the fallback.
- **PHONE:** one boot only and no Wi-Fi, as in v1.

Hold LINK on the Settings screen to restart into PHONE. In PHONE mode, hold it to restart back into AUTO. `POST /link ble` still selects PHONE.

In AUTO:
- Wi-Fi and the bus WebSocket behave as today: roaming, the secondary network, the remote bus URL, backoff and periodic primary rechecks. **Wi-Fi is never switched off** for the phone link, and the roam/bus tasks keep retrying in the background. That is the "periodically look for Wi-Fi" behaviour.
- **To phone:** when the WebSocket has not been connected (Paired) for **20 s**, for any reason (Wi-Fi lost, bus unreachable on LAN and remote), the Compact starts BLE advertising, initialising NimBLE on first use. When a phone subscribes and sends `relay_open`, the phone becomes the active link: hello, then deltas, as in v1. The face's link word shows `PHONE`.
- **Back to Wi-Fi:** when the WebSocket over Wi-Fi reaches Paired (welcome), Wi-Fi becomes the active link immediately. If a phone is linked, the Compact sends it `relay_standby`, waits up to 2 s for it to go out, disconnects BLE and stops advertising. The 20 s rule applies again if Wi-Fi drops later.
- **Only one active link at a time.** All sends (`link_send`, actions, view, state, voice) go to the active link. Frames arriving on the inactive link are dropped, except control frames, and the drop is logged. The `revs` in each hello resume the resources, so a switch costs no full reset unless a delta fails.
- **Restart watchdogs:**
  - The link watchdogs (5 min / 30 min without a bus) count time without *any* active link, not Wi-Fi alone.
  - The 2 h no-IP restart is skipped while the phone link is the active link.
  - The PHONE-mode 10-minute "no bus → restart onto Wi-Fi" rule applies only in PHONE mode.
- **Memory:** `esp_bt_mem_release` must not be called in AUTO mode. Measure internal heap (`/info` internal_free, internal_largest) with Wi-Fi up and NimBLE initialised and advertising.
  - If internal_free stays at or above ~30 KB and internal_largest at or above ~16 KB, initialising NimBLE at boot is fine.
  - Otherwise initialise it lazily on the first failover and keep it initialised afterwards.
  - Report the numbers either way.
- `/info` gains `"link_mode": "auto"|"phone"`, `"active_link": "wifi"|"phone"|"none"` and `"ble": "<ble_link_info>"`.
- Settings screen: the LINK row reads AUTO or PHONE. The WI-FI row behaves as today, and shows OFF only in PHONE mode.

## 5. The browser relay page (`relay/index.html`)
- It ignores BINARY chunks from the Compact and drops binary bus frames, as now.
- It handles `relay_standby` the way the app does: close the bus socket and show standby.
- It keeps working for text-only testing.
