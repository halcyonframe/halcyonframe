// Firmware updates without a cable: uploaded through the web UI, or copied to the TF card as firmware.bin.
// The new image goes into the inactive app slot; if it doesn't confirm itself (ota_mark_good) before its
// first reset, the bootloader goes back to the previous firmware.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define OTA_CARD_FILE "/sdcard/firmware.bin"

// Pulls the image in chunks: returns bytes read (>0), 0 at the end, <0 on error.
typedef int (*ota_read_fn)(void *ctx, char *buf, size_t len);

void ota_init(void); // at boot, before ota_mark_good(): works out how the last update went (ota_last_result)
esp_err_t ota_install(ota_read_fn read, void *ctx); // writes, verifies and selects the new image; caller restarts
bool ota_from_card(void);  // installs OTA_CARD_FILE if present; true = restart now
void ota_mark_good(void);  // this firmware works: cancel the pending rollback
bool ota_busy(void);
const char *ota_version(void);
const char *ota_slot(void); // running app partition, e.g. "ota_0"
const char *ota_incoming_version(void); // version being installed right now ("" when none)
// Called once the new firmware is fully received, verified and set to boot next (not while it uploads, when a
// panel refresh would compete with Wi-Fi for power): time to show the "Updating firmware" screen.
void ota_set_start_hook(void (*hook)(void));
// After a restart: what happened to the last update. 0 = there was none, 1 = now running it, 2 = it didn't
// start and the bootloader went back to the previous firmware. msg describes it ("" for 0).
int ota_last_result(const char **msg);
