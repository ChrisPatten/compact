/*
 * LVGL allocator (LV_USE_CUSTOM_MALLOC): everything LVGL allocates, including the tiny_ttf glyph
 * caches, lives in PSRAM, falling back to internal RAM only if PSRAM is exhausted. Internal RAM
 * stays for Wi-Fi, DMA and task stacks. The display draw buffers are allocated by the LVGL adapter
 * with explicit internal caps and are not affected (SPI DMA cannot read PSRAM without bouncing).
 */
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"
#include <stdlib.h>
#include "esp_heap_caps.h"

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, PSRAM_CAPS);
    return p ? p : malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, PSRAM_CAPS);
    return n ? n : realloc(p, new_size);
}

void lv_free_core(void *p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
