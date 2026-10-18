#pragma once
#include <stdbool.h>

void display_init(void);
// Synchronous: load and draw. Used on timer/button wakes where nothing else is running.
bool display_show(const char *id);
bool display_next(void);
// Asynchronous: queued to the display task so HTTP handlers return immediately.
void display_task_start(void);
void display_request(const char *id); // NULL = next photo
bool display_busy(void);
const char *display_current(void);
