#include "touch_logic.h"

bool touch_beyond_deadzone(int x0, int y0, int x, int y)
{
    int dx = x > x0 ? x - x0 : x0 - x, dy = y > y0 ? y - y0 : y0 - y;
    return (dx > dy ? dx : dy) >= TOUCH_DEADZONE_PX;
}

touch_class_t touch_classify(int x0, int y0, int x, int y, uint32_t elapsed_ms, uint32_t hold_ms, bool moved)
{
    if (moved || touch_beyond_deadzone(x0, y0, x, y)) return TOUCH_SWIPE;
    if (hold_ms && elapsed_ms >= hold_ms) return TOUCH_HOLD;
    return TOUCH_TAP;
}
