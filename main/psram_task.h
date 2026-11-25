#pragma once
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

/*
 * Task stacks in PSRAM: internal RAM is the scarce one (README "RAM budget").
 *
 * Why this is safe on this build: code and rodata run from PSRAM (CONFIG_SPIRAM_XIP_FROM_PSRAM), so flash reads,
 * writes and erases (NVS, esp_partition_*) never disable the cache (SPI_FLASH_CACHE_NO_DISABLE in IDF's
 * spi_flash_os_func_app.c). Two things still freeze the cache and need an internal stack:
 *   - flash mmap: esp_partition_mmap, and every esp_ota_* call that reads otadata. Keep those on internal-stack tasks
 *     (httpd, esp_timer, main); ota_mark_good() defers itself to the esp_timer task for that reason.
 *   - a CPU frequency switch down to the 40 MHz XTAL clock (esp_pm), run by whichever task releases the last PM lock.
 *     power.c never sets the light-sleep minimum below 80 MHz, so that switch does not happen.
 *
 * Long-lived tasks fall back to internal RAM if PSRAM is short. A task made here that ends must delete itself with
 * vTaskDeleteWithCaps(NULL), and must not use the fallback (see task_create_psram_only).
 */
static inline BaseType_t task_create_psram(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out)
{
    if (xTaskCreateWithCaps(fn, name, stack, arg, prio, out, MALLOC_CAP_SPIRAM) == pdPASS) return pdPASS;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}

static inline BaseType_t task_create_psram_only(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                                UBaseType_t prio, TaskHandle_t *out)
{
    return xTaskCreateWithCaps(fn, name, stack, arg, prio, out, MALLOC_CAP_SPIRAM);
}
