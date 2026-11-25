/* cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain test/host/cmd_logic_test.c main/cmd_logic.c -o /tmp/cmd_logic_test && /tmp/cmd_logic_test */
#include <stdio.h>
#include <string.h>
#include "cmd_logic.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    char b[512], op[24];
    /* escape */
    cmd_json_escape("a\"b\\c\nd", b, sizeof b);
    CHECK(!strcmp(b, "a\\\"b\\\\c\\u000ad"));
    cmd_json_escape("0123456789", b, 6);
    CHECK(strlen(b) == 5);
    cmd_json_escape("\"\"\"\"", b, 6);                              /* never cuts an escape in half */
    CHECK(!strcmp(b, "\\\"\\\""));

    /* ttl */
    CHECK(cmd_clamp_ttl(0, false) == 120 && cmd_clamp_ttl(-5, true) == 120 && cmd_clamp_ttl(30, true) == 30);
    CHECK(cmd_clamp_ttl(99999, true) == 3600);
    CHECK(!cmd_expired(1000, 1000 + 120000, 120));                  /* exactly ttl: still fine */
    CHECK(cmd_expired(1000, 1000 + 120001, 120));
    CHECK(cmd_expired(0, 5001, 5) && !cmd_expired(0, 5000, 5));
    CHECK(!cmd_expired(0, 119000, 0) && cmd_expired(0, 121000, 0)); /* 0 = default */

    /* frames */
    int n = cmd_result_frame(b, sizeof b, "abc-1", true, NULL, "{\"x\":1}");
    CHECK(n > 0 && !strcmp(b, "{\"type\":\"command_result\",\"id\":\"abc-1\",\"ok\":true,\"data\":{\"x\":1}}"));
    n = cmd_result_frame(b, sizeof b, "abc-2", true, NULL, NULL);
    CHECK(n > 0 && strstr(b, "\"data\":{}"));
    n = cmd_result_frame(b, sizeof b, "abc-3", false, "bad_args", "use \"start\" [n]");
    CHECK(n > 0 && !strcmp(b, "{\"type\":\"command_result\",\"id\":\"abc-3\",\"ok\":false,\"reason\":\"bad_args\",\"data\":\"use \\\"start\\\" [n]\"}"));
    CHECK(cmd_result_frame(b, 20, "abc", true, NULL, "{}") == -1);

    /* split */
    const char *a = cmd_split("  pomodoro   start 25 5", op, sizeof op);
    CHECK(!strcmp(op, "pomodoro") && !strcmp(a, "start 25 5"));
    a = cmd_split("chime", op, sizeof op);
    CHECK(!strcmp(op, "chime") && !*a);
    a = cmd_split("averyveryverylongopnamethatoverflows x", op, 8);
    CHECK(strlen(op) == 7 && !strcmp(a, "x"));

    /* dedupe: 16 ids, oldest evicted, repeats found */
    static cmd_dedupe_t d;
    char id[16], fr[48];
    CHECK(cmd_dedupe_find(&d, "x") == NULL);
    for (int i = 0; i < 16; i++) { snprintf(id, sizeof id, "id%d", i); snprintf(fr, sizeof fr, "frame%d", i); CHECK(cmd_dedupe_store(&d, id, fr)); }
    for (int i = 0; i < 16; i++) { snprintf(id, sizeof id, "id%d", i); snprintf(fr, sizeof fr, "frame%d", i); const char *f = cmd_dedupe_find(&d, id); CHECK(f && !strcmp(f, fr)); }
    CHECK(cmd_dedupe_store(&d, "id16", "frame16"));                /* evicts id0 */
    CHECK(cmd_dedupe_find(&d, "id0") == NULL);
    CHECK(cmd_dedupe_find(&d, "id1") && cmd_dedupe_find(&d, "id16"));
    CHECK(cmd_dedupe_store(&d, "id1", "again"));                   /* same id replaces in place, evicts nothing */
    CHECK(!strcmp(cmd_dedupe_find(&d, "id1"), "again") && cmd_dedupe_find(&d, "id2") && cmd_dedupe_find(&d, "id16"));
    CHECK(cmd_dedupe_store(&d, "id17", "f17"));                    /* now the oldest is id2 */
    CHECK(cmd_dedupe_find(&d, "id2") == NULL && cmd_dedupe_find(&d, "id3"));
    CHECK(!cmd_dedupe_store(&d, "", "x"));
    CHECK(!cmd_dedupe_store(&d, "0123456789012345678901234567890123456789012345", "x"));
    cmd_dedupe_clear(&d);
    CHECK(cmd_dedupe_find(&d, "id3") == NULL);

    printf(fails ? "%d FAILED\n" : "cmd_logic ok\n", fails);
    return fails != 0;
}
