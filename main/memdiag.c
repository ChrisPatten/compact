#include "memdiag.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_HEAP_TRACING_STANDALONE
#include "esp_heap_trace.h"
#endif

static const char *TAG = "mem";

void memdiag_mark(const char *what)
{
    ESP_LOGI(TAG, "%s: internal free %u largest %u, dma largest %u, psram free %u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* ---- POST /debug/ui "mem-hog <kb>": hold internal RAM for 15 s (fail-safe tests) ---- */

#define HOG_MAX 64
static void *s_hog[HOG_MAX];
static esp_timer_handle_t s_hog_t;

static void hog_free(void *arg)
{
    for (int i = 0; i < HOG_MAX; i++) { free(s_hog[i]); s_hog[i] = NULL; }
    memdiag_mark("mem-hog released");
}

int memdiag_hog(int kb)
{
    if (s_hog[0]) return 0;
    const esp_timer_create_args_t a = { .callback = hog_free, .name = "mem_hog" };
    if (!s_hog_t && esp_timer_create(&a, &s_hog_t) != ESP_OK) return 0;
    int got = 0;
    for (int i = 0; i < HOG_MAX && got < kb; i++) {
        s_hog[i] = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_hog[i]) break;
        got += 4;
    }
    memdiag_mark("mem-hog holding");
    esp_timer_start_once(s_hog_t, 15LL * 1000000);
    return got;
}

/* ---- heap tracing (debug builds only) ---- */

#if CONFIG_HEAP_TRACING_STANDALONE
#define TRACE_N 7000
static heap_trace_record_t *s_trace;
#endif

void memdiag_trace_start(void)
{
#if CONFIG_HEAP_TRACING_STANDALONE
    s_trace = heap_caps_calloc(TRACE_N, sizeof(heap_trace_record_t), MALLOC_CAP_SPIRAM);
    if (!s_trace || heap_trace_init_standalone(s_trace, TRACE_N) != ESP_OK || heap_trace_start(HEAP_TRACE_LEAKS) != ESP_OK)
        ESP_LOGE(TAG, "heap trace not started");
    else
        ESP_LOGW(TAG, "heap tracing on (%d records): a debug build", TRACE_N);
#endif
}

esp_err_t memdiag_send_trace(httpd_req_t *req)
{
#if CONFIG_HEAP_TRACING_STANDALONE
    if (!s_trace) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no trace");
    heap_trace_stop();
    size_t n = heap_trace_get_count();
    char *line = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!line) { heap_trace_resume(); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    httpd_resp_set_type(req, "text/plain");
    int o = snprintf(line, 512, "# live traced allocations %u (internal only below)\n", (unsigned)n);
    httpd_resp_send_chunk(req, line, o);
    for (size_t i = 0; i < n; i++) {
        heap_trace_record_t r;
        if (heap_trace_get(i, &r) != ESP_OK || !r.address || !esp_ptr_internal(r.address)) continue;
        o = snprintf(line, 512, "%u %p", (unsigned)r.size, r.address);
        for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH && o < 480; d++)
            o += snprintf(line + o, 512 - o, " %p", r.alloced_by[d]);
        line[o++] = '\n';
        if (httpd_resp_send_chunk(req, line, o) != ESP_OK) break;
    }
    free(line);
    heap_trace_resume();
    return httpd_resp_send_chunk(req, NULL, 0);
#else
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "build without heap tracing");
#endif
}

/* ---- GET /debug/heap ---- */

#define MAX_TASKS 40
#define TOP_N     40

typedef struct { void *ptr; size_t size; } blk_t;
#define MAX_HEAPS 16
typedef struct {
    intptr_t hs[MAX_HEAPS], he[MAX_HEAPS];   /* registered heaps: a used block that holds one (the DMA reserve pool) */
    int nh;                                  /* is not counted, or its contents would be counted twice */
    TaskStatus_t *ts;
    UBaseType_t nts;
    size_t stack_sz[MAX_TASKS];
    blk_t top[TOP_N];
    int ntop;
    unsigned hist[8];               /* <64, <256, <1K, <2K, <4K, <8K, <16K, >=16K */
    unsigned used_n;
    size_t used_bytes;
} walk_t;

static int hist_bin(size_t s)
{
    static const size_t lim[] = { 64, 256, 1024, 2048, 4096, 8192, 16384 };
    for (int i = 0; i < 7; i++) if (s < lim[i]) return i;
    return 7;
}

/* Runs under the heap lock: no allocation, no logging. */
static bool heaps_walker(walker_heap_into_t h, walker_block_info_t b, void *user)
{
    walk_t *w = user;
    if (w->nh && w->hs[w->nh - 1] == h.start) return false;     /* this heap is already listed: next heap */
    if (w->nh < MAX_HEAPS) { w->hs[w->nh] = h.start; w->he[w->nh] = h.end; w->nh++; }
    return false;
}

static bool walker(walker_heap_into_t h, walker_block_info_t b, void *user)
{
    walk_t *w = user;
    if (!b.used) return true;
    uintptr_t p = (uintptr_t)b.ptr, e = p + b.size;
    for (int i = 0; i < w->nh; i++)
        if ((uintptr_t)w->hs[i] >= p && (uintptr_t)w->he[i] <= e) return true;
    for (UBaseType_t i = 0; i < w->nts && i < MAX_TASKS; i++) {
        uintptr_t base = (uintptr_t)w->ts[i].pxStackBase;
        if (base >= p && base < e) w->stack_sz[i] = b.size;
    }
    if (!esp_ptr_internal(b.ptr)) return true;
    w->used_n++;
    w->used_bytes += b.size;
    w->hist[hist_bin(b.size)]++;
    int at;                                                /* insertion into a sorted top-N */
    if (w->ntop < TOP_N) at = w->ntop++;
    else if (w->top[TOP_N - 1].size >= b.size) return true;
    else at = TOP_N - 1;
    while (at > 0 && w->top[at - 1].size < b.size) { w->top[at] = w->top[at - 1]; at--; }
    w->top[at] = (blk_t){ b.ptr, b.size };
    return true;
}

static int heap_json(char *o, size_t n, const char *name, uint32_t caps)
{
    multi_heap_info_t i;
    heap_caps_get_info(&i, caps);
    return snprintf(o, n, "\"%s\":{\"free\":%u,\"allocated\":%u,\"largest\":%u,\"min_free\":%u,\"alloc_blocks\":%u,\"free_blocks\":%u}",
                    name, (unsigned)i.total_free_bytes, (unsigned)i.total_allocated_bytes, (unsigned)i.largest_free_block,
                    (unsigned)i.minimum_free_bytes, (unsigned)i.allocated_blocks, (unsigned)i.free_blocks);
}

/* Name of the task whose stack holds p, or "tcb:<task>" when p is a task's TCB (the handle is the TCB). */
static const char *owner(const walk_t *w, void *p, size_t sz, char *buf, size_t n)
{
    uintptr_t a = (uintptr_t)p, e = a + sz;
    for (UBaseType_t i = 0; i < w->nts && i < MAX_TASKS; i++) {
        uintptr_t base = (uintptr_t)w->ts[i].pxStackBase, tcb = (uintptr_t)w->ts[i].xHandle;
        if (base >= a && base < e) { snprintf(buf, n, "stack:%s", w->ts[i].pcTaskName); return buf; }
        if (tcb >= a && tcb < e) { snprintf(buf, n, "tcb:%s", w->ts[i].pcTaskName); return buf; }
    }
    return "";
}

esp_err_t memdiag_send_heap(httpd_req_t *req)
{
    const size_t cap = 12 * 1024;
    char *o = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    walk_t *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_SPIRAM);
    TaskStatus_t *ts = heap_caps_calloc(MAX_TASKS, sizeof *ts, MALLOC_CAP_SPIRAM);
    if (!o || !w || !ts) {
        free(o); free(w); free(ts);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    w->ts = ts;
    w->nts = uxTaskGetSystemState(ts, MAX_TASKS, NULL);
    heap_caps_walk_all(heaps_walker, w);
    heap_caps_walk_all(walker, w);

    size_t k = 0;
    k += snprintf(o + k, cap - k, "{\"heaps\":{");
    k += heap_json(o + k, cap - k, "internal", MALLOC_CAP_INTERNAL);
    k += snprintf(o + k, cap - k, ",");
    k += heap_json(o + k, cap - k, "dma", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    k += snprintf(o + k, cap - k, ",");
    k += heap_json(o + k, cap - k, "internal_8bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    k += snprintf(o + k, cap - k, ",");
    k += heap_json(o + k, cap - k, "psram", MALLOC_CAP_SPIRAM);
    k += snprintf(o + k, cap - k, "},\"regions\":[");
    for (int i = 0; i < w->nh && k + 40 < cap; i++)
        k += snprintf(o + k, cap - k, "%s[\"%p\",%u]", i ? "," : "", (void *)w->hs[i], (unsigned)(w->he[i] - w->hs[i]));
    k += snprintf(o + k, cap - k, "],\"tasks\":[");
    size_t st_int = 0, st_ext = 0;
    for (UBaseType_t i = 0; i < w->nts && k + 160 < cap; i++) {
        const TaskStatus_t *t = &ts[i];
        bool in = esp_ptr_internal(t->pxStackBase);
        if (in) st_int += w->stack_sz[i]; else st_ext += w->stack_sz[i];
        k += snprintf(o + k, cap - k, "%s{\"name\":\"%s\",\"prio\":%u,\"stack\":%u,\"in\":\"%s\",\"free_min\":%u}",
                      i ? "," : "", t->pcTaskName, (unsigned)t->uxBasePriority, (unsigned)w->stack_sz[i],
                      in ? "internal" : esp_ptr_external_ram(t->pxStackBase) ? "psram" : "other", (unsigned)t->usStackHighWaterMark);
    }
    k += snprintf(o + k, cap - k, "],\"stacks_internal\":%u,\"stacks_psram\":%u,\"internal_used\":{\"blocks\":%u,\"bytes\":%u,"
                  "\"hist\":{\"lt64\":%u,\"lt256\":%u,\"lt1k\":%u,\"lt2k\":%u,\"lt4k\":%u,\"lt8k\":%u,\"lt16k\":%u,\"ge16k\":%u}},\"largest\":[",
                  (unsigned)st_int, (unsigned)st_ext, w->used_n, (unsigned)w->used_bytes,
                  w->hist[0], w->hist[1], w->hist[2], w->hist[3], w->hist[4], w->hist[5], w->hist[6], w->hist[7]);
    char who[32];
    for (int i = 0; i < w->ntop && k + 80 < cap; i++)
        k += snprintf(o + k, cap - k, "%s[%u,\"%p\",\"%s\"]", i ? "," : "", (unsigned)w->top[i].size, w->top[i].ptr,
                      owner(w, w->top[i].ptr, w->top[i].size, who, sizeof who));
    k += snprintf(o + k, cap - k, "]}\n");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, o, k < cap ? k : cap - 1);
    free(o); free(w); free(ts);
    return r;
}

/* ---- GET /debug/perf: frame breakdown and CPU per task since the previous call ---- */

#include "display.h"

esp_err_t memdiag_send_perf(httpd_req_t *req)
{
    const size_t cap = 6 * 1024;
    char *o = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    TaskStatus_t *ts = heap_caps_calloc(MAX_TASKS, sizeof *ts, MALLOC_CAP_SPIRAM);
    if (!o || !ts) { free(o); free(ts); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    int n, avg, max;
    compact_display_frames(&n, &avg, &max, false);
    compact_perf_t p;
    compact_display_perf(&p);
    size_t k = snprintf(o, cap, "{\"frames\":%d,\"frame_avg_ms\":%d,\"frame_max_ms\":%d,\"over35\":%u,\"over120\":%u,"
                        "\"inval\":%u,\"bands\":%u,\"px\":%u,\"capture_draw_ms\":%u,\"face_draw_ms\":%u,\"ring_hook_ms\":%u,\"ring_step_ms\":%u",
                        n, avg, max, (unsigned)p.over35, (unsigned)p.over120, (unsigned)p.inval, (unsigned)p.bands,
                        (unsigned)p.px, (unsigned)(p.draw_us[PERF_DRAW_CAPTURE] / 1000), (unsigned)(p.draw_us[PERF_DRAW_FACE] / 1000),
                        (unsigned)(p.draw_us[PERF_DRAW_RING_HOOK] / 1000), (unsigned)(p.draw_us[PERF_DRAW_RING_STEP] / 1000));
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    /* CPU ms per task over the interval since the previous call (two cores: up to 2x the interval in total) */
    static struct { TaskHandle_t h; configRUN_TIME_COUNTER_TYPE rt; } prev[MAX_TASKS];
    static int nprev;
    static configRUN_TIME_COUNTER_TYPE prev_total;
    configRUN_TIME_COUNTER_TYPE total;
    UBaseType_t nt = uxTaskGetSystemState(ts, MAX_TASKS, &total);
    k += snprintf(o + k, cap - k, ",\"interval_ms\":%u,\"cpu_ms\":{", (unsigned)((total - prev_total) / 1000));
    bool first = true;
    for (UBaseType_t i = 0; i < nt && k + 48 < cap; i++) {
        configRUN_TIME_COUNTER_TYPE was = 0;
        for (int j = 0; j < nprev; j++) if (prev[j].h == ts[i].xHandle) { was = prev[j].rt; break; }
        unsigned ms = (unsigned)((ts[i].ulRunTimeCounter - was) / 1000);
        if (!ms) continue;
        k += snprintf(o + k, cap - k, "%s\"%s\":%u", first ? "" : ",", ts[i].pcTaskName, ms);
        first = false;
    }
    k += snprintf(o + k, cap - k, "}");
    nprev = 0;
    for (UBaseType_t i = 0; i < nt && i < MAX_TASKS; i++) { prev[nprev].h = ts[i].xHandle; prev[nprev].rt = ts[i].ulRunTimeCounter; nprev++; }
    prev_total = total;
#endif
    k += snprintf(o + k, cap - k, "}\n");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, o, k);
    free(o); free(ts);
    return r;
}
