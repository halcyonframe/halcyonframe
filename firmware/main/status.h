// The status screen: clock, date and four configurable cards, in a chosen typeface. Port of the approved
// design in tools/status_preview.py. Host-buildable.
#pragma once
#include "screens.h"

typedef enum {
    CARD_INDOOR, CARD_BATTERY, CARD_PHOTOS, CARD_WIFI, CARD_SUN, CARD_MOON, CARD_DAYLIGHT, CARD_NEXT, CARD_COUNT
} card_t;

extern const char *const CARD_NAMES[CARD_COUNT]; // config.txt / API spelling: indoor, battery, ...

void status_render(canvas_t *c, const screen_info_t *in);
