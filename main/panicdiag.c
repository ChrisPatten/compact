#include "panicdiag.h"
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_debug_helpers.h"
#include "esp_private/panic_internal.h"
#include "xtensa_context.h"
#include "esp_cpu_utils.h"
#include "esp32s3/rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PD_MAGIC 0x50414E31u   /* "PAN1" */
#define PD_DEPTH 24

typedef struct {
    uint32_t magic;
    uint32_t count;            /* panics recorded since power-on */
    uint32_t uptime_s;
    int32_t core, exception;
    uint32_t pc, exccause, excvaddr;
    uint32_t bt[PD_DEPTH];
    int32_t bt_n;
    char task[16];
    char reason[96];
    uint32_t sum;
} pd_t;

static RTC_NOINIT_ATTR pd_t s_pd;
static bool s_valid;

extern bool g_panic_abort;
extern char *g_panic_abort_details;
extern void __real_esp_panic_handler(panic_info_t *info);

static IRAM_ATTR uint32_t pd_sum(const pd_t *p)
{
    const uint32_t *w = (const uint32_t *)p;
    uint32_t s = 0x9E3779B9u;
    for (size_t i = 0; i < offsetof(pd_t, sum) / 4; i++) s = (s ^ w[i]) * 16777619u;
    return s;
}

static IRAM_ATTR void pd_copy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    if (src) for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    uint32_t count = (s_pd.magic == PD_MAGIC && s_pd.sum == pd_sum(&s_pd)) ? s_pd.count : 0;
    memset(&s_pd, 0, sizeof s_pd);
    s_pd.count = count + 1;
    s_pd.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_pd.core = info->core;
    s_pd.exception = info->exception;
    const XtExcFrame *f = (const XtExcFrame *)info->frame;
    if (f) {
        s_pd.pc = f->pc;
        s_pd.exccause = f->exccause;
        s_pd.excvaddr = f->excvaddr;
        esp_backtrace_frame_t fr = { .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f };
        int n = 0;
        s_pd.bt[n++] = esp_cpu_process_stack_pc(fr.pc);
        while (n < PD_DEPTH && fr.next_pc && esp_backtrace_get_next_frame(&fr)) s_pd.bt[n++] = esp_cpu_process_stack_pc(fr.pc);
        s_pd.bt_n = n;
    }
    if (g_panic_abort && g_panic_abort_details) pd_copy(s_pd.reason, g_panic_abort_details, sizeof s_pd.reason);
    else pd_copy(s_pd.reason, info->reason ? info->reason : info->description, sizeof s_pd.reason);
    s_pd.magic = PD_MAGIC;
    s_pd.sum = pd_sum(&s_pd);
    /* last, since it can fault on a broken scheduler: the record above is already complete */
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
    if (t) {
        pd_copy(s_pd.task, pcTaskGetName(t), sizeof s_pd.task);
        s_pd.sum = pd_sum(&s_pd);
    }
    Cache_WriteBack_All();          /* the log ring lives in no-init PSRAM behind a write-back cache: flush it */
    __real_esp_panic_handler(info);
}

/* esp_restart (OTA, power menu, watchdogs) runs shutdown handlers: flush the cache so GET /log?prev is whole. */
static void flush_on_restart(void) { Cache_WriteBack_All(); }

void panicdiag_init(void)
{
    esp_register_shutdown_handler(flush_on_restart);
    s_valid = s_pd.magic == PD_MAGIC && s_pd.sum == pd_sum(&s_pd);
    if (!s_valid) memset(&s_pd, 0, sizeof s_pd);
}

bool panicdiag_have(void) { return s_valid; }

static void esc(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 2 < n; in++) {
        char c = *in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if ((unsigned char)c < 0x20) out[o++] = ' ';
        else out[o++] = c;
    }
    out[o] = 0;
}

size_t panicdiag_json(char *out, size_t n)
{
    if (!s_valid) return (size_t)snprintf(out, n, "null");
    char reason[200], task[40], bt[PD_DEPTH * 11 + 1];
    esc(s_pd.reason, reason, sizeof reason);
    esc(s_pd.task, task, sizeof task);
    size_t o = 0;
    bt[0] = 0;
    for (int i = 0; i < s_pd.bt_n && i < PD_DEPTH; i++) o += snprintf(bt + o, sizeof bt - o, "%s0x%08lx", i ? " " : "", (unsigned long)s_pd.bt[i]);
    return (size_t)snprintf(out, n,
        "{\"count\":%lu,\"uptime_s\":%lu,\"core\":%ld,\"exception\":%ld,\"task\":\"%s\",\"reason\":\"%s\","
        "\"pc\":\"0x%08lx\",\"exccause\":%lu,\"excvaddr\":\"0x%08lx\",\"backtrace\":\"%s\"}",
        (unsigned long)s_pd.count, (unsigned long)s_pd.uptime_s, (long)s_pd.core, (long)s_pd.exception, task, reason,
        (unsigned long)s_pd.pc, (unsigned long)s_pd.exccause, (unsigned long)s_pd.excvaddr, bt);
}
