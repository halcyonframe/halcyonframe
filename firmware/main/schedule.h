// What the frame should do now, and when it should next wake: photo changes (every N minutes, or at set
// local times), the timed status screen (at :00, or :00 and :30), and quiet hours with no changes at all.
// Pure logic on time_t + the local time zone, so it builds and is tested on a PC (tools/test_schedule.c).
#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    // settings
    bool clock_ok;              // set times, status slots and quiet hours need a wall clock that has been set
    int interval_min;           // photo change interval; 0 = only on request
    const char *change_times;   // "07:00,19:30" local; replaces interval_min when non-empty (and clock_ok)
    int quiet_start, quiet_end; // minutes after local midnight; equal = no quiet hours
    int status_every;           // 0 = off, 60 = at :00, 30 = at :00 and :30
    int status_min;             // how long the status screen stays up
    // state, kept across deep sleep by the caller
    time_t next_photo;          // next photo change due (0 = not scheduled yet)
    time_t status_until;        // status screen up until then (0 = not showing)
    time_t last_slot;           // status slot already handled
} sched_t;

typedef enum { SCHED_NOTHING, SCHED_NEXT_PHOTO, SCHED_SHOW_STATUS, SCHED_END_STATUS } sched_action_t;

sched_action_t sched_due(const sched_t *s, time_t now);
time_t sched_slot(const sched_t *s, time_t now);        // the status slot that SCHED_SHOW_STATUS refers to
time_t sched_next_change(const sched_t *s, time_t now); // next photo change after now (0 = none)
time_t sched_next_wake(const sched_t *s, time_t now);   // absolute time for the next timer wake (0 = none)
bool sched_quiet(const sched_t *s, time_t t);
