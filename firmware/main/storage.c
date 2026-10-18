#include "storage.h"
#include "board.h"
#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "bootloader_random.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

config_t g_cfg;
static sdmmc_card_t *card;
static card_state_t card_state = CARD_MISSING;
static atomic_int writers;

void storage_write_begin(void) { atomic_fetch_add(&writers, 1); }
void storage_write_end(void) { atomic_fetch_sub(&writers, 1); }
bool storage_writing(void) { return atomic_load(&writers) > 0; }

// Leftovers from uploads interrupted by sleep or power loss.
static void sweep_tmp_files(void) {
    DIR *d = opendir(PHOTO_DIR);
    if (!d) return;
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".tmp")) {
            snprintf(path, sizeof(path), PHOTO_DIR "/%s", e->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

bool storage_mount(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false, // never wipe someone's card
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.d1 = PIN_SD_D1;
    slot.d2 = PIN_SD_D2;
    slot.d3 = PIN_SD_D3;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_ROOT, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TF card mount failed: %s", esp_err_to_name(err));
        // ESP_FAIL comes from the FAT layer (card answered, no usable filesystem); driver errors such as
        // timeouts mean no card responded at all.
        card_state = err == ESP_FAIL ? CARD_UNREADABLE : CARD_MISSING;
        card = NULL;
        return false;
    }
    card_state = CARD_OK;
    mkdir(PHOTO_DIR, 0775);
    sweep_tmp_files();
    return true;
}

bool storage_ok(void) { return card != NULL; }
card_state_t storage_card_state(void) { return card_state; }

void storage_space(uint64_t *total, uint64_t *free_bytes) {
    *total = *free_bytes = 0;
    if (card) esp_vfs_fat_info(SD_ROOT, total, free_bytes);
}

/* ---- config.txt: plain key=value lines so it can also be edited by hand on a PC ---- */

#define OLD_SHARED_AP_PASS "photoframe" // every frame used this before per-frame passwords

// Per-frame hotspot password. It's shown on the setup screen with a join QR code, so being random costs
// the user nothing. No look-alike characters (0/o, 1/l/i), in case it's typed.
// Runs before Wi-Fi starts, when the hardware RNG has no RF noise to draw on; the SAR ADC entropy source is
// switched on just for this (ESP-IDF's documented approach for early-boot random numbers).
static void random_ap_pass(char *out) {
    static const char set[] = "abcdefghjkmnpqrstuvwxyz23456789";
    const unsigned n = sizeof(set) - 1, limit = 256 - 256 % n; // reject bytes >= limit: no modulo bias
    bootloader_random_enable();
    for (int i = 0; i < 10;) {
        uint8_t b;
        esp_fill_random(&b, 1);
        if (b < limit) out[i++] = set[b % n];
    }
    bootloader_random_disable();
    out[10] = 0;
}

// Last two bytes of the hotspot MAC, so several frames in one home get distinct names ("Halcyon-C121").
static void unique_name(char *out, size_t n, const char *base, bool upper) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, n, upper ? "%s-%02X%02X" : "%s-%02x%02x", base, mac[4], mac[5]);
}

static void config_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    unique_name(g_cfg.ap_ssid, sizeof(g_cfg.ap_ssid), "Halcyon", true);
    random_ap_pass(g_cfg.ap_pass);
    unique_name(g_cfg.hostname, sizeof(g_cfg.hostname), "halcyon", false);
    g_cfg.interval_min = 60;
    g_cfg.awake_min = 10;
    g_cfg.shuffle = false;
    g_cfg.rotate180 = true;
    g_cfg.orientation = ORIENT_LANDSCAPE;
    strcpy(g_cfg.tz, "CST6CDT,M3.2.0,M11.1.0");
    g_cfg.status_every = 60;
    g_cfg.status_min = 5;
    g_cfg.key_status = true;
    strcpy(g_cfg.temp_unit, "F");
    strcpy(g_cfg.cards, "indoor,battery,photos,sun");
    g_cfg.auto_setup = true;
}

const char *const ORIENT_NAMES[3] = {"landscape", "cw", "ccw"};

#define SET_STR(field) if (!strcmp(key, #field)) { snprintf(g_cfg.field, sizeof(g_cfg.field), "%s", val); continue; }
#define SET_INT(field) if (!strcmp(key, #field)) { g_cfg.field = atoi(val); continue; }

void config_load(void) {
    config_defaults();
    struct stat st;
    // config_save() unlinks then renames; finish an interrupted save rather than losing the settings.
    if (card && stat(CONFIG_PATH, &st) != 0 && stat(CONFIG_PATH ".tmp", &st) == 0) rename(CONFIG_PATH ".tmp", CONFIG_PATH);
    FILE *f = card ? fopen(CONFIG_PATH, "r") : NULL;
    if (!f) {
        ESP_LOGW(TAG, "no %s, using defaults", CONFIG_PATH);
        if (card) config_save();
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '='); // split on the first '=' so passwords may contain '='
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        SET_STR(wifi_ssid) SET_STR(wifi_pass) SET_STR(ap_ssid) SET_STR(ap_pass) SET_STR(hostname)
        SET_INT(interval_min) SET_INT(awake_min) SET_INT(shuffle) SET_INT(rotate180)
        SET_STR(tz) SET_STR(change_times) SET_STR(quiet_start) SET_STR(quiet_end) SET_STR(temp_unit) SET_STR(ntp_server)
        SET_INT(status_every) SET_INT(status_min) SET_INT(key_status) SET_INT(typeface)
        SET_STR(cards) SET_STR(zip) SET_STR(lat) SET_STR(lon) SET_INT(auto_setup)
        SET_STR(geo_zone) SET_STR(geo_lat) SET_STR(geo_lon)
        if (!strcmp(key, "orientation"))
            for (int i = 0; i < 3; i++)
                if (!strcmp(val, ORIENT_NAMES[i])) g_cfg.orientation = (orient_t) i;
    }
    fclose(f);
    // One-time upgrade from the old shared hotspot defaults. The hostname is left alone on purpose: changing
    // it would break the photoframe.local address people already use.
    bool upgraded = false;
    if (!strcmp(g_cfg.ap_pass, OLD_SHARED_AP_PASS)) random_ap_pass(g_cfg.ap_pass), upgraded = true;
    if (!strcmp(g_cfg.ap_ssid, "PhotoFrame"))
        unique_name(g_cfg.ap_ssid, sizeof(g_cfg.ap_ssid), "Halcyon", true), upgraded = true;
    if (upgraded) {
        ESP_LOGW(TAG, "hotspot is now \"%s\" with a per-frame password", g_cfg.ap_ssid);
        config_save();
    }
    if (g_cfg.interval_min < 0) g_cfg.interval_min = 0;
    if (g_cfg.awake_min < 0) g_cfg.awake_min = 0;
    if (g_cfg.status_every != 30 && g_cfg.status_every != 60) g_cfg.status_every = 0;
    if (g_cfg.status_min < 1) g_cfg.status_min = 1;
}

bool config_save(void) {
    if (!card) return false;
    storage_write_begin();
    FILE *f = fopen(CONFIG_PATH ".tmp", "w");
    if (!f) {
        storage_write_end();
        return false;
    }
    fprintf(f,
            "# Halcyon Frame settings. Edit here or in the web UI.\n"
            "wifi_ssid=%s\nwifi_pass=%s\nap_ssid=%s\nap_pass=%s\nhostname=%s\n"
            "interval_min=%d\nawake_min=%d\nshuffle=%d\nrotate180=%d\n"
            "# landscape, cw (frame turned right) or ccw (frame turned left)\norientation=%s\n"
            "# POSIX time zone, e.g. EST5EDT,M3.2.0,M11.1.0 / CST6CDT,M3.2.0,M11.1.0 / MST7 / PST8PDT,M3.2.0,M11.1.0\n"
            "tz=%s\n"
            "# change photos at these local times instead of every interval_min (empty = use the interval)\n"
            "change_times=%s\nquiet_start=%s\nquiet_end=%s\n"
            "# status screen: 0 off, 60 at the top of the hour, 30 at :00 and :30\n"
            "status_every=%d\nstatus_min=%d\nkey_status=%d\ntemp_unit=%s\nntp_server=%s\n"
            "# status screen: typeface 0-6 (Clean, Warm, Dramatic, Classic, Friendly, Modern, Technical) and four cards\n"
            "# from indoor, battery, photos, wifi, sun, moon, daylight, next\n"
            "typeface=%d\ncards=%s\nzip=%s\nlat=%s\nlon=%s\n"
            "# 1 = set time (pool.ntp.org), time zone and location automatically when on Wi-Fi; 0 = fully local\n"
            "auto_setup=%d\ngeo_zone=%s\ngeo_lat=%s\ngeo_lon=%s\n",
            g_cfg.wifi_ssid, g_cfg.wifi_pass, g_cfg.ap_ssid, g_cfg.ap_pass, g_cfg.hostname,
            g_cfg.interval_min, g_cfg.awake_min, g_cfg.shuffle, g_cfg.rotate180, ORIENT_NAMES[g_cfg.orientation],
            g_cfg.tz, g_cfg.change_times, g_cfg.quiet_start, g_cfg.quiet_end, g_cfg.status_every, g_cfg.status_min,
            g_cfg.key_status, g_cfg.temp_unit, g_cfg.ntp_server, g_cfg.typeface, g_cfg.cards, g_cfg.zip, g_cfg.lat,
            g_cfg.lon, g_cfg.auto_setup, g_cfg.geo_zone, g_cfg.geo_lat, g_cfg.geo_lon);
    bool ok = fclose(f) == 0;
    // FAT rename won't overwrite an existing file
    if (ok) {
        unlink(CONFIG_PATH);
        ok = rename(CONFIG_PATH ".tmp", CONFIG_PATH) == 0;
    }
    storage_write_end();
    return ok;
}

bool wifi_pass_valid(const char *pass) {
    size_t n = strlen(pass);
    if (n == 0 || (n >= 8 && n <= 63)) return true;
    if (n != 64) return false;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char) pass[i])) return false;
    return true;
}

/* ---- photos: <id>.epd = packed 4bpp panel frame, <id>.jpg = browser thumbnail ---- */

// Restricting ids to [A-Za-z0-9_-] rules out path traversal and keeps JSON output escape-free.
bool photo_id_valid(const char *id) {
    size_t n = strlen(id);
    if (n == 0 || n >= PHOTO_ID_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

void photo_path(char *out, size_t n, const char *id, const char *ext) {
    snprintf(out, n, PHOTO_DIR "/%s.%s", id, ext);
}

static int cmp_id(const void *a, const void *b) { return strcmp(a, b); }

int photo_list(char (*ids)[PHOTO_ID_MAX], int max) {
    DIR *d = card ? opendir(PHOTO_DIR) : NULL;
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".epd") != 0 || dot - e->d_name >= PHOTO_ID_MAX) continue;
        if (ids) {
            if (n >= max) break;
            memcpy(ids[n], e->d_name, dot - e->d_name);
            ids[n][dot - e->d_name] = 0;
        }
        n++;
    }
    closedir(d);
    if (ids) qsort(ids, n, PHOTO_ID_MAX, cmp_id);
    return n;
}

orient_t photo_orient(const char *id) {
    size_t n = strlen(id);
    if (n > 4 && !strcmp(id + n - 4, "_ccw")) return ORIENT_CCW;
    if (n > 3 && !strcmp(id + n - 3, "_cw")) return ORIENT_CW;
    return ORIENT_LANDSCAPE;
}

static int last_bad;
int photo_bad_count(void) { return last_bad; }

int photo_scan(orient_t o, int *bad) {
    char (*ids)[PHOTO_ID_MAX] = heap_caps_malloc(PHOTO_MAX * PHOTO_ID_MAX, MALLOC_CAP_SPIRAM);
    if (!ids) return 0;
    int n = photo_list(ids, PHOTO_MAX), valid = 0;
    *bad = 0;
    for (int i = 0; i < n; i++) {
        char path[64];
        struct stat st;
        photo_path(path, sizeof(path), ids[i], "epd");
        if (stat(path, &st) != 0 || st.st_size != EPD_BUF_SIZE) (*bad)++;
        else if (photo_orient(ids[i]) == o) valid++;
    }
    heap_caps_free(ids);
    last_bad = *bad;
    return valid;
}

bool photo_load(const char *id, uint8_t *buf) {
    char path[64];
    photo_path(path, sizeof(path), id, "epd");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(buf, 1, EPD_BUF_SIZE, f);
    fclose(f);
    return got == EPD_BUF_SIZE;
}

bool photo_delete(const char *id) {
    char path[64];
    photo_path(path, sizeof(path), id, "jpg");
    unlink(path);
    photo_path(path, sizeof(path), id, "epd");
    return unlink(path) == 0;
}
