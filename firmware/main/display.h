#pragma once
#include <stdbool.h>
#include "screens.h"

void display_init(void);
// Synchronous: load and draw. Used on timer/button wakes where nothing else is running.
bool display_show(const char *id);
bool display_next(void); // false if there was nothing showable for the current orientation
void display_screen(screen_t s);
// Asynchronous: queued to the display task so HTTP handlers and the button loop return immediately.
void display_task_start(void);
void display_request(const char *id); // NULL = next photo
void display_request_screen(screen_t s);
bool display_busy(void);
bool display_known(void); // false after power-on: we don't know what the panel shows
screen_t display_shown_screen(void); // SCR_NONE when a photo is up
const char *display_current(void);
