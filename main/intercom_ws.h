#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "settings.h"

typedef struct {
    void (*link)(const char *text);                              /* Wi-Fi / socket state words */
    void (*net_up)(void);                                        /* first IP: start SNTP etc. */
    void (*paired)(void);                                        /* bus sent welcome */
    void (*activity)(const char *state, const char *status_text);
    void (*calendar)(const cJSON *payload);                      /* v1 calendar feed (unused for Moments) */
    void (*moments)(const cJSON *events);                        /* v2 `events` resource, id -> Event */
    void (*notices)(const cJSON *notices);                       /* v2 `notices` resource, id -> E89 notice */
    void (*widgets)(const cJSON *widgets);                       /* v2 `widgets` resource, id -> {type, data}: Now gauge payloads */
    void (*sessions)(const cJSON *sessions);                     /* v2 `sessions` resource, id -> agent job (<= 24) */
    void (*action_result)(const char *key, const char *status, const char *reason);   /* answer to an action */
    /* Push-to-talk and the Thread (all optional; called from the socket or fetch task) */
    void (*ack)(const char *cmid, const char *status, const char *reason, const char *interaction_id);   /* a send's `ack` */
    void (*transcript)(const char *cmid, bool final, const char *status, const char *text, const char *reason);
    void (*reply)(const cJSON *data);                            /* v1 `reply` event: the agent's answer in the window */
    void (*conv_append)(const char *conv_id, const cJSON *message);   /* live message in the viewed conversation */
    void (*conversation)(const char *conv_id, const cJSON *page, int http_status);   /* intercom_fetch_conversation */
    /* Spoken replies: `kind` is "start" (with rate), "end" or "cancel"; PCM comes through speech_data */
    void (*speech)(const char *kind, const char *id, int rate);
    void (*speech_data)(const uint8_t *pcm, size_t n);
    /* Capture: every `capture_ack` frame (socket task) */
    void (*capture_ack)(const cJSON *frame);
    /* v1 `notification` event data (E89 public shape, E105 fields): every change of a notification, including resolved
       and expired, which leave the v2 `notices` resource with a delete (socket task) */
    void (*notification)(const cJSON *data);
} intercom_cbs_t;

/* Join Wi-Fi, find the bus by mDNS name (fallback IP), keep the Intercom socket up. */
void intercom_start(const settings_t *cfg, const intercom_cbs_t *cbs);

/* The two bus links (a `command` frame's result goes back on the one it came in on). */
#define INTERCOM_LINK_WIFI  1
#define INTERCOM_LINK_PHONE 2
/* One text frame on `link`; if that link is no longer up, on the active one. False when nothing could send it. */
bool intercom_send_on(int link, const char *buf, size_t len, TickType_t to);

/* Report sleep (quiet), battery and speaker volume to the bus (`state` frame). No-op while disconnected. */
void intercom_send_state(bool quiet, int battery_pct);
void intercom_queue_state(bool quiet, int battery_pct);    /* the same, queued: safe from LVGL context */
void intercom_speech_stop(void);                           /* `speech_stop`: the device stopped playing (queued) */

/*
 * Fire a button (Surface API v2 `action` frame). `object` is e.g. "events:<id>"; `draft` is the exact
 * confirm.draft shown (NULL when the button has none); `key` is unique per tap and reused on a retry.
 * Queued and sent from a separate task, so it never blocks the caller. Returns false only when the queue
 * is full; a send that fails reports cbs.action_result(key, "failed", "not_sent").
 */
bool intercom_send_action(const char *object, const char *action_id, const char *draft, const char *key);

/* GOT IT on an E89 notice (`notification_ack`), through the same queue. Fire and forget. */
bool intercom_ack_notice(const char *id);
/* SKIP on a reply notification (E105): `notification_ack` with `skip: true`. Queued; fire and forget. */
bool intercom_skip_notice(const char *id);
/* An ordinary `send` frame (the late reply's SEND TO PEGGY, E105 contract 3.4); the answer is `ack` (cbs.ack). Queued. */
bool intercom_send_text(const char *cmid, const char *body);

/* ---- the followed conversation (Thread) ---- */

/* The followed conversation's id ("" until the first welcome): from welcome.follow, else hashed like the bus does. */
const char *intercom_conversation_id(void);
/* Tell the bus what the glass shows (`view` frame), e.g. "conversation:<id>"; NULL clears. Kept and resent after a
   reconnect, so live `conv_append` frames keep coming while it is set. Queued; any task. */
void intercom_set_view(const char *view);
/* GET /api/v1/surface/conversations/<followed id>?limit=N on its own task; the answer comes through
   cbs.conversation. False when two fetches are already running or nothing is followed yet. */
bool intercom_fetch_conversation(int limit);
/* The same for any conversation id (the bus allows the followed one and those listed in `sessions`). */
bool intercom_fetch_conversation_id(const char *conv_id, int limit);
/* Tap a choice button on a conversation message (`conversations:<id>` action, `index` = the button's index). */
bool intercom_send_conv_action(const char *conv_id, int index, const char *message_id, const char *key);

/* ---- push-to-talk audio (E92 frames): call from ONE task, never with the display lock held ---- */
bool intercom_connected(void);
bool intercom_debug_ble_start(void);                            /* POST /debug/ui ble-start: NimBLE up, no advertising */
void intercom_request_resync(void);    /* reconnect the active link for a fresh reset of every resource (testday off) */
void intercom_stop_for_restart(void);  /* restart.c: no more sends, and the bus websocket stopped (blocks up to ~1 s) */
bool intercom_voice_start(const char *cmid, int rate);          /* audio_start */
/* audio_start bound to a notification (E105 contract 3.2); NULL or "" = intercom_voice_start */
bool intercom_voice_start_bound(const char *cmid, int rate, const char *notification_id);
bool intercom_voice_chunk(const int16_t *pcm, size_t samples);  /* one binary frame of 16-bit mono PCM */
bool intercom_voice_end(const char *cmid);                      /* audio_end: transcribe and send */
bool intercom_voice_cancel(const char *cmid);                   /* audio_cancel: drop it */

/* ---- Capture uploads (capture.c): JSON text frames over the Wi-Fi socket only ---- */
bool intercom_capture_ready(void);                         /* Wi-Fi is the active link and the bus welcomed us */
uint32_t intercom_capture_gen(void);                       /* changes on every new Wi-Fi socket (resend capture_start) */
bool intercom_capture_send(const char *json, size_t len);  /* one text frame; false when not ready or the send failed */

const char *intercom_last_action(void);   /* last action send / answer, for /info */
void intercom_link_info(char *out, size_t n);   /* bus task phase, link word and ages, for /info */

/* ---- networks and the bus address ---- */

typedef struct {
    char ssid[33];          /* the network in use (or being tried); "" when none is configured */
    int slot;               /* 0 none, 1 primary, 2 secondary */
    bool online;            /* Wi-Fi has an IP */
    int rssi;               /* dBm, valid when online */
    const char *bus_via;    /* "lan", "remote", "phone" or "none" (no active link) */
    const char *roam;       /* "online", "searching" or "idle" */
    bool remote_set;        /* a remote bus URL is stored */
    bool other_set;         /* the network not in use is configured (a switch is possible) */
    int switch_state;       /* last manual switch: 0 none, 1 looking, 2 switched, 3 not in range */
    bool pinned;            /* on the secondary by choice: no automatic return home */
} intercom_net_t;
void intercom_net_info(intercom_net_t *out);

/* Move to the other configured network now (held on the settings screen). The roaming task scans for it and
   switches only if it is in range; switch_state in intercom_net_info reports the outcome. A move to the
   secondary sticks until that network drops or the primary is chosen again. False when there is no other
   network configured. Either apostrophe form of an SSID matches (iPhone hotspots use ’). */
bool intercom_net_switch(void);

/* Set or clear the secondary network (slot 2 only; the primary is seeded from sdkconfig.local). Saved to
   NVS; the roaming task applies it shortly after. False on a bad argument or an NVS error. */
bool intercom_wifi_set(int slot, const char *ssid, const char *pass, bool clear);

/* Set or clear the optional remote bus URL (wss://host[:port]/api/v1/intercom/ws). Saved to NVS; used only
   when the LAN bus cannot be reached. False when the URL is not a wss:// URL or NVS fails. */
bool intercom_bus_set(const char *url, bool clear);

/* ---- phone link (BLE), docs/phone-link-v2.md ----
 * AUTO (the default): Wi-Fi is preferred; after 20 s without a paired Wi-Fi socket the Compact advertises and a
 * phone relay becomes the link, until the Wi-Fi socket pairs again (the phone is then sent to standby).
 * PHONE (one boot, from the LINK setting or POST /link ble): Bluetooth only, no Wi-Fi. */

bool intercom_ble_mode(void);

/* power.c stopped (true) or restarted (false) Wi-Fi to save a low battery. While paused, AUTO does not fail over
   to the phone, and an active phone link stands down. */
void intercom_wifi_paused(bool paused);                  /* true on a PHONE-mode boot (no Wi-Fi) */
const char *intercom_link_mode(void);          /* "auto" or "phone" */
const char *intercom_active_link(void);        /* "wifi", "phone" or "none" */
void intercom_ble_info(char *out, size_t n);   /* the BLE side: "off", "advertising", "connected, mtu ...", "init failed ..." */
