#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

/*
 * Phone link v2 (docs/phone-link-v2.md): the Compact as a BLE peripheral, carrying the Intercom socket's frames to
 * and from a relay on the phone (the iOS relay app, or the relay page in Bluefy), which holds the WebSocket to the
 * bus. In AUTO mode it is the fallback while the Wi-Fi socket is down; in PHONE mode it is the only link.
 *
 * GATT: the Nordic UART Service, so nRF Connect recognises it.
 *   service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX      6E400002-...  write / write without response: phone -> Compact
 *   TX      6E400003-...  notify: Compact -> phone
 *
 * Framing, both directions: every write or notification is one chunk, [flags][bytes].
 *   bit0 0x01 FIRST, bit1 0x02 LAST: a text frame (UTF-8 JSON) may span chunks, FIRST ... LAST (0x03 = whole).
 *   bit2 0x04 BINARY: binary audio, always one chunk with flags 0x07 (an IMA ADPCM block, see adpcm.h).
 *   Any other flag combination: the chunk is dropped.
 * Text and binary frames share one FIFO in each direction, so they never overtake each other. As a convenience for
 * hand tests, a write that starts with '{' outside a frame is taken as a whole text frame.
 *
 * After connecting, the Compact asks for a 15-30 ms connection interval, latency 0, 4 s supervision timeout and
 * the longest LL data length; what it gets is logged (BLE_GAP_EVENT_CONN_UPDATE, DATA_LEN_CHG).
 * The host task never waits: what it receives goes through a queue to the link's own task (ble_rx), which calls
 * the callbacks below in arrival order.
 */

typedef struct {
    void (*connected)(bool up);                         /* a phone subscribed (true) or went away (false) */
    void (*frame)(const char *data, size_t len);        /* one whole text frame (NUL-terminated), on ble_rx */
    void (*binary)(const uint8_t *data, size_t len);    /* one binary frame's payload (after the flags), on ble_rx */
} ble_link_cbs_t;

/* Initialise NimBLE and the GATT service (once; later calls return true). `advertise` starts advertising when the
   host syncs. False when NimBLE could not be initialised (out of memory, most likely): nothing is left running,
   and it may be called again later. */
bool ble_link_start(const char *name, const ble_link_cbs_t *cbs, bool advertise);
bool ble_link_started(void);

/* Advertise (connectable) or not. Off also stops re-advertising after a disconnect. Safe from any task. */
void ble_link_set_advertising(bool on);
/* Drop the phone's connection, if any (the phone app keeps a pending reconnect). */
void ble_link_disconnect(void);
bool ble_link_busy(void);                           /* advertising, or a phone is connected */

/* Send one text frame to the phone (chunked notifications). False when no phone is subscribed or the send timed
   out. Text sends go ahead of waiting binary sends. Never call from the NimBLE host task. */
bool ble_link_send(const char *data, size_t len, TickType_t timeout);
/* Send one binary frame as a single 0x07 notification. False when it does not fit the MTU, nobody is subscribed,
   or it timed out. */
bool ble_link_send_binary(const uint8_t *data, size_t len, TickType_t timeout);

bool ble_link_ready(void);                          /* a phone is connected and subscribed to TX */
uint16_t ble_link_mtu(void);                        /* the negotiated ATT MTU (23 until exchanged) */
void ble_link_info(char *out, size_t n);            /* "off", "advertising", "connected, mtu 185, 30.00 ms, subscribed" */
