/* Day list gap row wording. Pure, host tested (test/host/gap_text_test.c). */
#pragma once
#include <stddef.h>

/* "45 MIN FREE", "2 H FREE", "1 H 15 MIN FREE": under two hours minutes only; from two hours on hours, and minutes only
   when non-zero. `free_min` < 0 counts as 0. Always NUL terminated (n >= 1). */
void gap_free_text(char *b, size_t n, int free_min);
