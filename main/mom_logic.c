#include "mom_logic.h"

void mom_scan(const rim_in_t *in, int n, time_t now, time_t d0, time_t d1, int gap_min, mom_scan_t *o)
{
    o->next = o->busy_next = o->running = o->free_running = -1;
    o->gap_from = 0;
    o->upcoming = 0;
    for (int i = 0; i < n; i++) {
        const rim_in_t *m = &in[i];
        time_t end = m->has_end ? m->end : m->start;
        if (m->start > now) {
            bool win = rim_in_window(now, gap_min, m->start);
            if (o->next < 0 && win) o->next = i;                              /* in start order */
            if (o->busy_next < 0 && win && !m->free) o->busy_next = i;
            if (m->start < d1) o->upcoming++;
        } else if (end > now) {
            if (m->free) {
                if (o->free_running < 0 || end < in[o->free_running].end) o->free_running = i;
            } else if (o->running < 0 || end < in[o->running].end) o->running = i;
        } else if (!m->free && end >= d0 && end > o->gap_from) o->gap_from = end;
    }
}

int mom_overlap_min(const rim_in_t *a, const rim_in_t *b)
{
    if (a->free || b->free || !a->has_end || !b->has_end) return 0;
    time_t s = a->start > b->start ? a->start : b->start, e = a->end < b->end ? a->end : b->end;
    return e > s ? (int)((e - s + 59) / 60) : 0;
}

int mom_gap_min(const rim_in_t *next_row, time_t max_busy_end)
{
    if (next_row->free || max_busy_end <= 0) return 0;      /* no busy row before it yet: nothing to be free of */
    return (int)((next_row->start - max_busy_end) / 60);
}
