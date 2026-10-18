// Host tests for main/astro.c: cc -I main tools/test_astro.c main/astro.c -lm && ./a.out
// Reference times are the commonly published values for these dates (rounded to the minute); the
// algorithm is good to ~2 min, so the tolerance is 3.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "astro.h"

static time_t local(int y, int mo, int d, int h, int mi) {
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1};
    return mktime(&tm);
}

static void near(const char *what, time_t got, time_t want, int tol_min) {
    struct tm g;
    localtime_r(&got, &g);
    long diff = (long) (got - want) / 60;
    printf("  %-28s %02d:%02d  (%+ld min)\n", what, g.tm_hour, g.tm_min, diff);
    assert(labs(diff) <= tol_min);
}

int main(void) {
    // Chicago, 2024-06-20 (solstice): sunrise 5:15, sunset 20:29 CDT
    setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
    tzset();
    sun_day_t s = astro_sun(local(2024, 6, 20, 12, 0), 41.8781, -87.6298);
    near("Chicago solstice sunrise", s.rise, local(2024, 6, 20, 5, 15), 3);
    near("Chicago solstice sunset", s.set, local(2024, 6, 20, 20, 29), 3);
    // Chicago, 2024-12-21: sunrise 7:15, sunset 16:23 CST
    s = astro_sun(local(2024, 12, 21, 12, 0), 41.8781, -87.6298);
    near("Chicago winter sunrise", s.rise, local(2024, 12, 21, 7, 15), 3);
    near("Chicago winter sunset", s.set, local(2024, 12, 21, 16, 23), 3);

    // Sydney, 2024-12-21: sunrise 5:41, sunset 20:05 AEDT (southern hemisphere, east of Greenwich)
    setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
    tzset();
    s = astro_sun(local(2024, 12, 21, 12, 0), -33.8688, 151.2093);
    near("Sydney summer sunrise", s.rise, local(2024, 12, 21, 5, 41), 3);
    near("Sydney summer sunset", s.set, local(2024, 12, 21, 20, 5), 3);

    // Tromso in June: midnight sun
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    s = astro_sun(local(2024, 6, 20, 12, 0), 69.6492, 18.9553);
    assert(s.rise == 0 && s.set == 0 && s.up_all_day);
    printf("  Tromso June                  sun up all day\n");

    // Moon: new moon 2024-01-11 11:57 UTC, full moon 2024-01-25 17:54 UTC, first quarter 2024-01-18 03:53 UTC
    setenv("TZ", "UTC0", 1);
    tzset();
    moon_t m = astro_moon(local(2024, 1, 11, 11, 57));
    printf("  new moon    phase %.3f lit %.2f %s\n", m.phase, m.lit, m.name);
    assert((m.phase < 0.03 || m.phase > 0.97) && m.lit < 0.02);
    m = astro_moon(local(2024, 1, 25, 17, 54));
    printf("  full moon   phase %.3f lit %.2f %s\n", m.phase, m.lit, m.name);
    assert(fabs(m.phase - 0.5) < 0.03 && m.lit > 0.98);
    m = astro_moon(local(2024, 1, 18, 3, 53));
    printf("  1st quarter phase %.3f lit %.2f %s\n", m.phase, m.lit, m.name);
    assert(fabs(m.phase - 0.25) < 0.03);

    puts("astro tests passed");
    return 0;
}
