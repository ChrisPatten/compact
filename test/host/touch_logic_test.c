/* Host test for main/touch_logic.c. Run through tools/host_tests.sh, or:
   cc -O1 -g -std=c11 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -Imain test/host/touch_logic_test.c main/touch_logic.c -o /tmp/touch_logic_test && /tmp/touch_logic_test */
#include <stdio.h>
#include "touch_logic.h"

static int s_fail, s_n;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define D TOUCH_DEADZONE_PX

int main(void)
{
    CHECK(D == 10);
    /* inside the dead zone: jitter in every direction is a tap, or a hold once the time is up */
    for (int dx = -(D - 1); dx <= D - 1; dx++)
        for (int dy = -(D - 1); dy <= D - 1; dy++) {
            CHECK(!touch_beyond_deadzone(200, 200, 200 + dx, 200 + dy));
            CHECK(touch_classify(200, 200, 200 + dx, 200 + dy, 100, 800, false) == TOUCH_TAP);
            CHECK(touch_classify(200, 200, 200 + dx, 200 + dy, 800, 800, false) == TOUCH_HOLD);
            CHECK(touch_classify(200, 200, 200 + dx, 200 + dy, 5000, 0, false) == TOUCH_TAP);   /* no hold on this target */
        }
    /* at or beyond the dead zone on either axis: a swipe, at any time, in any direction */
    CHECK(touch_beyond_deadzone(200, 200, 200 + D, 200));
    CHECK(touch_beyond_deadzone(200, 200, 200 - D, 200));
    CHECK(touch_beyond_deadzone(200, 200, 200, 200 + D));
    CHECK(touch_beyond_deadzone(200, 200, 200, 200 - D));
    CHECK(touch_classify(200, 200, 200, 200 + D, 50, 800, false) == TOUCH_SWIPE);
    CHECK(touch_classify(200, 200, 200 - D, 200, 5000, 800, false) == TOUCH_SWIPE);   /* a long press that drifted is not a hold */
    CHECK(touch_classify(200, 200, 200 + 233, 200, 300, 800, false) == TOUCH_SWIPE);
    /* the diagonal counts per axis, not by length: 9 and 9 is still jitter (12.7 px long), 10 and 0 is not */
    CHECK(touch_classify(0, 0, 9, 9, 10, 800, false) == TOUCH_TAP);
    CHECK(touch_classify(0, 0, 10, 0, 10, 800, false) == TOUCH_SWIPE);
    /* latched: an excursion that came back to the press point stays a swipe */
    CHECK(touch_classify(200, 200, 200, 200, 100, 800, true) == TOUCH_SWIPE);
    CHECK(touch_classify(200, 200, 201, 200, 900, 800, true) == TOUCH_SWIPE);
    /* just short of the hold time is still a tap */
    CHECK(touch_classify(1, 1, 3, 2, 799, 800, false) == TOUCH_TAP);
    printf("%d checks, %d failed\n", s_n, s_fail);
    return s_fail ? 1 : 0;
}
