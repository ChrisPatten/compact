#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * Device commands (data/intercom/device-command-contract.md): one registry of named ops that the bus (`command`
 * frame over the Wi-Fi socket or the BLE phone link) and HTTP `POST /cmd` both run. The bus treats `op` and `args` as
 * opaque; a new device feature is one command_register() line. Handlers run on the command task (or the httpd task for
 * /cmd), never on the websocket or BLE receive path, one at a time.
 *
 * Only safe ops are registered. Never register restart, Wi-Fi, bus URL, link mode or OTA.
 */
typedef enum { CMD_OK = 0, CMD_BAD_ARGS, CMD_BUSY, CMD_FAILED, CMD_UNKNOWN_OP } cmd_status_t;

/* args: "" when none. Success: write a JSON value (usually an object) to out. Failure: write a short human hint
   (usage) to out and return CMD_BAD_ARGS / CMD_BUSY / CMD_FAILED. */
typedef cmd_status_t (*command_fn)(const char *args, char *out, size_t out_n);

bool command_register(const char *op, const char *usage, command_fn fn);   /* op copied; usage must be a literal */
void commands_init(void);                                /* once, after pomodoro_init(); registers the phase 1 ops */

/* Run one op now on the caller's task (serialised with the command task). Returns the status; out as above. */
cmd_status_t commands_run(const char *op, const char *args, char *out, size_t out_n);
const char *commands_reason(cmd_status_t s);             /* "bad_args", "busy", "failed", "unknown_op"; NULL for CMD_OK */

/* `"ops":[{"op":..,"usage":..},...]` without braces or a leading comma, for the hello. Returns the length. */
size_t commands_ops_json(char *out, size_t n);

/* A bus `command` frame arrived on `link` (INTERCOM_LINK_*). Copies what it needs and queues it; never blocks. */
struct cJSON;
void commands_on_frame(const struct cJSON *frame, int link);

/* The HTTP body of POST /cmd: "<op> <args>" or {"op":..,"args":..}. Fills op and args buffers. */
bool commands_parse_http(const char *body, char *op, size_t op_n, char *args, size_t args_n);
