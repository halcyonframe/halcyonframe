// PhotoFrame firmware for the Waveshare ESP32-S3-PhotoPainter. Fully local: photos and config.txt on the
// TF card, web UI served by the frame, no cloud services.
//
// Wake behaviour:
//   timer wake / BOOT button  -> show next photo, deep sleep again (Wi-Fi never starts: saves battery)
//   power-on / KEY button     -> start Wi-Fi + web UI, stay up until awake_min of inactivity, then sleep
// While awake: BOOT = next photo, KEY = show "how to reach me" on the panel, PWR = sleep now.
// Holding PWR 4 s powers off (AXP2101).
//
// When there is nothing to show or something needs attention (no photos yet, Wi-Fi failed, no card,
// battery empty), the panel shows a status screen instead (screens.c), so a person always sees what to do.
#include "app.h"
#include "board.h"
#include "display.h"
#include "epd.h"
#include "pmic.h"
#include "storage.h"
#include "web.h"
#include "wifi.h"
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

// 32-bit ms so it is read/written atomically across cores (wraps after ~49 days; unsigned math handles it)
static atomic_uint last_activity_ms;
static volatile bool want_sleep, want_reboot, wifi_up;
static bool boot_info_pending; // info screen shown at reset, photo not yet restored

static uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }
void app_touch(void) { atomic_store(&last_activity_ms, now_ms()); }
void app_request_sleep(void) { want_sleep = true; }
void app_request_reboot(void) { want_reboot = true; }

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

// Slideshow timer for the coming sleep; 0 = no timer wake.
static int sleep_minutes(void) {
    if (pmic_battery_level() == BATT_CRITICAL) return CRITICAL_RECHECK_MIN;
    if (!storage_ok()) return g_cfg.interval_min; // keep checking, so the slideshow resumes once the card is back
    int bad;
    return photo_scan(g_cfg.orientation, &bad) > 0 ? g_cfg.interval_min : 0; // nothing to show: don't wake for nothing
}

static void deep_sleep(int wake_min) {
    while (display_busy() || storage_writing()) vTaskDelay(pdMS_TO_TICKS(200)); // never cut a refresh or SD write
    // These screens only make sense while someone is interacting; put the photo back before sleeping.
    screen_t s = display_shown_screen();
    if ((s == SCR_WIFI_FAIL || s == SCR_INFO) && storage_ok()) {
        const char *cur = display_current();
        if (!(cur[0] && photo_orient(cur) == g_cfg.orientation && display_show(cur))) display_next();
    }
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
    if (wake_min > 0) esp_sleep_enable_timer_wakeup(wake_min * 60ULL * 1000000ULL);

    ESP_LOGI(TAG, "deep sleep (timer: %d min)", wake_min);
    esp_deep_sleep_start();
}

// What the panel should show once any boot-time info is done. First match wins (see screens.h).
static void show_normal(void) {
    int bad, photos = photo_scan(g_cfg.orientation, &bad);
    if (bad) ESP_LOGW(TAG, "%d photo file(s) have the wrong size and will be skipped", bad);
    if (g_cfg.wifi_ssid[0] && wifi_is_ap()) display_request_screen(SCR_WIFI_FAIL);
    else if (!photos) display_request_screen(wifi_is_ap() ? SCR_SETUP : SCR_READY);
    // After power-on we don't know what's on the panel (often the vendor firmware's last image).
    else if (!display_known() || display_shown_screen() != SCR_NONE) display_request(NULL);
}

static void awake_loop(void) {
    // Seed with the current levels so a button still held from the wake press isn't seen as a new press.
    int boot_prev = gpio_get_level(PIN_BTN_BOOT);
    int key_prev = gpio_get_level(PIN_BTN_KEY);
    int pwr_prev = gpio_get_level(PIN_BTN_PWR);
    app_touch();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30)); // also acts as debounce
        int boot = gpio_get_level(PIN_BTN_BOOT), key = gpio_get_level(PIN_BTN_KEY), pwr = gpio_get_level(PIN_BTN_PWR);
        if (boot_prev && !boot) {
            app_touch();
            display_request(NULL);
        }
        if (key_prev && !key) {
            app_touch();
            // The card screen says "fix it, then press KEY": remount by restarting.
            if (!storage_ok()) esp_restart();
            if (!display_busy()) display_request_screen(SCR_INFO);
        }
        if (!pwr_prev && pwr) want_sleep = true;
        boot_prev = boot, key_prev = key, pwr_prev = pwr;

        if (want_reboot) {
            vTaskDelay(pdMS_TO_TICKS(500)); // let the HTTP response flush
            while (display_busy() || storage_writing()) vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        // The reset info screen has had its minute: back to the photo (unless someone moved on already).
        if (boot_info_pending && now_ms() > BOOT_INFO_MS && !display_busy()) {
            boot_info_pending = false;
            if (display_shown_screen() == SCR_INFO) show_normal();
        }
        bool idle = g_cfg.awake_min > 0 && !display_busy() &&
                    now_ms() - atomic_load(&last_activity_ms) > g_cfg.awake_min * 60000U;
        if (want_sleep || idle) {
            vTaskDelay(pdMS_TO_TICKS(500));
            deep_sleep(sleep_minutes());
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
    // Why we started. A crash or brownout shows up here (and in /api/status) even without a serial cable.
    esp_reset_reason_t why = esp_reset_reason();
    static const char *const REASONS[] = {"unknown", "power-on", "external", "software", "panic", "int-watchdog",
                                          "task-watchdog", "watchdog", "deep-sleep", "brownout", "sdio",
                                          "usb", "jtag", "efuse", "power-glitch", "cpu-lockup"}; // esp_reset_reason_t order
    ESP_LOGI(TAG, "reset reason: %s", why < sizeof(REASONS) / sizeof(REASONS[0]) ? REASONS[why] : "other");

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool boot_btn = cause == ESP_SLEEP_WAKEUP_EXT1 && (esp_sleep_get_ext1_wakeup_status() & BIT64(PIN_BTN_BOOT));

    // Nearly empty: put "Please charge me" up while there's still power to draw it; it stays after the
    // battery dies. No Wi-Fi, whatever woke us.
    if (pmic_battery_level() == BATT_CRITICAL) {
        ESP_LOGW(TAG, "battery critical");
        if (boot_btn) blink_red_twice();
        display_screen(SCR_BATTERY); // no refresh if it's already up
        deep_sleep(CRITICAL_RECHECK_MIN);
    }

    if (cause == ESP_SLEEP_WAKEUP_TIMER || boot_btn) {
        // Quiet wake: just the next photo. Problems wait until someone wakes the frame with KEY.
        bool shown = sd && display_next();
        if (!shown && boot_btn) blink_red_twice();
        deep_sleep(sleep_minutes());
    }

    ESP_LOGI(TAG, "starting web UI (wake cause %d)", cause);
    gpio_set_level(PIN_LED_RED, 0); // red = awake and reachable
    display_task_start();
    if (!sd) display_request_screen(SCR_CARD); // drawn while Wi-Fi comes up
    wifi_start();
    wifi_up = true;
    web_start();

    if (sd) {
        // After a reset (not a wake from sleep), spend the first minute showing where to find the frame and
        // how it's doing. A failed Wi-Fi join skips this: its own screen says what to fix.
        if (why != ESP_RST_DEEPSLEEP && !(g_cfg.wifi_ssid[0] && wifi_is_ap())) {
            display_request_screen(SCR_INFO);
            boot_info_pending = true;
        } else {
            show_normal();
        }
    }
    awake_loop();
}
