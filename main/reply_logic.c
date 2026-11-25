#include "reply_logic.h"
#include <stdio.h>
#include <string.h>

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

/* Copy at most n-1 bytes, never splitting a UTF-8 sequence. */
static void copy_utf8(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t len = src ? strlen(src) : 0;
    if (len >= n) {
        len = n - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;   /* src[len] starts the cut character */
    }
    if (len) memcpy(dst, src, len);
    dst[len] = 0;
}

static reply_mode_t parse_mode(const char *m)
{
    if (!m) return RM_NONE;
    if (!strcmp(m, "voice")) return RM_VOICE;
    if (!strcmp(m, "choices")) return RM_CHOICES;
    if (!strcmp(m, "both")) return RM_BOTH;
    return RM_NONE;                                         /* "none" and anything unknown */
}

static reply_state_t parse_state(const char *s)
{
    if (!s) return RS_NONE;
    if (!strcmp(s, "awaiting")) return RS_AWAITING;
    if (!strcmp(s, "received")) return RS_RECEIVED;
    if (!strcmp(s, "skipped")) return RS_SKIPPED;
    if (!strcmp(s, "closed")) return RS_CLOSED;
    return RS_NONE;
}

void reply_parse(const cJSON *item, reply_info_t *out)
{
    memset(out, 0, sizeof *out);
    if (!cJSON_IsObject(item)) return;
    const cJSON *rp = cJSON_GetObjectItemCaseSensitive(item, "reply");
    if (cJSON_IsObject(rp)) {
        out->mode = parse_mode(jstr(rp, "mode"));
        copy_utf8(out->prompt, sizeof out->prompt, jstr(rp, "prompt"));
    }
    out->state = parse_state(jstr(item, "reply_state"));
    const char *st = jstr(item, "state");
    out->open = st && (!strcmp(st, "active") || !strcmp(st, "acknowledged"));
    copy_utf8(out->resolution, sizeof out->resolution, jstr(item, "resolution"));
    copy_utf8(out->title, sizeof out->title, jstr(item, "title"));
    const char *via = jstr(item, "reply_via");
    if (via && (!strcmp(via, "voice") || !strcmp(via, "choice") || !strcmp(via, "skip"))) copy_utf8(out->via, sizeof out->via, via);
}

bool reply_mode_voice(reply_mode_t m) { return m == RM_VOICE || m == RM_BOTH; }
bool reply_mode_choices(reply_mode_t m) { return m == RM_CHOICES || m == RM_BOTH; }

bool reply_can_answer(const reply_info_t *r)
{
    return r && r->mode != RM_NONE && r->state == RS_AWAITING && r->open;
}

reply_bind_t reply_bind_decide(const reply_bind_ctx_t *c)
{
    if (!c || !c->enabled) return BIND_OFF;
    if (!c->view_top || !c->info) return BIND_NO_VIEW;
    if (c->test) return BIND_TEST;
    if (c->expired || !reply_can_answer(c->info)) return BIND_CLOSED;
    if (!reply_mode_voice(c->info->mode)) return BIND_NOT_VOICE;
    return BIND_YES;
}

const char *reply_bind_word(reply_bind_t b)
{
    switch (b) {
    case BIND_YES:       return "bound";
    case BIND_OFF:       return "off";
    case BIND_NO_VIEW:   return "no reply view";
    case BIND_NOT_VOICE: return "not a voice question";
    case BIND_CLOSED:    return "closed";
    case BIND_TEST:      return "test item";
    }
    return "?";
}

reply_outcome_t reply_outcome(const char *reason)
{
    if (!reason || !reason[0]) return OUT_OK;
    if (!strcmp(reason, "notification_not_found") || !strcmp(reason, "not_found") ||
        !strcmp(reason, "notification_expired") || !strcmp(reason, "already_replied")) return OUT_LATE;
    if (!strcmp(reason, "reply_not_allowed")) return OUT_NOT_ALLOWED;
    if (!strcmp(reason, "internal_error")) return OUT_RETRY;
    return OUT_FAILED;
}

void reply_layout(const reply_info_t *r, int nact, reply_layout_t *out)
{
    memset(out, 0, sizeof *out);
    if (!reply_can_answer(r)) return;
    if (nact < 0) nact = 0;
    if (nact > 3) nact = 3;
    if (reply_mode_choices(r->mode))
        for (int i = 0; i < nact; i++) out->choice[out->nchoice++] = i;
    out->voice_hint = reply_mode_voice(r->mode);
    out->skip = true;
}

bool reply_id_ok(const char *s)
{
    if (!s || !s[0]) return false;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++, n++)
        if (*p < 0x20 || *p == '"' || *p == '\\' || n >= 150) return false;
    return true;
}

int reply_audio_start_json(char *buf, size_t n, const char *cmid, int rate, const char *notification_id)
{
    if (!buf || !n || !cmid || !reply_id_ok(cmid)) return -1;
    int w;
    if (notification_id && notification_id[0]) {
        if (!reply_id_ok(notification_id)) return -1;
        w = snprintf(buf, n, "{\"type\":\"audio_start\",\"client_msg_id\":\"%.60s\",\"rate\":%d,\"format\":\"pcm_s16le\",\"notification_id\":\"%s\"}",
                     cmid, rate, notification_id);
    } else {
        w = snprintf(buf, n, "{\"type\":\"audio_start\",\"client_msg_id\":\"%.60s\",\"rate\":%d,\"format\":\"pcm_s16le\"}", cmid, rate);
    }
    return w < 0 || (size_t)w >= n ? -1 : w;
}

int reply_skip_json(char *buf, size_t n, const char *id)
{
    if (!buf || !n || !reply_id_ok(id)) return -1;
    int w = snprintf(buf, n, "{\"type\":\"notification_ack\",\"id\":\"%s\",\"skip\":true}", id);
    return w < 0 || (size_t)w >= n ? -1 : w;
}

int reply_late_body(char *buf, size_t n, const char *title, const char *answer)
{
    if (!buf || n < 2 || !answer || !answer[0]) return -1;
    size_t cap = n - 1 < REPLY_LATE_MAX ? n - 1 : REPLY_LATE_MAX;
    char head[REPLY_TITLE_MAX * 4 + 40];
    if (title && title[0]) snprintf(head, sizeof head, "(late reply to \"%s\") ", title);
    else snprintf(head, sizeof head, "(late reply) ");
    size_t hl = strlen(head);
    if (hl >= cap) return -1;
    memcpy(buf, head, hl);
    copy_utf8(buf + hl, cap - hl + 1, answer);
    return (int)strlen(buf);
}

char *reply_send_json(const char *cmid, const char *body)
{
    if (!reply_id_ok(cmid) || !body || !body[0]) return NULL;
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddStringToObject(j, "type", "send");
    cJSON_AddStringToObject(j, "client_msg_id", cmid);
    cJSON_AddStringToObject(j, "body", body);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}

bool reply_pill_visible(bool flag, const reply_info_t *r, bool notice_weight)
{
    return flag && notice_weight && r && reply_can_answer(r);
}

bool reply_needs_you(const reply_info_t *r, bool done, bool expired)
{
    return r && reply_can_answer(r) && !done && !expired;
}

int reply_needs_caption(bool flag, int n, char *buf, size_t sz)
{
    if (!buf || !sz) return 0;
    buf[0] = 0;
    if (!flag || n < 1) return 0;
    int w = snprintf(buf, sz, "%d NEEDS YOU", n > 99 ? 99 : n);
    if (w < 0 || (size_t)w >= sz) { buf[0] = 0; return 0; }
    return w;
}
