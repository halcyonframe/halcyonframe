#pragma once
// Hooks main.c exposes to the web server.
void app_touch(void);         // reset the inactivity-sleep timer
void app_request_sleep(void); // deep sleep once any refresh finishes
void app_request_reboot(void);
