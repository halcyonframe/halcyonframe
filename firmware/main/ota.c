#include "ota.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";
#define CHUNK 4096

static atomic_bool busy;
static char incoming[32];
static void (*start_hook)(void);
// What was being installed, kept through the restart (RTC_NOINIT survives resets but not power loss; the
// magic value rejects power-on garbage) so the start screen can say whether the update took.
#define PENDING_MAGIC 0x55504454
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    char from[32], to[32];
} pending;
static int result;      // ota_last_result()
static char result_msg[128];

void ota_set_start_hook(void (*hook)(void)) { start_hook = hook; }
const char *ota_incoming_version(void) { return incoming; }
int ota_last_result(const char **msg) {
    *msg = result_msg;
    return result;
}

// The image starts with a 24-byte image header and an 8-byte segment header, then the app description,
// whose version field is 16 bytes in. Reading it from the first chunk lets the screen name the new version
// before anything is written.
// Also the check that this is *our* app (project name in the description, 32 bytes after the version):
// a bootloader or the full USB image also starts with 0xE9 and could otherwise pass the image checks.
static bool read_incoming_version(const char *buf, int n) {
    uint32_t magic;
    if (n < 112 || (uint8_t) buf[0] != 0xE9) return false;
    memcpy(&magic, buf + 32, 4);
    if (magic != ESP_APP_DESC_MAGIC_WORD || strncmp(buf + 80, "halcyonframe", 32)) return false;
    snprintf(incoming, sizeof(incoming), "%.31s", buf + 48);
    return true;
}

bool ota_busy(void) { return atomic_load(&busy); }
const char *ota_version(void) { return esp_app_get_description()->version; }
const char *ota_slot(void) { return esp_ota_get_running_partition()->label; }

esp_err_t ota_install(ota_read_fn read, void *ctx) {
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    char *buf = malloc(CHUNK);
    if (!slot || !buf) {
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }
    if (atomic_exchange(&busy, true)) { // one update at a time
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }
    esp_ota_handle_t h;
    // Sequential writes erase as they go, so the first write doesn't stall long enough to time out the upload.
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &h);
    size_t total = 0;
    bool first = true;
    while (err == ESP_OK) {
        int n = read(ctx, buf, CHUNK);
        if (n == 0) break;
        if (first && n > 0) {
            first = false;
            incoming[0] = 0;
            if (!read_incoming_version(buf, n)) { // not a Halcyon Frame app image: refuse before writing
                err = ESP_ERR_INVALID_VERSION;
                break;
            }
        }
        // The first write also rejects anything that isn't an ESP32 app image.
        err = n < 0 ? ESP_FAIL : esp_ota_write(h, buf, n);
        total += n > 0 ? n : 0;
    }
    if (err == ESP_OK) err = esp_ota_end(h); // verifies the image (chip, checksum, SHA-256)
    else esp_ota_abort(h);
    if (err == ESP_OK) {
        pending.magic = PENDING_MAGIC;
        snprintf(pending.from, sizeof(pending.from), "%s", ota_version());
        snprintf(pending.to, sizeof(pending.to), "%s", incoming[0] ? incoming : "new firmware");
        err = esp_ota_set_boot_partition(slot);
        if (err != ESP_OK) pending.magic = 0;
    }
    if (err != ESP_OK) incoming[0] = 0;
    free(buf);
    atomic_store(&busy, false);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "installed %u bytes into %s", (unsigned) total, slot->label);
        // Drawn only now, not when the upload starts: a panel refresh on top of full-rate Wi-Fi traffic reset the
        // chip mid-upload on USB power, and a rejected upload would have left "Updating" on the panel.
        if (start_hook) start_hook();
    } else ESP_LOGE(TAG, "update failed after %u bytes: %s", (unsigned) total, esp_err_to_name(err));
    return err;
}

static int read_file(void *ctx, char *buf, size_t len) {
    size_t n = fread(buf, 1, len, ctx);
    return n > 0 ? (int) n : ferror((FILE *) ctx) ? -1 : 0;
}

bool ota_from_card(void) {
    FILE *f = fopen(OTA_CARD_FILE, "rb");
    if (!f) return false;
    ESP_LOGI(TAG, "found %s, installing", OTA_CARD_FILE);
    esp_err_t err = ota_install(read_file, f);
    fclose(f);
    // Rename either way so a bad file can't make the frame retry it on every boot.
    const char *done = err == ESP_OK ? "/sdcard/firmware.done" : "/sdcard/firmware.failed";
    unlink(done);
    rename(OTA_CARD_FILE, done);
    return err == ESP_OK;
}

// The record is only cleared once the new firmware confirms itself (ota_mark_good). If it crashes before
// that, the bootloader goes back to the old firmware, which then finds the record and reports the rollback.
void ota_init(void) {
    if (pending.magic != PENDING_MAGIC) return;
    const char *now = esp_app_get_description()->version;
    if (!strcmp(now, pending.to) || strcmp(pending.to, pending.from) == 0) {
        result = 1;
        snprintf(result_msg, sizeof(result_msg), "Updated to %s", now);
    } else {
        result = 2;
        snprintf(result_msg, sizeof(result_msg), "Update to %s didn't start; went back to %s", pending.to, now);
        pending.magic = 0;
    }
}

void ota_mark_good(void) {
    pending.magic = 0; // this firmware works: the update (if any) is settled
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware %s confirmed", ota_version());
    }
}
