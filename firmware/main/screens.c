#include "screens.h"
#include "epd.h"
#include <stdio.h>
#include <string.h>

// Copy is written for someone who has never seen the device before: say what to do, in order, and
// put anything they must type or join in large blue type. Everything is at least 32 px tall (~6 mm),
// which reads at arm's length.

// ST_BIG: headline-size blue, for names and addresses to type or join. ST_EXTRA: reassurance that is
// dropped first when very long names leave no room.
enum { ST_BODY, ST_RED, ST_BIG, ST_EXTRA };
enum { VIS_NONE, VIS_QR, VIS_BATTERY };

typedef struct {
    const char *text;
    uint8_t style;
} line_t;

typedef struct {
    int num;      // step number in a blue disc; 0 = none
    int visual;   // drawn to the right of the text
    const char *qr;
    int qr_module; // preferred QR module size in px; reduced if the screen would overflow
    line_t lines[6];
} step_t;

typedef struct {
    const char *title;
    bool check; // green tick after the title
    line_t intro[2];
    step_t steps[2];
    const char *footer; // on a yellow band at the bottom
} doc_t;

#define LINE_BODY 38 // 32 px glyphs plus spacing
#define LINE_BIG 68
#define BATTERY_W 160
#define BATTERY_H 80

static int imax(int a, int b) { return a > b ? a : b; }

// Word-wraps s into width w. Returns the height used; draws only when draw is set.
static int text_block(canvas_t *c, bool draw, int x, int y, int w, const font_t *f, int lh, uint8_t color,
                      const char *s) {
    char buf[160];
    int h = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        // Longest prefix that fits, preferring to break at a space; a single overlong word is split.
        size_t len = strlen(s), fit = 0, brk = 0;
        for (size_t i = 1; i <= len && i < sizeof(buf); i++) {
            if ((s[i] & 0xC0) == 0x80) continue; // never split a UTF-8 sequence
            memcpy(buf, s, i);
            buf[i] = 0;
            if (gfx_text_width(f, buf) > w) break;
            fit = i;
            if (s[i] == ' ' || s[i] == 0) brk = i;
        }
        size_t take = brk ? brk : fit ? fit : 1;
        memcpy(buf, s, take);
        buf[take] = 0;
        if (draw) gfx_text(c, x, y + h, f, buf, color);
        h += lh;
        s += take;
    }
    return h;
}

static int draw_line(canvas_t *c, bool draw, int x, int y, int w, line_t l) {
    // Big text falls back to body size (wrapped) when a long name won't fit on one line.
    if (l.style == ST_BIG && gfx_text_width(&font_32x64, l.text) <= w) {
        if (draw) gfx_text(c, x, y, &font_32x64, l.text, EPD_BLUE);
        return LINE_BIG;
    }
    uint8_t color = l.style == ST_RED ? EPD_RED : l.style == ST_BIG ? EPD_BLUE : EPD_BLACK;
    return text_block(c, draw, x, y, w, &font_16x32, LINE_BODY, color, l.text);
}

static void draw_check(canvas_t *c, int x, int y) {
    gfx_line(c, x, y + 28, x + 16, y + 44, 10, EPD_GREEN);
    gfx_line(c, x + 16, y + 44, x + 48, y + 8, 10, EPD_GREEN);
}

static void draw_battery(canvas_t *c, int x, int y) {
    gfx_frame(c, x, y, BATTERY_W - 16, BATTERY_H, 8, EPD_RED);
    gfx_rect(c, x + BATTERY_W - 16, y + BATTERY_H / 4, 16, BATTERY_H / 2, EPD_RED);
    gfx_rect(c, x + 16, y + 16, 16, BATTERY_H - 32, EPD_RED); // the last sliver of charge
}

// Lays the document out top to bottom. Returns false if it runs into the footer.
static bool layout(canvas_t *c, const doc_t *d, bool draw, int shrink) {
    bool land = c->orient == ORIENT_LANDSCAPE;
    int m = land ? 32 : 24, w = c->w - 2 * m, y = 16;

    int bottom = c->h;
    if (d->footer) {
        int fh = text_block(c, false, m, 0, w, &font_16x32, LINE_BODY, 0, d->footer) + 20;
        bottom -= fh;
        if (draw) {
            gfx_rect(c, 0, bottom, c->w, fh, EPD_YELLOW);
            text_block(c, true, m, bottom + 12, w, &font_16x32, LINE_BODY, EPD_BLACK, d->footer);
        }
    }

    int tw = w - (d->check ? 80 : 0);
    int th = text_block(c, draw, m, y, tw, &font_32x64, LINE_BIG, EPD_BLACK, d->title);
    if (d->check && draw) {
        int last = gfx_text_width(&font_32x64, d->title);
        draw_check(c, m + (last < tw ? last : tw) + 20, y + th - LINE_BIG + 4);
    }
    y += th + 12;

    for (int i = 0; i < 2 && d->intro[i].text; i++) y += draw_line(c, draw, m, y, w, d->intro[i]);
    if (d->intro[0].text) y += 12;

    for (int i = 0; i < 2; i++) {
        const step_t *st = &d->steps[i];
        if (!st->lines[0].text) break;
        int mod = imax(2, st->qr_module - shrink), vw = 0, vh = 0;
        if (st->visual == VIS_QR) vw = vh = gfx_qr(NULL, 0, 0, mod, st->qr);
        if (st->visual == VIS_BATTERY) vw = BATTERY_W, vh = BATTERY_H;
        int r = land ? 28 : 20, tx = m + (st->num ? 2 * r + 16 : 0);
        // Landscape: picture to the right of the text. Portrait is too narrow for that (names would drop
        // to small type), so the picture goes under the text instead.
        int tcol = c->w - m - tx - (land && vw ? vw + 24 : 0);
        if (st->num && draw) {
            int cy = y + (st->lines[0].style == ST_BIG ? LINE_BIG : LINE_BODY) / 2 - 2;
            char digit[2] = {(char) ('0' + st->num), 0};
            gfx_disc(c, m + r, cy, r, EPD_BLUE);
            gfx_text(c, m + r - 8, cy - 16, &font_16x32, digit, EPD_WHITE);
        }
        int ty = y;
        for (int k = 0; k < 6 && st->lines[k].text; k++)
            if (st->lines[k].style != ST_EXTRA || shrink < 2) ty += draw_line(c, draw, tx, ty, tcol, st->lines[k]);
        int vx = land ? c->w - m - vw : tx, vy = land ? y : ty + 4;
        if (draw && st->visual == VIS_QR) gfx_qr(c, vx, vy, mod, st->qr);
        if (draw && st->visual == VIS_BATTERY) draw_battery(c, vx, vy);
        int gap = shrink >= 3 ? 6 : 16;
        y = land ? y + imax(ty - y, vh) + gap : vy + vh + gap;
    }
    return y - 8 <= bottom;
}

// Wi-Fi join QR payload; these characters must be backslash-escaped in SSID and password.
static void qr_escape(char *out, size_t n, const char *s) {
    size_t o = 0;
    for (; *s && o + 2 < n; s++) {
        if (strchr("\\;,:\"", *s)) out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = 0;
}

static void interval_text(char *out, size_t n, int min) {
    if (min <= 0) snprintf(out, n, "change on request");
    else if (min % 60 == 0) snprintf(out, n, "new one every %d h", min / 60);
    else snprintf(out, n, "new one every %d min", min);
}

bool screen_render(screen_t s, const screen_info_t *in, uint8_t *buf) {
    canvas_t c;
    gfx_begin(&c, buf, in->orient);
    doc_t d = {0};
    // The open/WPA2 rule matches wifi.c: shorter than 8 characters means an open hotspot.
    bool open = strlen(in->ap_pass) < 8;
    char ssid_esc[80], pass_esc[140], wifi_qr[260], url[48], pass_line[96], t1[128], t2[128], t3[96], t4[96], t5[96];
    qr_escape(ssid_esc, sizeof(ssid_esc), in->ap_ssid);
    qr_escape(pass_esc, sizeof(pass_esc), in->ap_pass);
    if (open) snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:nopass;S:%s;;", ssid_esc);
    else snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:WPA;S:%s;P:%s;;", ssid_esc, pass_esc);
    // The QR code uses the IP: .local names don't resolve on many Android phones.
    snprintf(url, sizeof(url), "http://%s/", in->ip);
    if (open) snprintf(pass_line, sizeof(pass_line), "No password needed.");
    else snprintf(pass_line, sizeof(pass_line), "Password: %s", in->ap_pass);
    const char *footer_wake = "Red light off? Press KEY to wake me up.";

    switch (s) {
    case SCR_SETUP:
        d = (doc_t) {
            .title = "Hi! Let's add photos.",
            .steps = {
                {1, VIS_QR, wifi_qr, 4, {{"Join this Wi-Fi:", ST_BODY}, {in->ap_ssid, ST_BIG}, {pass_line, ST_BODY}}},
                {2, VIS_QR, url, 4, {{"Then open:", ST_BODY}, {in->ip, ST_BIG}, {"No internet? That's OK.", ST_EXTRA}}},
            },
            .footer = footer_wake,
        };
        break;
    case SCR_WIFI_FAIL:
        if (in->wifi_err == WIFI_ERR_AUTH) snprintf(t1, sizeof(t1), "The password for %s seems wrong.", in->wifi_ssid);
        else if (in->wifi_err == WIFI_ERR_NOT_FOUND)
            snprintf(t1, sizeof(t1), "I can't find %s. Is it on and nearby?", in->wifi_ssid);
        else snprintf(t1, sizeof(t1), "I couldn't connect to %s.", in->wifi_ssid);
        snprintf(t2, sizeof(t2), "Join %s", in->ap_ssid);
        snprintf(t3, sizeof(t3), "Open %s to fix it.", in->ip);
        d = (doc_t) {
            .title = "Wi-Fi problem",
            .intro = {{t1, ST_RED}},
            .steps = {
                {1, VIS_QR, wifi_qr, 3, {{t2, ST_BODY}, {pass_line, ST_BODY}}},
                {2, VIS_QR, url, 3, {{t3, ST_BODY}}},
            },
            .footer = "Red light off? Press KEY to try again.",
        };
        break;
    case SCR_READY:
        snprintf(t1, sizeof(t1), "On a phone or computer on %s, open:", in->wifi_ssid);
        snprintf(t2, sizeof(t2), "%s.local", in->hostname);
        snprintf(t3, sizeof(t3), "or %s", in->ip);
        d = (doc_t) {
            .title = "Ready for photos!",
            .check = true,
            .intro = {{t1, ST_BODY}},
            .steps = {{0, VIS_QR, url, 5, {{t2, ST_BIG}, {t3, ST_BODY}, {"Photos you add will appear here.", ST_EXTRA}}}},
            .footer = footer_wake,
        };
        break;
    case SCR_INFO: {
        char every[40], card[64];
        interval_text(every, sizeof(every), in->interval_min);
        every[0] = (char) (every[0] - 'a' + 'A'); // sentence case: "New one every 1 h"
        if (in->has_battery) snprintf(t3, sizeof(t3), "Battery: %d%%%s", in->battery_pct, in->charging ? ", charging" : "");
        else snprintf(t3, sizeof(t3), "Power: USB");
        snprintf(card, sizeof(card), "Card: %d photo%s, %.1f GB free", in->photos, in->photos == 1 ? "" : "s", in->free_gb);
        snprintf(t4, sizeof(t4), "%s", every);
        if (in->ap_mode) {
            // On its own hotspot: one QR joins it, the other opens the page.
            snprintf(t1, sizeof(t1), "Wi-Fi: %s (mine)", in->ap_ssid);
            snprintf(t2, sizeof(t2), "Open: http://%s", in->ip);
            d.steps[0] = (step_t) {0, VIS_QR, wifi_qr, 3, {{t1, ST_BODY}, {pass_line, ST_BODY}}};
            d.steps[1] = (step_t) {0, VIS_QR, url, 3, {{t2, ST_BODY}, {t3, ST_BODY}, {card, ST_BODY}, {t4, ST_EXTRA}}};
        } else {
            snprintf(t1, sizeof(t1), "Wi-Fi: %s", in->wifi_ssid);
            snprintf(t2, sizeof(t2), "Open: http://%s.local", in->hostname);
            snprintf(t5, sizeof(t5), "  or http://%s", in->ip);
            d.steps[0] = (step_t) {0, VIS_QR, url, 4, {{t1, ST_BODY}, {t2, ST_BODY}, {t5, ST_BODY}, {t3, ST_BODY}, {card, ST_BODY}, {t4, ST_EXTRA}}};
        }
        d.title = "How to reach me";
        d.footer = "Press BOOT for the next photo.";
        break;
    }
    case SCR_CARD:
        d.title = in->card == CARD_UNREADABLE ? "Can't read the card" : "No memory card";
        d.intro[0] = (line_t) {in->card == CARD_UNREADABLE
                                   ? "The card is in, but I can't read it. Format it as FAT32 on a computer "
                                     "(this erases it), then press KEY."
                                   : "I can't find my memory card. Push it all the way into its slot, then press KEY.",
                               ST_BODY};
        d.footer = footer_wake;
        break;
    case SCR_BATTERY:
        d = (doc_t) {
            .title = "Please charge me",
            .steps = {{0, VIS_BATTERY, NULL, 0, {{"My battery is empty, so the picture won't change.", ST_BODY},
                                                {"Plug in the USB-C cable. Your photos come back on their own.", ST_BODY}}}},
            .footer = "Plugged in? Press BOOT to see a photo now.",
        };
        break;
    default:
        return false;
    }

    // Long network names can push the content into the footer: shrink the QR codes until it fits.
    int shrink = 0;
    while (!layout(&c, &d, false, shrink) && shrink < 3) shrink++; // 3 = 2 px QR modules, tight gaps
    if (!layout(&c, &d, false, shrink)) d.footer = NULL; // last resort: the hint goes, the content stays
    gfx_fill(&c, EPD_WHITE);
    return layout(&c, &d, true, shrink);
}

void screen_badge(uint8_t *buf, orient_t orient) {
    canvas_t c;
    gfx_begin(&c, buf, orient);
    int w = 232, h = 56, x = c.w - w - 16, y = c.h - h - 16;
    gfx_rect(&c, x, y, w, h, EPD_WHITE);
    gfx_frame(&c, x, y, w, h, 2, EPD_BLACK);
    gfx_frame(&c, x + 14, y + 17, 40, 22, 3, EPD_RED);
    gfx_rect(&c, x + 54, y + 23, 4, 10, EPD_RED);
    gfx_rect(&c, x + 18, y + 21, 7, 14, EPD_RED);
    gfx_text(&c, x + 72, y + 12, &font_16x32, "Charge me", EPD_BLACK);
}
