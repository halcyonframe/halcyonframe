#include "astro.h"
#include <math.h>

#define RAD (M_PI / 180)

static double norm(double v, double range) {
    v = fmod(v, range);
    return v < 0 ? v + range : v;
}

// UTC hour of sunrise or sunset on day-of-year `doy` (sunrise equation from the US Naval Observatory's
// Almanac for Computers; zenith 90.833 deg allows for refraction and the sun's radius). NAN when the event
// doesn't happen; *always_up tells polar day from polar night.
static double event_utc(int doy, double lat, double lon, bool rise, bool *always_up) {
    double lng_hour = lon / 15, t = doy + ((rise ? 6 : 18) - lng_hour) / 24;
    double m = 0.9856 * t - 3.289;
    double l = norm(m + 1.916 * sin(m * RAD) + 0.020 * sin(2 * m * RAD) + 282.634, 360);
    double ra = norm(atan(0.91764 * tan(l * RAD)) / RAD, 360);
    ra = (ra + floor(l / 90) * 90 - floor(ra / 90) * 90) / 15; // same quadrant as l, in hours
    double sin_dec = 0.39782 * sin(l * RAD), cos_dec = cos(asin(sin_dec));
    double cos_h = (cos(90.833 * RAD) - sin_dec * sin(lat * RAD)) / (cos_dec * cos(lat * RAD));
    if (cos_h > 1 || cos_h < -1) {
        *always_up = cos_h < -1;
        return NAN;
    }
    double h = (rise ? 360 - acos(cos_h) / RAD : acos(cos_h) / RAD) / 15;
    return norm(h + ra - 0.06571 * t - 6.622 - lng_hour, 24);
}

sun_day_t astro_sun(time_t when, double lat, double lon) {
    struct tm local;
    localtime_r(&when, &local);
    // Local midnight; the events are placed relative to it so they land on the local day.
    struct tm mid = local;
    mid.tm_hour = mid.tm_min = mid.tm_sec = 0, mid.tm_isdst = -1;
    time_t day0 = mktime(&mid);
    sun_day_t s = {0};
    bool up = false;
    for (int i = 0; i < 2; i++) {
        double ut = event_utc(local.tm_yday + 1, lat, lon, i == 0, &up);
        if (isnan(ut)) return (sun_day_t) {0, 0, up};
        // UTC hour -> the instant on this local day: start from local midnight's UTC date and wrap.
        struct tm u;
        gmtime_r(&day0, &u);
        time_t utc_mid = day0 - (u.tm_hour * 3600 + u.tm_min * 60 + u.tm_sec);
        time_t t = utc_mid + (time_t) (ut * 3600);
        while (t < day0) t += 86400;
        while (t >= day0 + 86400) t -= 86400;
        if (i == 0) s.rise = t;
        else s.set = t;
    }
    return s;
}

moon_t astro_moon(time_t when) {
    // Mean synodic month, counted from the new moon of 2000-01-06 18:14 UTC. The real moon runs up to
    // ~14 h ahead of or behind this mean; fine for a phase name and a picture.
    const double synodic = 29.530588853, epoch = 947182440;
    double phase = norm((when - epoch) / 86400.0 / synodic, 1);
    static const char *const names[] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                                        "Full moon", "Waning gibbous", "Last quarter", "Waning crescent"};
    return (moon_t) {phase, (1 - cos(2 * M_PI * phase)) / 2, names[(int) floor(phase * 8 + 0.5) % 8]};
}
