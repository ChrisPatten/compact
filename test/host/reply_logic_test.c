/* Host test for main/reply_logic.c (notification replies, E105 contract v1). Run through tools/host_tests.sh, or:
   cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain \
      -I$IDF_PATH/components/json/cJSON test/host/reply_logic_test.c main/reply_logic.c \
      $IDF_PATH/components/json/cJSON/cJSON.c -lm -o /tmp/reply_logic_test && /tmp/reply_logic_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reply_logic.h"

static int s_fail, s_n;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { s_n++; if (strcmp((a), (b))) { s_fail++; printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static void parse(const char *json, reply_info_t *r)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) { printf("bad test json: %s\n", json); exit(2); }
    reply_parse(j, r);
    cJSON_Delete(j);
}

/* The public shape from contract section 2, as the `notices` resource carries it. */
static const char *ITEM_BOTH =
    "{\"id\":\"n1\",\"key\":\"dave-1on1\",\"title\":\"Dave asked to move the 1:1\",\"body\":\"Thu 2:00 or Fri 10:30?\","
    "\"level\":\"attention\",\"state\":\"active\",\"actions\":[{\"label\":\"Thu 2:00\"},{\"label\":\"Fri 10:30\"}],"
    "\"source\":\"agent:peggy\",\"created_at\":\"2026-11-23T14:00:00Z\",\"updated_at\":\"2026-11-23T14:00:00Z\","
    "\"expires_at\":\"2026-11-23T16:00:00Z\",\"reply\":{\"mode\":\"both\",\"prompt\":\"Say a time\",\"hints\":[\"Dave\"],"
    "\"on_expire\":\"note\"},\"reply_state\":\"awaiting\",\"replied_at\":null,\"reply_via\":null,\"resolution\":null}";

static void test_parse(void)
{
    reply_info_t r;
    parse(ITEM_BOTH, &r);
    CHECK(r.mode == RM_BOTH);
    CHECK(r.state == RS_AWAITING);
    CHECK(r.open);
    CHECK_STR(r.prompt, "Say a time");
    CHECK_STR(r.title, "Dave asked to move the 1:1");
    CHECK_STR(r.resolution, "");
    CHECK_STR(r.via, "");
    CHECK(reply_can_answer(&r));

    /* a bus without E105: no reply keys at all */
    parse("{\"id\":\"n2\",\"title\":\"Package\",\"state\":\"active\",\"level\":\"info\"}", &r);
    CHECK(r.mode == RM_NONE && r.state == RS_NONE && r.open && !reply_can_answer(&r));

    /* E105 bus, no reply mode: reply null, reply_state none */
    parse("{\"state\":\"active\",\"reply\":null,\"reply_state\":\"none\"}", &r);
    CHECK(r.mode == RM_NONE && r.state == RS_NONE && !reply_can_answer(&r));

    /* each mode */
    parse("{\"state\":\"active\",\"reply\":{\"mode\":\"voice\"},\"reply_state\":\"awaiting\"}", &r);
    CHECK(r.mode == RM_VOICE && reply_mode_voice(r.mode) && !reply_mode_choices(r.mode));
    parse("{\"state\":\"active\",\"reply\":{\"mode\":\"choices\"},\"reply_state\":\"awaiting\"}", &r);
    CHECK(r.mode == RM_CHOICES && !reply_mode_voice(r.mode) && reply_mode_choices(r.mode));
    parse("{\"state\":\"active\",\"reply\":{\"mode\":\"none\"},\"reply_state\":\"none\"}", &r);
    CHECK(r.mode == RM_NONE);
    parse("{\"state\":\"active\",\"reply\":{\"mode\":\"telepathy\"},\"reply_state\":\"awaiting\"}", &r);
    CHECK(r.mode == RM_NONE && !reply_can_answer(&r));          /* unknown mode: no answer offered */
    parse("{\"state\":\"active\",\"reply\":{\"mode\":7},\"reply_state\":\"awaiting\"}", &r);
    CHECK(r.mode == RM_NONE);

    /* acknowledged ("seen") but still awaiting: a reply is still allowed (contract 2) */
    parse("{\"state\":\"acknowledged\",\"reply\":{\"mode\":\"voice\"},\"reply_state\":\"awaiting\"}", &r);
    CHECK(r.open && reply_can_answer(&r));

    /* received, skipped, closed */
    parse("{\"state\":\"acknowledged\",\"reply\":{\"mode\":\"voice\"},\"reply_state\":\"received\",\"reply_via\":\"voice\","
          "\"replied_at\":\"2026-11-23T14:05:00Z\"}", &r);
    CHECK(r.state == RS_RECEIVED && !reply_can_answer(&r));
    CHECK_STR(r.via, "voice");
    parse("{\"state\":\"acknowledged\",\"reply\":{\"mode\":\"both\"},\"reply_state\":\"skipped\",\"reply_via\":\"skip\"}", &r);
    CHECK(r.state == RS_SKIPPED && !reply_can_answer(&r));
    CHECK_STR(r.via, "skip");
    parse("{\"state\":\"resolved\",\"reply\":{\"mode\":\"both\"},\"reply_state\":\"closed\",\"resolution\":\"Booked 3:30\"}", &r);
    CHECK(r.state == RS_CLOSED && !r.open && !reply_can_answer(&r));
    CHECK_STR(r.resolution, "Booked 3:30");
    parse("{\"state\":\"expired\",\"reply\":{\"mode\":\"voice\"},\"reply_state\":\"closed\"}", &r);
    CHECK(!r.open && !reply_can_answer(&r));

    /* awaiting on a state the device does not know */
    parse("{\"state\":\"weird\",\"reply\":{\"mode\":\"voice\"},\"reply_state\":\"awaiting\"}", &r);
    CHECK(!reply_can_answer(&r));
    /* unknown reply_via is dropped */
    parse("{\"state\":\"active\",\"reply_via\":\"carrier pigeon\"}", &r);
    CHECK_STR(r.via, "");

    /* malformed: not an object, wrong types */
    reply_parse(NULL, &r);
    CHECK(r.mode == RM_NONE && !r.open);
    parse("[1,2,3]", &r);
    CHECK(r.mode == RM_NONE);
    parse("{\"reply\":\"voice\",\"reply_state\":3,\"state\":true,\"title\":5}", &r);
    CHECK(r.mode == RM_NONE && r.state == RS_NONE && !r.open && !r.title[0]);
}

static void test_parse_lengths(void)
{
    /* a prompt far over the limit is cut, and never inside a UTF-8 sequence */
    char json[8192], big[2048];
    size_t o = 0;
    while (o + 3 < 1500) { memcpy(big + o, "\xC3\xA9", 2); o += 2; }   /* é repeated */
    big[o] = 0;
    snprintf(json, sizeof json, "{\"state\":\"active\",\"title\":\"%s\",\"resolution\":\"%s\",\"reply\":{\"mode\":\"voice\",\"prompt\":\"%s\"},"
             "\"reply_state\":\"awaiting\"}", big, big, big);
    reply_info_t r;
    parse(json, &r);
    CHECK(strlen(r.prompt) < sizeof r.prompt && strlen(r.prompt) > 0);
    CHECK(strlen(r.prompt) % 2 == 0);                             /* whole é characters only */
    CHECK(strlen(r.resolution) % 2 == 0 && strlen(r.resolution) < sizeof r.resolution);
    CHECK(strlen(r.title) % 2 == 0 && strlen(r.title) < sizeof r.title);
    /* 4-byte characters (emoji) at the cut */
    o = 0;
    while (o + 5 < 1500) { memcpy(big + o, "\xF0\x9F\x98\x80", 4); o += 4; }
    big[o] = 0;
    snprintf(json, sizeof json, "{\"reply\":{\"mode\":\"voice\",\"prompt\":\"a%s\"}}", big);
    parse(json, &r);
    CHECK((strlen(r.prompt) - 1) % 4 == 0);
}

static reply_info_t mk(reply_mode_t m, reply_state_t s, bool open)
{
    reply_info_t r; memset(&r, 0, sizeof r);
    r.mode = m; r.state = s; r.open = open;
    return r;
}

static void test_binding(void)
{
    reply_info_t both = mk(RM_BOTH, RS_AWAITING, true), voice = mk(RM_VOICE, RS_AWAITING, true);
    reply_info_t choices = mk(RM_CHOICES, RS_AWAITING, true), done = mk(RM_VOICE, RS_RECEIVED, true);
    reply_info_t skipped = mk(RM_BOTH, RS_SKIPPED, true), closed = mk(RM_VOICE, RS_CLOSED, false);
    reply_info_t none = mk(RM_NONE, RS_NONE, true);
    reply_bind_ctx_t c = { .enabled = true, .view_top = true, .info = &voice };

    CHECK(reply_bind_decide(&c) == BIND_YES);
    c.info = &both;    CHECK(reply_bind_decide(&c) == BIND_YES);
    c.info = &choices; CHECK(reply_bind_decide(&c) == BIND_NOT_VOICE);      /* "NOT A VOICE QUESTION", unbound */
    c.info = &done;    CHECK(reply_bind_decide(&c) == BIND_CLOSED);
    c.info = &skipped; CHECK(reply_bind_decide(&c) == BIND_CLOSED);
    c.info = &closed;  CHECK(reply_bind_decide(&c) == BIND_CLOSED);
    c.info = &none;    CHECK(reply_bind_decide(&c) == BIND_CLOSED);

    /* only while the Reply view is the top screen (decision 4) */
    c.info = &voice; c.view_top = false; CHECK(reply_bind_decide(&c) == BIND_NO_VIEW);
    c.view_top = true; c.info = NULL;    CHECK(reply_bind_decide(&c) == BIND_NO_VIEW);
    /* the flag gates everything, before any other rule */
    c.info = &voice; c.enabled = false;  CHECK(reply_bind_decide(&c) == BIND_OFF);
    c.view_top = false;                  CHECK(reply_bind_decide(&c) == BIND_OFF);
    CHECK(reply_bind_decide(NULL) == BIND_OFF);
    /* the test item never binds */
    c.enabled = true; c.view_top = true; c.test = true; CHECK(reply_bind_decide(&c) == BIND_TEST);
    /* expired on the device clock */
    c.test = false; c.expired = true; CHECK(reply_bind_decide(&c) == BIND_CLOSED);

    /* exhaustive: BIND_YES only when every condition holds */
    reply_info_t infos[] = { both, voice, choices, done, skipped, closed, none };
    for (int e = 0; e < 2; e++) for (int v = 0; v < 2; v++) for (int t = 0; t < 2; t++) for (int x = 0; x < 2; x++)
        for (int i = 0; i < 7; i++) {
            reply_bind_ctx_t k = { .enabled = e, .view_top = v, .test = t, .expired = x, .info = &infos[i] };
            bool want = e && v && !t && !x && reply_can_answer(&infos[i]) && reply_mode_voice(infos[i].mode);
            CHECK((reply_bind_decide(&k) == BIND_YES) == want);
        }
    for (int b = BIND_YES; b <= BIND_TEST; b++) CHECK(strcmp(reply_bind_word((reply_bind_t)b), "?"));
}

static void test_outcomes(void)
{
    CHECK(reply_outcome(NULL) == OUT_OK);
    CHECK(reply_outcome("") == OUT_OK);
    /* v1 ack / transcript reasons */
    CHECK(reply_outcome("notification_not_found") == OUT_LATE);
    CHECK(reply_outcome("notification_expired") == OUT_LATE);
    CHECK(reply_outcome("already_replied") == OUT_LATE);
    CHECK(reply_outcome("reply_not_allowed") == OUT_NOT_ALLOWED);
    CHECK(reply_outcome("no_route") == OUT_FAILED);
    CHECK(reply_outcome("invalid_body") == OUT_FAILED);
    CHECK(reply_outcome("internal_error") == OUT_RETRY);
    /* v2 action_result reasons: not_found stands for notification_not_found */
    CHECK(reply_outcome("not_found") == OUT_LATE);
    CHECK(reply_outcome("invalid") == OUT_FAILED);
    CHECK(reply_outcome("not_sent") == OUT_FAILED);
    CHECK(reply_outcome("read_only") == OUT_FAILED);
}

static void test_layout(void)
{
    reply_layout_t L;
    reply_info_t r = mk(RM_CHOICES, RS_AWAITING, true);
    reply_layout(&r, 2, &L);
    CHECK(L.nchoice == 2 && L.choice[0] == 0 && L.choice[1] == 1 && L.skip && !L.voice_hint);
    reply_layout(&r, 3, &L);
    CHECK(L.nchoice == 3 && L.choice[2] == 2 && L.skip);
    reply_layout(&r, 9, &L);
    CHECK(L.nchoice == 3);                                        /* the bus allows at most 3 */
    reply_layout(&r, -1, &L);
    CHECK(L.nchoice == 0 && L.skip);
    r.mode = RM_BOTH;
    reply_layout(&r, 1, &L);
    CHECK(L.nchoice == 1 && L.voice_hint && L.skip);
    r.mode = RM_VOICE;
    reply_layout(&r, 2, &L);
    CHECK(L.nchoice == 0 && L.voice_hint && L.skip);              /* a tap would be refused reply_not_allowed */
    r.state = RS_RECEIVED;
    reply_layout(&r, 2, &L);
    CHECK(L.nchoice == 0 && !L.skip && !L.voice_hint);            /* nothing to answer */
    r = mk(RM_NONE, RS_NONE, true);
    reply_layout(&r, 2, &L);
    CHECK(L.nchoice == 0 && !L.skip);
}

static void test_frames(void)
{
    char b[400];
    /* unbound audio_start: exactly what the firmware sent before E105 */
    int n = reply_audio_start_json(b, sizeof b, "ptt-0123abcd-4567", 16000, NULL);
    CHECK_STR(b, "{\"type\":\"audio_start\",\"client_msg_id\":\"ptt-0123abcd-4567\",\"rate\":16000,\"format\":\"pcm_s16le\"}");
    CHECK(n == (int)strlen(b));
    reply_audio_start_json(b, sizeof b, "ptt-0123abcd-4567", 16000, "");
    CHECK_STR(b, "{\"type\":\"audio_start\",\"client_msg_id\":\"ptt-0123abcd-4567\",\"rate\":16000,\"format\":\"pcm_s16le\"}");
    /* bound (contract 3.2) */
    n = reply_audio_start_json(b, sizeof b, "ptt-1", 16000, "3f2a9c1e-77aa-4b1e-9d2c-0123456789ab");
    CHECK(n > 0);
    cJSON *j = cJSON_Parse(b);
    CHECK(j != NULL);
    if (j) {
        CHECK_STR(cJSON_GetObjectItem(j, "type")->valuestring, "audio_start");
        CHECK_STR(cJSON_GetObjectItem(j, "client_msg_id")->valuestring, "ptt-1");
        CHECK(cJSON_GetObjectItem(j, "rate")->valueint == 16000);
        CHECK_STR(cJSON_GetObjectItem(j, "notification_id")->valuestring, "3f2a9c1e-77aa-4b1e-9d2c-0123456789ab");
        CHECK(!cJSON_GetObjectItem(j, "hold") && !cJSON_GetObjectItem(j, "private"));   /* dropped by decision 5 */
        cJSON_Delete(j);
    }
    /* ids that would break the frame are refused, not escaped */
    CHECK(reply_audio_start_json(b, sizeof b, "ptt-1", 16000, "a\"b") == -1);
    CHECK(reply_audio_start_json(b, sizeof b, "ptt-1", 16000, "a\\b") == -1);
    CHECK(reply_audio_start_json(b, sizeof b, "ptt\n1", 16000, NULL) == -1);
    CHECK(reply_audio_start_json(b, sizeof b, NULL, 16000, NULL) == -1);
    CHECK(reply_audio_start_json(b, 20, "ptt-1", 16000, "n") == -1);   /* does not fit */
    char longid[200]; memset(longid, 'x', 160); longid[160] = 0;
    CHECK(reply_audio_start_json(b, sizeof b, "ptt-1", 16000, longid) == -1);
    CHECK(!reply_id_ok("") && !reply_id_ok(NULL) && reply_id_ok("reply-test"));

    /* SKIP (contract 3.3) */
    n = reply_skip_json(b, sizeof b, "n1");
    CHECK_STR(b, "{\"type\":\"notification_ack\",\"id\":\"n1\",\"skip\":true}");
    j = cJSON_Parse(b);
    CHECK(j && cJSON_IsTrue(cJSON_GetObjectItem(j, "skip")));
    cJSON_Delete(j);
    CHECK(reply_skip_json(b, sizeof b, "") == -1);
    CHECK(reply_skip_json(b, 10, "n1") == -1);

    /* late reply body (contract 3.4) */
    n = reply_late_body(b, sizeof b, "Dave asked to move the 1:1", "Friday at ten thirty");
    CHECK_STR(b, "(late reply to \"Dave asked to move the 1:1\") Friday at ten thirty");
    reply_late_body(b, sizeof b, "", "yes");
    CHECK_STR(b, "(late reply) yes");
    reply_late_body(b, sizeof b, NULL, "yes");
    CHECK_STR(b, "(late reply) yes");
    CHECK(reply_late_body(b, sizeof b, "T", "") == -1);
    CHECK(reply_late_body(b, sizeof b, "T", NULL) == -1);
    CHECK(reply_late_body(b, 8, "A long title", "x") == -1);    /* the head alone does not fit */
    /* a long answer is cut to fit, at a character boundary */
    char *ans = malloc(9000), *out = malloc(9000);
    size_t o = 0;
    while (o + 3 < 8990) { memcpy(ans + o, "\xE2\x80\x99", 3); o += 3; }   /* ’ repeated */
    ans[o] = 0;
    n = reply_late_body(out, 9000, "T", ans);
    CHECK(n > 0 && n <= REPLY_LATE_MAX);
    CHECK((strlen(out) - strlen("(late reply to \"T\") ")) % 3 == 0);
    n = reply_late_body(out, 50, "T", ans);
    CHECK(n > 0 && n <= 49 && (strlen(out) - strlen("(late reply to \"T\") ")) % 3 == 0);

    /* the send frame escapes the title's quotes and keeps the text exact */
    reply_late_body(b, sizeof b, "Say \"yes\" or no", "yes \\ no");
    char *s = reply_send_json("late-1", b);
    CHECK(s != NULL);
    if (s) {
        j = cJSON_Parse(s);
        CHECK(j != NULL);
        if (j) {
            CHECK_STR(cJSON_GetObjectItem(j, "type")->valuestring, "send");
            CHECK_STR(cJSON_GetObjectItem(j, "client_msg_id")->valuestring, "late-1");
            CHECK_STR(cJSON_GetObjectItem(j, "body")->valuestring, "(late reply to \"Say \"yes\" or no\") yes \\ no");
            CHECK(!cJSON_GetObjectItem(j, "notification_id"));   /* a late reply is an ordinary send (3.4) */
            cJSON_Delete(j);
        }
        cJSON_free(s);
    }
    CHECK(reply_send_json("late-1", "") == NULL);
    CHECK(reply_send_json("bad\"id", "x") == NULL);
    free(ans); free(out);
}

static void test_entry_points(void)
{
    reply_info_t both, none, closed;
    parse(ITEM_BOTH, &both);
    parse("{\"state\":\"active\",\"level\":\"info\"}", &none);
    parse("{\"state\":\"active\",\"reply\":{\"mode\":\"voice\"},\"reply_state\":\"received\"}", &closed);

    /* REPLY pill: flag on, notice weight, an answer still possible */
    CHECK(reply_pill_visible(true, &both, true));
    CHECK(!reply_pill_visible(false, &both, true));      /* flag off: ordinary Inform */
    CHECK(!reply_pill_visible(true, &both, false));      /* decision weight is the Reply view itself */
    CHECK(!reply_pill_visible(true, &none, true));       /* no reply mode */
    CHECK(!reply_pill_visible(true, &closed, true));     /* already answered */
    CHECK(!reply_pill_visible(true, NULL, true));

    /* what counts toward NEEDS YOU */
    CHECK(reply_needs_you(&both, false, false));
    CHECK(!reply_needs_you(&both, true, false));         /* answered, skipped or cleared here */
    CHECK(!reply_needs_you(&both, false, true));         /* expired */
    CHECK(!reply_needs_you(&none, false, false));
    CHECK(!reply_needs_you(&closed, false, false));
    CHECK(!reply_needs_you(NULL, false, false));

    /* caption text */
    char b[32];
    CHECK(reply_needs_caption(true, 3, b, sizeof b) == 11);
    CHECK_STR(b, "3 NEEDS YOU");
    CHECK(reply_needs_caption(true, 1, b, sizeof b) > 0);
    CHECK_STR(b, "1 NEEDS YOU");
    CHECK(reply_needs_caption(true, 250, b, sizeof b) > 0);
    CHECK_STR(b, "99 NEEDS YOU");                        /* capped */
    CHECK(reply_needs_caption(true, 0, b, sizeof b) == 0);
    CHECK_STR(b, "");
    CHECK(reply_needs_caption(true, -2, b, sizeof b) == 0);
    strcpy(b, "x");
    CHECK(reply_needs_caption(false, 5, b, sizeof b) == 0);   /* flag off: nothing */
    CHECK_STR(b, "");
    char tiny[6];
    CHECK(reply_needs_caption(true, 4, tiny, sizeof tiny) == 0);   /* does not fit: empty, never cut */
    CHECK_STR(tiny, "");
    CHECK(reply_needs_caption(true, 4, NULL, 0) == 0);
}

int main(void)
{
    test_parse();
    test_parse_lengths();
    test_binding();
    test_outcomes();
    test_layout();
    test_frames();
    test_entry_points();
    printf("%d checks, %d failed\n", s_n, s_fail);
    return s_fail ? 1 : 0;
}
