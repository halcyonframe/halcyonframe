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

esp_err_t ota_install(ota_read_fn read, void *ctx); // writes, verifies and selects the new image; caller restarts
bool ota_from_card(void);  // installs OTA_CARD_FILE if present; true = restart now
void ota_mark_good(void);  // this firmware works: cancel the pending rollback
bool ota_busy(void);
const char *ota_version(void);
const char *ota_slot(void); // running app partition, e.g. "ota_0"
