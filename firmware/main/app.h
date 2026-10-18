#pragma once
#include <stdbool.h>
#include <time.h>

// Hooks main.c exposes to the web server and the display.
void app_touch(void);              // reset the inactivity-sleep timer
void app_request_sleep(void);      // deep sleep once any refresh finishes
void app_request_reboot(void);
void app_schedule_changed(void);   // settings or clock changed: recompute the next photo change
time_t app_next_photo(void);       // next scheduled photo change (0 = none), for the status screen
void app_show_status(void);        // put the status screen up now (web UI preview of typeface/cards)
bool app_boot_screen(void);        // the start-up info screen is (still) up
