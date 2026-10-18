// Renders every status screen in every orientation to PPM files, so layouts can be checked on a PC
// instead of waiting for 20 s panel refreshes. Build and run with tools/render_screens.sh.
#include <stdio.h>
#include <stdlib.h>
#include "../main/epd.h"
#include "../main/screens.h"

// Approximate panel colours (same as PALETTE in www/index.html), indexed by panel code.
static const uint8_t RGB[8][3] = {{25, 30, 33}, {232, 232, 232}, {239, 222, 68}, {178, 19, 24},
                                  {255, 0, 255}, {33, 87, 186}, {18, 95, 32}, {255, 0, 255}};

static void write_ppm(const char *path, const uint8_t *buf, orient_t o) {
    int w = o == ORIENT_LANDSCAPE ? 800 : 480, h = o == ORIENT_LANDSCAPE ? 480 : 800;
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int px = x, py = y; // viewer -> panel, as in gfx.c
            if (o == ORIENT_CW) px = 799 - y, py = x;
            if (o == ORIENT_CCW) px = y, py = 479 - x;
            uint8_t b = buf[(py * 800 + px) >> 1], code = (px & 1) ? b & 15 : b >> 4;
            fwrite(RGB[code & 7], 1, 3, f);
        }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : ".";
    static uint8_t buf[800 * 480 / 2];
    const char *names[] = {"", "setup", "wifi_fail", "ready", "info", "card", "battery"};
    const char *onames[] = {"land", "cw", "ccw"};
    screen_info_t info = {
        .ap_ssid = "PhotoFrame", .ap_pass = "photoframe", .wifi_ssid = "HomeNet", .wifi_err = WIFI_ERR_AUTH,
        .ip = "192.168.4.1", .hostname = "photoframe", .card = CARD_MISSING, .has_battery = true,
        .battery_pct = 64, .charging = true, .photos = 660, .interval_min = 60, .free_gb = 14.6f,
    };
    int bad = 0;
    for (int o = 0; o < 3; o++)
        for (int s = SCR_SETUP; s <= SCR_BATTERY; s++) {
            info.orient = (orient_t) o;
            info.ap_mode = s == SCR_SETUP || s == SCR_WIFI_FAIL;
            info.ip = info.ap_mode ? "192.168.4.1" : "192.168.1.37";
            bool fits = screen_render((screen_t) s, &info, buf);
            char path[256];
            snprintf(path, sizeof(path), "%s/%s_%s.ppm", out, names[s], onames[o]);
            write_ppm(path, buf, (orient_t) o);
            if (!fits) printf("OVERFLOW %s\n", path), bad++;
        }
    // Worst case: longest allowed names must still fit (QR codes shrink to make room).
    info.wifi_ssid = "The-Longest-Network-Name-Ever-32";
    info.ap_ssid = "PhotoFrame-Living-Room-Upstairs";
    info.ap_pass = "a-very-long-hotspot-password-1234";
    for (int o = 0; o < 3; o++)
        for (int s = SCR_SETUP; s <= SCR_INFO; s++) {
            info.orient = (orient_t) o;
            info.ap_mode = s == SCR_SETUP || s == SCR_WIFI_FAIL || s == SCR_INFO;
            info.ip = info.ap_mode ? "192.168.4.1" : "192.168.1.37";
            char path[256];
            snprintf(path, sizeof(path), "%s/long_%s_%s.ppm", out, names[s], onames[o]);
            if (!screen_render((screen_t) s, &info, buf)) printf("OVERFLOW %s\n", path), bad++;
            write_ppm(path, buf, (orient_t) o);
        }
    // Low-battery badge over a mid-grey "photo"
    for (int o = 0; o < 3; o++) {
        for (int i = 0; i < (int) sizeof(buf); i++) buf[i] = (i / 400) % 2 ? 0x15 : 0x51;
        screen_badge(buf, (orient_t) o);
        char path[256];
        snprintf(path, sizeof(path), "%s/badge_%s.ppm", out, onames[o]);
        write_ppm(path, buf, (orient_t) o);
    }
    return bad ? 1 : 0;
}
