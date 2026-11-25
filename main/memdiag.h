#pragma once
#include <stddef.h>
#include "esp_http_server.h"

/* RAM diagnostics for GET /debug/heap (Bearer; registered by ota.c).

   memdiag_send_heap: JSON with heap_caps_get_info for internal, DMA and PSRAM, every task's stack (size, where it
   lives, least free), the largest live internal blocks (with the task whose stack or TCB each one is, when known)
   and a size histogram of live internal blocks.

   memdiag_send_trace: in a build with CONFIG_HEAP_TRACING_STANDALONE (sdkconfig.trace), every live internal
   allocation since boot as "size addr pc pc pc ..." lines, for xtensa-esp32s3-elf-addr2line. 404 otherwise. */
void memdiag_trace_start(void);      /* first thing in app_main; nothing without heap tracing */
esp_err_t memdiag_send_heap(httpd_req_t *req);
esp_err_t memdiag_send_trace(httpd_req_t *req);
/* GET /debug/perf: frame breakdown since /info?reset and CPU ms per task since the previous call. */
esp_err_t memdiag_send_perf(httpd_req_t *req);

/* POST /debug/ui "mem-hog <kb>": take up to kb KB of internal DMA RAM (4 KB blocks, at most 256 KB) for 15 s, to
   test what runs short (the Bluetooth start refusal). Returns KB taken. */
int memdiag_hog(int kb);

/* One line in the log: "<what>: internal free X largest Y" (boot phases, BLE start). */
void memdiag_mark(const char *what);
