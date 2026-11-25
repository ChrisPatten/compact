#include "gap_text.h"
#include <stdio.h>

void gap_free_text(char *b, size_t n, int free_min)
{
    if (!n) return;
    if (free_min < 0) free_min = 0;
    if (free_min < 120) snprintf(b, n, "%d MIN FREE", free_min);
    else if (free_min % 60 == 0) snprintf(b, n, "%d H FREE", free_min / 60);
    else snprintf(b, n, "%d H %d MIN FREE", free_min / 60, free_min % 60);
}
