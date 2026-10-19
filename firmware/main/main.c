// Halcyon Frame firmware for the Waveshare ESP32-S3-PhotoPainter. Fully local: photos and config.txt on the
// TF card, web UI served by the frame, no cloud services.
//
// Wake behaviour:
//   timer wake            -> whatever the schedule says is due (next photo, status screen in or out), sleep again
//   BOOT button           -> next photo, sleep again (Wi-Fi never starts on these wakes: saves battery)
//   power-on / KEY button -> start Wi-Fi + web UI, stay up until awake_min of inactivity, then sleep
// While awake: BOOT = next photo; KEY press = status screen (time, weather, battery...); KEY held 2 s =
// "how to reach me"; PWR = sleep now. Holding PWR 4 s powers off (AXP2101).
//
// When there is nothing to show or something needs attention (no photos yet, Wi-Fi failed, no card,
// battery empty), the panel shows a status screen instead (screens.c), so a person always sees what to do.
#include "app.h"
#include "board.h"
#include "clock.h"
#include "display.h"
#include "epd.h"
#include "ota.h"
#include "pmic.h"
#include "schedule.h"
#include "storage.h"
#include "web.h"
#include "wifi.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";
#define CRITICAL_RECHECK_MIN (6 * 60)
#define BOOT_INFO_MS 60000 // after a reset, the info screen stays up until this point, then the photo returns
#define KEY_HOLD_MS 2000
#define UPDATE_READ_MS 30000 // "Updating firmware" stays up this long after it's drawn, so it can be read
#define FAULT_BACKOFF_MIN 30 // after repeated crashes or brownouts, rest this long before trying again
#define FAULT_MAGIC 0x46415554

// 32-bit ms so it is read/written atomically across cores (wraps after ~49 days; unsigned math handles it)
static atomic_uint last_activity_ms;
static volatile bool want_sleep, want_reboot, wifi_up;
static bool boot_info_pending; // info screen shown at reset, photo not yet restored
static volatile bool reschedule; // settings or clock changed: recompute the next photo change
// Schedule state (next change, status screen timing) must survive deep sleep.
static RTC_DATA_ATTR sched_t sched;
// Consecutive abnormal resets (brownout, panic, watchdog). RTC_NOINIT survives those resets; the magic
// value rejects power-on garbage.
static RTC_NOINIT_ATTR uint32_t fault_magic, fault_count;
static bool healthy = true; // false while backing off: no panel refreshes, and a crashing new firmware must not be kept

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }
void app_touch(void) { atomic_store(&last_activity_ms, now_ms()); }
void app_request_sleep(void) { want_sleep = true; }
void app_request_reboot(void) { want_reboot = true; }
void app_schedule_changed(void) { reschedule = true; }
time_t app_next_photo(void) { return sched.next_photo; }
bool app_boot_screen(void) { return boot_info_pending; }

// The new firmware is installed: put "Updating firmware" up before the restart. During boot (a
// firmware.bin on the card) the display task isn't running yet, so draw directly.
static bool display_task_up;
static void update_started(void) {
    if (display_task_up) display_request_screen(SCR_UPDATE);
    else display_screen(SCR_UPDATE);
}

static void gpio_setup(void) {
    gpio_config_t leds = {.pin_bit_mask = BIT64(PIN_LED_RED) | BIT64(PIN_LED_GREEN), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&leds);
    gpio_set_level(PIN_LED_RED, 1); // active-low: off
    gpio_set_level(PIN_LED_GREEN, 1);

    gpio_config_t low_btns = {
        .pin_bit_mask = BIT64(PIN_BTN_BOOT) | BIT64(PIN_BTN_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&low_btns);
    gpio_config_t pwr_btn = {
        .pin_bit_mask = BIT64(PIN_BTN_PWR),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&pwr_btn);
}

// Answer to a BOOT press while asleep when there's nothing new to show (no photos, or battery empty).
static void blink_red_twice(void) {
    for (int i = 0; i < 2; i++) {
        gpio_set_level(PIN_LED_RED, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(PIN_LED_RED, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static int parse_hhmm(const char *s) {
    int h, m;
    return sscanf(s, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60 ? h * 60 + m : -1;
}

// Settings -> schedule inputs (cheap; done before every decision so web changes apply immediately).
static void sched_settings(void) {
    int qs = parse_hhmm(g_cfg.quiet_start), qe = parse_hhmm(g_cfg.quiet_end);
    sched.clock_ok = clock_valid();
    sched.interval_min = g_cfg.interval_min;
    sched.change_times = g_cfg.change_times;
    sched.quiet_start = qs < 0 || qe < 0 ? 0 : qs, sched.quiet_end = qs < 0 || qe < 0 ? 0 : qe;
    sched.status_every = g_cfg.status_every;
    sched.status_min = g_cfg.status_min;
    if (reschedule) {
        // Settings changed or the clock jumped (browser, NTP): drop timing state that may now point hours
        // into the future or past, so photo changes and status slots carry on from the new time.
        time_t now = time(NULL);
        sched.next_photo = 0;
        if (sched.status_until > now + g_cfg.status_min * 60) sched.status_until = now;
        if (sched.last_slot > now) sched.last_slot = 0;
        reschedule = false;
    }
    if (!sched.next_photo) sched.next_photo = sched_next_change(&sched, time(NULL));
}

static bool have_photos(void) { return storage_ok() && photo_count(g_cfg.orientation) > 0; }

// Counting broken photo files means opening every one: far too slow for the wake path with thousands of
// photos, so it runs once in the background while awake (for the web UI's warning banner).
static void bad_scan_task(void *arg) {
    int bad;
    photo_scan(g_cfg.orientation, &bad);
    if (bad) ESP_LOGW(TAG, "%d photo file(s) have the wrong size and will be skipped", bad);
    vTaskDelete(NULL);
}
void app_rescan_photos(void) {
    if (storage_ok()) xTaskCreate(bad_scan_task, "scan", 4096, NULL, 1, NULL);
}

// Status screen up for status_min minutes (timed slot or a KEY press).
static void show_status(bool async) {
    sched.status_until = time(NULL) + g_cfg.status_min * 60;
    if (async) display_request_screen(SCR_STATUS);
    else display_screen(SCR_STATUS);
}

void app_show_status(void) { show_status(true); }

// Back to a photo: the next one if a change came due meanwhile, else the one that was up.
static void restore_photo(bool async) {
    time_t now = time(NULL);
    bool due = sched.next_photo && now >= sched.next_photo - 5;
    if (due) sched.next_photo = sched_next_change(&sched, now);
    const char *cur = display_current();
    bool same = !due && cur[0] && photo_orient(cur) == g_cfg.orientation;
    if (async) display_request(same ? cur : NULL);
    else if (!(same && display_show(cur))) display_next();
}

// Acts on whatever the schedule says is due now.
static void run_schedule(bool async) {
    sched_settings();
    time_t now = time(NULL);
    switch (sched_due(&sched, now)) {
    case SCHED_SHOW_STATUS:
        ESP_LOGI(TAG, "scheduled status screen");
        sched.last_slot = sched_slot(&sched, now + 5);
        show_status(async);
        break;
    case SCHED_END_STATUS:
        sched.status_until = 0;
        if (have_photos()) restore_photo(async);
        break;
    case SCHED_NEXT_PHOTO:
        sched.next_photo = sched_next_change(&sched, now);
        if (have_photos()) {
            if (async) display_request(NULL);
            else display_next();
        }
        break;
    default:
        break;
    }
}

// Seconds until the next timer wake (0 = none).
static int64_t sleep_seconds(void) {
    if (pmic_battery_level() == BATT_CRITICAL) return CRITICAL_RECHECK_MIN * 60;
    sched_settings();
    if (!have_photos()) sched.next_photo = 0; // nothing to change: only status slots (if any) wake us
    time_t now = time(NULL), wake = sched_next_wake(&sched, now);
    if (!storage_ok() && g_cfg.interval_min > 0) return g_cfg.interval_min * 60; // resume once the card is back
    if (!wake) return 0;
    return wake - now < 30 ? 30 : wake - now;
}

static void deep_sleep(int64_t wake_s) {
    // Never cut a refresh, an SD write or a firmware update short.
    while (display_busy() || storage_writing() || ota_busy()) vTaskDelay(pdMS_TO_TICKS(200));
    // Waking from sleep is a reset: a new firmware that got this far must confirm itself first, or the
    // bootloader would roll it back on the next wake.
    if (healthy) ota_mark_good();
    // These screens only make sense while someone is interacting; put the photo back before sleeping.
    screen_t s = display_shown_screen();
    if (healthy && (s == SCR_WIFI_FAIL || s == SCR_INFO) && have_photos()) restore_photo(false);
    if (wifi_up) wifi_stop();
    gpio_set_level(PIN_LED_RED, 1);
    gpio_set_level(PIN_LED_GREEN, 1);

    const uint64_t wake_pins = BIT64(PIN_BTN_BOOT) | BIT64(PIN_BTN_KEY);
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(wake_pins, ESP_EXT1_WAKEUP_ANY_LOW));
    // Internal pull-ups on the wake pins need the RTC peripheral domain powered during sleep.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pulldown_dis(PIN_BTN_BOOT);
    rtc_gpio_pullup_en(PIN_BTN_BOOT);
    rtc_gpio_pulldown_dis(PIN_BTN_KEY);
    rtc_gpio_pullup_en(PIN_BTN_KEY);
    if (wake_s > 0) esp_sleep_enable_timer_wakeup(wake_s * 1000000ULL);

    ESP_LOGI(TAG, "deep sleep (timer: %lld s)", (long long) wake_s);
    esp_deep_sleep_start();
}

// What the panel should show once any boot-time screen is done. First match wins (see screens.h).
static void show_normal(void) {
    int photos = photo_count(g_cfg.orientation);
    if (g_cfg.wifi_ssid[0] && wifi_is_ap()) display_request_screen(SCR_WIFI_FAIL);
    else if (!photos) display_request_screen(wifi_is_ap() ? SCR_SETUP : SCR_READY);
    // After power-on we don't know what's on the panel (often the vendor firmware's last image).
    else if (!display_known() || display_shown_screen() != SCR_NONE) display_request(NULL);
}

static void awake_loop(void) {
    // Seed with the current levels so a button still held from the wake press isn't seen as a new press.
    int boot_prev = gpio_get_level(PIN_BTN_BOOT), key_prev = gpio_get_level(PIN_BTN_KEY);
    int pwr_prev = gpio_get_level(PIN_BTN_PWR);
    uint32_t key_down = 0, last_sched = now_ms();
    bool key_held = !key_prev; // a press that woke us was already handled at boot
    app_touch();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30)); // also acts as debounce
        int boot = gpio_get_level(PIN_BTN_BOOT), key = gpio_get_level(PIN_BTN_KEY), pwr = gpio_get_level(PIN_BTN_PWR);
        if (boot_prev && !boot) {
            app_touch();
            sched.status_until = 0;
            display_request(NULL);
        }
        // KEY: a short press shows the status screen, holding it 2 s shows "how to reach me".
        if (key_prev && !key) key_down = now_ms(), key_held = false, app_touch();
        bool short_press = !key_prev && key && !key_held, long_press = !key && !key_held && now_ms() - key_down >= KEY_HOLD_MS;
        if (short_press || long_press) {
            key_held = true;
            app_touch();
            if (!storage_ok()) esp_restart(); // the card screen says "fix it, then press KEY": remount
            if (long_press) display_request_screen(SCR_INFO);
            else if (!display_busy()) show_status(true);
        }
        if (!pwr_prev && pwr) want_sleep = true;
        boot_prev = boot, key_prev = key, pwr_prev = pwr;

        if (want_reboot) {
            vTaskDelay(pdMS_TO_TICKS(500)); // let the HTTP response flush
            while (display_busy() || storage_writing() || ota_busy()) vTaskDelay(pdMS_TO_TICKS(200));
            // The update screen takes ~20 s to draw; restarting the moment it's done gives nobody time to read it.
            if (display_shown_screen() == SCR_UPDATE) vTaskDelay(pdMS_TO_TICKS(UPDATE_READ_MS));
            esp_restart();
        }
        // The reset info screen has had its minute: back to the photo (unless someone moved on already).
        if (boot_info_pending && now_ms() > BOOT_INFO_MS && !display_busy()) {
            boot_info_pending = false;
            if (display_shown_screen() == SCR_INFO) show_normal();
        }
        // Timed things (status slots, photo changes) also happen while awake.
        if (now_ms() - last_sched > 10000 && !display_busy()) {
            last_sched = now_ms();
            run_schedule(true);
        }
        bool idle = g_cfg.awake_min > 0 && !display_busy() && !ota_busy() &&
                    now_ms() - atomic_load(&last_activity_ms) > g_cfg.awake_min * 60000U;
        if (want_sleep || idle) {
            vTaskDelay(pdMS_TO_TICKS(500));
            deep_sleep(sleep_seconds());
        }
    }
}

void app_main(void) {
    gpio_setup();
    esp_err_t err = nvs_flash_init(); // required by the Wi-Fi driver for calibration data only
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_log_level_set("QRCODE", ESP_LOG_WARN); // it logs the encoded text, which includes the hotspot password
    pmic_init();
    epd_init();
    display_init();
    bool sd = storage_mount();
    config_load();
    clock_init(g_cfg.tz);
    ota_init();
    ota_set_start_hook(update_started);
    // Why we started. A crash or brownout shows up here (and in /api/status) even without a serial cable.
    esp_reset_reason_t why = esp_reset_reason();
    static const char *const REASONS[] = {"unknown", "power-on", "external", "software", "panic", "int-watchdog",
                                          "task-watchdog", "watchdog", "deep-sleep", "brownout", "sdio",
                                          "usb", "jtag", "efuse", "power-glitch", "cpu-lockup"}; // esp_reset_reason_t order
    ESP_LOGI(TAG, "reset reason: %s, firmware %s", why < sizeof(REASONS) / sizeof(REASONS[0]) ? REASONS[why] : "other",
             ota_version());
    if (why != ESP_RST_DEEPSLEEP) sched = (sched_t) {0}; // RTC memory is only meaningful across deep sleep

    // A brownout during a refresh at low battery, or a crash, followed by the full start-up (Wi-Fi + two
    // panel refreshes) would likely trip it again. After two in a row, rest instead of looping.
    bool fault = why == ESP_RST_BROWNOUT || why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT ||
                 why == ESP_RST_WDT;
    if (fault_magic != FAULT_MAGIC) fault_magic = FAULT_MAGIC, fault_count = 0;
    fault_count = fault ? fault_count + 1 : 0;
    if (fault_count >= 2) {
        ESP_LOGE(TAG, "%u abnormal resets in a row: resting %d min", (unsigned) fault_count, FAULT_BACKOFF_MIN);
        healthy = false;
        deep_sleep(FAULT_BACKOFF_MIN * 60);
    }

    // firmware.bin on the card: install and restart into it. Not on a nearly empty battery, where power
    // could fail mid-write (the old firmware would still boot, but there's no point trying).
    if (sd && pmic_battery_level() != BATT_CRITICAL) {
        gpio_set_level(PIN_LED_GREEN, 0);
        bool installed = ota_from_card();
        gpio_set_level(PIN_LED_GREEN, 1);
        if (installed) {
            vTaskDelay(pdMS_TO_TICKS(UPDATE_READ_MS)); // the update screen (drawn once the image is installed) stays readable
            esp_restart();
        }
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    uint64_t pins = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
    bool boot_btn = pins & BIT64(PIN_BTN_BOOT), key_btn = pins & BIT64(PIN_BTN_KEY);

    // Nearly empty: put "Please charge me" up while there's still power to draw it; it stays after the
    // battery dies. No Wi-Fi, whatever woke us.
    if (pmic_battery_level() == BATT_CRITICAL) {
        ESP_LOGW(TAG, "battery critical");
        if (boot_btn) blink_red_twice();
        display_screen(SCR_BATTERY); // no refresh if it's already up
        deep_sleep(CRITICAL_RECHECK_MIN * 60);
    }

    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        run_schedule(false); // quiet wake: whatever is due, no Wi-Fi
        deep_sleep(sleep_seconds());
    }
    if (boot_btn) {
        // Next photo on request; the slideshow interval restarts from now.
        sched_settings();
        sched.status_until = 0;
        sched.next_photo = sched_next_change(&sched, time(NULL));
        if (!(sd && display_next())) blink_red_twice();
        deep_sleep(sleep_seconds());
    }

    // KEY woke us: held for 2 s means "how to reach me", a short press the status screen.
    bool key_hold = false;
    if (key_btn) {
        while (!gpio_get_level(PIN_BTN_KEY) && now_ms() < KEY_HOLD_MS) vTaskDelay(pdMS_TO_TICKS(20));
        key_hold = !gpio_get_level(PIN_BTN_KEY);
    }

    ESP_LOGI(TAG, "starting web UI (wake cause %d)", cause);
    gpio_set_level(PIN_LED_RED, 0); // red = awake and reachable
    display_task_start();
    display_task_up = true;
    if (!sd) display_request_screen(SCR_CARD); // drawn while Wi-Fi comes up
    wifi_start();
    wifi_up = true;
    if (!wifi_is_ap()) {
        if (g_cfg.auto_setup) clock_auto_setup();
        else clock_start_ntp(g_cfg.ntp_server);
    }
    web_start();
    app_rescan_photos();
    ota_mark_good(); // Wi-Fi and the web UI came up: this firmware is good enough to keep

    if (sd) {
        bool wifi_failed = g_cfg.wifi_ssid[0] && wifi_is_ap();
        if (wifi_failed) {
            show_normal(); // its own screen says what to fix
        } else if (key_hold || why != ESP_RST_DEEPSLEEP) {
            // After a reset (or KEY held), the first minute shows where to find the frame and how it's doing.
            display_request_screen(SCR_INFO);
            boot_info_pending = true;
        } else if (key_btn && g_cfg.key_status) {
            sched_settings();
            show_status(true);
        } else {
            show_normal();
        }
    }
    awake_loop();
}
