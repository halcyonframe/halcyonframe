// Sun and moon for the status cards, computed on the frame (no internet). Pure math; host-tested
// (tools/test_astro.c).
#pragma once
#include <stdbool.h>
#include <time.h>

typedef struct {
    time_t rise, set;   // 0 when the sun doesn't rise or set that day (polar day/night)
    bool up_all_day;    // polar day; otherwise polar night when rise == set == 0
} sun_day_t;

// Sunrise and sunset on the local calendar day containing `when` (time zone from TZ). Accuracy ~1-2 min.
sun_day_t astro_sun(time_t when, double lat, double lon);

typedef struct {
    double phase;       // 0 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter
    double lit;         // illuminated fraction 0..1
    const char *name;   // "Waxing gibbous" etc.
} moon_t;

moon_t astro_moon(time_t when);
