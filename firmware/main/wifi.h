#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "screens.h"

// Joins the configured network; falls back to a SoftAP (192.168.4.1) when none is set or it fails.
void wifi_start(void);
void wifi_stop(void);
bool wifi_is_ap(void);
const char *wifi_ip(void);
wifi_err_t wifi_last_error(void); // why joining the home network failed (WIFI_ERR_NONE when connected)

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_net_t;
#define WIFI_SCAN_MAX 20

// Nearby networks for the settings page, strongest first, one entry per name, hidden ones left out.
// wifi_networks() returns the cached list (scanning on first use); wifi_scan() refreshes it. A scan takes
// ~2 s and briefly interrupts clients of the frame's own hotspot.
int wifi_networks(const wifi_net_t **out);
int wifi_scan(void);
