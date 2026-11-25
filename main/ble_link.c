#include "ble_link.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "psram_task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "memdiag.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define FRAME_MAX   (256 * 1024)   /* a full `events` reset is a few tens of KB */
#define F_FIRST     0x01
#define F_LAST      0x02
#define F_BINARY    0x04
#define F_BIN_ALL   (F_FIRST | F_LAST | F_BINARY)
#define RXQ_LEN     48             /* ~0.8 s of 16 kHz audio blocks, or a burst of text chunks' frames */
#define RXQ_RESERVE 8              /* audio may not take the last slots: they are kept for text and link changes */

/* Nordic UART Service (little-endian byte order) */
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                                                       0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                                                      0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                                                      0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static ble_link_cbs_t s_cbs;
static char s_name[32];
static uint8_t s_own_addr_type;
static volatile bool s_started, s_want_adv;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_mtu = 23;
static volatile uint16_t s_itvl;                  /* connection interval, 1.25 ms units; 0 unknown */
static volatile bool s_subscribed, s_advertising;
static uint16_t s_tx_handle;
static SemaphoreHandle_t s_tx_mu;
static volatile int s_text_waiting;               /* text senders waiting for s_tx_mu: binary sends hold back */
static volatile unsigned s_drop_text, s_drop_bin, s_bad_chunks;   /* counted on the host task, logged on ble_rx */

/* Everything the host task learns goes to ble_rx through this queue, so the host task never waits on the UI.
   Posts never block: audio is dropped when the queue is nearly full, a lost link change is flagged instead. */
enum { K_FRAME, K_BIN, K_UP, K_DOWN };
typedef struct { int kind; void *data; size_t len; } rx_item_t;
static QueueHandle_t s_rxq;
static volatile bool s_link_dirty;

static bool post(int kind, void *data, size_t len, bool audio)
{
    rx_item_t it = { kind, data, len };
    if (!s_rxq || (audio && uxQueueSpacesAvailable(s_rxq) <= RXQ_RESERVE) || xQueueSend(s_rxq, &it, 0) != pdTRUE) {
        free(data);
        return false;
    }
    return true;
}

static void post_link(bool up)
{
    if (!post(up ? K_UP : K_DOWN, NULL, 0, false)) s_link_dirty = true;    /* ble_rx reports the current state */
}

static void rx_task(void *arg)
{
    unsigned seen_text = 0, seen_bin = 0, seen_bad = 0;
    int64_t last_log = 0;
    for (;;) {
        rx_item_t it;
        if (xQueueReceive(s_rxq, &it, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (it.kind == K_FRAME) {
                if (s_cbs.frame) s_cbs.frame(it.data, it.len);
                free(it.data);
            } else if (it.kind == K_BIN) {
                if (s_cbs.binary) s_cbs.binary(it.data, it.len);
                free(it.data);
            } else if (s_cbs.connected) {
                s_cbs.connected(it.kind == K_UP);
            }
        }
        if (s_link_dirty) {
            s_link_dirty = false;
            ESP_LOGW(TAG, "rx queue was full for a link change: re-reporting");
            if (s_cbs.connected) { s_cbs.connected(false); if (ble_link_ready()) s_cbs.connected(true); }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_log > 2000000 && (s_drop_text != seen_text || s_drop_bin != seen_bin || s_bad_chunks != seen_bad)) {
            ESP_LOGW(TAG, "rx dropped: %u text frames, %u audio blocks, %u bad chunks (rx queue full or bad flags)",
                     s_drop_text - seen_text, s_drop_bin - seen_bin, s_bad_chunks - seen_bad);
            seen_text = s_drop_text; seen_bin = s_drop_bin; seen_bad = s_bad_chunks;
            last_log = now;
        }
    }
}

/* ---- phone -> Compact: chunk reassembly (host task only) ---- */

static char *s_asm;
static size_t s_asm_len, s_asm_cap;
static bool s_in_frame;

static void asm_reset(void) { s_in_frame = false; s_asm_len = 0; }

static bool asm_append(const uint8_t *p, size_t n)
{
    if (s_asm_len + n > FRAME_MAX) { ESP_LOGW(TAG, "frame over %d bytes: dropped", FRAME_MAX); asm_reset(); return false; }
    if (s_asm_len + n + 1 > s_asm_cap) {
        size_t cap = s_asm_cap ? s_asm_cap : 4096;
        while (cap < s_asm_len + n + 1) cap *= 2;
        char *nb = heap_caps_realloc(s_asm, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) { ESP_LOGE(TAG, "no memory for a %u-byte frame", (unsigned)cap); asm_reset(); return false; }
        s_asm = nb;
        s_asm_cap = cap;
    }
    memcpy(s_asm + s_asm_len, p, n);
    s_asm_len += n;
    return true;
}

static void deliver_copy(int kind, const void *p, size_t n)
{
    char *d = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) { ESP_LOGE(TAG, "no memory for a %u-byte frame", (unsigned)n); return; }
    memcpy(d, p, n);
    d[n] = 0;
    if (!post(kind, d, n, kind == K_BIN)) {
        if (kind == K_BIN) s_drop_bin++;
        else s_drop_text++;
    }
}

static void on_chunk(const uint8_t *p, size_t n)
{
    if (!n) return;
    if (!s_in_frame && p[0] == '{') { deliver_copy(K_FRAME, p, n); return; }   /* a hand test: one write, one frame */
    uint8_t fl = p[0];
    if (fl & F_BINARY) {                       /* binary audio: one chunk, flags exactly 0x07; text assembly untouched */
        if (fl != F_BIN_ALL || n < 2) { s_bad_chunks++; return; }
        deliver_copy(K_BIN, p + 1, n - 1);
        return;
    }
    if (fl & ~(F_FIRST | F_LAST)) { s_bad_chunks++; asm_reset(); return; }
    if (fl & F_FIRST) {
        if (s_in_frame) ESP_LOGW(TAG, "new frame before the last one ended: dropped %u bytes", (unsigned)s_asm_len);
        asm_reset();
        s_in_frame = true;
    } else if (!s_in_frame) {
        ESP_LOGW(TAG, "continuation chunk with no frame open: dropped");
        return;
    }
    if (!asm_append(p + 1, n - 1)) return;
    if (fl & F_LAST) {
        deliver_copy(K_FRAME, s_asm, s_asm_len);
        asm_reset();
    }
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static uint8_t buf[BLE_ATT_ATTR_MAX_LEN];
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t got = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof buf || ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &got) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    on_chunk(buf, got);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;    /* notify only */
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &RX_UUID.u, .access_cb = rx_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &TX_UUID.u, .access_cb = tx_access, .val_handle = &s_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP ---- */

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    if (!s_want_adv || s_conn != BLE_HS_CONN_HANDLE_NONE || !ble_hs_synced()) return;
    if (ble_gap_adv_active()) { s_advertising = true; return; }
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) { ESP_LOGE(TAG, "adv fields: %d", rc); return; }
    struct ble_hs_adv_fields r = {0};
    r.name = (uint8_t *)s_name;
    r.name_len = strlen(s_name);
    r.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc) { ESP_LOGE(TAG, "scan response fields: %d", rc); return; }
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) { ESP_LOGE(TAG, "advertising failed: %d", rc); return; }
    if (!s_advertising) ESP_LOGI(TAG, "advertising as %s", s_name);
    s_advertising = true;
}

static void log_params(uint16_t conn, const char *what, int status)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return;
    s_itvl = d.conn_itvl;
    unsigned us = d.conn_itvl * 1250u;
    ESP_LOGI(TAG, "%s: interval %u.%02u ms, latency %u, supervision timeout %u ms%s", what, us / 1000, (us % 1000) / 10,
             d.conn_latency, d.supervision_timeout * 10u, status ? " (update failed)" : "");
    if (status) ESP_LOGW(TAG, "connection update status %d", status);
}

/* For audio throughput (docs/phone-link-v2.md s1): 15-30 ms, latency 0, 4 s timeout; and the longest LL packets,
   so a 134-byte block is one air packet. iOS may grant something else; CONN_UPDATE logs what we got. */
static void request_params(uint16_t conn)
{
    const struct ble_gap_upd_params p = { .itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400 };
    int rc = ble_gap_update_params(conn, &p);
    if (rc) ESP_LOGW(TAG, "connection update request failed: %d", rc);
    else ESP_LOGI(TAG, "asked for a 15-30 ms interval, latency 0, timeout 4 s");
    rc = ble_gap_set_data_len(conn, 251, 2120);
    if (rc) ESP_LOGW(TAG, "data length request failed: %d", rc);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_advertising = false;
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_mtu = 23;
            asm_reset();
            ESP_LOGI(TAG, "phone connected");
            log_params(s_conn, "connected", 0);
            request_params(s_conn);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_itvl = 0;
        asm_reset();
        post_link(false);
        advertise();                               /* only if still wanted */
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        log_params(ev->conn_update.conn_handle, "connection parameters", ev->conn_update.status);
        break;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "data length: tx %u, rx %u octets", ev->data_len_chg.max_tx_octets, ev->data_len_chg.max_rx_octets);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            bool was = s_subscribed;
            s_subscribed = ev->subscribe.cur_notify;
            ESP_LOGI(TAG, "TX notifications %s", s_subscribed ? "on" : "off");
            if (s_subscribed != was) post_link(s_subscribed);
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        ESP_LOGI(TAG, "mtu %u", s_mtu);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset (%d)", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* The controller and host take about 35 KB of internal DMA-capable RAM when they start (measured Nov 19: 34.5 KB,
   the largest piece a 9.5 KB block, plus two 4 KB task stacks and ~60 small blocks). The controller does not survive
   running short: its buffer allocation fails inside its own task's reset, after esp_bt_controller_enable has already
   returned ESP_OK, and that is a NULL dereference (the Nov 19 btController panic, r_ble_util_buf_rx_alloc). So refuse
   to start without that much plus margin; the caller stays on Wi-Fi (or offline) and retries later. */
#define BLE_NEED_FREE    (52 * 1024)
#define BLE_NEED_LARGEST (16 * 1024)

bool ble_link_start(const char *name, const ble_link_cbs_t *cbs, bool advertise_now)
{
    if (s_started) { if (advertise_now) ble_link_set_advertising(true); return true; }
    s_cbs = *cbs;
    strlcpy(s_name, name, sizeof s_name);
    s_want_adv = advertise_now;
    if (!s_tx_mu && !(s_tx_mu = xSemaphoreCreateMutex())) return false;
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    memdiag_mark("before NimBLE");
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (dma_free < BLE_NEED_FREE || dma_largest < BLE_NEED_LARGEST) {
        ESP_LOGE(TAG, "not starting Bluetooth: internal DMA RAM free %u (need %u), largest %u (need %u)",
                 (unsigned)dma_free, BLE_NEED_FREE, (unsigned)dma_largest, BLE_NEED_LARGEST);
        return false;
    }
    bool mon = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s (internal free %u, largest %u)", esp_err_to_name(e), (unsigned)before,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (mon) heap_caps_monitor_local_minimum_free_size_stop();
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (!rc) rc = ble_gatts_add_svcs(s_svcs);
    if (rc) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        nimble_port_deinit();
        if (mon) heap_caps_monitor_local_minimum_free_size_stop();
        return false;
    }
    ble_svc_gap_device_name_set(s_name);
    if (!s_rxq) {
        s_rxq = xQueueCreate(RXQ_LEN, sizeof(rx_item_t));
        if (!s_rxq || task_create_psram(rx_task, "ble_rx", 6144, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "no memory for the rx task");
            if (s_rxq) { vQueueDelete(s_rxq); s_rxq = NULL; }
            nimble_port_deinit();
            if (mon) heap_caps_monitor_local_minimum_free_size_stop();
            return false;
        }
    }
    nimble_port_freertos_init(host_task);
    s_started = true;
    vTaskDelay(pdMS_TO_TICKS(300));                   /* the controller task resets and allocates its buffers */
    if (mon) {
        ESP_LOGI(TAG, "NimBLE start: lowest internal free during it %u (from %u)",
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)before);
        heap_caps_monitor_local_minimum_free_size_stop();
    }
    memdiag_mark("after NimBLE");
    ESP_LOGI(TAG, "NimBLE up: internal free %u -> %u, largest %u", (unsigned)before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

bool ble_link_started(void) { return s_started; }

void ble_link_set_advertising(bool on)
{
    s_want_adv = on;
    if (!s_started || !ble_hs_synced()) return;               /* on_sync starts it when wanted */
    if (on) { advertise(); return; }
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        ESP_LOGI(TAG, "advertising stopped%s", rc ? " (stop failed)" : "");
    }
    s_advertising = false;
}

void ble_link_disconnect(void)
{
    uint16_t c = s_conn;
    if (!s_started || c == BLE_HS_CONN_HANDLE_NONE) return;
    int rc = ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "disconnect failed: %d", rc);
}

bool ble_link_busy(void) { return s_started && (s_advertising || s_conn != BLE_HS_CONN_HANDLE_NONE); }
bool ble_link_ready(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed; }
uint16_t ble_link_mtu(void) { return s_mtu; }

/* ---- Compact -> phone: notifications ---- */

static bool timed_out(TickType_t start, TickType_t timeout)
{
    return timeout != portMAX_DELAY && xTaskGetTickCount() - start > timeout;
}

/* One notification [fl][data]. Retries while NimBLE is out of buffers (the radio drains them), until timeout. */
static bool notify(uint16_t conn, uint8_t fl, const void *data, size_t n, TickType_t start, TickType_t timeout)
{
    for (;;) {
        if (!ble_link_ready() || s_conn != conn) return false;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&fl, 1);
        if (om && os_mbuf_append(om, data, n) != 0) { os_mbuf_free_chain(om); om = NULL; }
        if (om) {
            int rc = ble_gatts_notify_custom(conn, s_tx_handle, om);    /* consumes om either way */
            if (rc == 0) return true;
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) { ESP_LOGW(TAG, "notify failed: %d", rc); return false; }
        }
        if (timed_out(start, timeout)) return false;
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

bool ble_link_send(const char *data, size_t len, TickType_t timeout)
{
    if (!ble_link_ready() || !len) return false;
    TickType_t start = xTaskGetTickCount();
    __atomic_add_fetch(&s_text_waiting, 1, __ATOMIC_SEQ_CST);
    bool got = xSemaphoreTake(s_tx_mu, timeout) == pdTRUE;
    __atomic_sub_fetch(&s_text_waiting, 1, __ATOMIC_SEQ_CST);
    if (!got) return false;
    uint16_t conn = s_conn;
    bool ok = true;
    for (size_t off = 0; ok && off < len;) {
        size_t room = (s_mtu > 24 ? s_mtu : 23) - 3 - 1;          /* ATT header, then our flags byte */
        size_t n = len - off < room ? len - off : room;
        uint8_t fl = (off == 0 ? F_FIRST : 0) | (off + n == len ? F_LAST : 0);
        ok = notify(conn, fl, data + off, n, start, timeout);
        off += n;
    }
    xSemaphoreGive(s_tx_mu);
    return ok;
}

bool ble_link_send_binary(const uint8_t *data, size_t len, TickType_t timeout)
{
    if (!ble_link_ready() || !len) return false;
    uint16_t mtu = s_mtu;
    if (len + 1 > (size_t)(mtu > 23 ? mtu : 23) - 3) return false;    /* one chunk, never split */
    TickType_t start = xTaskGetTickCount();
    while (__atomic_load_n(&s_text_waiting, __ATOMIC_SEQ_CST) > 0) {   /* a text frame is waiting: it goes first */
        if (timed_out(start, timeout)) return false;
        vTaskDelay(1);
    }
    TickType_t left = timeout;
    if (timeout != portMAX_DELAY) {
        TickType_t used = xTaskGetTickCount() - start;
        left = used >= timeout ? 0 : timeout - used;
    }
    if (xSemaphoreTake(s_tx_mu, left) != pdTRUE) return false;
    bool ok = notify(s_conn, F_BIN_ALL, data, len, start, timeout);
    xSemaphoreGive(s_tx_mu);
    return ok;
}

void ble_link_info(char *out, size_t n)
{
    if (!s_started) { snprintf(out, n, "off"); return; }
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) { snprintf(out, n, "%s", s_advertising ? "advertising" : "idle"); return; }
    unsigned us = s_itvl * 1250u;
    int o = snprintf(out, n, "connected, mtu %u, %u.%02u ms, %s", s_mtu, us / 1000, (us % 1000) / 10,
                     s_subscribed ? "subscribed" : "not subscribed");
    if (o > 0 && (size_t)o < n && (s_drop_text || s_drop_bin))
        snprintf(out + o, n - o, ", dropped %u text %u audio", s_drop_text, s_drop_bin);
}
