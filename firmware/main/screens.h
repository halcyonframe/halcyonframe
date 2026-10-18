// Status and setup screens drawn on the panel when there's no photo to show or something needs the
// person's attention. Content only; main.c decides which screen applies. Host-buildable (no ESP-IDF).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"

typedef enum {
    SCR_NONE = 0,
    SCR_SETUP,     // hotspot up, no photos yet: how to join and open the page
    SCR_WIFI_FAIL, // home Wi-Fi configured but joining failed, so the hotspot is up
    SCR_READY,     // on home Wi-Fi, no photos yet
    SCR_INFO,      // after a reset (first minute) or KEY while awake: network, address, battery, card
    SCR_CARD,      // TF card missing or unreadable
    SCR_BATTERY,   // battery about to die: stays on the panel after it does
} screen_t;

typedef enum { WIFI_ERR_NONE, WIFI_ERR_AUTH, WIFI_ERR_NOT_FOUND, WIFI_ERR_OTHER } wifi_err_t;
typedef enum { CARD_OK, CARD_MISSING, CARD_UNREADABLE } card_state_t;

typedef struct {
    orient_t orient;
    bool ap_mode;
    const char *ap_ssid, *ap_pass; // the frame's own hotspot (password shown: it's printed on a device in the home)
    const char *wifi_ssid;         // home network name; its password is never shown
    wifi_err_t wifi_err;
    const char *ip, *hostname;
    card_state_t card;
    bool has_battery, charging;
    int battery_pct;
    int photos, interval_min; // photos = all photos on the card
    float free_gb;
} screen_info_t;

// Renders into buf (EPD_BUF_SIZE). Returns false if the content didn't fit (only a host-tool concern).
bool screen_render(screen_t s, const screen_info_t *info, uint8_t *buf);
// Small "Charge me" box in the viewer's bottom-right corner, drawn over a photo.
void screen_badge(uint8_t *buf, orient_t orient);
