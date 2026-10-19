// Wall-clock time and room climate. The PCF85063A RTC keeps time (in UTC) through deep sleep and power-off;
// the time itself comes from the browser whenever the web UI is opened (no internet needed), or from an
// optional NTP server. The SHTC3 measures temperature and humidity. Both chips share the board's I2C bus.
#pragma once
#include <stdbool.h>
#include <time.h>

void clock_init(const char *posix_tz); // after pmic_init(); restores the time from the RTC after power loss
bool clock_valid(void);                // false until something has set the time
void clock_set(time_t utc);            // system clock + RTC
void clock_start_ntp(const char *server); // no-op for ""; call once Wi-Fi is connected
bool clock_climate(float *temp_c, float *rh);
// auto_setup on: NTP (pool.ntp.org unless ntp_server is set) plus a one-off IP lookup that sets the time zone
// and a city-level location. Call once Wi-Fi is connected (not on the frame's own hotspot).
void clock_auto_setup(void);
// Hush just went on: stop the running time sync (a lookup in flight discards its answer).
void clock_go_offline(void);
