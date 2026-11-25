#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pure parts of the device command mechanism (commands.c, data/intercom/device-command-contract.md): JSON text
 * helpers, the 16-id dedupe table, the TTL rule and the result frame. No IDF or cJSON here so test/host/cmd_logic_test.c
 * can run on the Mac.
 */
#define CMD_DEDUPE_N 16
#define CMD_ID_MAX 40               /* longest id kept (a UUID is 36) */
#define CMD_TTL_DEFAULT_S 120
#define CMD_TTL_MAX_S 3600

typedef struct {
    char id[CMD_ID_MAX];            /* "" = free */
    char *frame;                    /* the command_result frame sent for it (heap) */
    uint32_t seq;                   /* insertion order, oldest evicted first */
} cmd_slot_t;
typedef struct { cmd_slot_t s[CMD_DEDUPE_N]; uint32_t next_seq; } cmd_dedupe_t;

/* JSON-escape `in` into out (always NUL terminated, truncates on a whole character boundary of the output). */
void cmd_json_escape(const char *in, char *out, size_t n);

/* The stored frame for `id`, or NULL. */
const char *cmd_dedupe_find(const cmd_dedupe_t *d, const char *id);
/* Remember (copies `frame`); evicts the oldest of the 16. False if the copy failed or id is too long. */
bool cmd_dedupe_store(cmd_dedupe_t *d, const char *id, const char *frame);
void cmd_dedupe_clear(cmd_dedupe_t *d);

/* True when a command queued at queued_ms has waited longer than ttl_s by now_ms. ttl_s <= 0 means the default. */
bool cmd_expired(int64_t queued_ms, int64_t now_ms, int ttl_s);
int cmd_clamp_ttl(double ttl_s, bool present);

/* Write {"type":"command_result","id":..,"ok":true,"data":<data_json>} (data_json is raw JSON, NULL = {}) or the
   failure form {"ok":false,"reason":..,"data":"<hint>"} (hint is escaped here). Returns the length, or -1 if it does
   not fit in n. */
int cmd_result_frame(char *out, size_t n, const char *id, bool ok, const char *reason_or_null, const char *data);

/* Split "<op> <args>" into op (at most op_n-1 chars) and a pointer to the args (leading blanks skipped, "" if none). */
const char *cmd_split(const char *line, char *op, size_t op_n);
