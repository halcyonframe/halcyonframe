#pragma once
#include <stdint.h>

// Fixed-cell bitmap font covering printable ASCII (32..126) plus the degree sign as glyph 127 (FONT_DEGREE);
// rows are MSB-first, w/8 bytes each.
#define FONT_DEGREE 127
typedef struct {
    uint8_t w, h;
    const uint8_t *bits;
} font_t;

extern const font_t font_16x32; // body text: ~6 mm tall on the panel, readable at arm's length
extern const font_t font_32x64; // headlines: readable across a room
