// HTTP UI + JSON API. Image processing (resize, dithering, palette mapping, 4bpp packing) happens in the
// browser (www/index.html), so the device only stores and streams finished panel frames.
#include "web.h"
#include "app.h"
#include "board.h"
#include "display.h"
#include "pmic.h"
#include "storage.h"
#include "wifi.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

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
        else out[o++] = c;
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
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"sd\":%s,\"sd_total\":%llu,\"sd_free\":%llu,\"photos\":%d,\"current\":\"%s\",\"busy\":%s,"
             "\"battery\":%s,\"battery_mv\":%d,\"percent\":%d,\"charging\":%s,\"usb\":%s,"
             "\"ap_mode\":%s,\"ip\":\"%s\"}",
             storage_ok() ? "true" : "false", total, free_bytes, photo_list(NULL, 0), display_current(),
             display_busy() ? "true" : "false", p.battery ? "true" : "false", p.battery_mv, p.percent,
             p.charging ? "true" : "false", p.usb ? "true" : "false", wifi_is_ap() ? "true" : "false",
             wifi_ip());
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

// POST /api/upload?id=<id>&type=jpg|epd[&show=1]   body: raw file bytes
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
    if (epd && show[0] == '1') display_request(id);
    return send_ok(req);
}

static esp_err_t h_delete(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    char id[PHOTO_ID_MAX];
    if (!query_id(req, id)) return send_err(req, "400 Bad Request", "bad id");
    return photo_delete(id) ? send_ok(req) : send_err(req, "404 Not Found", "not found");
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
    char buf[512] = "{\"wifi_ssid\":";
    json_str(buf, sizeof(buf), g_cfg.wifi_ssid);
    strlcat(buf, ",\"ap_ssid\":", sizeof(buf));
    json_str(buf, sizeof(buf), g_cfg.ap_ssid);
    strlcat(buf, ",\"hostname\":", sizeof(buf));
    json_str(buf, sizeof(buf), g_cfg.hostname);
    // Passwords are write-only; only report whether one is set.
    size_t o = strlen(buf);
    snprintf(buf + o, sizeof(buf) - o,
             ",\"wifi_pass_set\":%s,\"ap_pass_set\":%s,\"interval_min\":%d,\"awake_min\":%d,"
             "\"shuffle\":%s,\"rotate180\":%s}",
             g_cfg.wifi_pass[0] ? "true" : "false", g_cfg.ap_pass[0] ? "true" : "false", g_cfg.interval_min,
             g_cfg.awake_min, g_cfg.shuffle ? "true" : "false", g_cfg.rotate180 ? "true" : "false");
    return send_json(req, buf);
}

// POST /api/settings, application/x-www-form-urlencoded. Only fields present are changed, so the UI
// omits password fields the user didn't touch.
static esp_err_t h_settings_post(httpd_req_t *req) {
    REQUIRE_CSRF(req);
    app_touch();
    char body[1024], v[200]; // values arrive URL-encoded: up to 3x their decoded length
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
    INT_FIELD(interval_min) INT_FIELD(awake_min) INT_FIELD(shuffle) INT_FIELD(rotate180)
    if (!c.ap_ssid[0] || !c.hostname[0]) return send_err(req, "400 Bad Request", "AP name and hostname required");
    if (!wifi_pass_valid(c.wifi_pass)) return send_err(req, "400 Bad Request", "Wi-Fi password must be 8-63 chars");
    if (!wifi_pass_valid(c.ap_pass)) return send_err(req, "400 Bad Request", "AP password must be 8-63 chars");
    g_cfg = c;
    return config_save() ? send_ok(req) : send_err(req, "500 Internal Server Error", "could not save config.txt");
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

void web_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
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
        {.uri = "/api/sleep", .method = HTTP_POST, .handler = h_sleep},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    ESP_LOGI(TAG, "web UI on http://%s/", wifi_ip());
}
