#include "display.h"
#include "clock.h"
#include "app.h"
#include "ota.h"
#include "status.h"
#include <stdlib.h>
#include "board.h"
#include "epd.h"
#include "pmic.h"
#include "storage.h"
#include "wifi.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "display";
#define MAX_BAD_TRIES 5 // corrupt files skipped per request before giving up

// Survive deep sleep: where the slideshow is, and what the panel shows. The panel keeps its image with no
// power, so remembering a hash of it lets us skip 20 s refreshes that wouldn't change anything.
static RTC_DATA_ATTR char current_id[PHOTO_ID_MAX];
static RTC_DATA_ATTR uint32_t shown_hash; // 0 = unknown (after power-on)
static RTC_DATA_ATTR screen_t shown_screen; // SCR_NONE while a photo is up

typedef struct {
    screen_t screen; // SCR_NONE = photo request
    char id[PHOTO_ID_MAX]; // "" = next photo
} request_t;

static uint8_t *frame;
static SemaphoreHandle_t lock;
static QueueHandle_t queue;
// Requests queued or being drawn. A counter (not a flag) so busy stays true across back-to-back refreshes.
static atomic_int pending;

void display_init(void) {
    frame = heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    assert(frame);
    lock = xSemaphoreCreateMutex();
}

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h ? h : 1; // keep 0 for "unknown"
}

// Sends frame to the panel unless it already shows exactly this. Caller holds lock.
static void present(void) {
    uint32_t h = fnv1a(frame, EPD_BUF_SIZE);
    if (h == shown_hash) {
        ESP_LOGI(TAG, "panel already shows this, skipping refresh");
        return;
    }
    gpio_set_level(PIN_LED_GREEN, 0);
    epd_show(frame, g_cfg.rotate180);
    gpio_set_level(PIN_LED_GREEN, 1);
    shown_hash = h;
}

bool display_show(const char *id) {
    xSemaphoreTake(lock, portMAX_DELAY);
    // Advance even on failure, so a corrupt file can't pin the slideshow to itself forever.
    snprintf(current_id, sizeof(current_id), "%s", id);
    bool ok = photo_load(id, frame);
    if (ok) {
        ESP_LOGI(TAG, "showing %s", id);
        if (pmic_battery_level() == BATT_LOW) screen_badge(frame, photo_orient(id));
        present();
        history_record(id);
        shown_screen = SCR_NONE;
    } else {
        ESP_LOGE(TAG, "could not load %s (missing or wrong size)", id);
    }
    xSemaphoreGive(lock);
    return ok;
}

bool display_next(void) {
    char (*ids)[PHOTO_ID_MAX] = heap_caps_malloc(PHOTO_MAX * PHOTO_ID_MAX, MALLOC_CAP_SPIRAM);
    assert(ids);
    // Only photos cropped for the way the frame hangs now.
    int all = photo_list(ids, PHOTO_MAX), n = 0;
    for (int i = 0; i < all; i++)
        if (photo_orient(ids[i]) == g_cfg.orientation) memmove(ids[n++], ids[i], PHOTO_ID_MAX);
    play_t *play = heap_caps_malloc((n ? n : 1) * sizeof(play_t), MALLOC_CAP_SPIRAM);
    assert(play);
    history_load((const char (*)[PHOTO_ID_MAX]) ids, n, play);
    bool ok = false;
    for (int attempt = 0; attempt < MAX_BAD_TRIES && attempt < n && !ok; attempt++) {
        int cur = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(ids[i], current_id)) cur = i;
        // The hardware random numbers are only partly random while Wi-Fi is off, so mix in the clock too.
        uint32_t entropy = esp_random() ^ (uint32_t) time(NULL) * 2246822519u ^ (uint32_t) esp_timer_get_time();
        int pick = shuffle_pick(g_cfg.shuffle, n, play, cur, entropy);
        ok = display_show(ids[pick]);
    }
    heap_caps_free(play);
    heap_caps_free(ids);
    if (!n) ESP_LOGW(TAG, "no photos for this orientation");
    return ok;
}

static void gather(screen_info_t *in) {
    pmic_status_t p;
    pmic_status(&p);
    *in = (screen_info_t) {
        .orient = g_cfg.orientation,
        .ap_mode = wifi_is_ap(),
        .ap_ssid = g_cfg.ap_ssid,
        .ap_pass = g_cfg.ap_pass,
        .wifi_ssid = g_cfg.wifi_ssid,
        .wifi_err = wifi_last_error(),
        .ip = wifi_ip(),
        .hostname = g_cfg.hostname,
        .card = storage_card_state(),
        .has_battery = p.battery,
        .charging = p.charging,
        .battery_pct = p.percent,
        .photos = photo_list(NULL, 0),
        .interval_min = g_cfg.interval_min,
    };
    uint64_t total, free_bytes;
    storage_space(&total, &free_bytes);
    in->free_gb = free_bytes / 1e9f;
    in->now = time(NULL);
    in->clock_ok = clock_valid();
    in->has_climate = clock_climate(&in->temp_c, &in->rh);
    in->fahrenheit = g_cfg.temp_unit[0] != 'C';
    in->rssi = wifi_rssi();
    in->card_total = total, in->card_used = total - free_bytes;
    in->typeface = g_cfg.typeface;
    in->next_photo = app_next_photo();
    in->fw_version = ota_version();
    in->fw_incoming = ota_incoming_version();
    in->update_result = ota_last_result(&in->update_msg);
    in->boot = app_boot_screen();
    // A ZIP code entered in the UI wins; otherwise the automatic (IP-based) location.
    bool zip = g_cfg.lat[0] && g_cfg.lon[0], geo = g_cfg.auto_setup && g_cfg.geo_lat[0] && g_cfg.geo_lon[0];
    in->has_location = zip || geo;
    in->lat = atof(zip ? g_cfg.lat : g_cfg.geo_lat), in->lon = atof(zip ? g_cfg.lon : g_cfg.geo_lon);
    // cards=indoor,battery,photos,sun ; unknown or missing entries fall back to the default set
    static const uint8_t def[4] = {CARD_INDOOR, CARD_BATTERY, CARD_PHOTOS, CARD_SUN};
    memcpy(in->cards, def, 4);
    const char *cp = g_cfg.cards;
    for (int i = 0; i < 4 && *cp; i++) {
        size_t n = strcspn(cp, ",");
        for (int k = 0; k < CARD_COUNT; k++)
            if (strlen(CARD_NAMES[k]) == n && !strncmp(cp, CARD_NAMES[k], n)) in->cards[i] = k;
        cp += n + (cp[n] == ',');
    }
}

void display_screen(screen_t s) {
    screen_info_t info;
    gather(&info);
    xSemaphoreTake(lock, portMAX_DELAY);
    ESP_LOGI(TAG, "status screen %d", s);
    screen_render(s, &info, frame);
    present();
    shown_screen = s;
    xSemaphoreGive(lock);
}

static void display_task(void *arg) {
    request_t r;
    for (;;) {
        xQueueReceive(queue, &r, portMAX_DELAY);
        if (r.screen != SCR_NONE) display_screen(r.screen);
        else if (r.id[0]) display_show(r.id);
        else if (!display_next() && storage_ok())
            // Nothing to show in this orientation (e.g. it was just changed): explain instead of going blank.
            display_screen(wifi_is_ap() ? SCR_SETUP : SCR_READY);
        atomic_fetch_sub(&pending, 1);
    }
}

void display_task_start(void) {
    queue = xQueueCreate(4, sizeof(request_t));
    xTaskCreate(display_task, "display", 6144, NULL, 4, NULL);
}

static void enqueue(const request_t *r) {
    atomic_fetch_add(&pending, 1); // counted before queueing so the sleep check can't race it
    if (xQueueSend(queue, r, 0) != pdTRUE) {
        atomic_fetch_sub(&pending, 1);
        ESP_LOGW(TAG, "display queue full, dropped request");
    }
}

void display_request(const char *id) {
    request_t r = {.screen = SCR_NONE};
    if (id) snprintf(r.id, sizeof(r.id), "%s", id);
    enqueue(&r);
}

void display_request_screen(screen_t s) {
    request_t r = {.screen = s};
    enqueue(&r);
}

bool display_busy(void) { return atomic_load(&pending) > 0; }
bool display_known(void) { return shown_hash != 0; }
screen_t display_shown_screen(void) { return shown_screen; }
const char *display_current(void) { return current_id; }
