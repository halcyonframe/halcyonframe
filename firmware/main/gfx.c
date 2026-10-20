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

// Next glyph to draw from UTF-8: printable ASCII as-is, the degree sign (U+00B0) as FONT_DEGREE, any other
// code point as '?'.
static int next_char(const char **s) {
    unsigned char ch = (unsigned char) *(*s)++;
    if (ch >= 0x80) {
        bool degree = ch == 0xC2 && (unsigned char) **s == 0xB0;
        while ((**s & 0xC0) == 0x80) (*s)++; // skip continuation bytes
        return degree ? FONT_DEGREE : '?';
    }
    return ch < 32 || ch > 126 ? '?' : ch;
}

int gfx_text_width(const font_t *f, const char *s) {
    int n = 0;
    while (*s) next_char(&s), n++;
    return n * f->w;
}

int gfx_text_scaled(canvas_t *c, int x, int y, const font_t *f, const char *s, uint8_t color, int scale) {
    int x0 = x, row_bytes = f->w / 8;
    while (*s) {
        const uint8_t *g = f->bits + (next_char(&s) - 32) * row_bytes * f->h;
        for (int j = 0; j < f->h; j++)
            for (int i = 0; i < f->w; i++)
                if (g[j * row_bytes + i / 8] & (0x80 >> (i % 8)))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) put(c, x + i * scale + dx, y + j * scale + dy, color);
        x += f->w * scale;
    }
    return x - x0;
}

int gfx_text(canvas_t *c, int x, int y, const font_t *f, const char *s, uint8_t color) {
    return gfx_text_scaled(c, x, y, f, s, color, 1);
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

/* ---- proportional typefaces ---- */

// Next code point from UTF-8 (malformed bytes come back as themselves; unknown glyphs are skipped).
static uint32_t next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *) *s;
    uint32_t cp = *p++;
    int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (extra) cp &= 0x3F >> extra;
    while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
    *s = (const char *) p;
    return cp;
}

static const pglyph_t *find(const pfont_t *f, uint32_t cp) {
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

int gfx_ptext(canvas_t *c, int x, int baseline, const pfont_t *f, const char *s, uint8_t color, int tracking) {
    int x0 = x;
    bool first = true;
    while (*s) {
        const pglyph_t *g = find(f, next_cp(&s));
        if (!g) continue;
        if (!first) x += tracking;
        first = false;
        if (c) {
            int row = (g->w + 7) / 8;
            const uint8_t *bits = (f->bits ? f->bits : GLYPH_BITS) + g->off;
            for (int j = 0; j < g->h; j++)
                for (int i = 0; i < g->w; i++)
                    if (bits[j * row + i / 8] & (0x80 >> (i % 8))) put(c, x + g->x + i, baseline + g->y + j, color);
        }
        x += g->adv;
    }
    return x - x0;
}

int gfx_pwidth(const pfont_t *f, const char *s, int tracking) { return gfx_ptext(NULL, 0, 0, f, s, 0, tracking); }

int gfx_pcap(const pfont_t *f) {
    const pglyph_t *g = find(f, '0');
    return g ? -g->y : f->ascent;
}

void gfx_poly(canvas_t *c, const int (*pts)[2], int n, uint8_t color) {
    int ymin = pts[0][1], ymax = pts[0][1];
    for (int i = 1; i < n; i++) ymin = pts[i][1] < ymin ? pts[i][1] : ymin, ymax = pts[i][1] > ymax ? pts[i][1] : ymax;
    for (int y = ymin; y <= ymax; y++) {
        int xs[16], k = 0;
        for (int i = 0; i < n && k < 16; i++) {
            const int *a = pts[i], *b = pts[(i + 1) % n];
            if ((a[1] <= y && b[1] > y) || (b[1] <= y && a[1] > y))
                xs[k++] = a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
        }
        for (int i = 1; i < k; i++) // sort the few crossings
            for (int j = i; j > 0 && xs[j] < xs[j - 1]; j--) {
                int t = xs[j];
                xs[j] = xs[j - 1], xs[j - 1] = t;
            }
        for (int i = 0; i + 1 < k; i += 2)
            for (int x = xs[i]; x <= xs[i + 1]; x++) put(c, x, y, color);
    }
}

void gfx_half_disc(canvas_t *c, int cx, int cy, int r, uint8_t color) {
    for (int j = -r; j <= 0; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r) put(c, cx + i, cy + j, color);
}

/* ---- the mark ---- */

// Same geometry as docs/mark.svg (100x100 units), so the frame, the web page and the website show one mark.
static bool in_rrect(float u, float v, float x0, float y0, float x1, float y1, float r) {
    if (u < x0 || u > x1 || v < y0 || v > y1) return false;
    float cx = u < x0 + r ? x0 + r : u > x1 - r ? x1 - r : u, cy = v < y0 + r ? y0 + r : v > y1 - r ? y1 - r : v;
    return (u - cx) * (u - cx) + (v - cy) * (v - cy) <= r * r;
}

// 0 = background, 1 = black, 2 = yellow, 3 = blue, for the point (u, v) in mark units.
static int mark_color(float u, float v) {
    static const float waves[3][3] = {{62, 18, 82}, {72, 28, 72}, {82, 38, 62}}; // y, x from, x to (6 thick, round ends)
    for (int i = 0; i < 3; i++) {
        float y = waves[i][0], a = waves[i][1], b = waves[i][2];
        float cx = u < a ? a : u > b ? b : u;
        if ((u - cx) * (u - cx) + (v - y) * (v - y) <= 9) return 3;
    }
    float d2 = (u - 50) * (u - 50) + (v - 58) * (v - 58);
    // the sun's outline (3 thick) is an arc above the waterline plus the straight edge under it
    if ((d2 >= 20.5f * 20.5f && d2 <= 23.5f * 23.5f && v <= 59.5f) || (v >= 56.5f && v <= 59.5f && u >= 26.5f && u <= 73.5f))
        return 1;
    if (in_rrect(u, v, 2.5f, 2.5f, 97.5f, 97.5f, 17.5f) && !in_rrect(u, v, 9.5f, 9.5f, 90.5f, 90.5f, 10.5f)) return 1; // frame, 7 thick
    if (d2 <= 22.f * 22.f && v <= 58.f) return 2;
    return 0;
}

void gfx_mark(canvas_t *c, int x, int y, int size) {
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            int n[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < 4; sy++) // 4x4 samples per pixel, then the majority colour wins
                for (int sx = 0; sx < 4; sx++)
                    n[mark_color((i + (sx + .5f) / 4) * 100.f / size, (j + (sy + .5f) / 4) * 100.f / size)]++;
            if (n[1] + n[2] + n[3] < 8) continue; // under half covered: leave the pixel alone
            int best = n[3] >= n[1] && n[3] >= n[2] ? 3 : n[1] >= n[2] ? 1 : 2;
            put(c, x + i, y + j, best == 3 ? EPD_BLUE : best == 1 ? EPD_BLACK : EPD_YELLOW);
        }
}
