// PhotoFrame firmware for the Waveshare ESP32-S3-PhotoPainter. Fully local: photos and config.txt on the
// TF card, web UI served by the frame, no cloud services.
//
// Wake behaviour:
//   timer wake / BOOT button  -> show next photo, deep sleep again (Wi-Fi never starts: saves battery)
//   power-on / KEY button     -> start Wi-Fi + web UI, stay up until awake_min of inactivity, then sleep
// While awake: BOOT = next photo, KEY = keep awake, PWR = sleep now. Holding PWR 4 s powers off (AXP2101).
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
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

// 32-bit ms so it is read/written atomically across cores (wraps after ~49 days; unsigned math handles it)
static atomic_uint last_activity_ms;
static volatile bool want_sleep, want_reboot, wifi_up;

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

static void deep_sleep(void) {
    while (display_busy() || storage_writing()) vTaskDelay(pdMS_TO_TICKS(200)); // never cut a refresh or SD write
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
    if (g_cfg.interval_min > 0) esp_sleep_enable_timer_wakeup(g_cfg.interval_min * 60ULL * 1000000ULL);

    ESP_LOGI(TAG, "deep sleep (next photo in %d min)", g_cfg.interval_min);
    esp_deep_sleep_start();
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
        if (key_prev && !key) app_touch();
        if (!pwr_prev && pwr) want_sleep = true;
        boot_prev = boot, key_prev = key, pwr_prev = pwr;

        if (want_reboot) {
            vTaskDelay(pdMS_TO_TICKS(500)); // let the HTTP response flush
            while (display_busy() || storage_writing()) vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        bool idle = g_cfg.awake_min > 0 && !display_busy() &&
                    now_ms() - atomic_load(&last_activity_ms) > g_cfg.awake_min * 60000U;
        if (want_sleep || idle) {
            vTaskDelay(pdMS_TO_TICKS(500));
            deep_sleep();
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
    pmic_init();
    epd_init();
    display_init();
    bool sd = storage_mount();
    config_load();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool boot_btn = cause == ESP_SLEEP_WAKEUP_EXT1 && (esp_sleep_get_ext1_wakeup_status() & BIT64(PIN_BTN_BOOT));
    if (cause == ESP_SLEEP_WAKEUP_TIMER || boot_btn) {
        if (sd) display_next();
        deep_sleep();
    }

    ESP_LOGI(TAG, "starting web UI (wake cause %d)", cause);
    gpio_set_level(PIN_LED_RED, 0); // red = awake and reachable
    wifi_start();
    wifi_up = true;
    display_task_start();
    web_start();
    awake_loop();
}
