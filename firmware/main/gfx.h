// Minimal drawing into the packed 4bpp panel frame, in the viewer's orientation.
// Plain C with no ESP-IDF dependencies apart from the QR component, so it also builds on a PC
// (tools/render_screens.c) for checking layouts without a 20 s panel refresh.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "font.h"

// How the frame hangs. cw/ccw match pack() in www/index.html: cw = frame turned right (panel top on the
// viewer's right), ccw = frame turned left.
typedef enum { ORIENT_LANDSCAPE = 0, ORIENT_CW = 1, ORIENT_CCW = 2 } orient_t;

typedef struct {
    uint8_t *buf; // EPD_BUF_SIZE bytes, panel layout
    int w, h;     // logical size: 800x480 landscape, 480x800 portrait
    orient_t orient;
} canvas_t;

void gfx_begin(canvas_t *c, uint8_t *buf, orient_t orient);
void gfx_fill(canvas_t *c, uint8_t color);
void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint8_t color);
void gfx_frame(canvas_t *c, int x, int y, int w, int h, int thick, uint8_t color); // outline only
void gfx_disc(canvas_t *c, int cx, int cy, int r, uint8_t color);
void gfx_line(canvas_t *c, int x0, int y0, int x1, int y1, int thick, uint8_t color);
// Draws UTF-8 text; characters outside printable ASCII render as '?'. Returns the width drawn.
int gfx_text(canvas_t *c, int x, int y, const font_t *f, const char *s, uint8_t color);
int gfx_text_width(const font_t *f, const char *s);
// QR code with a white quiet zone of 4 modules on each side. Returns the drawn size in pixels, 0 on error.
// Pass c = NULL to only measure.
int gfx_qr(canvas_t *c, int x, int y, int module_px, const char *text);
