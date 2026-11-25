#pragma once
#include <stddef.h>

/* Keeps recent ESP_LOG output in PSRAM for GET /log (Bearer). Call first thing in app_main.
   The buffer is in no-init PSRAM, so after a panic or software restart the previous boot's tail is kept
   and GET /log?prev returns it (the panic printout itself goes to the UART only; see panicdiag.h). */
void logbuf_init(void);
size_t logbuf_copy(char *out, size_t cap);        /* oldest first; returns bytes copied */
size_t logbuf_copy_prev(char *out, size_t cap);   /* the previous boot's log; 0 after a power-on */
