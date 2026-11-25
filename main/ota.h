#pragma once

/* Call early at boot: if this image is new and unverified, arm a rollback timer. */
void ota_boot_check(void);

/* The new image has reached the bus: keep it and cancel the rollback. */
void ota_mark_good(void);

/* Start the HTTP server: POST /ota (Bearer token) to flash, GET /info for version. */
void ota_server_start(const char *token);
