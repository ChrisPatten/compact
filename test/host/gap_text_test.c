/* Host test for main/gap_text.c. Run through tools/host_tests.sh. */
#include <stdio.h>
#include <string.h>
#include "gap_text.h"

static int s_fail, s_n;
static void eq(int m, const char *want)
{
    char b[32]; gap_free_text(b, sizeof b, m); s_n++;
    if (strcmp(b, want)) { s_fail++; printf("FAIL %d min: got '%s' want '%s'\n", m, b, want); }
}

int main(void)
{
    eq(30, "30 MIN FREE"); eq(45, "45 MIN FREE"); eq(119, "119 MIN FREE");
    eq(120, "2 H FREE"); eq(121, "2 H 1 MIN FREE"); eq(135, "2 H 15 MIN FREE"); eq(180, "3 H FREE"); eq(605, "10 H 5 MIN FREE");
    eq(-5, "0 MIN FREE");
    char t[6]; gap_free_text(t, sizeof t, 135); s_n++;
    if (strlen(t) != 5) { s_fail++; printf("FAIL truncation\n"); }
    printf("gap_text_test: %d checks, %d failed\n", s_n, s_fail);
    return s_fail ? 1 : 0;
}
