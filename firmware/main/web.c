// HTTP UI + JSON API. Image processing (resize, dithering, palette mapping, 4bpp packing) happens in the
// browser (www/index.html), so the device only stores and streams finished panel frames.
#include "web.h"
#include "app.h"
#include "board.h"
#include "clock.h"
#include "display.h"
#include "ota.h"
#include "typefaces.h"
#include "tzdb.h"
#include "pmic.h"
#include "storage.h"
#include "wifi.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "web";
#define IO_BUF 8192
#define THUMB_MAX (256 * 1024)
#define MAX_RECV_TIMEOUTS 3 // each is CONFIG_HTTPD recv_wait_timeout (5 s); then give up on a stalled client

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ---- helpers ---- */

// Every state-changing request must carry X-PF. A custom header forces a CORS preflight (which this
// server never answers), so other web pages open on the LAN can't trigger these endpoints.
static bool csrf_ok(httpd_req_t *req) { return httpd_req_get_hdr_value_len(req, "X-PF") > 0; }
#define REQUIRE_CSRF(req) \
    if (!csrf_ok(req)) return send_err(req, "403 Forbidden", "missing X-PF header")

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_ok(httpd_req_t *req) { return send_json(req, "{\"ok\":true}"); }

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, buf);
}

// Appends s as a JSON string literal. Settings strings are user-supplied, so quotes/controls are escaped.
static void json_str(char *out, size_t n, const char *s) {
    size_t o = strlen(out);
    if (o + 2 >= n) return;
    out[o++] = '"';
    for (; *s && o + 7 < n; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) o += snprintf(out + o, n - o, "\\u%04x", c);
        else if (c < 0x80) out[o++] = c;
        else {
            // Copy a well-formed UTF-8 sequence whole; anything else (e.g. a Latin-1 network name nearby)
            // becomes U+FFFD, since one bad byte would make the whole response unparseable.
            int len = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
            int k = 1;
            while (len && k < len && ((unsigned char) s[k] & 0xC0) == 0x80) k++;
            if (len && k == len && o + len < n - 1) { memcpy(out + o, s, len); o += len; s += len - 1; }
            else o += snprintf(out + o, n - o, "\\ufffd");
        }
    }
    out[o++] = '"';
    out[o] = 0;
}

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char) s[1]) && isxdigit((unsigned char) s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char) strtol(hex, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

// Reads ?id=... from the query string and validates it.
static bool query_id(httpd_req_t *req, char *id) {
    char q[96];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, "id", id, PHOTO_ID_MAX) == ESP_OK && photo_id_valid(id);
}

/* ---- handlers ---- */

static esp_err_t h_index(httpd_req_t *req) {
    app_touch();
    httpd_resp_set_type(req, "text/html");
    // Everything the page needs comes from the frame itself; this keeps it that way (inline code is ours).
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; img-src 'self' data: blob:; style-src 'self' 'unsafe-inline'; "
                       "script-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
    // EMBED_TXTFILES appends a NUL terminator; don't send it
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

// Deliberately doesn't call app_touch(): the UI polls this, and a forgotten browser tab must not keep the
// frame awake and drain the battery.
static esp_err_t h_status(httpd_req_t *req) {
    pmic_status_t p;
    pmic_status(&p);
    uint64_t total, free_bytes;
    storage_space(&total, &free_bytes);
    static const char *const WIFI_ERRS[] = {"", "auth", "not_found", "other"};
    static const char *const BATT[] = {"ok", "low", "critical"};
    char temp[12] = "null", rh[12] = "null";
    float tc, h;
    if (clock_climate(&tc, &h)) snprintf(temp, sizeof(temp), "%.1f", tc), snprintf(rh, sizeof(rh), "%.0f", h);
    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"sd\":%s,\"sd_total\":%llu,\"sd_free\":%llu,\"photos\":%d,\"bad_photos\":%d,\"current\":\"%s\","
             "\"busy\":%s,\"battery\":%s,\"battery_mv\":%d,\"percent\":%d,\"charging\":%s,\"usb\":%s,"
             "\"battery_state\":\"%s\",\"ap_mode\":%s,\"ip\":\"%s\",\"wifi_error\":\"%s\","
             "\"orientation\":\"%s\",\"reset_reason\":%d,\"uptime_s\":%lld,\"version\":\"%s\",\"slot\":\"%s\","
             "\"time\":%lld,\"clock_ok\":%s,\"temp_c\":%s,\"rh\":%s,\"rssi\":%d,\"wifi_ssid\":",
             storage_ok() ? "true" : "false", total, free_bytes, photo_list(NULL, 0), photo_bad_count(),
             display_current(), display_busy() ? "true" : "false", p.battery ? "true" : "false", p.battery_mv,
             p.percent, p.charging ? "true" : "false", p.usb ? "true" : "false", BATT[pmic_battery_level()],
             wifi_is_ap() ? "true" : "false", wifi_ip(), WIFI_ERRS[wifi_last_error()],
             ORIENT_NAMES[g_cfg.orientation], (int) esp_reset_reason(), esp_timer_get_time() / 1000000,
             ota_version(), ota_slot(), (long long) time(NULL), clock_valid() ? "true" : "false", temp, rh,
             wifi_rssi());
    json_str(buf, sizeof(buf), g_cfg.wifi_ssid);
    strlcat(buf, "}", sizeof(buf));
    return send_json(req, buf);
}

static esp_err_t h_photos(httpd_req_t *req) {
    app_touch();
    char (*ids)[PHOTO_ID_MAX] = heap_caps_malloc(PHOTO_MAX * PHOTO_ID_MAX, MALLOC_CAP_SPIRAM);
    if (!ids) return send_err(req, "500 Internal Server Error", "oom");
    int n = photo_list(ids, PHOTO_MAX);
    // Stream the array in chunks; ids are validated [A-Za-z0-9_-] so no escaping is needed.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "{\"photos\":[");
    char item[PHOTO_ID_MAX + 4];
    for (int i = 0; i < n; i++) {
        snprintf(item, sizeof(item), "%s\"%s\"", i ? "," : "", ids[i]);
        httpd_resp_sendstr_chunk(req, item);
    }
    heap_caps_free(ids);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// GET /photos/<id>.jpg | /photos/<id>.epd
static esp_err_t h_photo_file(httpd_req_t *req) {
    app_touch();
    char name[64];
    snprintf(name, sizeof(name), "%s", req->uri + strlen("/photos/"));
    name[strcspn(name, "?")] = 0;
    char *dot = strrchr(name, '.');
    if (!dot) return send_err(req, "404 Not Found", "not found");
    *dot = 0;
    const char *ext = dot + 1;
    bool jpg = !strcmp(ext, "jpg");
    if (!photo_id_valid(name) || (!jpg && strcmp(ext, "epd"))) return send_err(req, "404 Not Found", "not found");

    char path[64];
    photo_path(path, sizeof(path), name, ext);
    FILE *f = fopen(path, "rb");
    if (!f) return send_err(req, "404 Not Found", "not found");
    httpd_resp_set_type(req, jpg ? "image/jpeg" : "application/octet-stream");
    // Ids are never reused, so files under them never change.
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    char *buf = heap_caps_malloc(IO_BUF, MALLOC_CAP_SPIRAM);
    size_t n;
    esp_err_t err = ESP_OK;
    while (buf && (n = fread(buf, 1, IO_BUF, f)) > 0)
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) break;
    fclose(f);
    heap_caps_free(buf);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

// POST /api/upload?id=<id>&type=jpg|epd[&show=1|0]   body: raw file bytes
// The browser sends the thumbnail first, then the frame; a photo only appears once its .epd exists.
static esp_err_t h_upload(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    char q[128], id[PHOTO_ID_MAX], type[8] = "", show[4] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "id", id, sizeof(id)) != ESP_OK || !photo_id_valid(id) ||
        httpd_query_key_value(q, "type", type, sizeof(type)) != ESP_OK)
        return send_err(req, "400 Bad Request", "bad id or type");
    httpd_query_key_value(q, "show", show, sizeof(show));
    bool epd = !strcmp(type, "epd");
    if (!epd && strcmp(type, "jpg")) return send_err(req, "400 Bad Request", "bad type");
    if (epd ? req->content_len != EPD_BUF_SIZE : (req->content_len == 0 || req->content_len > THUMB_MAX))
        return send_err(req, "400 Bad Request", "bad size");
    if (!storage_ok()) return send_err(req, "503 Service Unavailable", "no TF card");

    char path[64], tmp[68];
    photo_path(path, sizeof(path), id, type);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    storage_write_begin();
    FILE *f = fopen(tmp, "wb");
    char *buf = heap_caps_malloc(IO_BUF, MALLOC_CAP_SPIRAM);
    if (!f || !buf) {
        if (f) fclose(f);
        heap_caps_free(buf);
        storage_write_end();
        return send_err(req, "500 Internal Server Error", "cannot write to TF card");
    }
    size_t left = req->content_len;
    bool ok = true;
    int timeouts = 0;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < IO_BUF ? left : IO_BUF);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) continue;
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t) r) { ok = false; break; }
        left -= r;
        app_touch();
    }
    ok = (fclose(f) == 0) && ok;
    heap_caps_free(buf);
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) unlink(tmp);
    storage_write_end();
    if (!ok) {
        ESP_LOGE(TAG, "upload of %s.%s failed", id, type);
        return send_err(req, "500 Internal Server Error", "write failed (card full?)");
    }
    ESP_LOGI(TAG, "stored %s.%s", id, type);
    // show=1: show now. No show param: a first photo still replaces a setup screen, since nobody wants the
    // instructions to stay up once they've been followed. show=0 (batch uploads): never touch the panel.
    bool on_status_screen = display_shown_screen() != SCR_NONE && photo_orient(id) == g_cfg.orientation;
    if (epd && (show[0] == '1' || (!show[0] && on_status_screen))) display_request(id);
    return send_ok(req);
}

static esp_err_t h_delete(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    char id[PHOTO_ID_MAX];
    if (!query_id(req, id)) return send_err(req, "400 Bad Request", "bad id");
    if (!photo_delete(id)) return send_err(req, "404 Not Found", "not found");
    app_rescan_photos(); // refresh the broken-photo count the UI shows, in the background
    return send_ok(req);
}

static esp_err_t h_show(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    char id[PHOTO_ID_MAX];
    if (!query_id(req, id)) return send_err(req, "400 Bad Request", "bad id");
    display_request(id);
    return send_ok(req);
}

static esp_err_t h_next(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    display_request(NULL);
    return send_ok(req);
}

static esp_err_t h_settings_get(httpd_req_t *req) {
    app_touch();
    char buf[1536] = "{\"wifi_ssid\":";
    json_str(buf, sizeof(buf), g_cfg.wifi_ssid);
    strlcat(buf, ",\"ap_ssid\":", sizeof(buf));
    json_str(buf, sizeof(buf), g_cfg.ap_ssid);
    strlcat(buf, ",\"hostname\":", sizeof(buf));
    json_str(buf, sizeof(buf), g_cfg.hostname);
    const struct { const char *k, *v; } strs[] = {{"tz", g_cfg.tz}, {"change_times", g_cfg.change_times},
        {"quiet_start", g_cfg.quiet_start}, {"quiet_end", g_cfg.quiet_end}, {"temp_unit", g_cfg.temp_unit},
        {"ntp_server", g_cfg.ntp_server}, {"cards", g_cfg.cards}, {"zip", g_cfg.zip}, {"lat", g_cfg.lat},
        {"lon", g_cfg.lon}, {"geo_zone", g_cfg.geo_zone}};
    for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        strlcat(buf, ",\"", sizeof(buf));
        strlcat(buf, strs[i].k, sizeof(buf));
        strlcat(buf, "\":", sizeof(buf));
        json_str(buf, sizeof(buf), strs[i].v);
    }
    // Passwords are write-only; only report whether one is set.
    size_t o = strlen(buf);
    snprintf(buf + o, sizeof(buf) - o,
             ",\"wifi_pass_set\":%s,\"ap_pass_set\":%s,\"interval_min\":%d,\"awake_min\":%d,"
             "\"shuffle\":%s,\"rotate180\":%s,\"orientation\":\"%s\",\"status_every\":%d,\"status_min\":%d,"
             "\"key_status\":%s,\"typeface\":%d,\"auto_setup\":%s,\"has_geo\":%s,\"hush\":%s,\"wizard_done\":%s}",
             g_cfg.wifi_pass[0] ? "true" : "false", g_cfg.ap_pass[0] ? "true" : "false", g_cfg.interval_min,
             g_cfg.awake_min, g_cfg.shuffle ? "true" : "false", g_cfg.rotate180 ? "true" : "false",
             ORIENT_NAMES[g_cfg.orientation], g_cfg.status_every, g_cfg.status_min, g_cfg.key_status ? "true" : "false",
             g_cfg.typeface, g_cfg.auto_setup ? "true" : "false", g_cfg.geo_lat[0] ? "true" : "false",
             g_cfg.hush ? "true" : "false", g_cfg.wizard_done ? "true" : "false");
    return send_json(req, buf);
}

// POST /api/settings, application/x-www-form-urlencoded. Only fields present are changed, so the UI
// omits password fields the user didn't touch.
static esp_err_t settings_post(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    char body[2048], v[200]; // values arrive URL-encoded: up to 3x their decoded length
    if (req->content_len >= sizeof(body)) return send_err(req, "400 Bad Request", "too large");
    int got = 0, timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;

    config_t c = g_cfg;
    esp_err_t err;
#define STR_FIELD(name)                                                                     \
    if ((err = httpd_query_key_value(body, #name, v, sizeof(v))) != ESP_ERR_NOT_FOUND) {    \
        if (err == ESP_OK) url_decode(v);                                                   \
        if (err != ESP_OK || strlen(v) >= sizeof(c.name) || strpbrk(v, "\r\n"))             \
            return send_err(req, "400 Bad Request", #name " invalid");                      \
        strcpy(c.name, v);                                                                  \
    }
#define INT_FIELD(name) \
    if (httpd_query_key_value(body, #name, v, sizeof(v)) == ESP_OK) c.name = atoi(v) < 0 ? 0 : atoi(v);
    STR_FIELD(wifi_ssid) STR_FIELD(wifi_pass) STR_FIELD(ap_ssid) STR_FIELD(ap_pass) STR_FIELD(hostname)
    STR_FIELD(tz) STR_FIELD(change_times) STR_FIELD(quiet_start) STR_FIELD(quiet_end) STR_FIELD(temp_unit)
    STR_FIELD(ntp_server) STR_FIELD(cards) STR_FIELD(zip) STR_FIELD(lat) STR_FIELD(lon)
    INT_FIELD(interval_min) INT_FIELD(awake_min) INT_FIELD(shuffle) INT_FIELD(rotate180)
    INT_FIELD(status_every) INT_FIELD(status_min) INT_FIELD(key_status) INT_FIELD(typeface) INT_FIELD(auto_setup)
    INT_FIELD(hush) INT_FIELD(wizard_done)
    if (c.typeface >= TYPEFACE_COUNT) c.typeface = 0;
    // Coordinates are typed by hand outside the US (the ZIP table is US-only); they feed the sun and moon cards.
    if (c.lat[0] || c.lon[0]) {
        char *e1, *e2;
        double la = strtod(c.lat, &e1), lo = strtod(c.lon, &e2);
        if (!c.lat[0] || !c.lon[0] || *e1 || *e2 || fabs(la) > 90 || fabs(lo) > 180)
            return send_err(req, "400 Bad Request", "latitude/longitude invalid");
    }
    // POSIX TZ strings only use these; anything else is a mistake or an attempt to inject markup.
    if (strspn(c.tz, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789,.:/+-<>") != strlen(c.tz))
        return send_err(req, "400 Bad Request", "time zone invalid");
    if (httpd_query_key_value(body, "orientation", v, sizeof(v)) == ESP_OK) {
        int o = -1;
        for (int i = 0; i < 3; i++)
            if (!strcmp(v, ORIENT_NAMES[i])) o = i;
        if (o < 0) return send_err(req, "400 Bad Request", "orientation invalid");
        c.orientation = (orient_t) o;
    }
    if (!c.ap_ssid[0] || !c.hostname[0]) return send_err(req, "400 Bad Request", "AP name and hostname required");
    // The hostname becomes a DNS label: letters, digits, hyphens.
    if (strspn(c.hostname, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != strlen(c.hostname))
        return send_err(req, "400 Bad Request", "hostname: letters, digits and - only");
    if (c.status_every != 30 && c.status_every != 60) c.status_every = 0;
    c.status_min = c.status_min < 1 ? 1 : c.status_min > 30 ? 30 : c.status_min;
    if (c.interval_min > 10080) c.interval_min = 10080;
    if (c.awake_min > 1440) c.awake_min = 1440;
    if (!wifi_pass_valid(c.wifi_pass)) return send_err(req, "400 Bad Request", "Wi-Fi password must be 8-63 chars");
    if (!wifi_pass_valid(c.ap_pass)) return send_err(req, "400 Bad Request", "AP password must be 8-63 chars");
    bool turned = c.orientation != g_cfg.orientation;
    bool tz_changed = strcmp(c.tz, g_cfg.tz) != 0;
    bool was_online = online_allowed();
    g_cfg = c;
    // Apply the privacy switch now, not at the next boot.
    if (was_online && !online_allowed()) clock_go_offline();
    else if (!was_online && online_allowed() && !wifi_is_ap()) clock_auto_setup();
    if (tz_changed) {
        setenv("TZ", g_cfg.tz[0] ? g_cfg.tz : "UTC0", 1);
        tzset();
    }
    app_schedule_changed(); // interval, set times, quiet hours or status timing may have changed
    if (!config_save()) return send_err(req, "500 Internal Server Error", "could not save config.txt");
    // Frame turned: switch to a photo cropped for the new orientation (or the matching status screen).
    if (turned) display_request(NULL);
    return send_ok(req);
}

// The whole read-modify-write runs under the settings lock, so a result from the auto-setup task can't be
// lost or interleaved with this save (every early return above stays inside it).
static esp_err_t h_settings_post(httpd_req_t *req) {
    config_lock();
    esp_err_t r = settings_post(req);
    config_unlock();
    return r;
}

// GET /api/wifi: nearby networks for the settings page. POST /api/wifi/scan refreshes the list first
// (POST because a scan briefly interrupts hotspot clients, so other pages shouldn't be able to trigger it).
static esp_err_t send_networks(httpd_req_t *req) {
    const wifi_net_t *nets;
    int n = wifi_networks(&nets);
    char buf[WIFI_SCAN_MAX * 100 + 32] = "{\"networks\":[";
    for (int i = 0; i < n; i++) {
        size_t o = strlen(buf);
        snprintf(buf + o, sizeof(buf) - o, "%s{\"ssid\":", i ? "," : "");
        json_str(buf, sizeof(buf), nets[i].ssid);
        o = strlen(buf);
        snprintf(buf + o, sizeof(buf) - o, ",\"rssi\":%d,\"secure\":%s}", nets[i].rssi, nets[i].secure ? "true" : "false");
    }
    strlcat(buf, "]}", sizeof(buf));
    return send_json(req, buf);
}

static esp_err_t h_wifi_get(httpd_req_t *req) {
    app_touch();
    return send_networks(req);
}

static esp_err_t h_wifi_scan(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    wifi_scan();
    return send_networks(req);
}

// GET /api/tz?zone=Europe/Paris -> {"posix":"CET-1CEST,..."}. The browser knows its own zone by IANA name; the frame
// holds the table that turns it into the rule it needs, so no internet lookup is involved.
static esp_err_t h_tz(httpd_req_t *req) {
    char q[96] = "", zone[64] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "zone", zone, sizeof(zone)) != ESP_OK)
        return send_err(req, "400 Bad Request", "zone required");
    url_decode(zone); // "America%2FChicago"
    const char *posix = tzdb_posix(zone);
    if (!posix) return send_err(req, "404 Not Found", "unknown zone");
    char buf[128] = "{\"posix\":";
    json_str(buf, sizeof(buf), posix);
    strlcat(buf, "}", sizeof(buf));
    return send_json(req, buf);
}

// POST /api/time  body: epoch=<seconds>. The web UI sends the browser's clock on load: that's how the frame
// gets the time without internet access. Small differences are ignored to avoid needless RTC writes.
static esp_err_t h_time(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    char body[48] = "", v[24];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return send_err(req, "400 Bad Request", "no body");
    body[n] = 0;
    if (httpd_query_key_value(body, "epoch", v, sizeof(v)) != ESP_OK) return send_err(req, "400 Bad Request", "epoch missing");
    long long t = atoll(v);
    if (t < 1735689600LL) return send_err(req, "400 Bad Request", "implausible time"); // before 2025
    if (llabs(t - (long long) time(NULL)) > 30) {
        clock_set((time_t) t);
        app_schedule_changed();
    }
    return send_ok(req);
}

// POST /api/ota   body: the app image (build/halcyonframe.bin). Restarts into it on success.
typedef struct {
    httpd_req_t *req;
    size_t left;
    int timeouts;
} ota_upload_t;

// Fills the chunk completely (except at the end), so the first one always holds the whole image header
// that ota_install() checks.
static int ota_recv(void *ctx, char *buf, size_t len) {
    ota_upload_t *u = ctx;
    size_t got = 0;
    while (got < len && u->left > 0) {
        size_t want = len - got < u->left ? len - got : u->left;
        int r = httpd_req_recv(u->req, buf + got, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++u->timeouts < MAX_RECV_TIMEOUTS) continue;
        if (r <= 0) return -1;
        u->left -= r, got += r;
        app_touch();
    }
    return (int) got;
}

static esp_err_t h_ota(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    if (req->content_len == 0) return send_err(req, "400 Bad Request", "empty file");
    if (pmic_battery_level() == BATT_CRITICAL) return send_err(req, "503 Service Unavailable", "battery too low, charge first");
    ota_upload_t u = {req, req->content_len, 0};
    esp_err_t err = ota_install(ota_recv, &u);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "not a valid firmware file for this frame (%s)", esp_err_to_name(err));
        return send_err(req, "400 Bad Request", msg);
    }
    send_ok(req);
    app_request_reboot();
    return ESP_OK;
}

static esp_err_t h_show_status(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    app_show_status();
    return send_ok(req);
}

static esp_err_t h_sleep(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    send_ok(req);
    app_request_sleep();
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    send_ok(req);
    app_request_reboot();
    return ESP_OK;
}

// DNS rebinding defence. A malicious web page can point its own domain at the frame's IP, and the
// browser then treats it as same-origin (so X-PF no longer helps). The Host header still carries that
// foreign domain, so only accept requests addressed to an IP literal or this frame's own names.
static char boot_host[33]; // hostname at start-up: still what mDNS advertises until the next restart

static bool host_ok(httpd_req_t *req) {
    char host[80];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    host[strcspn(host, ":")] = 0; // drop the port
    if (host[0] && strspn(host, "0123456789.") == strlen(host)) return true;
    char local[48];
    snprintf(local, sizeof(local), "%s.local", g_cfg.hostname);
    char boot_local[48];
    snprintf(boot_local, sizeof(boot_local), "%s.local", boot_host);
    return !strcasecmp(host, g_cfg.hostname) || !strcasecmp(host, local) || !strcasecmp(host, boot_host) ||
           !strcasecmp(host, boot_local);
}

// Every route goes through here; user_ctx holds the real handler.
static esp_err_t guarded(httpd_req_t *req) {
    if (!host_ok(req)) return send_err(req, "421 Misdirected Request", "unexpected Host header");
    return ((esp_err_t(*)(httpd_req_t *)) req->user_ctx)(req);
}

void web_start(void) {
    snprintf(boot_host, sizeof(boot_host), "%s", g_cfg.hostname);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/photos", .method = HTTP_GET, .handler = h_photos},
        {.uri = "/photos/*", .method = HTTP_GET, .handler = h_photo_file},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/show", .method = HTTP_POST, .handler = h_show},
        {.uri = "/api/next", .method = HTTP_POST, .handler = h_next},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = h_settings_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = h_settings_post},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = h_wifi_get},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = h_wifi_scan},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/time", .method = HTTP_POST, .handler = h_time},
        {.uri = "/api/tz", .method = HTTP_GET, .handler = h_tz},
        {.uri = "/api/show_status", .method = HTTP_POST, .handler = h_show_status},
        {.uri = "/api/sleep", .method = HTTP_POST, .handler = h_sleep},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t r = routes[i];
        r.user_ctx = (void *) r.handler;
        r.handler = guarded;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &r));
    }
    ESP_LOGI(TAG, "web UI on http://%s/", wifi_ip());
}
