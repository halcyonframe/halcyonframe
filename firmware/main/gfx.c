#include "gfx.h"
#include "epd.h"
#include <stdlib.h>
#include <string.h>
#include "qrcode.h"

#define PANEL_W 800
#define PANEL_H 480

void gfx_begin(canvas_t *c, uint8_t *buf, orient_t orient) {
    c->buf = buf;
    c->orient = orient;
    c->w = orient == ORIENT_LANDSCAPE ? PANEL_W : PANEL_H;
    c->h = orient == ORIENT_LANDSCAPE ? PANEL_H : PANEL_W;
}

// Logical (viewer) coordinates -> panel pixel. Inverse of pack() in www/index.html.
static inline void put(canvas_t *c, int x, int y, uint8_t color) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    int px, py;
    switch (c->orient) {
    case ORIENT_CW: px = PANEL_W - 1 - y; py = x; break;
    case ORIENT_CCW: px = y; py = PANEL_H - 1 - x; break;
    default: px = x; py = y; break;
    }
    uint8_t *b = &c->buf[(py * PANEL_W + px) >> 1];
    *b = (px & 1) ? (*b & 0xF0) | color : (*b & 0x0F) | (color << 4);
}

void gfx_fill(canvas_t *c, uint8_t color) {
    memset(c->buf, color << 4 | color, PANEL_W * PANEL_H / 2);
}

void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint8_t color) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(c, i, j, color);
}

void gfx_frame(canvas_t *c, int x, int y, int w, int h, int t, uint8_t color) {
    gfx_rect(c, x, y, w, t, color);
    gfx_rect(c, x, y + h - t, w, t, color);
    gfx_rect(c, x, y, t, h, color);
    gfx_rect(c, x + w - t, y, t, h, color);
}

void gfx_disc(canvas_t *c, int cx, int cy, int r, uint8_t color) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r) put(c, cx + i, cy + j, color);
}

void gfx_line(canvas_t *c, int x0, int y0, int x1, int y1, int thick, uint8_t color) {
    int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    for (int s = 0; s <= steps; s++) {
        int x = steps ? x0 + (x1 - x0) * s / steps : x0, y = steps ? y0 + (y1 - y0) * s / steps : y0;
        gfx_disc(c, x, y, thick / 2, color);
    }
}

// Next character to draw from UTF-8: printable ASCII as-is, any other code point as '?'.
static char next_char(const char **s) {
    unsigned char ch = (unsigned char) *(*s)++;
    if (ch >= 0x80) {
        while ((**s & 0xC0) == 0x80) (*s)++; // skip continuation bytes
        return '?';
    }
    return ch < 32 || ch > 126 ? '?' : (char) ch;
}

int gfx_text_width(const font_t *f, const char *s) {
    int n = 0;
    while (*s) next_char(&s), n++;
    return n * f->w;
}

int gfx_text(canvas_t *c, int x, int y, const font_t *f, const char *s, uint8_t color) {
    int x0 = x, row_bytes = f->w / 8;
    while (*s) {
        const uint8_t *g = f->bits + (next_char(&s) - 32) * row_bytes * f->h;
        for (int j = 0; j < f->h; j++)
            for (int i = 0; i < f->w; i++)
                if (g[j * row_bytes + i / 8] & (0x80 >> (i % 8))) put(c, x + i, y + j, color);
        x += f->w;
    }
    return x - x0;
}

typedef struct {
    canvas_t *c;
    int x, y, module, size_px;
} qr_job_t;

static void qr_draw(esp_qrcode_handle_t qr, void *arg) {
    qr_job_t *job = arg;
    int n = esp_qrcode_get_size(qr), m = job->module;
    job->size_px = (n + 8) * m;
    if (!job->c) return;
    gfx_rect(job->c, job->x, job->y, job->size_px, job->size_px, EPD_WHITE);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
            if (esp_qrcode_get_module(qr, i, j)) gfx_rect(job->c, job->x + (i + 4) * m, job->y + (j + 4) * m, m, m, EPD_BLACK);
}

int gfx_qr(canvas_t *c, int x, int y, int module_px, const char *text) {
    qr_job_t job = {c, x, y, module_px, 0};
    esp_qrcode_config_t cfg = {
        .display_func_with_cb = qr_draw,
        .max_qrcode_version = 6,
        // Medium ECC tolerates glare and slight blur on a phone camera without making the code much denser.
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
        .user_data = &job,
    };
    return esp_qrcode_generate(&cfg, text) == 0 ? job.size_px : 0;
}
