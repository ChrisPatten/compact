#include "cmd_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CMD_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define CMD_ALLOC(n) malloc(n)
#endif

void cmd_json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (!n) return;
    for (; in && *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { if (o + 3 > n) break; out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { if (o + 7 > n) break; o += (size_t)snprintf(out + o, n - o, "\\u%04x", c); }
        else { if (o + 2 > n) break; out[o++] = (char)c; }
    }
    out[o] = 0;
}

const char *cmd_dedupe_find(const cmd_dedupe_t *d, const char *id)
{
    for (int i = 0; i < CMD_DEDUPE_N; i++)
        if (d->s[i].id[0] && d->s[i].frame && !strcmp(d->s[i].id, id)) return d->s[i].frame;
    return NULL;
}

bool cmd_dedupe_store(cmd_dedupe_t *d, const char *id, const char *frame)
{
    if (!id[0] || strlen(id) >= CMD_ID_MAX) return false;
    size_t len = strlen(frame) + 1;
    char *copy = CMD_ALLOC(len);
    if (!copy) return false;
    memcpy(copy, frame, len);
    int pick = -1;
    for (int i = 0; i < CMD_DEDUPE_N; i++) {                       /* same id (replace), else a free slot, else the oldest */
        if (d->s[i].id[0] && !strcmp(d->s[i].id, id)) { pick = i; break; }
    }
    for (int i = 0; pick < 0 && i < CMD_DEDUPE_N; i++) if (!d->s[i].id[0]) pick = i;
    if (pick < 0) {
        pick = 0;
        for (int i = 1; i < CMD_DEDUPE_N; i++) if ((int32_t)(d->s[i].seq - d->s[pick].seq) < 0) pick = i;
    }
    free(d->s[pick].frame);
    strcpy(d->s[pick].id, id);
    d->s[pick].frame = copy;
    d->s[pick].seq = d->next_seq++;
    return true;
}

void cmd_dedupe_clear(cmd_dedupe_t *d)
{
    for (int i = 0; i < CMD_DEDUPE_N; i++) { free(d->s[i].frame); d->s[i].frame = NULL; d->s[i].id[0] = 0; }
}

int cmd_clamp_ttl(double ttl_s, bool present)
{
    if (!present || !(ttl_s >= 1)) return CMD_TTL_DEFAULT_S;
    if (ttl_s > CMD_TTL_MAX_S) return CMD_TTL_MAX_S;
    return (int)ttl_s;
}

bool cmd_expired(int64_t queued_ms, int64_t now_ms, int ttl_s)
{
    if (ttl_s <= 0) ttl_s = CMD_TTL_DEFAULT_S;
    return now_ms - queued_ms > (int64_t)ttl_s * 1000;
}

int cmd_result_frame(char *out, size_t n, const char *id, bool ok, const char *reason, const char *data)
{
    char eid[2 * CMD_ID_MAX + 8];
    cmd_json_escape(id, eid, sizeof eid);
    int w;
    if (ok) {
        w = snprintf(out, n, "{\"type\":\"command_result\",\"id\":\"%s\",\"ok\":true,\"data\":%s}", eid, data && data[0] ? data : "{}");
    } else {
        char hint[400];
        cmd_json_escape(data ? data : "", hint, sizeof hint);
        w = snprintf(out, n, "{\"type\":\"command_result\",\"id\":\"%s\",\"ok\":false,\"reason\":\"%s\",\"data\":\"%s\"}",
                     eid, reason ? reason : "failed", hint);
    }
    return w < 0 || (size_t)w >= n ? -1 : w;
}

const char *cmd_split(const char *line, char *op, size_t op_n)
{
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
    size_t o = 0;
    while (*line && *line != ' ' && *line != '\t' && *line != '\r' && *line != '\n') { if (o + 1 < op_n) op[o++] = *line; line++; }
    op[o] = 0;
    while (*line == ' ' || *line == '\t') line++;
    return line;
}
