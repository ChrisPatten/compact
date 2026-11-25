#include "logbuf.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

/* The last LOG_BYTES of ESP_LOG output, kept in no-init PSRAM so GET /log can show it without a USB cable,
   and so the tail before a panic or restart survives into the next boot (GET /log?prev). */
#define LOG_BYTES (16 * 1024)
#define LB_MAGIC  0x4C4F4731u   /* "LOG1" */

typedef struct {
    uint32_t magic;
    uint32_t head;              /* next write position */
    uint32_t wrapped;
    uint32_t check;             /* magic ^ head ^ wrapped, so a torn header is rejected */
    char buf[LOG_BYTES];
} lb_t;

static EXT_RAM_NOINIT_ATTR lb_t s_lb;
static char *s_prev;            /* PSRAM copy of the previous boot's log */
static size_t s_prev_len;
static bool s_on;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_fn;

static int hook(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int r = s_prev_fn ? s_prev_fn(fmt, ap) : 0;
    char line[200];
    int n = vsnprintf(line, sizeof line, fmt, cp);
    va_end(cp);
    if (n <= 0) return r;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    taskENTER_CRITICAL(&s_mux);
    uint32_t h = s_lb.head;
    for (int i = 0; i < n; i++) {
        s_lb.buf[h++] = line[i];
        if (h == LOG_BYTES) { h = 0; s_lb.wrapped = 1; }
    }
    s_lb.head = h;
    s_lb.check = s_lb.magic ^ s_lb.head ^ s_lb.wrapped;
    taskEXIT_CRITICAL(&s_mux);
    return r;
}

static size_t ring_copy(const char *ring, size_t head, bool wrapped, char *out, size_t cap)
{
    size_t len = wrapped ? LOG_BYTES : head, start = wrapped ? head : 0, n = 0;
    if (len > cap) { start = (start + len - cap) % LOG_BYTES; len = cap; }
    for (size_t i = 0; i < len; i++) out[n++] = ring[(start + i) % LOG_BYTES];
    return n;
}

void logbuf_init(void)
{
    bool kept = s_lb.magic == LB_MAGIC && s_lb.check == (s_lb.magic ^ s_lb.head ^ s_lb.wrapped) && s_lb.head < LOG_BYTES
                && esp_reset_reason() != ESP_RST_POWERON;
    if (kept && (s_prev = heap_caps_malloc(LOG_BYTES, MALLOC_CAP_SPIRAM)))
        s_prev_len = ring_copy(s_lb.buf, s_lb.head, s_lb.wrapped, s_prev, LOG_BYTES);
    s_lb.magic = LB_MAGIC;
    s_lb.head = 0;
    s_lb.wrapped = 0;
    s_lb.check = s_lb.magic;
    s_on = true;
    s_prev_fn = esp_log_set_vprintf(hook);
}

size_t logbuf_copy(char *out, size_t cap)
{
    if (!s_on || !cap) return 0;
    taskENTER_CRITICAL(&s_mux);
    size_t n = ring_copy(s_lb.buf, s_lb.head, s_lb.wrapped, out, cap);
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

size_t logbuf_copy_prev(char *out, size_t cap)
{
    if (!s_prev || !cap) return 0;
    size_t n = s_prev_len < cap ? s_prev_len : cap;
    memcpy(out, s_prev + (s_prev_len - n), n);
    return n;
}
