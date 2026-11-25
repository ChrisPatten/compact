#pragma once
/*
 * What the Now face and the Day list make of a list of Moments, with FREE Moments (bus `free`: true) taken out of the
 * busy picture: pure logic, no LVGL, host tested (test/host/mom_logic_test.c). Inputs are rim_in_t (rim12.h) in start
 * order. A free Moment still happens (it is the `next` heading, a rim mark, a Day row) but it never makes the device
 * read busy, never ends or starts a free stretch and never overlaps anything.
 */
#include <stdbool.h>
#include <time.h>
#include "rim12.h"

typedef struct {
    int next;            /* the window's next Moment, busy or free: names the NEXT heading and the title; -1 none */
    int busy_next;       /* the window's next busy Moment: the upper arc's target; -1 none */
    int running;         /* a busy Moment running now (BUSY UNTIL, the arc); the earliest end wins; -1 none */
    int free_running;    /* a free Moment with an end running now, with nothing busy implied; -1 none */
    time_t gap_from;     /* end of the latest busy Moment already over today, 0 none: where the free stretch began */
    int upcoming;        /* Moments still ahead today (busy and free) */
} mom_scan_t;

/* d1: the end of today. gap_min: the rim gap (the window is 12 h minus it). */
void mom_scan(const rim_in_t *in, int n, time_t now, time_t d0, time_t d1, int gap_min, mom_scan_t *o);

/* Minutes two Moments overlap (rounded up), 0 when either is free or has no end: a free Moment is not a conflict. */
int mom_overlap_min(const rim_in_t *a, const rim_in_t *b);

/* The free gap row after a list row: minutes from the latest busy end so far to the next row's start, 0 when the next
   row is itself free (the gap is shown before the next busy row, so a free Moment does not interrupt it) and 0 while no busy
   row has come yet (max_busy_end 0), as the list never opened with a gap row. */
int mom_gap_min(const rim_in_t *next_row, time_t max_busy_end);
