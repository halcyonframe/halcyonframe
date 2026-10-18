// Host tests for main/schedule.c: cc -I main tools/test_schedule.c main/schedule.c && ./a.out
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "schedule.h"

static time_t at(int y, int mo, int d, int h, int mi) { // local time
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1};
    return mktime(&tm);
}
static void show(const char *what, time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    char b[32];
    strftime(b, sizeof b, "%m-%d %H:%M", &tm);
    printf("  %-34s %s\n", what, t ? b : "none");
}

int main(void) {
    setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
    tzset();
    sched_t s = {.clock_ok = true, .interval_min = 60, .status_every = 60, .status_min = 5,
                 .quiet_start = 23 * 60, .quiet_end = 7 * 60};

    // 10:20: next status slot 11:00; photo due 11:20 -> wake at 11:00
    time_t now = at(2026, 10, 18, 10, 20);
    s.next_photo = sched_next_change(&s, now);
    assert(s.next_photo == at(2026, 10, 18, 11, 20));
    assert(sched_due(&s, now) == SCHED_NOTHING);
    assert(sched_next_wake(&s, now) == at(2026, 10, 18, 11, 0));

    // Woken 3 s early for 11:00 -> show status; the status ends at 11:05
    now = at(2026, 10, 18, 11, 0) - 3;
    assert(sched_due(&s, now) == SCHED_SHOW_STATUS);
    s.last_slot = sched_slot(&s, now + 5);
    s.status_until = now + 5 * 60;
    assert(sched_next_wake(&s, now) == s.status_until);
    assert(sched_due(&s, s.status_until) == SCHED_END_STATUS);
    s.status_until = 0;
    // Same slot isn't shown twice
    assert(sched_due(&s, at(2026, 10, 18, 11, 1)) == SCHED_NOTHING);
    assert(sched_next_wake(&s, at(2026, 10, 18, 11, 5)) == at(2026, 10, 18, 11, 20));

    // Half-hour slots
    s.status_every = 30;
    assert(sched_next_wake(&s, at(2026, 10, 18, 11, 5)) == at(2026, 10, 18, 11, 20)); // photo first
    s.next_photo = at(2026, 10, 18, 12, 20);
    assert(sched_next_wake(&s, at(2026, 10, 18, 11, 5)) == at(2026, 10, 18, 11, 30));
    s.status_every = 60;

    // Quiet hours: 22:30 photo due at 23:10 -> pushed to 07:00; 23:00 slot skipped, next slot 07:00
    now = at(2026, 10, 18, 22, 30);
    s.last_slot = at(2026, 10, 18, 22, 0);
    s.next_photo = at(2026, 10, 18, 23, 10);
    assert(sched_next_wake(&s, now) == at(2026, 10, 19, 7, 0));
    assert(sched_due(&s, at(2026, 10, 18, 23, 10)) == SCHED_NOTHING);
    assert(sched_quiet(&s, at(2026, 10, 19, 6, 59)) && !sched_quiet(&s, at(2026, 10, 19, 7, 0)));

    // Set change times beat the interval and wrap to tomorrow
    s.change_times = "07:00, 19:30";
    assert(sched_next_change(&s, at(2026, 10, 18, 12, 0)) == at(2026, 10, 18, 19, 30));
    assert(sched_next_change(&s, at(2026, 10, 18, 20, 0)) == at(2026, 10, 19, 7, 0));
    s.change_times = "garbage";
    assert(sched_next_change(&s, at(2026, 10, 18, 12, 0)) == at(2026, 10, 18, 13, 0)); // falls back to interval

    // Clock never set: no wall-clock features, interval still works
    sched_t raw = {.clock_ok = false, .interval_min = 60, .status_every = 60, .change_times = "07:00",
                   .quiet_start = 0, .quiet_end = 1439};
    assert(sched_next_change(&raw, 1000) == 1000 + 3600);
    raw.next_photo = 4600;
    assert(sched_next_wake(&raw, 1000) == 4600 && sched_due(&raw, 4600) == SCHED_NEXT_PHOTO);

    // DST end (Nov 1 2026, 02:00 -> 01:00): daily 07:00 change stays at 07:00 local
    s.change_times = "07:00";
    s.quiet_start = s.quiet_end = 0;
    assert(sched_next_change(&s, at(2026, 10, 31, 8, 0)) == at(2026, 11, 1, 7, 0));
    show("DST: next 07:00 after Oct 31 08:00", sched_next_change(&s, at(2026, 10, 31, 8, 0)));

    // Nothing scheduled
    sched_t none = {.clock_ok = true};
    assert(sched_next_wake(&none, now) == 0);

    puts("schedule tests passed");
    return 0;
}
