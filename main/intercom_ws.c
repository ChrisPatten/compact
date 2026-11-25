#include "intercom_ws.h"
#include "restart.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "psram_task.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "esp_http_client.h"
#include "mbedtls/sha256.h"
#include "prefs.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "ble_link.h"
#include "adpcm.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/rom_layout.h"
#endif
#include <sys/time.h>
#include <time.h>
#include "freertos/semphr.h"
#include "memdiag.h"
#include "reply_logic.h"
#include "commands.h"
#include <stdlib.h>

#define BIT_IP        BIT0
#define BIT_WS        BIT1      /* the WebSocket over Wi-Fi is connected (hello sent) */
#define BIT_PHONE     BIT2      /* the phone's relay socket to the bus is open (relay_open) */
#define STALL_US      (30LL * 1000 * 1000)

static const char *TAG = "intercom";
static intercom_cbs_t s_cbs;
static settings_t s_cfg;
static EventGroupHandle_t s_eg;
static esp_websocket_client_handle_t s_ws;
static char s_last_action[160] = "none";   /* diagnostics for /info */

const char *intercom_last_action(void) { return s_last_action; }

/* Link diagnostics for /info, and a watchdog: the bus task stamps s_beat each pass of its loop. */
static const char *volatile s_phase = "boot";
static char s_link_word[24] = "none";
static volatile int64_t s_link_at, s_beat, s_up_at;
static volatile int s_link_restarts;

void intercom_link_info(char *out, size_t n)
{
    int64_t now = esp_timer_get_time();
    snprintf(out, n, "%s, %s %llds ago, loop %llds ago, %d restarts", s_phase, s_link_word,
             (long long)((now - s_link_at) / 1000000), (long long)((now - s_beat) / 1000000), s_link_restarts);
}
static char *s_rx; static size_t s_rx_cap;
static char s_bus_base[160];       /* http(s)://host[:port] of the bus the socket last used: conversation fetches go there too */
static char s_conv_id[65];         /* the followed conversation (welcome.follow) */
static char s_view[100];           /* what we last told the bus the glass shows; resent after a reconnect */
static volatile int s_fetching;    /* conversation fetches in flight (each its own task) */
static void queue_view(void);
static volatile bool s_resync_ws, s_resync_phone;   /* a delta did not apply: reconnect that link for a fresh reset */

/* Links (docs/phone-link-v2.md s4). AUTO (the default): Wi-Fi is preferred and the phone (BLE relay, ble_link.c)
   is the fallback; PHONE (one boot, chosen with the LINK setting): Bluetooth only, no Wi-Fi. Exactly one link is
   active; every send goes to it, and frames from the other one are dropped (control frames excepted). */
enum { L_NONE, L_WIFI, L_PHONE };
static bool s_phone_only;                         /* PHONE mode this boot */
static volatile int s_active = L_NONE;
static volatile bool s_ws_paired;                 /* welcome received on the current WebSocket */
static volatile uint32_t s_wifi_gen;              /* Wi-Fi welcomes so far: a new value means a new socket */
static volatile bool s_ws_kick;                   /* the phone took over: drop the Wi-Fi socket, so a return is a fresh welcome */
static volatile int64_t s_wifi_bad_since;         /* AUTO: when "socket paired and an IP" last stopped being true; 0 while it is */
static volatile bool s_wifi_paused;              /* power.c turned Wi-Fi off on purpose (asleep, battery < 10%) */
static volatile int64_t s_switch_at;              /* last change of the active link */
static volatile int64_t s_last_active;            /* last time any link was active (the restart watchdogs) */
static volatile int64_t s_phone_active_at;        /* last time the phone was the active link (the 2 h no-IP rule) */
static TaskHandle_t s_phone_task;
static const char *link_name(int l) { return l == L_WIFI ? "wifi" : l == L_PHONE ? "phone" : "none"; }

static bool link_up(void)
{
    if (!s_eg) return false;
    EventBits_t b = xEventGroupGetBits(s_eg);
    if (s_active == L_PHONE) return b & BIT_PHONE;
    if (s_active == L_WIFI) return (b & BIT_WS) && s_ws;
    return false;
}
static bool wifi_ok(void) { return s_ws_paired && s_eg && (xEventGroupGetBits(s_eg) & BIT_IP); }

static void set_active(int l)
{
    if (s_active == l) return;
    ESP_LOGI(TAG, "active link: %s -> %s", link_name(s_active), link_name(l));
    int64_t now = esp_timer_get_time();
    if (s_active != L_NONE) s_last_active = now;
    if (s_active == L_PHONE) s_phone_active_at = now;
    s_active = l;
    s_switch_at = now;
}

static int ws_send_text(const char *buf, size_t len, TickType_t to)
{
    esp_websocket_client_handle_t ws = s_ws;
    return ws ? esp_websocket_client_send_text(ws, buf, len, to) : -1;
}
static int link_send(const char *buf, size_t len, TickType_t to)
{
    int a = s_active;
    if (a == L_PHONE) return ble_link_send(buf, len, to) ? (int)len : -1;
    if (a == L_WIFI) return ws_send_text(buf, len, to);
    return -1;
}
_Static_assert(INTERCOM_LINK_WIFI == L_WIFI && INTERCOM_LINK_PHONE == L_PHONE, "link ids are shared with commands.c");
bool intercom_send_on(int link, const char *buf, size_t len, TickType_t to)
{
    if (link == L_PHONE ? ble_link_send(buf, len, to) : ws_send_text(buf, len, to) > 0) return true;
    return s_active != link && link_send(buf, len, to) > 0;
}
bool intercom_ble_mode(void) { return s_phone_only; }
const char *intercom_link_mode(void) { return s_phone_only ? "phone" : "auto"; }
const char *intercom_active_link(void) { return link_name(s_active); }

/* Frames from the inactive link: dropped, logged at most every 5 s with a count. */
static void drop_inactive(int src, const char *what)
{
    static int64_t last;
    static unsigned n;
    n++;
    int64_t now = esp_timer_get_time();
    if (now - last > 5000000) {
        ESP_LOGW(TAG, "dropped %u frame(s) from the inactive %s link (last: %s; active: %s)", n, link_name(src), what,
                 link_name(s_active));
        n = 0;
        last = now;
    }
}

/* Audio window (s2): from audio_start or speech_start until 30 s after the last audio_end, final transcript, reply
   or speech_end (also audio_cancel and speech_cancel), the phone gets a relay_ping every second. An open window
   with no end frame closes after 3 min. */
#define AUDIO_TAIL_US  (30LL * 1000000)
#define AUDIO_OPEN_MAX (180LL * 1000000)
static volatile int64_t s_audio_open_at, s_audio_until;
static void audio_window_open(void) { s_audio_open_at = esp_timer_get_time(); }
static void audio_window_tail(void) { s_audio_until = esp_timer_get_time() + AUDIO_TAIL_US; s_audio_open_at = 0; }
static bool audio_window(int64_t now)
{
    return (s_audio_open_at && now - s_audio_open_at < AUDIO_OPEN_MAX) || now < s_audio_until;
}

/* Surface v2 resources we keep (id -> item): `events` (Moments), `notices` (E89 notifications), `sessions`
   (agent jobs; the bus caps it at 24 items, so the kept tree stays a few tens of KB of PSRAM) and `widgets` (gauge
   payloads for the Now arcs, data/intercom/gauge-payload.md; the bus caps it at 24 items of 4 KB). */
typedef struct { const char *name; cJSON *items; int rev; } res_t;
#define N_RES 4
static res_t s_res[N_RES] = { { "events", NULL, -1 }, { "notices", NULL, -1 }, { "sessions", NULL, -1 }, { "widgets", NULL, -1 } };

static void res_changed(res_t *r)
{
    if (r == &s_res[0] && s_cbs.moments) s_cbs.moments(r->items);
    if (r == &s_res[1] && s_cbs.notices) s_cbs.notices(r->items);
    if (r == &s_res[2] && s_cbs.sessions) s_cbs.sessions(r->items);
    if (r == &s_res[3] && s_cbs.widgets) s_cbs.widgets(r->items);
}

/* Apply a Surface v2 delta frame (docs/SURFACE_API.md, "Delta frame"). */
static void apply_delta(res_t *r, const cJSON *j, int src)
{
    const cJSON *rev = cJSON_GetObjectItem(j, "rev");
    if (!cJSON_IsNumber(rev)) return;
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "reset"))) {
        const cJSON *items = cJSON_GetObjectItem(j, "items");
        cJSON_Delete(r->items);
        r->items = cJSON_IsObject(items) ? cJSON_Duplicate(items, true) : cJSON_CreateObject();
        r->rev = rev->valueint;
        ESP_LOGI(TAG, "%s reset at rev %d: %d items", r->name, r->rev, cJSON_GetArraySize(r->items));
        res_changed(r);
        return;
    }
    const cJSON *base = cJSON_GetObjectItem(j, "base_rev");
    if (!r->items || !cJSON_IsNumber(base) || base->valueint != r->rev) {
        /* Just after a link switch the new socket may replay a step the old one already brought (its hello's revs
           were taken a moment before the old link's last deltas). Same bus, same rev numbers: skip it. */
        if (r->items && cJSON_IsNumber(base) && base->valueint < r->rev && rev->valueint <= r->rev &&
            esp_timer_get_time() - s_switch_at < 15LL * 1000000) {
            ESP_LOGI(TAG, "%s delta %d->%d already applied (link switch): skipped", r->name, base->valueint, rev->valueint);
            return;
        }
        ESP_LOGW(TAG, "%s delta base %d does not match rev %d: resyncing", r->name,
                 cJSON_IsNumber(base) ? base->valueint : -1, r->rev);
        if (src == L_PHONE) s_resync_phone = true;
        else s_resync_ws = true;
        return;
    }
    const cJSON *op;
    cJSON_ArrayForEach(op, cJSON_GetObjectItem(j, "ops")) {
        const cJSON *kind = cJSON_GetObjectItem(op, "op"), *id = cJSON_GetObjectItem(op, "id");
        const cJSON *val = cJSON_GetObjectItem(op, "value");
        if (!cJSON_IsString(kind) || !cJSON_IsString(id)) continue;
        if (!strcmp(kind->valuestring, "remove")) {
            cJSON_DeleteItemFromObjectCaseSensitive(r->items, id->valuestring);
        } else if (!strcmp(kind->valuestring, "set") && cJSON_IsObject(val)) {
            cJSON_DeleteItemFromObjectCaseSensitive(r->items, id->valuestring);
            cJSON_AddItemToObject(r->items, id->valuestring, cJSON_Duplicate(val, true));
        } else if (!strcmp(kind->valuestring, "patch") && cJSON_IsObject(val)) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(r->items, id->valuestring);
            if (!item) continue;
            const cJSON *f;
            cJSON_ArrayForEach(f, val) {
                cJSON_DeleteItemFromObjectCaseSensitive(item, f->string);
                if (!cJSON_IsNull(f)) cJSON_AddItemToObject(item, f->string, cJSON_Duplicate(f, true));
            }
        }
    }
    r->rev = rev->valueint;
    res_changed(r);
}

static void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static void status(const char *t)
{
    ESP_LOGI(TAG, "%s", t);
    snprintf(s_link_word, sizeof s_link_word, "%s", t);
    s_link_at = esp_timer_get_time();
    if (s_cbs.link) s_cbs.link(t);
}

/* A state word from one link while the other is the active one only goes to the log: the face keeps showing the
   link in use (PHONE while the Wi-Fi side keeps retrying in the background). */
static void status_src(int src, const char *t)
{
    int a = s_active;
    if (a != L_NONE && a != src) { ESP_LOGI(TAG, "%s link: %s (%s is the active link)", link_name(src), t, link_name(a)); return; }
    status(t);
}
static void wstatus(const char *t) { status_src(L_WIFI, t); }

static void phone_task_poke(void) { if (s_phone_task) xTaskNotifyGive(s_phone_task); }

/* The followed conversation: welcome.follow.conversation_id (bus with E92), else the bus's own rule,
   sha256 of sorted [contact, channel, topic or "general"] joined with ':'. */
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void learn_conversation(const cJSON *f)
{
    const cJSON *cid = cJSON_GetObjectItem(f, "conversation_id");
    if (cJSON_IsString(cid) && strlen(cid->valuestring) < sizeof s_conv_id) { strlcpy(s_conv_id, cid->valuestring, sizeof s_conv_id); return; }
    const cJSON *c = cJSON_GetObjectItem(f, "contact"), *ch = cJSON_GetObjectItem(f, "channel"), *t = cJSON_GetObjectItem(f, "topic");
    if (!cJSON_IsString(c) || !cJSON_IsString(ch)) return;
    const char *contact = c->valuestring;
    if (!strncmp(contact, "contact:", 8)) contact += 8;
    const char *parts[3] = { contact, ch->valuestring, cJSON_IsString(t) && t->valuestring[0] ? t->valuestring : "general" };
    qsort(parts, 3, sizeof parts[0], cmp_str);
    char joined[320];
    snprintf(joined, sizeof joined, "%s:%s:%s", parts[0], parts[1], parts[2]);
    uint8_t h[32];
    mbedtls_sha256((const unsigned char *)joined, strlen(joined), h, 0);
    for (int i = 0; i < 32; i++) snprintf(s_conv_id + 2 * i, 3, "%02x", h[i]);
}

const char *intercom_conversation_id(void) { return s_conv_id; }

/* One bus frame from `src` (L_WIFI: the WebSocket; L_PHONE: the relay). Frames from the inactive link are dropped,
   except a Wi-Fi welcome (which makes Wi-Fi the active link) and bus errors (logged). */
static void handle_frame(const char *data, size_t len, int src)
{
    cJSON *j = cJSON_ParseWithLength(data, len);
    if (!j) { ESP_LOGW(TAG, "unparseable frame (%u bytes)", (unsigned)len); return; }
    const cJSON *type = cJSON_GetObjectItem(j, "type");
    bool welcome = cJSON_IsString(type) && !strcmp(type->valuestring, "welcome");
    if (src != s_active && !(welcome && src == L_WIFI) && !(cJSON_IsString(type) && !strcmp(type->valuestring, "error"))) {
        drop_inactive(src, cJSON_IsString(type) ? type->valuestring : "?");
        cJSON_Delete(j);
        return;
    }
    if (welcome) {
        if (src == L_WIFI) {
            int was = s_active;
            s_ws_paired = true;
            s_wifi_gen++;
            set_active(L_WIFI);
            if (was == L_PHONE) ESP_LOGI(TAG, "Wi-Fi is back: the phone link stands by");
            phone_task_poke();                              /* relay_standby, then drop the phone */
        }
        status(src == L_PHONE ? "Phone" : "Paired");
        learn_conversation(cJSON_GetObjectItem(j, "follow"));
        if (s_view[0]) queue_view();                    /* a fresh socket has no view: say it again */
        if (s_cbs.paired) s_cbs.paired();
        memdiag_mark("paired");
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "event")) {
        const cJSON *ev = cJSON_GetObjectItem(j, "event");
        const cJSON *d = cJSON_GetObjectItem(j, "data");
        if (cJSON_IsString(ev) && d) {
            if (!strcmp(ev->valuestring, "activity") && s_cbs.activity) {
                const cJSON *st = cJSON_GetObjectItem(d, "status_text");
                const cJSON *state = cJSON_GetObjectItem(d, "state");
                s_cbs.activity(cJSON_IsString(state) ? state->valuestring : "idle",
                               cJSON_IsString(st) ? st->valuestring : NULL);
            } else if (!strcmp(ev->valuestring, "calendar") && s_cbs.calendar) {
                s_cbs.calendar(d);
            } else if (!strcmp(ev->valuestring, "reply")) {
                audio_window_tail();
                if (s_cbs.reply) s_cbs.reply(d);
            } else if (!strcmp(ev->valuestring, "notification") && s_cbs.notification) {
                s_cbs.notification(d);                      /* v1 event: the last word on a resolved / expired item */
            } else if (!strcmp(ev->valuestring, "conversation_append") && s_cbs.conv_append) {
                const cJSON *cid = cJSON_GetObjectItem(d, "conversation_id"), *m = cJSON_GetObjectItem(d, "message");
                if (cJSON_IsString(cid) && cJSON_IsObject(m)) s_cbs.conv_append(cid->valuestring, m);
            }
        }
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "delta")) {
        const cJSON *res = cJSON_GetObjectItem(j, "resource");
        for (int i = 0; i < N_RES && cJSON_IsString(res); i++)
            if (!strcmp(res->valuestring, s_res[i].name)) apply_delta(&s_res[i], j, src);
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "action_result")) {
        const cJSON *k = cJSON_GetObjectItem(j, "idempotency_key"), *st = cJSON_GetObjectItem(j, "status");
        const cJSON *rs = cJSON_GetObjectItem(j, "reason");
        ESP_LOGI(TAG, "action_result %s: %s %s", cJSON_IsString(k) ? k->valuestring : "?",
                 cJSON_IsString(st) ? st->valuestring : "?", cJSON_IsString(rs) ? rs->valuestring : "");
        snprintf(s_last_action, sizeof s_last_action, "result %s %s", cJSON_IsString(st) ? st->valuestring : "?", cJSON_IsString(rs) ? rs->valuestring : "");
        if (s_cbs.action_result && cJSON_IsString(k) && cJSON_IsString(st))
            s_cbs.action_result(k->valuestring, st->valuestring, cJSON_IsString(rs) ? rs->valuestring : NULL);
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "ack")) {
        const cJSON *id = cJSON_GetObjectItem(j, "client_msg_id"), *st = cJSON_GetObjectItem(j, "status");
        const cJSON *rs = cJSON_GetObjectItem(j, "reason"), *iid = cJSON_GetObjectItem(j, "interaction_id");
        ESP_LOGI(TAG, "ack %s: %s", cJSON_IsString(id) ? id->valuestring : "?", cJSON_IsString(st) ? st->valuestring : "?");
        if (s_cbs.ack && cJSON_IsString(id) && cJSON_IsString(st))
            s_cbs.ack(id->valuestring, st->valuestring, cJSON_IsString(rs) ? rs->valuestring : NULL, cJSON_IsString(iid) ? iid->valuestring : NULL);
    } else if (cJSON_IsString(type) && !strncmp(type->valuestring, "speech_", 7)) {
        const cJSON *id = cJSON_GetObjectItem(j, "id"), *rate = cJSON_GetObjectItem(j, "rate");
        ESP_LOGI(TAG, "%s %s", type->valuestring, cJSON_IsString(id) ? id->valuestring : "?");
        if (!strcmp(type->valuestring, "speech_start")) audio_window_open();
        else if (!strcmp(type->valuestring, "speech_end") || !strcmp(type->valuestring, "speech_cancel")) audio_window_tail();
        if (s_cbs.speech && cJSON_IsString(id))
            s_cbs.speech(type->valuestring + 7, id->valuestring, cJSON_IsNumber(rate) ? rate->valueint : 16000);
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "transcript")) {
        const cJSON *id = cJSON_GetObjectItem(j, "client_msg_id"), *st = cJSON_GetObjectItem(j, "status");
        const cJSON *tx = cJSON_GetObjectItem(j, "text"), *rs = cJSON_GetObjectItem(j, "reason");
        bool fin = cJSON_IsTrue(cJSON_GetObjectItem(j, "final"));
        if (fin) audio_window_tail();
        if (fin) ESP_LOGI(TAG, "transcript %s: %s %s", cJSON_IsString(id) ? id->valuestring : "?", cJSON_IsString(st) ? st->valuestring : "?", cJSON_IsString(rs) ? rs->valuestring : "");
        if (s_cbs.transcript && cJSON_IsString(id))
            s_cbs.transcript(id->valuestring, fin, cJSON_IsString(st) ? st->valuestring : (fin ? "failed" : "partial"),
                             cJSON_IsString(tx) ? tx->valuestring : "", cJSON_IsString(rs) ? rs->valuestring : NULL);
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "command")) {
        commands_on_frame(j, src);                      /* queued to the command task; the result goes back on `src` */
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "capture_ack")) {
        if (s_cbs.capture_ack) s_cbs.capture_ack(j);
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "error")) {
        const cJSON *code = cJSON_GetObjectItem(j, "code");
        ESP_LOGE(TAG, "bus error: %s", cJSON_IsString(code) ? code->valuestring : "?");
        snprintf(s_last_action, sizeof s_last_action, "bus error %s", cJSON_IsString(code) ? code->valuestring : "?");
    }
    cJSON_Delete(j);
}

/* Resume each resource from the rev we hold; one we never had gets a reset. `via` is the link the hello goes over:
   on a WebSocket connect that is the socket itself, whichever link is active. */
static void send_hello(int via, TickType_t to)
{
    char revs[128] = "";
    for (int i = 0; i < N_RES; i++) {
        if (s_res[i].rev < 0) continue;
        size_t o = strlen(revs);
        snprintf(revs + o, sizeof revs - o, "%s\"%s\":%d", o ? "," : "", s_res[i].name, s_res[i].rev);
    }
    /* the device commands it can run (commands.c) ride in the hello, re-sent at every pairing */
    enum { BUF = 2048 };
    char *buf = psram_malloc(BUF);
    if (!buf) { ESP_LOGE(TAG, "hello: no memory"); return; }
    int o = snprintf(buf, BUF, "{\"type\":\"hello\",\"token\":\"%s\",\"cursor\":0,\"client\":\"%s\","
                     "\"protocol\":2,\"revs\":{%s},", s_cfg.token, via == L_PHONE ? "compact-fw/0.4+ble" : "compact-fw/0.4", revs);
    o += (int)commands_ops_json(buf + o, BUF - o - 2);
    buf[o++] = '}';
    buf[o] = 0;
    if (via == L_PHONE) ble_link_send(buf, o, to);
    else ws_send_text(buf, o, to);
    free(buf);
}

/* The Wi-Fi socket is gone (or being dropped): it is no longer paired, and no longer the active link. */
static void ws_down(void)
{
    xEventGroupClearBits(s_eg, BIT_WS);
    s_ws_paired = false;
    if (s_active == L_WIFI) set_active(L_NONE);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        wstatus("Connected");
        s_up_at = esp_timer_get_time();
        xEventGroupSetBits(s_eg, BIT_WS);
        send_hello(L_WIFI, portMAX_DELAY);              /* Wi-Fi becomes the active link at its welcome */
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        ws_down();
        wstatus("Disconnected");
        break;
    /* A clean close from the bus (a bus-core restart sends one) is CLOSED, not DISCONNECTED. Missing it
       left the connected bit set while the client task had stopped, so neither the supervisor nor the
       watchdog ever reconnected (Nov 8 and Nov 9). */
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH:
        ws_down();
        wstatus(id == WEBSOCKET_EVENT_CLOSED ? "Closed by bus" : "Client stopped");
        break;
    case WEBSOCKET_EVENT_DATA: {
        /* Binary frames are spoken-reply PCM: handed on piece by piece (it is a byte stream, no reassembly). */
        static bool rx_binary;
        if (e->payload_offset == 0 && (e->op_code == 0x1 || e->op_code == 0x2)) rx_binary = e->op_code == 0x2;
        if ((e->op_code == 0x2 || (e->op_code == 0x0 && e->payload_offset > 0 && rx_binary)) && e->data_len > 0) {
            if (s_active != L_WIFI) drop_inactive(L_WIFI, "speech audio");
            else if (s_cbs.speech_data) s_cbs.speech_data((const uint8_t *)e->data_ptr, e->data_len);
            break;
        }
        /* frames larger than the client buffer arrive in pieces: reassemble by payload_offset */
        if ((e->op_code == 0x1 || (e->op_code == 0x0 && e->payload_offset > 0)) && e->data_len > 0) {
            size_t total = e->payload_len;
            if (total + 1 > s_rx_cap) {
                char *n = heap_caps_realloc(s_rx, total + 1, MALLOC_CAP_SPIRAM);
                if (!n) { ESP_LOGE(TAG, "frame of %u bytes too large", (unsigned)total); break; }
                s_rx = n; s_rx_cap = total + 1;
            }
            if (e->payload_offset + e->data_len > total) break;
            memcpy(s_rx + e->payload_offset, e->data_ptr, e->data_len);
            if (e->payload_offset + e->data_len == total) handle_frame(s_rx, total, L_WIFI);
        }
        break;
    }
    default: break;
    }
}

/* ---- networks: a primary (home) and an optional secondary (phone hotspot) ----
 *
 * One task (roam_task) owns every connect, scan and switch; the Wi-Fi event handler only records what
 * happened and wakes it. States:
 *   CONNECTING  trying slot s_slot. Reconnects are paced (1 s apart); a disconnect while online first retries
 *               the same network. After ROAM_FAIL_S with at least 2 failed attempts the network is given up on:
 *               scan for the other slot's SSID; if visible, switch to it; if not, back off (5, 10, 20, 30 s)
 *               and retry the preferred (primary) network.
 *   ONLINE      GOT_IP. On the secondary, a directed scan for the primary every RECHECK_S (doubling to
 *               RECHECK_MAX_S after a return that did not stick); when it is seen at RETURN_MIN_RSSI or better,
 *               move back. Nothing happens while on the primary.
 * The bus task re-resolves the bus after any GOT_IP on a different network or address (s_epoch). */

#define ROAM_FAIL_S     18
#define RECHECK_S       180
#define RECHECK_MAX_S   900
#define BACKOFF_MAX_S   30
#define RETURN_MIN_RSSI (-78)
#define NO_IP_RESTART_US (2LL * 3600 * 1000000)   /* wedge insurance only: searching this long restarts the chip */

enum { R_CONNECTING, R_ONLINE };
static volatile int s_state = R_CONNECTING;
static volatile int s_slot;                       /* 1 or 2: the network in use or being tried; 0 none */
static volatile int64_t s_since, s_next_connect, s_last_connect, s_next_recheck, s_no_ip_since, s_apply_at, s_roam_beat;
static volatile int s_fails, s_backoff_s = 5, s_recheck_s = RECHECK_S, s_changed;
static volatile bool s_need_connect;
static volatile bool s_switch_req;                /* the settings screen asked for the other network */
static volatile bool s_pinned;                    /* on the secondary by choice: no automatic return home */
static volatile int s_switch_state;               /* 0 none, 1 looking, 2 switched, 3 not in range */
static char s_air[3][33];                         /* per slot: the on-air SSID when it differs from the stored one */
static volatile int s_epoch;                     /* bumped when GOT_IP lands on a different network or address */
static uint32_t s_last_ip;
static int s_last_slot;
static volatile int s_bus_mode;                   /* 0 none, 1 lan, 2 remote: while the socket is connected */
static TaskHandle_t s_roam;
static SemaphoreHandle_t s_mu;                    /* guards the credential and remote URL fields of s_cfg */

static bool slot_cfg(int slot, char *ssid, char *pass)    /* copies out; false when that slot has no SSID */
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const char *a = slot == 1 ? s_cfg.ssid : s_cfg.ssid2, *b = slot == 1 ? s_cfg.pass : s_cfg.pass2;
    if (ssid) strlcpy(ssid, a, 33);
    if (pass) strlcpy(pass, b, 65);
    bool ok = a[0] != 0;
    xSemaphoreGive(s_mu);
    return ok;
}
static int preferred(void) { return slot_cfg(1, NULL, NULL) ? 1 : slot_cfg(2, NULL, NULL) ? 2 : 0; }
static int other_slot(int s) { return s == 1 ? 2 : 1; }

static void apply_cfg(int slot)
{
    wifi_config_t w = {0};
    char ssid[33], pass[65];
    if (!slot_cfg(slot, ssid, pass)) return;
    if (s_air[slot][0]) strlcpy(ssid, s_air[slot], sizeof ssid);   /* the apostrophe form a scan found */
    strlcpy((char *)w.sta.ssid, ssid, sizeof w.sta.ssid);
    strlcpy((char *)w.sta.password, pass, sizeof w.sta.password);
    w.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    w.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    w.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;            /* WPA3 (and WPA2/WPA3 transition) hotspots */
    w.sta.pmf_cfg.capable = true;
    w.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &w);
}

static void begin_slot(int slot)
{
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "joining the %s network", slot == 1 ? "primary" : "secondary");
    if (slot == 1) s_pinned = false;
    esp_wifi_disconnect();
    apply_cfg(slot);
    s_slot = slot;
    s_state = R_CONNECTING;
    s_since = now;
    s_fails = 0;
    s_need_connect = true;
    s_next_connect = now + 300000;
}

/* A directed active scan for one SSID: the best RSSI seen, or -127 when it is not there. */
static int scan_rssi(const char *ssid)
{
    wifi_scan_config_t sc = { .ssid = (uint8_t *)ssid, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time = { .active = { .min = 60, .max = 120 } } };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -127;
    wifi_ap_record_t recs[8];
    uint16_t n = 8;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) { esp_wifi_clear_ap_list(); return -127; }
    int best = -127;
    for (int i = 0; i < n; i++)
        if (!strcmp((const char *)recs[i].ssid, ssid) && recs[i].rssi > best) best = recs[i].rssi;
    return best;
}

/* iPhones name their hotspot with a curly apostrophe (U+2019, "Alex’s iPhone"); a typed SSID usually has a
   straight one. Writes the name with every apostrophe in the other form; false when there is none (or it
   would not fit in 32 bytes). */
static bool apostrophe_swap(const char *in, char *out, size_t n)
{
    size_t o = 0;
    bool any = false;
    for (const unsigned char *p = (const unsigned char *)in; *p; ) {
        if (p[0] == '\'') {
            if (o + 3 >= n) return false;
            memcpy(out + o, "\xE2\x80\x99", 3); o += 3; p++; any = true;
        } else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) {
            if (o + 1 >= n) return false;
            out[o++] = '\''; p += 3; any = true;
        } else {
            if (o + 1 >= n) return false;
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return any;
}

/* A directed scan for a slot's network, under its stored name and then the other apostrophe form. The form
   that answers is remembered for apply_cfg. The best RSSI, or -127 when neither is there. */
static int scan_slot(int slot)
{
    char ssid[33], alt[33];
    if (!slot_cfg(slot, ssid, NULL)) return -127;
    int r = scan_rssi(ssid);
    if (r > -127) { s_air[slot][0] = 0; return r; }
    if (apostrophe_swap(ssid, alt, sizeof alt) && (r = scan_rssi(alt)) > -127) {
        if (strcmp(s_air[slot], alt)) ESP_LOGI(TAG, "slot %d answers under the other apostrophe form", slot);
        strlcpy(s_air[slot], alt, sizeof s_air[slot]);
    }
    return r;
}

static void search_failed(void)
{
    int cur = s_slot, alt = other_slot(cur);
    ESP_LOGW(TAG, "the %s network did not come up", cur == 1 ? "primary" : "secondary");
    if (cur == 2) s_pinned = false;                             /* the chosen hotspot is gone: home may take over */
    esp_wifi_disconnect();                                      /* stop connect attempts so a scan can run */
    if (slot_cfg(alt, NULL, NULL)) {
        int r = scan_slot(alt);
        ESP_LOGI(TAG, "scan for the %s network: %s", alt == 1 ? "primary" : "secondary", r > -127 ? "visible" : "not seen");
        if (r > -127) { begin_slot(alt); return; }
    }
    int pref = preferred();                                     /* nothing else visible: back off, then the preferred again */
    int64_t now = esp_timer_get_time();
    if (!pref) { s_slot = 0; return; }
    apply_cfg(pref);
    s_slot = pref;
    s_since = now;
    s_fails = 0;
    s_need_connect = true;
    s_next_connect = now + (int64_t)s_backoff_s * 1000000;
    ESP_LOGI(TAG, "no known network reachable; retrying in %d s", s_backoff_s);
    s_backoff_s = s_backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : s_backoff_s * 2;
    wstatus("Searching");
}

static void recheck_primary(void)
{
    int64_t now = esp_timer_get_time();
    if (!slot_cfg(1, NULL, NULL)) { s_next_recheck = now + (int64_t)RECHECK_MAX_S * 1000000; return; }
    int r = scan_slot(1);
    if (r >= RETURN_MIN_RSSI) {
        ESP_LOGI(TAG, "primary network is back (%d dBm): moving over", r);
        s_recheck_s = s_recheck_s * 2 > RECHECK_MAX_S ? RECHECK_MAX_S : s_recheck_s * 2;   /* undone once it sticks */
        s_next_recheck = now + (int64_t)s_recheck_s * 1000000;
        begin_slot(1);
    } else {
        s_next_recheck = now + (int64_t)s_recheck_s * 1000000;
    }
}

/* Held on the settings screen: move to the other configured network now, if a scan sees it. A move to the
   secondary is pinned (no return home every RECHECK_S) until it drops or the primary is chosen again. */
static void manual_switch(void)
{
    int cur = s_slot ? s_slot : 1, alt = other_slot(cur);
    if (!slot_cfg(alt, NULL, NULL)) { s_switch_state = 3; return; }
    bool online = s_state == R_ONLINE;
    if (!online) esp_wifi_disconnect();                         /* a scan cannot run during connect attempts */
    int r = scan_slot(alt);
    ESP_LOGI(TAG, "switch to the %s network: %s", alt == 1 ? "primary" : "secondary", r > -127 ? "visible" : "not seen");
    if (r > -127) {
        begin_slot(alt);
        s_pinned = alt == 2;
        if (alt == 1) s_recheck_s = RECHECK_S;
        s_backoff_s = 5;
        s_switch_state = 2;
    } else {
        s_switch_state = 3;
        if (!online) { s_need_connect = true; s_next_connect = esp_timer_get_time(); }
    }
}

static void roam_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();
        s_roam_beat = now;
        if (s_apply_at && now >= s_apply_at) {                  /* a credential changed over HTTP */
            int changed = s_changed;
            s_apply_at = 0; s_changed = 0;
            if (changed & (1 << 2)) { s_air[2][0] = 0; s_pinned = false; }
            int pref = preferred();
            if (!pref) { esp_wifi_disconnect(); s_slot = 0; s_state = R_CONNECTING; s_no_ip_since = now; }
            else if (!s_slot || (changed & (1 << s_slot)) || !slot_cfg(s_slot, NULL, NULL)) { begin_slot(pref); s_backoff_s = 5; }
            else if (s_state == R_ONLINE && s_slot == 2) s_next_recheck = 0;    /* primary may have changed: look now */
            continue;
        }
        if (s_switch_req) { s_switch_req = false; manual_switch(); continue; }
        if (s_state == R_ONLINE) {
            if (s_slot == 2 && !s_pinned && now >= s_next_recheck) recheck_primary();
            continue;
        }
        if (!s_slot) {                                          /* nothing configured yet */
            int pref = preferred();
            if (pref) begin_slot(pref);
            continue;
        }
        if (now - s_since > (int64_t)ROAM_FAIL_S * 1000000 && s_fails >= 2) { search_failed(); continue; }
        if (s_need_connect && now >= s_next_connect) {
            s_need_connect = false;
            s_last_connect = now;
            esp_wifi_connect();
        } else if (!s_need_connect && now - s_last_connect > 15LL * 1000000) {
            s_need_connect = true;                              /* no event came back: ask again */
            s_next_connect = now;
        }
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    int64_t now = esp_timer_get_time();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_need_connect = true;
        s_next_connect = 0;
        if (s_roam) xTaskNotifyGive(s_roam);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        int reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
        xEventGroupClearBits(s_eg, BIT_IP);
        if (s_state == R_ONLINE) { s_state = R_CONNECTING; s_since = now; s_fails = 0; s_no_ip_since = now; }
        else if (reason != WIFI_REASON_ASSOC_LEAVE) s_fails++;   /* our own disconnect is not a failure */
        if (reason != WIFI_REASON_ASSOC_LEAVE) s_next_connect = now + 1000000;   /* paced reconnects */
        s_need_connect = true;
        ESP_LOGW(TAG, "Wi-Fi lost (reason %d)", reason);
        wstatus("Wi-Fi lost");
        if (s_roam) xTaskNotifyGive(s_roam);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        uint32_t ip = data ? ((ip_event_got_ip_t *)data)->ip_info.ip.addr : 0;
        s_state = R_ONLINE;
        s_fails = 0;
        s_backoff_s = 5;
        if (s_slot == 1) s_recheck_s = RECHECK_S;                /* the return stuck */
        else s_next_recheck = now + (int64_t)s_recheck_s * 1000000;
        if (ip != s_last_ip || s_slot != s_last_slot) s_epoch++;   /* new network: the bus link must be re-resolved */
        s_last_ip = ip;
        s_last_slot = s_slot;
        wstatus("Wi-Fi up");
        if (s_cbs.net_up) s_cbs.net_up();
        xEventGroupSetBits(s_eg, BIT_IP);
    }
}

/* ---- the bus address: LAN by mDNS, or the optional remote wss:// URL ---- */

/* Resolve the bus by mDNS each time we (re)connect, so a new DHCP address does not strand us. */
static bool resolve_bus(char *ip, size_t n)
{
    esp_ip4_addr_t a = {0};
    if (s_cfg.bus_host[0] && mdns_query_a(s_cfg.bus_host, 3000, &a) == ESP_OK && a.addr) {
        snprintf(ip, n, IPSTR, IP2STR(&a));
        ESP_LOGI(TAG, "%s.local -> %s", s_cfg.bus_host, ip);
        return true;
    }
    if (s_cfg.bus_ip[0]) {
        ESP_LOGW(TAG, "mDNS lookup of %s failed; using fallback %s", s_cfg.bus_host, s_cfg.bus_ip);
        strlcpy(ip, s_cfg.bus_ip, n);
        return true;
    }
    return false;
}

/* wss://host[:port]/path -> https://host[:port] (the Surface API's HTTP base for the same bus) */
static bool remote_base(const char *url, char *base, size_t n)
{
    if (strncmp(url, "wss://", 6)) return false;
    const char *h = url + 6, *e = strchr(h, '/');
    size_t len = e ? (size_t)(e - h) : strlen(h);
    if (len == 0 || len + 9 > n) return false;
    snprintf(base, n, "https://%.*s", (int)len, h);
    return true;
}

static bool s_use_remote;

static void bus_task(void *arg)
{
    int seen_epoch = -1;
    for (;;) {
        s_phase = "wait_ip";
        s_beat = esp_timer_get_time();
        xEventGroupWaitBits(s_eg, BIT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
        int epoch = s_epoch;
        char url[sizeof s_cfg.remote_url], ip[16], uri[200];
        xSemaphoreTake(s_mu, portMAX_DELAY);
        strlcpy(url, s_cfg.remote_url, sizeof url);
        xSemaphoreGive(s_mu);
        bool remote_ok = url[0] != 0;
        if (epoch != seen_epoch) {            /* a new network: the secondary has no LAN bus, so go remote first there */
            seen_epoch = epoch;
            s_use_remote = remote_ok && s_slot == 2;
        }
        if (s_use_remote && !remote_ok) s_use_remote = false;
        s_phase = "resolve";
        s_beat = esp_timer_get_time();
        int mode = 1;
        if (!s_use_remote && !resolve_bus(ip, sizeof ip)) {
            if (remote_ok) { s_use_remote = true; continue; }    /* mDNS and the fallback IP both failed */
            wstatus("Bus not found");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        esp_websocket_client_config_t cfg = {
            .reconnect_timeout_ms = 5000,
            .network_timeout_ms = 10000,
            .buffer_size = 4096,
            .enable_close_reconnect = true,   /* after the bus closes cleanly, reconnect like after a drop */
            /* Frames are handled on this task (handle_frame -> resource callbacks -> LVGL layout and tiny_ttf glyph
               metrics under the display lock). The 4 KB default left 988 bytes free right after boot on 6ce7852
               (/info stack_free_min), the likely cause of the intermittent panics, push-to-talk replies among them. */
            .task_stack = 8192,
        };
        if (s_use_remote && remote_base(url, s_bus_base, sizeof s_bus_base)) {
            mode = 2;
            cfg.uri = url;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            ESP_LOGI(TAG, "using the remote bus");
        } else {
            mode = 1;
            if (s_use_remote) { s_use_remote = false; continue; }   /* a malformed URL: ignore it */
            snprintf(uri, sizeof uri, "ws://%s:%u/api/v1/intercom/ws", ip, s_cfg.bus_port);
            snprintf(s_bus_base, sizeof s_bus_base, "http://%s:%u", ip, s_cfg.bus_port);
            cfg.uri = uri;
        }
        s_phase = "start";
        s_ws = esp_websocket_client_init(&cfg);
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        s_bus_mode = 0;
        esp_websocket_client_start(s_ws);
        /* supervise: if the link stays down for 30 s, tear down and re-resolve; so does a network change */
        int64_t down_since = esp_timer_get_time();
        bool was_up = false;
        for (;;) {
            s_phase = "supervise";
            s_beat = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (s_resync_ws) { s_resync_ws = false; for (int i = 0; i < N_RES; i++) s_res[i].rev = -1; ESP_LOGW(TAG, "reconnecting for a fresh reset"); break; }
            if (s_epoch != epoch) { ESP_LOGW(TAG, "network changed: re-resolving the bus"); break; }
            if (s_ws_kick) { s_ws_kick = false; ESP_LOGW(TAG, "the phone is the link: dropping the Wi-Fi socket (a return starts fresh)"); break; }
            if (xEventGroupGetBits(s_eg) & BIT_WS) { down_since = esp_timer_get_time(); was_up = true; s_bus_mode = mode; continue; }
            if (!(xEventGroupGetBits(s_eg) & BIT_IP)) { down_since = esp_timer_get_time(); continue; }   /* Wi-Fi is the problem, not the bus */
            if (esp_timer_get_time() - down_since > STALL_US) {
                if (!was_up && remote_ok) s_use_remote = !s_use_remote;   /* never connected: try the other route next */
                break;
            }
        }
        ESP_LOGW(TAG, "restarting bus link");
        s_link_restarts++;
        s_bus_mode = 0;
        ws_down();                                       /* no sends to a socket being torn down */
        s_phase = "stop";
        s_beat = esp_timer_get_time();
        esp_websocket_client_stop(s_ws);
        s_phase = "destroy";
        esp_websocket_client_handle_t old = s_ws;
        s_ws = NULL;
        esp_websocket_client_destroy(old);
        ws_down();
    }
}

/* Last resort: no active link (Wi-Fi socket or phone) for 5 min with Wi-Fi up, or the bus task stopped looping
   for 2 min (stuck in a socket call). Restart the chip; the bus resends everything on the next hello.
   Not a reason to restart: being out of range of every known network. The roaming task keeps searching
   with backoff; only a 2 h stretch without any IP restarts (a wedged radio looks the same), and not while the
   phone is the active link (the 2 h then count from when it stopped being so). */
static void link_watchdog(void *arg)
{
    int64_t now = esp_timer_get_time();
    bool wifi = xEventGroupGetBits(s_eg) & BIT_IP;
    if (s_active == L_PHONE) s_phone_active_at = now;
    if (link_up()) { s_last_active = now; return; }
    if (wifi && s_beat && now - s_beat > 120LL * 1000000) {
        ESP_LOGE(TAG, "bus task stuck in %s for %llds: restarting", s_phase, (long long)((now - s_beat) / 1000000));
        compact_restart("bus task stuck");
        return;
    }
    int64_t quiet = s_link_at > s_last_active ? s_link_at : s_last_active;     /* last link change, or last active link */
    if (wifi && quiet && now - quiet > 300LL * 1000000 && now > 600LL * 1000000) {
        ESP_LOGE(TAG, "no bus link for 5 min (%s): restarting", s_phase);
        compact_restart("no bus link for 5 min");
        return;
    }
    if (wifi && quiet && now - quiet > 1800LL * 1000000) {
        ESP_LOGE(TAG, "no bus link for 30 min: restarting");
        compact_restart("no bus link for 30 min");
        return;
    }
    int64_t no_ip = s_no_ip_since > s_phone_active_at ? s_no_ip_since : s_phone_active_at;
    if (!wifi && s_no_ip_since && now - no_ip > NO_IP_RESTART_US) {
        ESP_LOGE(TAG, "no Wi-Fi for 2 h: restarting");
        compact_restart("no Wi-Fi for 2 h");
        return;
    }
}

void intercom_net_info(intercom_net_t *o)
{
    memset(o, 0, sizeof *o);
    o->slot = s_slot;
    if (o->slot) slot_cfg(o->slot, o->ssid, NULL);
    o->online = s_eg && (xEventGroupGetBits(s_eg) & BIT_IP);
    wifi_ap_record_t ap;
    if (s_phone_only) o->online = false;
    if (o->online && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) o->rssi = ap.rssi;
    o->bus_via = !intercom_connected() ? "none" : s_active == L_PHONE ? "phone" : s_bus_mode == 2 ? "remote" : "lan";
    o->roam = !s_slot ? "idle" : s_state == R_ONLINE ? "online" : "searching";
    xSemaphoreTake(s_mu, portMAX_DELAY);
    o->remote_set = s_cfg.remote_url[0] != 0;
    o->other_set = (o->slot == 2 ? s_cfg.ssid : s_cfg.ssid2)[0] != 0;
    xSemaphoreGive(s_mu);
    o->switch_state = s_switch_state;
    o->pinned = s_pinned;
}

bool intercom_net_switch(void)
{
    int cur = s_slot ? s_slot : 1;
    if (!slot_cfg(other_slot(cur), NULL, NULL)) return false;
    s_switch_state = 1;
    s_switch_req = true;
    if (s_roam) xTaskNotifyGive(s_roam);
    return true;
}

bool intercom_wifi_set(int slot, const char *ssid, const char *pass, bool clear)
{
    if (slot != 2) return false;
    if (!clear && (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64))) return false;
    bool ok = clear ? (settings_save_str("ssid2", NULL) && settings_save_str("pass2", NULL))
                    : (settings_save_str("ssid2", ssid) && settings_save_str("pass2", pass));
    if (!ok) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strlcpy(s_cfg.ssid2, clear ? "" : ssid, sizeof s_cfg.ssid2);
    strlcpy(s_cfg.pass2, clear || !pass ? "" : pass, sizeof s_cfg.pass2);
    xSemaphoreGive(s_mu);
    s_changed |= 1 << 2;
    s_apply_at = esp_timer_get_time() + 800000;      /* let the HTTP answer out first */
    ESP_LOGI(TAG, "secondary network %s", clear ? "cleared" : "saved");
    if (s_roam) xTaskNotifyGive(s_roam);
    return true;
}

bool intercom_bus_set(const char *url, bool clear)
{
    char base[160];
    if (!clear && (!url || strlen(url) >= sizeof s_cfg.remote_url || !remote_base(url, base, sizeof base) || strpbrk(url, " \\\"'"))) return false;
    if (!settings_save_str("bus_url", clear ? NULL : url)) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strlcpy(s_cfg.remote_url, clear ? "" : url, sizeof s_cfg.remote_url);
    xSemaphoreGive(s_mu);
    s_epoch++;                                       /* re-run the bus choice */
    ESP_LOGI(TAG, "remote bus %s", clear ? "cleared" : "saved");
    return true;
}

/* ---- phone link (BLE) ----
 * The relay (the iOS app, or relay/index.html) sends control frames of its own: {"type":"relay_open","now":<epoch ms>}
 * once its socket to the bus is open (we then say hello, as on a WebSocket connect), {"type":"relay_closed"} when it
 * drops, and {"type":"relay_restart"} (restart the chip). Everything else is a bus frame. When a delta does not apply
 * we ask the relay to reconnect ({"type":"relay_reconnect"}) and resume from no revs, which gets a fresh reset.
 * To the relay: {"type":"relay_ping"}, a wake-up (every 15 s, every 1 s in an audio window), and in AUTO, once the
 * Wi-Fi socket is paired again, {"type":"relay_standby"}, after which we disconnect and stop advertising.
 * Audio both ways is binary frames, one IMA ADPCM block each (adpcm.h, docs/phone-link-v2.md s3). */

#define FAILOVER_US          (20LL * 1000000)       /* AUTO: no paired Wi-Fi socket this long -> look for the phone */
#define BLE_RETRY_US         (5LL * 60 * 1000000)   /* a failed NimBLE init is retried at most this often */
#define RELAY_PING_US        (15LL * 1000000)
#define RELAY_PING_AUDIO_US  (1LL * 1000000)

static void ble_connected(bool up);
static void ble_frame(const char *data, size_t len);
static void ble_binary(const uint8_t *data, size_t len);
static const ble_link_cbs_t s_ble_cbs = { .connected = ble_connected, .frame = ble_frame, .binary = ble_binary };

static volatile int64_t s_ble_retry_at;           /* AUTO: earliest next NimBLE init after a failure */
static volatile bool s_ble_failed;                /* the last NimBLE init failed (for /info) */

/* POST /debug/ui "ble-start": start NimBLE as the failover would, but stay on Wi-Fi and do not advertise. For RAM
   measurements (the controller is kept once started, as after a real failover). */
void intercom_request_resync(void)
{
    if (s_active == L_PHONE) s_resync_phone = true;   /* acted on after the next relay frame */
    else s_resync_ws = true;                          /* the supervise loop reconnects within 2 s */
}

void intercom_stop_for_restart(void)
{
    esp_websocket_client_handle_t ws = s_ws;
    ws_down();                                        /* no sends from here on */
    if (ws && esp_websocket_client_is_connected(ws)) esp_websocket_client_stop(ws);
}

bool intercom_debug_ble_start(void)
{
    if (s_phone_only) return ble_link_started();
    if (ble_link_started()) return true;
    s_ble_failed = !ble_link_start("Compact", &s_ble_cbs, false);
    if (s_ble_failed) s_ble_retry_at = esp_timer_get_time() + BLE_RETRY_US;   /* as a failover start does */
    return !s_ble_failed;
}


/* Safety net: on a PHONE-mode boot, 10 min without the bus (no phone, or the relay closed) restarts the chip, which
   comes back in AUTO (the link setting is one boot only). */
#define BLE_NO_BUS_RESTART_US (10LL * 60 * 1000000)
static volatile int64_t s_ble_down_since;
static void ble_watchdog(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (link_up()) { s_ble_down_since = now; return; }
    if (now - s_ble_down_since > BLE_NO_BUS_RESTART_US) {
        ESP_LOGE(TAG, "phone link: no bus for 10 min, restarting on Wi-Fi");
        compact_restart("phone link: no bus for 10 min");
    }
}

/* AUTO, Wi-Fi is the link again: tell the phone to stand by, give it up to 2 s to go out, then disconnect and stop
   advertising. The phone app keeps a pending reconnect for when we advertise again. Phone task only. */
static void phone_standby(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    ble_link_set_advertising(false);
    xEventGroupClearBits(s_eg, BIT_PHONE);
    if (s_active == L_PHONE) set_active(L_NONE);
    if (last && now - last < 3000000) return;             /* the disconnect is on its way */
    last = now;
    if (ble_link_ready()) {
        static const char sb[] = "{\"type\":\"relay_standby\"}";
        bool sent = ble_link_send(sb, sizeof sb - 1, pdMS_TO_TICKS(1500));
        if (sent) vTaskDelay(pdMS_TO_TICKS(400));         /* a few connection events for it to go out over the air */
        ESP_LOGI(TAG, "phone link: Wi-Fi is back, relay_standby %s; disconnecting", sent ? "sent" : "not sent");
    } else {
        ESP_LOGI(TAG, "phone link: Wi-Fi is back, standing by");
    }
    ble_link_disconnect();
}

/* The phone link's own task. AUTO: after FAILOVER_US without a paired Wi-Fi socket it starts NimBLE (first time
   only; kept afterwards) and advertises; once Wi-Fi pairs again it sends the phone to standby. Both modes: the
   relay_ping wake-ups. Created at the first failover (AUTO) or at boot (PHONE). */
static void phone_task(void *arg)
{
    int64_t last_ping = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        int64_t now = esp_timer_get_time();
        if (!s_phone_only) {
            int64_t bad = s_wifi_bad_since;
            if (wifi_ok() || s_wifi_paused) {                /* paused: no radio at all while asleep on low battery */
                if (ble_link_busy()) phone_standby();
            } else if (bad && now - bad >= FAILOVER_US) {
                if (!ble_link_started()) {
                    if (now >= s_ble_retry_at) {
                        ESP_LOGW(TAG, "no Wi-Fi bus link for %llds: starting the phone link", (long long)((now - bad) / 1000000));
                        s_ble_failed = !ble_link_start("Compact", &s_ble_cbs, true);
                        if (s_ble_failed) {
                            s_ble_retry_at = now + BLE_RETRY_US;
                            ESP_LOGE(TAG, "phone link unavailable (NimBLE init failed): Wi-Fi only, next try in 5 min");
                        }
                    }
                } else if (!ble_link_busy()) {
                    static int64_t said;                     /* until the host syncs this repeats: log it once */
                    if (now - said > 10000000) { ESP_LOGI(TAG, "no Wi-Fi bus link: advertising for the phone"); said = now; }
                    ble_link_set_advertising(true);
                }
            }
        }
        if (!ble_link_ready()) { last_ping = now; continue; }   /* the first ping comes one period after subscribing */
        if (now - last_ping >= (audio_window(now) ? RELAY_PING_AUDIO_US : RELAY_PING_US)) {
            static const char ping[] = "{\"type\":\"relay_ping\"}";
            ble_link_send(ping, sizeof ping - 1, pdMS_TO_TICKS(1000));
            last_ping = now;
        }
    }
}

/* AUTO: once a second, track how long Wi-Fi has not been usable (socket paired and an IP), and create the phone
   task the first time that lasts FAILOVER_US. Internal RAM for its stack and for NimBLE is spent only then. */
void intercom_wifi_paused(bool paused)
{
    s_wifi_paused = paused;
    if (paused) s_wifi_bad_since = 0;
    ESP_LOGI(TAG, "Wi-Fi %s by power saving: %s", paused ? "off" : "back on", paused ? "no phone failover" : "failover armed");
    if (s_phone_task) xTaskNotifyGive(s_phone_task);
}

static void failover_tick(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (wifi_ok() || s_wifi_paused) { s_wifi_bad_since = 0; return; }   /* a power-saving Wi-Fi stop is not a failure */
    if (!s_wifi_bad_since) s_wifi_bad_since = now;
    if (s_phone_task || now - s_wifi_bad_since < FAILOVER_US) return;
    static int64_t failed_at;
    if (failed_at && now - failed_at < BLE_RETRY_US) return;
    if (task_create_psram(phone_task, "phone_link", 4096, NULL, 4, &s_phone_task) != pdPASS) {
        s_phone_task = NULL;
        failed_at = now;
        ESP_LOGE(TAG, "no memory for the phone link task: Wi-Fi only, next try in 5 min");
    }
}

static void ble_connected(bool up)
{
    if (up) {
        if (!s_phone_only && wifi_ok()) { ESP_LOGI(TAG, "phone subscribed while Wi-Fi is the link: standby"); phone_task_poke(); return; }
        status_src(L_PHONE, "Phone linked");
        return;
    }
    xEventGroupClearBits(s_eg, BIT_PHONE);
    if (s_active == L_PHONE) { set_active(L_NONE); status(s_phone_only ? "Waiting for phone" : "Phone lost"); }
    else if (s_phone_only) status("Waiting for phone");
}

static void ble_frame(const char *data, size_t len)
{
    if (s_phone_only) s_beat = esp_timer_get_time();
    if (len < 256 && strstr(data, "\"relay_")) {
        cJSON *j = cJSON_ParseWithLength(data, len);
        const cJSON *t = j ? cJSON_GetObjectItem(j, "type") : NULL;
        if (cJSON_IsString(t) && !strncmp(t->valuestring, "relay_", 6)) {
            if (!strcmp(t->valuestring, "relay_open")) {
                const cJSON *now = cJSON_GetObjectItem(j, "now");
                if (cJSON_IsNumber(now) && now->valuedouble > 1.7e12) {   /* the phone's clock: no SNTP without Wi-Fi */
                    long long ms = (long long)now->valuedouble;
                    struct timeval tv = { .tv_sec = (time_t)(ms / 1000), .tv_usec = (suseconds_t)((ms % 1000) * 1000) };
                    settimeofday(&tv, NULL);
                }
                if (!s_phone_only && wifi_ok()) {                /* Wi-Fi paired meanwhile: it stays the link */
                    ESP_LOGI(TAG, "relay open while Wi-Fi is the link: standby");
                    phone_task_poke();
                } else {
                    s_up_at = esp_timer_get_time();
                    xEventGroupSetBits(s_eg, BIT_PHONE);
                    if (!s_phone_only && s_ws_paired) { s_ws_paired = false; s_ws_kick = true; }   /* a stale socket */
                    set_active(L_PHONE);
                    status("Connected");
                    send_hello(L_PHONE, pdMS_TO_TICKS(5000));
                }
            } else if (!strcmp(t->valuestring, "relay_restart")) {   /* from the relay: back to Wi-Fi */
                ESP_LOGW(TAG, "relay asked for a restart");
                vTaskDelay(pdMS_TO_TICKS(300));
                compact_restart("relay asked for a restart");
            } else if (!strcmp(t->valuestring, "relay_closed")) {
                xEventGroupClearBits(s_eg, BIT_PHONE);
                if (s_active == L_PHONE) { set_active(L_NONE); status("Phone: no bus"); }
            }
            cJSON_Delete(j);
            return;
        }
        cJSON_Delete(j);
    }
    handle_frame(data, len, L_PHONE);
    if (s_resync_phone) {
        s_resync_phone = false;
        for (int i = 0; i < N_RES; i++) s_res[i].rev = -1;
        ESP_LOGW(TAG, "asking the relay to reconnect for a fresh reset");
        static const char rc[] = "{\"type\":\"relay_reconnect\"}";
        ble_link_send(rc, sizeof rc - 1, pdMS_TO_TICKS(2000));
    }
}

/* Spoken-reply audio from the relay: decode each block on ble_rx, in order with the text frames. */
static void ble_binary(const uint8_t *data, size_t len)
{
    if (s_active != L_PHONE) { drop_inactive(L_PHONE, "audio"); return; }
    int16_t pcm[ADPCM_MAX_SAMPLES];
    int n = adpcm_decode_block(data, len, pcm, ADPCM_MAX_SAMPLES);
    if (n < 0) {
        static int64_t last;
        int64_t now = esp_timer_get_time();
        if (now - last > 5000000) { ESP_LOGW(TAG, "phone link: bad audio block (%u bytes, codec 0x%02x)", (unsigned)len, len ? data[0] : 0); last = now; }
        return;
    }
    if (s_cbs.speech_data) s_cbs.speech_data((const uint8_t *)pcm, (size_t)n * sizeof pcm[0]);
}

void intercom_ble_info(char *out, size_t n)
{
    if (!ble_link_started() && s_ble_failed) snprintf(out, n, "init failed, retry in %llds",
                                                     (long long)((s_ble_retry_at - esp_timer_get_time()) / 1000000));
    else ble_link_info(out, n);
}

void intercom_start(const settings_t *cfg, const intercom_cbs_t *cbs)
{
    s_cfg = *cfg;
    s_cbs = *cbs;
    s_mu = xSemaphoreCreateMutex();
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    s_eg = xEventGroupCreate();
    s_phone_only = cfg->ble_link;
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);       /* every boot: an AUTO boot away from Wi-Fi fails over to the */
    tzset();                                         /* phone without ever reaching dial_net_up, and showed UTC (Nov 19) */
    if (s_phone_only) {                              /* PHONE: Bluetooth only, no Wi-Fi this boot */
        esp_event_loop_create_default();
        status("Waiting for phone");
        if (!ble_link_start("Compact", &s_ble_cbs, true)) { s_ble_failed = true; ESP_LOGE(TAG, "phone link: NimBLE init failed"); }
        s_ble_down_since = esp_timer_get_time();
        task_create_psram(phone_task, "phone_link", 4096, NULL, 4, &s_phone_task);
        const esp_timer_create_args_t wd = { .callback = ble_watchdog, .name = "ble_wd" };
        esp_timer_handle_t h;
        if (esp_timer_create(&wd, &h) == ESP_OK) esp_timer_start_periodic(h, 30LL * 1000000);
        return;
    }
    /* AUTO: Bluetooth's memory is kept (no esp_bt_mem_release), so the phone link can start on the first failover. */
#if CONFIG_IDF_TARGET_ESP32S3
    {
        const ets_rom_layout_t *r = ets_rom_layout_p;
        size_t kept = ((char *)r->data_end_btdm - (char *)r->data_start_btdm) + ((char *)r->bss_end_btdm - (char *)r->bss_start_btdm) +
                      ((char *)r->data_end_interface_btdm - (char *)r->data_start_interface_btdm) +
                      ((char *)r->bss_end_interface_btdm - (char *)r->bss_start_interface_btdm);
        ESP_LOGI(TAG, "link AUTO: Bluetooth ROM data/BSS kept for the phone link (%u bytes, not returned to the heap)", (unsigned)kept);
    }
#endif
    memdiag_mark("before Wi-Fi");
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    memdiag_mark("after esp_wifi_init");
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);

    mdns_init();
    mdns_hostname_set(CONFIG_INTERCOM_HOSTNAME);
    mdns_instance_name_set("The Compact");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    memdiag_mark("after mdns");

    esp_wifi_set_mode(WIFI_MODE_STA);
    s_slot = preferred();
    if (s_slot) apply_cfg(s_slot);
    s_since = s_no_ip_since = s_wifi_bad_since = esp_timer_get_time();
    status("Connecting");
    esp_wifi_start();
    memdiag_mark("after esp_wifi_start");
    task_create_psram(roam_task, "roam", 5120, NULL, 5, &s_roam);
    task_create_psram(bus_task, "bus", 6144, NULL, 5, NULL);
    const esp_timer_create_args_t wd = { .callback = link_watchdog, .name = "link_wd" };
    esp_timer_handle_t h;
    if (esp_timer_create(&wd, &h) == ESP_OK) esp_timer_start_periodic(h, 30LL * 1000000);
    const esp_timer_create_args_t fo = { .callback = failover_tick, .name = "failover" };
    if (esp_timer_create(&fo, &h) == ESP_OK) esp_timer_start_periodic(h, 1000000);
}

/* `volume` tells the bus whether to speak replies at all (0 = muted). The real value over either link: the phone
   link carries spoken replies too (v2). */
static void state_json(char *buf, size_t n, bool quiet, int battery_pct)
{
    char bat[12] = "null";
    if (battery_pct >= 0) snprintf(bat, sizeof bat, "%d", battery_pct);
    snprintf(buf, n, "{\"type\":\"state\",\"quiet\":%s,\"battery\":%s,\"volume\":%d}", quiet ? "true" : "false", bat, prefs_volume());
}

void intercom_send_state(bool quiet, int battery_pct)
{
    if (!link_up()) return;
    char buf[112];
    state_json(buf, sizeof buf, quiet, battery_pct);
    link_send(buf, strlen(buf), pdMS_TO_TICKS(1000));
}

/* ---- actions: queued and sent from their own task ----
 * A tap arrives on the LVGL task with the display lock held. Sending from there can deadlock for the
 * send timeout: the socket task holds its client lock while it hands a frame to us, and our frame
 * handlers take the display lock. So the UI only queues; this task sends. */

enum { REQ_ACTION, REQ_NOTICE_ACK, REQ_VIEW, REQ_RAW, REQ_RAW_HEAP };   /* REQ_RAW_HEAP: the frame is in `draft` */
typedef struct { int kind; char object[160], action_id[40], key[48], message_id[64]; char *draft; } act_req_t;
static QueueHandle_t s_txq;

static bool send_action_now(const act_req_t *r)
{
    if (!link_up()) { snprintf(s_last_action, sizeof s_last_action, "not sent: socket down"); return false; }
    if (r->kind == REQ_RAW) return link_send(r->object, strlen(r->object), pdMS_TO_TICKS(3000)) >= 0;
    if (r->kind == REQ_RAW_HEAP) return r->draft && link_send(r->draft, strlen(r->draft), pdMS_TO_TICKS(3000)) >= 0;
    if (r->kind == REQ_VIEW) {                                    /* what the glass shows (`view`) */
        char buf[200];
        if (r->object[0]) snprintf(buf, sizeof buf, "{\"type\":\"view\",\"view\":\"%.150s\"}", r->object);
        else snprintf(buf, sizeof buf, "{\"type\":\"view\",\"view\":null}");
        return link_send(buf, strlen(buf), pdMS_TO_TICKS(3000)) >= 0;
    }
    if (r->kind == REQ_NOTICE_ACK) {                              /* GOT IT on an E89 notice */
        char buf[200];
        snprintf(buf, sizeof buf, "{\"type\":\"notification_ack\",\"id\":\"%.150s\"}", r->object);
        return link_send(buf, strlen(buf), pdMS_TO_TICKS(3000)) >= 0;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "action");
    cJSON_AddStringToObject(j, "object", r->object);
    cJSON_AddStringToObject(j, "action_id", r->action_id);
    cJSON_AddStringToObject(j, "idempotency_key", r->key);
    if (r->message_id[0]) cJSON_AddStringToObject(j, "message_id", r->message_id);
    if (r->draft) cJSON_AddStringToObject(j, "draft", r->draft);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return false;
    int n = link_send(txt, strlen(txt), pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "action %s/%s key %s: %s", r->object, r->action_id, r->key, n >= 0 ? "sent" : "send failed");
    snprintf(s_last_action, sizeof s_last_action, "%s %.60s/%.32s (%d bytes)", n >= 0 ? "sent" : "send failed", r->object, r->action_id, n);
    cJSON_free(txt);
    return n >= 0;
}

static void tx_task(void *arg)
{
    for (;;) {
        act_req_t *r;
        if (xQueueReceive(s_txq, &r, portMAX_DELAY) != pdTRUE) continue;
        if (!send_action_now(r) && r->kind == REQ_ACTION && r->key[0] && s_cbs.action_result) s_cbs.action_result(r->key, "failed", "not_sent");
        free(r->draft);
        free(r);
    }
}

static bool enqueue(int kind, const char *object, const char *action_id, const char *draft, const char *key, const char *message_id)
{
    if (!s_txq) {
        s_txq = xQueueCreate(6, sizeof(act_req_t *));
        task_create_psram(tx_task, "bus_tx", 4096, NULL, 5, NULL);
    }
    act_req_t *r = calloc(1, sizeof *r);                      /* cJSON-sized allocs land in PSRAM */
    if (!r) return false;
    r->kind = kind;
    snprintf(r->object, sizeof r->object, "%s", object);
    snprintf(r->action_id, sizeof r->action_id, "%s", action_id);
    snprintf(r->key, sizeof r->key, "%s", key);
    if (message_id) snprintf(r->message_id, sizeof r->message_id, "%s", message_id);
    if (draft) r->draft = strdup(draft);
    if (xQueueSend(s_txq, &r, 0) != pdTRUE) { free(r->draft); free(r); return false; }
    return true;
}

bool intercom_ack_notice(const char *id)
{
    return enqueue(REQ_NOTICE_ACK, id, "", NULL, "", NULL);
}

/* SKIP on a reply notification (E105 contract 3.3): notification_ack with skip. */
bool intercom_skip_notice(const char *id)
{
    char buf[224];                                        /* ids up to 150 bytes: too long for the request's object slot */
    if (reply_skip_json(buf, sizeof buf, id) < 0) return false;
    return enqueue(REQ_RAW_HEAP, "", "", buf, "", NULL);
}

/* An ordinary `send` (the late reply, contract 3.4). The answer is an `ack` with the same client_msg_id. */
bool intercom_send_text(const char *cmid, const char *body)
{
    char *f = reply_send_json(cmid, body);
    if (!f) return false;
    bool ok = enqueue(REQ_RAW_HEAP, "", "", f, "", NULL);
    cJSON_free(f);                                        /* enqueue keeps its own copy (strdup) */
    return ok;
}

bool intercom_send_action(const char *object, const char *action_id, const char *draft, const char *key)
{
    return enqueue(REQ_ACTION, object, action_id, draft, key, NULL);
}

bool intercom_send_conv_action(const char *conv_id, int index, const char *message_id, const char *key)
{
    char obj[160], idx[12];
    snprintf(obj, sizeof obj, "conversations:%s", conv_id);
    snprintf(idx, sizeof idx, "%d", index);
    return enqueue(REQ_ACTION, obj, idx, NULL, key, message_id);
}

static void queue_view(void) { enqueue(REQ_VIEW, s_view, "", NULL, "", NULL); }

void intercom_speech_stop(void)
{
    if (intercom_connected()) enqueue(REQ_RAW, "{\"type\":\"speech_stop\"}", "", NULL, "", NULL);
}

void intercom_queue_state(bool quiet, int battery_pct)
{
    char buf[112];
    state_json(buf, sizeof buf, quiet, battery_pct);
    if (intercom_connected()) enqueue(REQ_RAW, buf, "", NULL, "", NULL);
}

void intercom_set_view(const char *view)
{
    const char *v = view ? view : "";
    if (!strcmp(v, s_view)) return;
    strlcpy(s_view, v, sizeof s_view);
    if (intercom_connected()) queue_view();                   /* otherwise the next welcome sends it */
}

bool intercom_connected(void) { return link_up(); }

/* ---- Capture uploads: Wi-Fi only (the phone link's MTU and the relay are not sized for them) ---- */
bool intercom_capture_ready(void) { return s_active == L_WIFI && s_ws_paired && link_up(); }
uint32_t intercom_capture_gen(void) { return s_wifi_gen; }
bool intercom_capture_send(const char *json, size_t len)
{
    if (!intercom_capture_ready()) return false;
    esp_websocket_client_handle_t ws = s_ws;
    return ws && esp_websocket_client_send_text(ws, json, (int)len, pdMS_TO_TICKS(8000)) >= 0;
}

/* ---- the conversation page (GET /api/v1/surface/conversations/:id) on its own task ---- */

#define CONV_MAX_BYTES (96 * 1024)
#define CONV_FETCH_MAX 2          /* the Thread's page and an Agents question may run side by side */
typedef struct { char conv[65]; int limit; } fetch_req_t;

static void fetch_task(void *arg)
{
    fetch_req_t *rq = arg;
    char url[300], conv[65], auth[112];
    int limit = rq->limit;
    strlcpy(conv, rq->conv, sizeof conv);
    free(rq);
    snprintf(url, sizeof url, "%s/api/v1/surface/conversations/%s?limit=%d", s_bus_base, conv, limit);
    snprintf(auth, sizeof auth, "Bearer %s", s_cfg.token);
    int code = -1;
    cJSON *page = NULL;
    char *buf = heap_caps_malloc(CONV_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);
    esp_http_client_config_t hc = { .url = url, .timeout_ms = 8000, .buffer_size = 2048, .disable_auto_redirect = true,
                                  .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t c = buf ? esp_http_client_init(&hc) : NULL;
    if (c) {
        esp_http_client_set_header(c, "Authorization", auth);
        if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
            code = esp_http_client_get_status_code(c);
            int n = 0, r;
            while (n < CONV_MAX_BYTES && (r = esp_http_client_read(c, buf + n, CONV_MAX_BYTES - n)) > 0) n += r;
            buf[n] = 0;
            if (code == 200) page = cJSON_ParseWithLength(buf, n);
        }
        esp_http_client_cleanup(c);
    }
    free(buf);
    ESP_LOGI(TAG, "conversation fetch: HTTP %d, %s", code, page ? "parsed" : "no page");
    if (s_cbs.conversation) s_cbs.conversation(conv, page, code);
    cJSON_Delete(page);
    __atomic_fetch_sub(&s_fetching, 1, __ATOMIC_SEQ_CST);
    vTaskDeleteWithCaps(NULL);                         /* created in PSRAM by task_create_psram_only */
}

bool intercom_fetch_conversation_id(const char *conv_id, int limit)
{
    if (!conv_id || !conv_id[0] || strlen(conv_id) > 64 || !s_bus_base[0]) return false;
    if (__atomic_fetch_add(&s_fetching, 1, __ATOMIC_SEQ_CST) >= CONV_FETCH_MAX) { __atomic_fetch_sub(&s_fetching, 1, __ATOMIC_SEQ_CST); return false; }
    fetch_req_t *rq = calloc(1, sizeof *rq);
    if (rq) { strlcpy(rq->conv, conv_id, sizeof rq->conv); rq->limit = limit; }
    if (!rq || task_create_psram_only(fetch_task, "conv_get", 6144, rq, 4, NULL) != pdPASS) {
        free(rq);
        __atomic_fetch_sub(&s_fetching, 1, __ATOMIC_SEQ_CST);
        return false;
    }
    return true;
}

bool intercom_fetch_conversation(int limit) { return intercom_fetch_conversation_id(s_conv_id, limit); }

/* ---- push-to-talk audio ----
 * Over Wi-Fi: PCM in WebSocket binary frames, as before. Over the phone: the PCM is IMA ADPCM encoded here (on the
 * ptt task, the only caller) in blocks of 256 samples (fewer when the MTU is small: 2*(MTU-9)), one BINARY
 * notification each; the relay decodes them back to PCM for the bus. The encoder starts each stream at predictor
 * 0, index 0 and carries its state across blocks; audio_end first flushes the partial block. A stream stays on
 * the link it started on: if the active link changes mid-stream, the rest fails (ptt reports NOT SENT). */

static int s_voice_via = L_NONE;                  /* the link the open stream went out on */
static adpcm_state_t s_enc;
static int16_t *s_enc_pcm;                        /* the partial block (PSRAM) */
static size_t s_enc_n, s_enc_bs;                  /* samples buffered; block size for this stream */

static bool phone_block(const int16_t *pcm, size_t n)
{
    uint8_t blk[ADPCM_BLOCK_BYTES(ADPCM_MAX_SAMPLES)];
    size_t len = adpcm_encode_block(&s_enc, pcm, n, blk);
    return len && ble_link_send_binary(blk, len, pdMS_TO_TICKS(5000));
}

static bool voice_text(const char *type, const char *cmid, int rate, const char *nid)
{
    if (!intercom_connected()) return false;
    bool start = !strcmp(type, "audio_start"), end = !strcmp(type, "audio_end");
    if (start) {
        s_voice_via = s_active;
        adpcm_reset(&s_enc);
        s_enc_n = 0;
        if (s_voice_via == L_PHONE) {
            if (!s_enc_pcm) s_enc_pcm = heap_caps_malloc(ADPCM_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!s_enc_pcm) { ESP_LOGE(TAG, "no memory for the audio encoder"); return false; }
            size_t room = ble_link_mtu() > 9 ? (size_t)ble_link_mtu() - 3 - 1 - ADPCM_HDR : 1;
            s_enc_bs = 2 * room > ADPCM_MAX_SAMPLES ? ADPCM_MAX_SAMPLES : 2 * room;
            ESP_LOGI(TAG, "voice over the phone link: ADPCM blocks of %u samples", (unsigned)s_enc_bs);
        }
        audio_window_open();
    } else if (s_voice_via != s_active) {
        ESP_LOGW(TAG, "%s: the link changed during the stream", type);
        return false;
    }
    if (end && s_voice_via == L_PHONE && s_enc_n) {
        bool ok = phone_block(s_enc_pcm, s_enc_n);
        s_enc_n = 0;
        if (!ok) return false;
    }
    if (!start) { s_enc_n = 0; audio_window_tail(); }
    char buf[400];
    if (start && nid && nid[0]) {                         /* a reply bound to a notification (E105 contract 3.2) */
        if (reply_audio_start_json(buf, sizeof buf, cmid, rate, nid) < 0) { ESP_LOGE(TAG, "bound audio_start: bad id"); return false; }
        ESP_LOGI(TAG, "audio_start %s bound to notification %s", cmid, nid);
    } else if (rate) snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"client_msg_id\":\"%.60s\",\"rate\":%d,\"format\":\"pcm_s16le\"}", type, cmid, rate);
    else snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"client_msg_id\":\"%.60s\"}", type, cmid);
    return link_send(buf, strlen(buf), pdMS_TO_TICKS(2000)) >= 0;
}

bool intercom_voice_start(const char *cmid, int rate) { return voice_text("audio_start", cmid, rate, NULL); }
bool intercom_voice_start_bound(const char *cmid, int rate, const char *notification_id) { return voice_text("audio_start", cmid, rate, notification_id); }
bool intercom_voice_end(const char *cmid) { return voice_text("audio_end", cmid, 0, NULL); }
bool intercom_voice_cancel(const char *cmid) { return voice_text("audio_cancel", cmid, 0, NULL); }

bool intercom_voice_chunk(const int16_t *pcm, size_t samples)
{
    if (!intercom_connected() || s_voice_via != s_active) return false;
    if (s_voice_via == L_WIFI) {
        esp_websocket_client_handle_t ws = s_ws;
        return ws && esp_websocket_client_send_bin(ws, (const char *)pcm, (int)(samples * 2), pdMS_TO_TICKS(2000)) >= 0;
    }
    if (s_voice_via != L_PHONE || !s_enc_pcm) return false;
    while (samples) {
        size_t take = s_enc_bs - s_enc_n < samples ? s_enc_bs - s_enc_n : samples;
        memcpy(s_enc_pcm + s_enc_n, pcm, take * sizeof *pcm);
        s_enc_n += take;
        pcm += take;
        samples -= take;
        if (s_enc_n == s_enc_bs) {
            s_enc_n = 0;
            if (!phone_block(s_enc_pcm, s_enc_bs)) return false;
        }
    }
    return true;
}
