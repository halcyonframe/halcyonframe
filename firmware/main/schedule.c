#include "schedule.h"
#include <stdio.h>
#include <stdlib.h>

#define EARLY 5     // seconds: a timer wake a little early still counts as on time
#define SLOT_LATE 120 // a status slot is still shown if we wake up to 2 min after it

static int minute_of_day(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

// Next local time hh:mm (given as minutes after midnight) strictly after `from`. mktime normalises
// day overflow and DST.
static time_t next_local(time_t from, int minute) {
    struct tm tm;
    localtime_r(&from, &tm);
    tm.tm_hour = minute / 60, tm.tm_min = minute % 60, tm.tm_sec = 0, tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t <= from) {
        tm.tm_mday++;
        tm.tm_hour = minute / 60, tm.tm_min = minute % 60, tm.tm_sec = 0, tm.tm_isdst = -1;
        t = mktime(&tm);
    }
    return t;
}

bool sched_quiet(const sched_t *s, time_t t) {
    if (!s->clock_ok || s->quiet_start == s->quiet_end) return false;
    int m = minute_of_day(t);
    return s->quiet_start < s->quiet_end ? m >= s->quiet_start && m < s->quiet_end  // e.g. 13:00-14:00
                                         : m >= s->quiet_start || m < s->quiet_end; // e.g. 23:00-07:00
}

time_t sched_next_change(const sched_t *s, time_t now) {
    if (s->clock_ok && s->change_times && s->change_times[0]) {
        time_t best = 0;
        for (const char *p = s->change_times; *p;) {
            int h, m, n = 0;
            if (sscanf(p, "%d:%d%n", &h, &m, &n) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
                time_t t = next_local(now, h * 60 + m);
                if (!best || t < best) best = t;
            }
            p += n ? n : 1;
            while (*p == ',' || *p == ' ') p++;
        }
        if (best) return best;
    }
    return s->interval_min > 0 ? now + s->interval_min * 60 : 0;
}

// Slots are aligned to the local clock (:00, :30), so zones with :30 or :45 offsets work too.
time_t sched_slot(const sched_t *s, time_t now) {
    struct tm tm;
    localtime_r(&now, &tm);
    int period = s->status_every * 60, into = (tm.tm_min * 60 + tm.tm_sec) % period;
    return now - into;
}

sched_action_t sched_due(const sched_t *s, time_t now) {
    if (s->status_until) return now >= s->status_until - EARLY ? SCHED_END_STATUS : SCHED_NOTHING;
    if (sched_quiet(s, now)) return SCHED_NOTHING;
    if (s->status_every && s->clock_ok) {
        // A slot reached by waking slightly early counts as the upcoming one.
        time_t slot = sched_slot(s, now + EARLY);
        if (slot != s->last_slot && now + EARLY - slot < SLOT_LATE) return SCHED_SHOW_STATUS;
    }
    if (s->next_photo && now >= s->next_photo - EARLY) return SCHED_NEXT_PHOTO;
    return SCHED_NOTHING;
}

// Quiet hours postpone anything that would fall inside them to the end of the quiet period.
static time_t after_quiet(const sched_t *s, time_t t) { return sched_quiet(s, t) ? next_local(t, s->quiet_end) : t; }

static void consider(time_t *best, time_t t) {
    if (t && (!*best || t < *best)) *best = t;
}

time_t sched_next_wake(const sched_t *s, time_t now) {
    time_t best = 0;
    if (s->status_until) consider(&best, s->status_until); // always end the status screen on time
    if (s->next_photo) consider(&best, after_quiet(s, s->next_photo > now ? s->next_photo : now));
    if (s->status_every && s->clock_ok) {
        time_t slot = sched_slot(s, now + EARLY);
        if (slot <= s->last_slot || slot <= now - SLOT_LATE + EARLY) slot += s->status_every * 60;
        // Skip slots in quiet hours (step through them; at most a day's worth).
        for (int i = 0; i < 48 && sched_quiet(s, slot); i++) slot += s->status_every * 60;
        consider(&best, slot);
    }
    return best;
}
