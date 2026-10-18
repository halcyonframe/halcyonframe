#pragma once
#include <stdbool.h>
#include <stdint.h>

// Panel color codes (the 4-bit values the controller expects). Code 4 is unused on Spectra 6.
enum { EPD_BLACK = 0, EPD_WHITE = 1, EPD_YELLOW = 2, EPD_RED = 3, EPD_BLUE = 5, EPD_GREEN = 6 };

void epd_init(void);
// buf is EPD_BUF_SIZE bytes in logical (viewer) orientation. Blocks for the full refresh (~20 s).
void epd_show(const uint8_t *buf, bool rotate180);
void epd_clear(uint8_t color);
