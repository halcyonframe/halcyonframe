#include "clock.h"
#include "pmic.h"
#include "app.h"
#include "storage.h"
#include "tzdb.h"
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

static const char *TAG = "clock";

// Register and command values from the NXP PCF85063A and Sensirion SHTC3 datasheets.
#define RTC_ADDR 0x51
#define RTC_REG_SECONDS 0x04 // seconds..years follow; bit 7 of seconds (OS) = oscillator stopped, time invalid
#define SHT_ADDR 0x70
#define SHT_WAKEUP 0x3517
#define SHT_MEASURE 0x7866 // normal mode, temperature first, no clock stretching
#define SHT_SLEEP 0xB098
#define VALID_AFTER 1735689600 // 2025-01-01: anything earlier means the clock was never set

static i2c_master_dev_handle_t rtc, sht;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static uint8_t bin2bcd(int v) { return (uint8_t) ((v / 10) << 4 | (v % 10)); }

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil). Used instead of
// timegm(), which newlib doesn't reliably provide, to turn the RTC's UTC fields into a time_t.
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t) era * 146097 + doe - 719468;
}

bool clock_valid(void) { return time(NULL) > VALID_AFTER; }

static void set_system(time_t utc) {
    struct timeval tv = {.tv_sec = utc};
    settimeofday(&tv, NULL);
}

static void rtc_write(time_t utc) {
    struct tm t;
    gmtime_r(&utc, &t);
    // One burst from the seconds register: the chip freezes its counters during the access, so writing
    // seconds..years together keeps them consistent. Seconds bit 7 = 0 also clears the OS flag.
    uint8_t b[8] = {RTC_REG_SECONDS, bin2bcd(t.tm_sec), bin2bcd(t.tm_min), bin2bcd(t.tm_hour), bin2bcd(t.tm_mday),
                    (uint8_t) t.tm_wday, bin2bcd(t.tm_mon + 1), bin2bcd(t.tm_year % 100)};
    if (i2c_master_transmit(rtc, b, sizeof(b), 100) != ESP_OK) ESP_LOGW(TAG, "RTC write failed");
}

void clock_set(time_t utc) {
    set_system(utc);
    rtc_write(utc);
    ESP_LOGI(TAG, "time set");
}

void clock_init(const char *posix_tz) {
    setenv("TZ", posix_tz[0] ? posix_tz : "UTC0", 1);
    tzset();
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .scl_speed_hz = 100000};
    dc.device_address = RTC_ADDR;
    i2c_master_bus_add_device(board_i2c_bus(), &dc, &rtc);
    dc.device_address = SHT_ADDR;
    i2c_master_bus_add_device(board_i2c_bus(), &dc, &sht);

    // The ESP32 keeps counting through deep sleep, but on its internal RC oscillator, which drifts by
    // seconds per hour with temperature. The PCF85063 runs on a crystal, so take the time from it on every
    // boot and wake (one I2C read) whenever it holds a valid time.
    uint8_t reg = RTC_REG_SECONDS, t[7];
    if (i2c_master_transmit_receive(rtc, &reg, 1, t, sizeof(t), 100) != ESP_OK) {
        ESP_LOGW(TAG, "RTC not answering");
        return;
    }
    if (t[0] & 0x80) {
        ESP_LOGW(TAG, "RTC lost its time; waiting for the web UI or NTP to set it");
        return;
    }
    int64_t days = days_from_civil(2000 + bcd2bin(t[6]), bcd2bin(t[5] & 0x1F), bcd2bin(t[3] & 0x3F));
    set_system(days * 86400 + bcd2bin(t[2] & 0x3F) * 3600 + bcd2bin(t[1] & 0x7F) * 60 + bcd2bin(t[0] & 0x7F));
    ESP_LOGI(TAG, "time restored from RTC");
}

static void ntp_synced(struct timeval *tv) {
    rtc_write(tv->tv_sec);
    app_schedule_changed(); // the clock may have jumped
}

void clock_start_ntp(const char *server) {
    if (!server[0]) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.sync_cb = ntp_synced;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) ESP_LOGI(TAG, "NTP from %s", server);
}

static bool sht_cmd(uint16_t cmd) {
    uint8_t b[2] = {cmd >> 8, cmd & 0xFF};
    return i2c_master_transmit(sht, b, 2, 50) == ESP_OK;
}

// CRC-8, polynomial 0x31, init 0xFF, no reflection (SHTC3 datasheet).
static uint8_t crc8(const uint8_t *d, int n) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = crc & 0x80 ? (uint8_t) (crc << 1) ^ 0x31 : (uint8_t) (crc << 1);
    }
    return crc;
}

bool clock_climate(float *temp_c, float *rh) {
    uint8_t r[6];
    bool ok = sht_cmd(SHT_WAKEUP);
    esp_rom_delay_us(500); // wake-up takes at most 240 us (a vTaskDelay this short would round to 0 ticks)
    ok = ok && sht_cmd(SHT_MEASURE);
    // A normal-mode measurement takes at most 12.1 ms; without clock stretching the sensor NACKs reads until
    // it's done. 20 ms is two ticks at the default 100 Hz tick, so this really waits long enough.
    vTaskDelay(pdMS_TO_TICKS(20));
    ok = ok && i2c_master_receive(sht, r, sizeof(r), 50) == ESP_OK && crc8(r, 2) == r[2] && crc8(r + 3, 2) == r[5];
    sht_cmd(SHT_SLEEP); // the sensor idles at a higher current when left awake
    if (!ok) return false;
    *temp_c = -45 + 175.0f * (r[0] << 8 | r[1]) / 65536;
    *rh = 100.0f * (r[3] << 8 | r[4]) / 65536;
    return true;
}

/* ---- automatic time zone and location (auto_setup) ---- */

// ip-api.com: free, no key, HTTP only on the free tier, non-commercial use. Returns the IANA zone and
// city-level coordinates for the network's public IP. Runs once per wake on home Wi-Fi, off the main path.
#define GEO_URL "http://ip-api.com/json/?fields=status,timezone,lat,lon"

static void geo_task(void *arg) {
    char body[256] = "";
    esp_http_client_config_t hc = {.url = GEO_URL, .timeout_ms = 6000};
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    int n = 0;
    if (h && esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0)
        n = esp_http_client_read(h, body, sizeof(body) - 1);
    if (h) esp_http_client_cleanup(h);
    body[n > 0 ? n : 0] = 0;

    cJSON *j = cJSON_Parse(body);
    const cJSON *zone = cJSON_GetObjectItemCaseSensitive(j, "timezone"), *lat = cJSON_GetObjectItemCaseSensitive(j, "lat"),
                *lon = cJSON_GetObjectItemCaseSensitive(j, "lon");
    // The lookup is plain HTTP, so treat the answer as untrusted: only a zone name from our own table is
    // accepted (that also rules out line breaks that could inject lines into config.txt), and the
    // coordinates are re-formatted as numbers.
    const char *posix = cJSON_IsString(zone) ? tzdb_posix(zone->valuestring) : NULL;
    if (posix && cJSON_IsNumber(lat) && cJSON_IsNumber(lon) && fabs(lat->valuedouble) <= 90 && fabs(lon->valuedouble) <= 180) {
        char la[16], lo[16];
        snprintf(la, sizeof(la), "%.2f", lat->valuedouble); // ~1 km: city level is all the sun cards need
        snprintf(lo, sizeof(lo), "%.2f", lon->valuedouble);
        config_lock();
        bool changed = strcmp(g_cfg.geo_zone, zone->valuestring) || strcmp(g_cfg.geo_lat, la) || strcmp(g_cfg.geo_lon, lo) ||
                       strcmp(g_cfg.tz, posix);
        if (changed) {
            snprintf(g_cfg.geo_zone, sizeof(g_cfg.geo_zone), "%s", zone->valuestring);
            snprintf(g_cfg.geo_lat, sizeof(g_cfg.geo_lat), "%s", la);
            snprintf(g_cfg.geo_lon, sizeof(g_cfg.geo_lon), "%s", lo);
            snprintf(g_cfg.tz, sizeof(g_cfg.tz), "%s", posix);
            setenv("TZ", g_cfg.tz, 1);
            tzset();
            config_save(); // only on change: the card shouldn't be rewritten every wake
            app_schedule_changed();
        }
        config_unlock();
        ESP_LOGI(TAG, "auto setup: zone %s", zone->valuestring);
    } else {
        ESP_LOGW(TAG, "auto setup lookup failed or returned an unknown zone");
    }
    cJSON_Delete(j);
    vTaskDelete(NULL);
}

void clock_auto_setup(void) {
    if (!g_cfg.auto_setup) return;
    clock_start_ntp(g_cfg.ntp_server[0] ? g_cfg.ntp_server : "pool.ntp.org");
    xTaskCreate(geo_task, "geo", 6144, NULL, 3, NULL);
}
