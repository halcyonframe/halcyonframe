#include "status.h"
#include "astro.h"
#include "epd.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const CARD_NAMES[CARD_COUNT] = {"indoor", "battery", "photos", "wifi", "sun", "moon", "daylight", "next"};

#define MAX_LINE 72

typedef struct {
    const char *label;
    char value[MAX_LINE];
    char sub[MAX_LINE];
    uint8_t sub_color;
    char aside[24];      // small text after the value: "feels 74°", "PM"
    int icon_w;          // space an icon takes before the value (battery)
    int label_icon_w;    // space an icon takes after the label (bars, sun, moon)
    bool needs_setup;    // waiting on something only the web page can provide
} card_view_t;

/* ---- helpers ---- */

static const char *signal_word(int rssi) {
    return rssi >= -55 ? "excellent signal" : rssi >= -67 ? "good signal" : rssi >= -75 ? "fair signal" : "weak signal";
}
static int signal_bars(int rssi) {
    if (!rssi) return 0;
    return rssi >= -55 ? 5 : rssi >= -62 ? 4 : rssi >= -70 ? 3 : rssi >= -78 ? 2 : 1;
}

// NWS heat index. The simple Steadman form below ~80 F, the full Rothfusz regression above it.
static double feels_like_f(double t, double rh) {
    double hi = 0.5 * (t + 61.0 + (t - 68.0) * 1.2 + rh * 0.094);
    if ((hi + t) / 2 < 80) return hi;
    return -42.379 + 2.04901523 * t + 10.14333127 * rh - 0.22475541 * t * rh - 6.83783e-3 * t * t - 5.481717e-2 * rh * rh +
           1.22874e-3 * t * t * rh + 8.5282e-4 * t * rh * rh - 1.99e-6 * t * t * rh * rh;
}

// EPA guidance: keep indoor humidity at 30-50%, and below 60% to prevent mould.
static void humidity_level(double rh, const char **tag, uint8_t *fg, uint8_t *bg) {
    if (rh < 30) *tag = "DRY", *fg = EPD_WHITE, *bg = EPD_BLUE;
    else if (rh <= 50) *tag = "NORMAL", *fg = EPD_WHITE, *bg = EPD_GREEN;
    else if (rh <= 60) *tag = "ELEVATED", *fg = EPD_BLACK, *bg = EPD_YELLOW;
    else *tag = "HIGH \xC2\xB7 MOLD RISK", *fg = EPD_WHITE, *bg = EPD_RED;
}

// "6:38" plus "PM" (12 h, with Fahrenheit: the US convention) or "18:38" (24 h).
static void clock_text(time_t t, bool twelve, char *hm, size_t n, char *ampm) {
    struct tm tm;
    localtime_r(&t, &tm);
    if (twelve) {
        snprintf(hm, n, "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
        strcpy(ampm, tm.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(hm, n, "%d:%02d", tm.tm_hour, tm.tm_min);
        ampm[0] = 0;
    }
}

/* ---- icons (sizes as in the approved previews) ---- */

static void battery_icon(canvas_t *c, int x, int y, int pct, bool charging) {
    gfx_frame(c, x, y, 45, 24, 3, EPD_BLACK);
    gfx_rect(c, x + 45, y + 7, 4, 10, EPD_BLACK);
    int fill = (int) lround(36.0 * (pct > 100 ? 100 : pct < 0 ? 0 : pct) / 100);
    if (fill) gfx_rect(c, x + 4, y + 4, fill, 16, pct <= 15 ? EPD_RED : EPD_GREEN);
    if (charging) { // yellow bolt with a black outline: yellow alone vanishes on the light panel
        int cx = x + 22;
        const int bolt[6][2] = {{cx + 3, y - 6}, {cx - 8, y + 14}, {cx - 1, y + 14}, {cx - 4, y + 30}, {cx + 9, y + 8}, {cx + 1, y + 8}};
        int outline[6][2];
        for (int d = 0; d < 9; d++) { // fatten the outline by drawing the shape shifted around
            for (int i = 0; i < 6; i++) outline[i][0] = bolt[i][0] + d % 3 - 1, outline[i][1] = bolt[i][1] + d / 3 - 1;
            gfx_poly(c, (const int (*)[2]) outline, 6, EPD_BLACK);
        }
        gfx_poly(c, bolt, 6, EPD_YELLOW);
    }
}

static void bars_icon(canvas_t *c, int x, int bottom, int n) {
    uint8_t color = n <= 2 ? EPD_RED : EPD_BLUE;
    for (int i = 0; i < 5; i++) {
        int h = 6 + i * 5;
        if (i < n) gfx_rect(c, x + i * 9, bottom - h, 7, h + 1, color);
        else gfx_frame(c, x + i * 9, bottom - h, 7, h + 1, 1, EPD_BLACK);
    }
}

static void sun_icon(canvas_t *c, int x, int y) { // setting sun: half disc on a horizon, short rays
    gfx_half_disc(c, x + 13, y + 13, 14, EPD_BLACK);
    gfx_half_disc(c, x + 13, y + 13, 11, EPD_YELLOW);
    static const int rays[5] = {200, 240, 270, 300, 340};
    for (int i = 0; i < 5; i++) {
        double a = rays[i] * M_PI / 180;
        gfx_line(c, x + 13 + (int) (17 * cos(a)), y + 13 + (int) (17 * sin(a)), x + 13 + (int) (23 * cos(a)),
                 y + 13 + (int) (23 * sin(a)), 2, EPD_BLACK);
    }
    gfx_rect(c, x - 4, y + 12, 35, 2, EPD_BLACK);
}

static void moon_icon(canvas_t *c, int cx, int cy, int r, double phase) {
    double k = cos(2 * M_PI * phase);
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r) continue;
            double edge = k * sqrt((double) (r * r - y * y)); // the terminator is a half-ellipse
            bool lit = phase < 0.5 ? x > edge : x < -edge;
            gfx_rect(c, cx + x, cy + y, 1, 1, lit ? EPD_YELLOW : EPD_BLACK);
        }
    for (int a = 0; a < 360; a += 2) { // outline
        double t = a * M_PI / 180;
        gfx_rect(c, cx + (int) lround(r * cos(t)), cy + (int) lround(r * sin(t)), 2, 2, EPD_BLACK);
    }
}

static void usage_bar(canvas_t *c, int x, int y, int w, double frac) {
    gfx_frame(c, x, y, w, 13, 2, EPD_BLACK);
    int fill = frac > 0 ? (int) fmax(2, lround((w - 4) * frac)) : 0;
    if (fill) gfx_rect(c, x + 2, y + 2, fill, 9, frac >= 0.9 ? EPD_RED : EPD_BLUE);
}

static int tag(canvas_t *c, int x, int y, const char *text, uint8_t fg, uint8_t bg) {
    const pfont_t *f = LABEL_FONT;
    int w = gfx_pwidth(f, text, 1);
    gfx_rect(c, x, y, w + 12, f->size + 7, bg);
    gfx_ptext(c, x + 6, y + 5 + gfx_pcap(f), f, text, fg, 1);
    return w + 12;
}

/* ---- network names: up to 32 characters, often with no spaces ---- */

typedef struct {
    const pfont_t *f;
    char line[2][MAX_LINE];
    int lines;
} fitted_t;

static bool split_fits(const pfont_t *f, const char *s, int w, bool natural, fitted_t *out) {
    int n = strlen(s), best = -1;
    char a[MAX_LINE], b[MAX_LINE];
    for (int i = 1; i < n; i++) {
        if (natural && !strchr(" -_", s[i - 1])) continue;
        if ((s[i] & 0xC0) == 0x80) continue; // never split a UTF-8 sequence
        snprintf(a, sizeof(a), "%.*s", i, s);
        if (natural && a[i - 1] == ' ') a[i - 1] = 0;
        snprintf(b, sizeof(b), "%s", s + i + (s[i] == ' '));
        if (gfx_pwidth(out->f, a, 0) <= w && gfx_pwidth(out->f, b, 0) <= w) best = i;
    }
    if (best < 0) return false;
    snprintf(out->line[0], MAX_LINE, "%.*s", best, s);
    if (out->line[0][best - 1] == ' ') out->line[0][best - 1] = 0;
    snprintf(out->line[1], MAX_LINE, "%s", s + best + (s[best] == ' '));
    out->lines = 2;
    return true;
}

static void ellipsize(const pfont_t *f, char *s, int w) {
    size_t n = strlen(s);
    while (n > 1 && gfx_pwidth(f, s, 0) > w) {
        do n--; while (n && (s[n] & 0xC0) == 0x80);
        strcpy(s + n, "\xE2\x80\xA6"); // ellipsis
    }
}

// Preference: one line (largest size first); two lines at a word break; a word break with the second line
// shortened; two lines split mid-word; finally mid-word with an ellipsis.
static fitted_t fit_name(const typeface_t *tf, const char *name, int w) {
    fitted_t r = {0};
    for (int i = 0; i < 2; i++) { // 46 and 36 px: still read as a value
        r.f = tf->value[i];
        if (gfx_pwidth(r.f, name, 0) <= w) {
            snprintf(r.line[0], MAX_LINE, "%s", name);
            r.lines = 1;
            return r;
        }
    }
    for (int i = 2; i < VALUE_SIZES; i++) {
        r.f = tf->value[i];
        if (split_fits(r.f, name, w, true, &r)) return r;
    }
    r.f = tf->value[VALUE_SIZES - 1];
    for (int i = strlen(name) - 1; i > 0; i--) {
        if (!strchr(" -_", name[i - 1])) continue;
        snprintf(r.line[0], MAX_LINE, "%.*s", i, name);
        if (r.line[0][i - 1] == ' ') r.line[0][i - 1] = 0;
        if (gfx_pwidth(r.f, r.line[0], 0) > w) continue;
        snprintf(r.line[1], MAX_LINE, "%s", name + i + (name[i] == ' '));
        ellipsize(r.f, r.line[1], w);
        r.lines = 2;
        return r;
    }
    for (int i = 2; i < VALUE_SIZES; i++) {
        r.f = tf->value[i];
        if (split_fits(r.f, name, w, false, &r)) return r;
    }
    int n = strlen(name);
    while (n > 1 && gfx_pwidth(r.f, (snprintf(r.line[0], MAX_LINE, "%.*s", n, name), r.line[0]), 0) > w) n--;
    snprintf(r.line[1], MAX_LINE, "%s", name + n);
    ellipsize(r.f, r.line[1], w);
    r.lines = 2;
    return r;
}

/* ---- the cards ---- */

static void fill_card(card_view_t *v, card_t k, const screen_info_t *in, bool twelve) {
    memset(v, 0, sizeof(*v));
    v->sub_color = EPD_BLACK;
    char ampm[4];
    switch (k) {
    case CARD_INDOOR: {
        v->label = "INDOOR";
        if (!in->has_climate) {
            strcpy(v->value, "--");
            strcpy(v->sub, "sensor unavailable");
            break;
        }
        double tf = in->temp_c * 9 / 5 + 32, feels = feels_like_f(tf, in->rh);
        double shown = in->fahrenheit ? tf : in->temp_c, feels_shown = in->fahrenheit ? feels : (feels - 32) * 5 / 9;
        snprintf(v->value, MAX_LINE, "%.0f\xC2\xB0", shown);
        if (fabs(round(feels_shown) - round(shown)) >= 1) // only worth saying when it differs
            snprintf(v->aside, sizeof(v->aside), "feels %.0f\xC2\xB0", feels_shown);
        snprintf(v->sub, MAX_LINE, "%.0f%% humidity", in->rh);
        break;
    }
    case CARD_BATTERY:
        v->label = "BATTERY";
        if (in->has_battery) {
            snprintf(v->value, MAX_LINE, "%d%%", in->battery_pct);
            strcpy(v->sub, in->charging ? "charging" : "on battery");
            v->icon_w = 62;
        } else {
            strcpy(v->value, "USB");
            strcpy(v->sub, "no battery");
        }
        break;
    case CARD_PHOTOS:
        v->label = "PHOTOS";
        snprintf(v->value, MAX_LINE, "%d", in->photos);
        snprintf(v->sub, MAX_LINE, "%.1f of %.1f GB used", in->card_used / 1e9, in->card_total / 1e9);
        break;
    case CARD_WIFI:
        v->label = "WI-FI";
        v->label_icon_w = 56;
        if (in->ap_mode) snprintf(v->value, MAX_LINE, "%s", in->ap_ssid), strcpy(v->sub, "hotspot mode");
        else if (in->wifi_ssid[0]) snprintf(v->value, MAX_LINE, "%s", in->wifi_ssid), strcpy(v->sub, in->rssi ? signal_word(in->rssi) : "");
        else strcpy(v->value, "Off"), strcpy(v->sub, "not set up");
        break;
    case CARD_SUN: {
        v->label = "SUNSET";
        v->label_icon_w = 44;
        if (!in->has_location || !in->clock_ok) {
            strcpy(v->value, "--");
            strcpy(v->sub, in->has_location ? "clock not set" : "set a ZIP code");
            v->needs_setup = true;
            break;
        }
        sun_day_t d = astro_sun(in->now, in->lat, in->lon);
        if (!d.rise) {
            strcpy(v->value, d.up_all_day ? "No set" : "No rise");
            strcpy(v->sub, d.up_all_day ? "sun up all day" : "sun down all day");
            break;
        }
        // The next sun event: before sunrise or after sunset, show the coming sunrise.
        bool before_set = in->now >= d.rise && in->now < d.set;
        time_t main_t = before_set ? d.set : d.rise, other_t = before_set ? d.rise : d.set;
        if (in->now >= d.set) main_t = astro_sun(in->now + 86400, in->lat, in->lon).rise;
        v->label = before_set ? "SUNSET" : "SUNRISE";
        clock_text(main_t, twelve, v->value, MAX_LINE, ampm);
        strcpy(v->aside, ampm);
        char hm[16], ap[4];
        clock_text(other_t, twelve, hm, sizeof(hm), ap);
        snprintf(v->sub, MAX_LINE, "%s %s%s%s", before_set ? "sunrise" : "sunset", hm, ap[0] ? " " : "", ap);
        break;
    }
    case CARD_MOON: {
        v->label = "MOON";
        v->label_icon_w = 40;
        moon_t mo = astro_moon(in->now);
        // The value is the short form; the caption carries waxing/waning and how much is lit.
        const char *shortname = strstr(mo.name, "gibbous") ? "Gibbous" : strstr(mo.name, "crescent") ? "Crescent"
                              : strstr(mo.name, "quarter") ? "Quarter" : strstr(mo.name, "Full") ? "Full" : "New";
        strcpy(v->value, shortname);
        snprintf(v->sub, MAX_LINE, "%s, %.0f%% lit", mo.phase < 0.5 ? "waxing" : "waning", mo.lit * 100);
        break;
    }
    case CARD_DAYLIGHT: {
        v->label = "DAYLIGHT";
        if (!in->has_location || !in->clock_ok) {
            strcpy(v->value, "--");
            strcpy(v->sub, in->has_location ? "clock not set" : "set a ZIP code");
            v->needs_setup = true;
            break;
        }
        sun_day_t d = astro_sun(in->now, in->lat, in->lon), y = astro_sun(in->now - 86400, in->lat, in->lon);
        long len = d.rise ? (long) (d.set - d.rise) : d.up_all_day ? 86400 : 0;
        long prev = y.rise ? (long) (y.set - y.rise) : y.up_all_day ? 86400 : 0;
        snprintf(v->value, MAX_LINE, "%ldh %02ldm", len / 3600, len % 3600 / 60);
        long diff = (len - prev) / 60;
        if (diff) snprintf(v->sub, MAX_LINE, "%ld min %s", labs(diff), diff > 0 ? "longer" : "shorter");
        else strcpy(v->sub, "same as yesterday");
        v->sub_color = diff > 0 ? EPD_GREEN : diff < 0 ? EPD_BLUE : EPD_BLACK;
        break;
    }
    case CARD_NEXT:
        v->label = "NEXT PHOTO";
        if (!in->next_photo) {
            strcpy(v->value, "--");
            strcpy(v->sub, "changes on request");
            break;
        }
        clock_text(in->next_photo, twelve, v->value, MAX_LINE, ampm);
        strcpy(v->aside, ampm);
        if (in->interval_min > 0 && in->interval_min % 60 == 0)
            snprintf(v->sub, MAX_LINE, "then every %d h", in->interval_min / 60);
        else if (in->interval_min > 0) snprintf(v->sub, MAX_LINE, "then every %d min", in->interval_min);
        else strcpy(v->sub, "at set times");
        break;
    default:
        v->label = "";
    }
}

void status_render(canvas_t *c, const screen_info_t *in) {
    bool land = c->orient == ORIENT_LANDSCAPE, twelve = in->fahrenheit;
    const typeface_t *tf = &TYPEFACES[in->typeface >= 0 && in->typeface < TYPEFACE_COUNT ? in->typeface : 0];
    const pfont_t *ftime = land ? tf->time_land : tf->time_port, *fdate = land ? tf->date_land : tf->date_port;
    const pfont_t *fval = tf->value[0], *flab = LABEL_FONT;
    int W = c->w, H = c->h, m = land ? 40 : 32;
    gfx_fill(c, EPD_WHITE);

    card_view_t v[4];
    bool setup = !in->clock_ok;
    for (int i = 0; i < 4; i++) {
        fill_card(&v[i], (card_t) (in->cards[i] < CARD_COUNT ? in->cards[i] : i), in, twelve);
        setup |= v[i].needs_setup;
    }
    // Something only the web page can fix (location, clock): put its address beside the clock as a QR code.
    if (setup && in->ip) {
        char url[48];
        snprintf(url, sizeof(url), "http://%s/", in->ip);
        int mod = land ? 4 : 3, size = gfx_qr(NULL, 0, 0, mod, url), qx = W - m - size + 12, qy = land ? 14 : 10;
        gfx_qr(c, qx, qy, mod, url);
        // Caption centred under the code, but never past the right margin (it's wider than the small portrait code).
        const char *cap = "FINISH SETUP";
        int cw = gfx_pwidth(LABEL_FONT, cap, 2), cx = qx + (size - cw) / 2;
        if (cx + cw > W - m) cx = W - m - cw;
        gfx_ptext(c, cx, qy + size + 4, LABEL_FONT, cap, EPD_BLACK, 2);
    }

    // Clock, with the meridiem set small on the same baseline. In portrait the setup code sits above it.
    int top = land ? 18 : (setup ? 150 : 105), cap = gfx_pcap(ftime), y;
    if (in->clock_ok) {
        char hm[16], ampm[4];
        clock_text(in->now, twelve, hm, sizeof(hm), ampm);
        int x = m - 4 + gfx_ptext(c, m - 4, top + cap, ftime, hm, EPD_BLACK, 0);
        if (ampm[0]) gfx_ptext(c, x + 14, top + cap, tf->ampm, ampm, EPD_BLACK, 0);
        y = top + cap + 34;
        char date[48];
        struct tm tm;
        localtime_r(&in->now, &tm);
        strftime(date, sizeof(date), land ? "%A, %B %e" : "%A, %b %e", &tm);
        // strftime pads %e with a space ("October  8"); collapse it.
        for (char *p = strstr(date, "  "); p; p = strstr(date, "  ")) memmove(p, p + 1, strlen(p));
        gfx_ptext(c, m, y + gfx_pcap(fdate), fdate, date, EPD_BLUE, 0);
        y += gfx_pcap(fdate) + 34;
    } else {
        gfx_ptext(c, m, top + gfx_pcap(fval) + 20, fval, "Clock not set", EPD_BLACK, 0);
        y = top + gfx_pcap(fval) + 40;
        gfx_ptext(c, m, y + 30, tf->sub[0], "Open the frame's web page once to set it.", EPD_BLACK, 0);
        y += 80;
    }

    // Columns: sized to their content (typefaces differ a lot in width); captions shrink until all four fit;
    // spare width is shared out. Portrait: a 2x2 grid.
    int widths[4], px[4], py[4], sub_i = 0;
    const pfont_t *fsub = tf->sub[0];
    if (land) {
        int avail = W - 2 * m, nat[4], sum;
        for (;;) {
            fsub = tf->sub[sub_i], sum = 0;
            for (int i = 0; i < 4; i++) {
                int a = gfx_pwidth(flab, v[i].label, 3) + v[i].label_icon_w;
                int b = gfx_pwidth(fval, v[i].value, 0) + v[i].icon_w + (v[i].aside[0] ? 8 + gfx_pwidth(fsub, v[i].aside, 0) : 0);
                int s = gfx_pwidth(fsub, v[i].sub, 0);
                nat[i] = (a > b ? a : b) > s ? (a > b ? a : b) : s;
                if (in->cards[i] == CARD_WIFI && nat[i] > 200) nat[i] = 200; // long names wrap instead
                nat[i] += 22;
                sum += nat[i];
            }
            if (sum <= avail || sub_i == SUB_SIZES - 1) break;
            sub_i++;
        }
        int spare = (avail - sum) / 4, x = m;
        y = y + 30 > H - 40 - 140 ? y + 30 : H - 40 - 140;
        gfx_rect(c, m, y - 30, W - 2 * m, 2, EPD_BLACK);
        for (int i = 0; i < 4; i++) widths[i] = nat[i] + spare, px[i] = x, py[i] = y, x += widths[i];
    } else {
        gfx_rect(c, m, y, W - 2 * m, 2, EPD_BLACK);
        y += 50;
        int half = (W - 2 * m) / 2;
        for (int i = 0; i < 4; i++) widths[i] = half, px[i] = m + (i % 2) * half, py[i] = y + (i / 2) * 175;
    }

    for (int i = 0; i < 4; i++) {
        card_view_t *cv = &v[i];
        int cx = px[i], cy = py[i], cw = widths[i], vy = cy + 32, k = in->cards[i];
        int lx = cx + gfx_ptext(c, cx, cy + flab->ascent, flab, cv->label, EPD_BLACK, 3);
        if (k == CARD_WIFI) bars_icon(c, lx + 12, cy + 17, in->ap_mode ? 0 : signal_bars(in->rssi));
        if (k == CARD_SUN && cv->value[0] != '-') sun_icon(c, lx + 12, cy - 6);
        if (k == CARD_MOON) moon_icon(c, lx + 24, cy + 9, 11, astro_moon(in->now).phase);

        int base = vy + gfx_pcap(fval);
        if (k == CARD_WIFI) {
            fitted_t fit = fit_name(tf, cv->value, cw - 12);
            int ly = vy + gfx_pcap(fit.f) - (fit.lines > 1 ? 6 : 0);
            for (int l = 0; l < fit.lines; l++, ly += fit.f->size * 108 / 100)
                gfx_ptext(c, cx, ly, fit.f, fit.line[l], EPD_BLACK, 0);
        } else {
            if (k == CARD_BATTERY && cv->icon_w) battery_icon(c, cx, vy + 8, in->battery_pct, in->charging);
            const pfont_t *f = fval;
            for (int s = 1; s < VALUE_SIZES && gfx_pwidth(f, cv->value, 0) + cv->icon_w > cw - 12; s++) f = tf->value[s];
            int x = cx + cv->icon_w + gfx_ptext(c, cx + cv->icon_w, base, f, cv->value, EPD_BLACK, 0);
            if (cv->aside[0]) gfx_ptext(c, x + 8, base, fsub, cv->aside, EPD_BLACK, 0);
        }
        gfx_ptext(c, cx, vy + 58 + fsub->ascent, fsub, cv->sub, cv->sub_color, 0);
        if (k == CARD_PHOTOS && in->card_total)
            usage_bar(c, cx, vy + 92, gfx_pwidth(fsub, cv->sub, 0), (double) in->card_used / in->card_total);
        if (k == CARD_INDOOR && in->has_climate) {
            const char *t;
            uint8_t fg, bg;
            humidity_level(in->rh, &t, &fg, &bg);
            tag(c, cx, vy + 88, t, fg, bg);
        }
    }
}
