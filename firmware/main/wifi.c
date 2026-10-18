#include "wifi.h"
#include "storage.h"
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"

static const char *TAG = "wifi";
#define GOT_IP BIT0
#define STA_CONNECT_TIMEOUT_MS 20000

static EventGroupHandle_t events;
static esp_netif_t *netif;
static bool ap_mode;
static bool sta_giving_up;
static char ip[16] = "0.0.0.0";

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Keep retrying (router reboots etc.) unless we are switching to AP mode.
        if (!sta_giving_up) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "connected, ip %s", ip);
        xEventGroupSetBits(events, GOT_IP);
    }
}

static bool start_sta(void) {
    netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, g_cfg.hostname);
    wifi_config_t wc = {0};
    // Driver fields are fixed-size and need no NUL when full; config strings are length-capped to fit.
    memcpy(wc.sta.ssid, g_cfg.wifi_ssid, strnlen(g_cfg.wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, g_cfg.wifi_pass, strnlen(g_cfg.wifi_pass, sizeof(wc.sta.password)));
    // A bad hand-edited config.txt must fall back to the AP, not crash-loop.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "STA config rejected: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "joining \"%s\"", g_cfg.wifi_ssid);
    if (err == ESP_OK && xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS)) & GOT_IP)
        return true;

    ESP_LOGW(TAG, "could not join \"%s\", falling back to AP", g_cfg.wifi_ssid);
    sta_giving_up = true;
    esp_wifi_stop();
    esp_netif_destroy_default_wifi(netif);
    return false;
}

static void start_ap(void) {
    ap_mode = true;
    netif = esp_netif_create_default_wifi_ap();
    wifi_config_t wc = {0};
    wc.ap.ssid_len = strnlen(g_cfg.ap_ssid, sizeof(wc.ap.ssid));
    memcpy(wc.ap.ssid, g_cfg.ap_ssid, wc.ap.ssid_len);
    memcpy(wc.ap.password, g_cfg.ap_pass, strnlen(g_cfg.ap_pass, sizeof(wc.ap.password)));
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    // WPA2 requires 8+ characters; anything shorter means an open network.
    wc.ap.authmode = strlen(g_cfg.ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    if (esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK) {
        // e.g. invalid password in a hand-edited config.txt: open AP so the user can fix it in the UI
        ESP_LOGE(TAG, "AP config rejected, starting open AP");
        memset(wc.ap.password, 0, sizeof(wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    strcpy(ip, "192.168.4.1");
    ESP_LOGI(TAG, "AP \"%s\" up at %s", g_cfg.ap_ssid, ip);
}

void wifi_start(void) {
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    if (!g_cfg.wifi_ssid[0] || !start_sta()) start_ap();

    // http://<hostname>.local
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_cfg.hostname);
        mdns_instance_name_set("PhotoFrame");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

void wifi_stop(void) {
    sta_giving_up = true;
    mdns_free();
    esp_wifi_stop();
}

bool wifi_is_ap(void) { return ap_mode; }
const char *wifi_ip(void) { return ip; }
