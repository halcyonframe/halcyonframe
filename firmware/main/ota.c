#include "ota.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";
#define CHUNK 4096

static atomic_bool busy;

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
    atomic_store(&busy, true);
    esp_ota_handle_t h;
    // Sequential writes erase as they go, so the first write doesn't stall long enough to time out the upload.
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &h);
    size_t total = 0;
    while (err == ESP_OK) {
        int n = read(ctx, buf, CHUNK);
        if (n == 0) break;
        // The first write also rejects anything that isn't an ESP32 app image.
        err = n < 0 ? ESP_FAIL : esp_ota_write(h, buf, n);
        total += n > 0 ? n : 0;
    }
    if (err == ESP_OK) err = esp_ota_end(h); // verifies the image (chip, checksum, SHA-256)
    else esp_ota_abort(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(slot);
    free(buf);
    atomic_store(&busy, false);
    if (err == ESP_OK) ESP_LOGI(TAG, "installed %u bytes into %s", (unsigned) total, slot->label);
    else ESP_LOGE(TAG, "update failed after %u bytes: %s", (unsigned) total, esp_err_to_name(err));
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

void ota_mark_good(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware %s confirmed", ota_version());
    }
}
