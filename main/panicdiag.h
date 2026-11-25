#pragma once
#include <stddef.h>
#include <stdbool.h>

/*
 * Post-mortem for panics without a cable or a coredump partition. The linker wraps esp_panic_handler
 * (main/CMakeLists.txt); the wrapper records the faulting PC, EXCCAUSE / EXCVADDR, the task, the abort
 * message and up to 24 backtrace PCs in RTC memory that survives the panic reset, then runs the normal
 * handler. GET /info shows it as `last_panic`; decode the PCs with
 *   xtensa-esp32s3-elf-addr2line -pfiaC -e build/intercom_firmware.elf <pc> <pc> ...
 * against the ELF of the version that crashed. The record is kept until the next panic or power loss.
 */
void panicdiag_init(void);                    /* once at boot: validate the record */
bool panicdiag_have(void);
size_t panicdiag_json(char *out, size_t n);  /* a JSON object, or "null" */
