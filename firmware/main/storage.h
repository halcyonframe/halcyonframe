#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "screens.h"
#include "shuffle.h"

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
    int shuffle;          // order_mode_t: 0 in order, 1 shuffle (random), 2 advanced shuffle (shown evenly)
    bool rotate180;   // the panel is mounted upside down in the stock frame, so this defaults on
    orient_t orientation; // how the frame hangs; config.txt: landscape | cw (turned right) | ccw (turned left)
    char tz[64];          // POSIX TZ string, e.g. CST6CDT,M3.2.0,M11.1.0 (US Central)
    char change_times[64]; // "07:00,19:30": change photos at these local times instead of every interval_min
    char quiet_start[6], quiet_end[6]; // "23:00"/"07:00": no photo changes or status screens in between
    int status_every;     // status screen (time, weather, battery, signal): 0 off, 60 at :00, 30 at :00 and :30
    int status_min;       // minutes it stays up before the photo returns
    bool key_status;      // short KEY press also shows the status screen (not just waking the frame)
    char temp_unit[2];    // "F" or "C"
    char ntp_server[64];  // optional; "" = time only comes from the browser when the web UI is opened
    int typeface;         // status screen typeface (TYPEFACES index)
    char cards[64];       // status cards left to right, e.g. "indoor,battery,photos,sun" (status.h CARD_NAMES)
    char zip[11];         // shown in the UI; the browser turns it into lat/lon (sun cards)
    char lat[16], lon[16];
    bool auto_setup;      // time (pool.ntp.org), time zone and location from the internet when on Wi-Fi
    bool hush;            // master switch: the frame makes no outbound connections at all (overrides auto_setup, ntp_server)
    bool wizard_done;     // the first-visit wizard has been answered; until then the frame behaves as if hush were on
    char geo_zone[48], geo_lat[16], geo_lon[16]; // last automatic lookup (zone name for the UI, city-level coords)
} config_t;

extern config_t g_cfg;

// The one gate for anything that leaves the home network (time, location, and any future online feature).
// Undecided counts as hush, so a new frame never calls out before its owner has chosen.
bool online_allowed(void);
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
// Held around read-modify-write of g_cfg plus config_save(): the web server and the auto-setup task both
// change settings.
void config_lock(void);
void config_unlock(void);
bool wifi_pass_valid(const char *pass); // empty, 8-63 chars, or 64 hex digits

bool photo_id_valid(const char *id);
void photo_path(char *out, size_t n, const char *id, const char *ext);
// Copies sorted photo ids into ids (PHOTO_ID_MAX each); returns the count. ids may be NULL to just count.
int photo_list(char (*ids)[PHOTO_ID_MAX], int max);

// What the shuffle remembers: /history.txt, one line per photo "id count last" (times shown, play sequence number of
// the latest showing). A plain text file rather than a database: a few hundred rows, rewritten when the photo changes.
// history_load fills p[i] for the sorted ids[i] (zero for photos never shown) and returns the highest sequence number.
// history_record notes that a photo is being shown now.
int history_load(const char (*ids)[PHOTO_ID_MAX], int n, play_t *p);
void history_record(const char *id);
// The browser encodes the orientation a photo was cropped for in its id: "<id>_cw", "<id>_ccw", or no
// suffix for landscape. The slideshow only shows photos matching the frame's orientation.
orient_t photo_orient(const char *id);
// Photos that can be shown in orientation o (right file size). *bad counts files of any orientation that
// have the wrong size, e.g. copied by hand or cut short. Stats every file, so call it sparingly.
int photo_scan(orient_t o, int *bad);
int photo_bad_count(void); // from the last photo_scan()
int photo_count(orient_t o); // photos for orientation o, by name only: cheap, used on every wake
bool photo_load(const char *id, uint8_t *buf);
bool photo_delete(const char *id);
