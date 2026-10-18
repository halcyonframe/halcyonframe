#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "screens.h"

#define SD_ROOT      "/sdcard"
#define PHOTO_DIR    SD_ROOT "/photos"
#define CONFIG_PATH  SD_ROOT "/config.txt"
#define PHOTO_ID_MAX 33 // 32 chars + NUL
#define PHOTO_MAX    2000

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char ap_ssid[33];
    char ap_pass[65];
    char hostname[33];
    int interval_min; // slideshow interval; 0 = only change on button/web request
    int awake_min;    // minutes of inactivity before deep sleep while the web UI is up; 0 = never
    bool shuffle;
    bool rotate180;   // the panel is mounted upside down in the stock frame, so this defaults on
    orient_t orientation; // how the frame hangs; config.txt: landscape | cw (turned right) | ccw (turned left)
} config_t;

extern config_t g_cfg;
extern const char *const ORIENT_NAMES[3]; // config.txt / API spelling of orient_t

bool storage_mount(void);
// Brackets SD writes so deep sleep can wait for them instead of cutting a write in half.
void storage_write_begin(void);
void storage_write_end(void);
bool storage_writing(void);
bool storage_ok(void);
card_state_t storage_card_state(void);
void storage_space(uint64_t *total, uint64_t *free_bytes);

void config_load(void); // falls back to defaults and writes the file if it is missing
bool config_save(void);
bool wifi_pass_valid(const char *pass); // empty, 8-63 chars, or 64 hex digits

bool photo_id_valid(const char *id);
void photo_path(char *out, size_t n, const char *id, const char *ext);
// Copies sorted photo ids into ids (PHOTO_ID_MAX each); returns the count. ids may be NULL to just count.
int photo_list(char (*ids)[PHOTO_ID_MAX], int max);
// The browser encodes the orientation a photo was cropped for in its id: "<id>_cw", "<id>_ccw", or no
// suffix for landscape. The slideshow only shows photos matching the frame's orientation.
orient_t photo_orient(const char *id);
// Photos that can be shown in orientation o (right file size). *bad counts files of any orientation that
// have the wrong size, e.g. copied by hand or cut short. Stats every file, so call it sparingly.
int photo_scan(orient_t o, int *bad);
int photo_bad_count(void); // from the last photo_scan()
bool photo_load(const char *id, uint8_t *buf);
bool photo_delete(const char *id);
