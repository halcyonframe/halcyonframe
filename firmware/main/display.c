#include "display.h"
#include "board.h"
#include "epd.h"
#include "storage.h"
#include <stdatomic.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

// Survives deep sleep, so the slideshow continues where it left off.
static RTC_DATA_ATTR char current_id[PHOTO_ID_MAX];

static uint8_t *frame;
static SemaphoreHandle_t lock;
static QueueHandle_t queue;
// Requests queued or being drawn. A counter (not a flag) so busy stays true across back-to-back refreshes.
static atomic_int pending;

void display_init(void) {
    frame = heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    assert(frame);
    lock = xSemaphoreCreateMutex();
}

bool display_show(const char *id) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = photo_load(id, frame);
    if (ok) {
        ESP_LOGI(TAG, "showing %s", id);
        gpio_set_level(PIN_LED_GREEN, 0);
        epd_show(frame, g_cfg.rotate180);
        gpio_set_level(PIN_LED_GREEN, 1);
        snprintf(current_id, sizeof(current_id), "%s", id);
    } else {
        ESP_LOGE(TAG, "could not load %s", id);
    }
    xSemaphoreGive(lock);
    return ok;
}

bool display_next(void) {
    char (*ids)[PHOTO_ID_MAX] = heap_caps_malloc(PHOTO_MAX * PHOTO_ID_MAX, MALLOC_CAP_SPIRAM);
    assert(ids);
    int n = photo_list(ids, PHOTO_MAX);
    int pick = -1;
    if (n == 1) {
        pick = 0;
    } else if (n > 1) {
        int cur = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(ids[i], current_id)) cur = i;
        if (g_cfg.shuffle) {
            do pick = esp_random() % n; while (pick == cur);
        } else {
            // When the current photo was deleted, cur is -1 and we restart at the first photo.
            pick = (cur + 1) % n;
        }
    }
    char id[PHOTO_ID_MAX] = "";
    if (pick >= 0) strcpy(id, ids[pick]);
    heap_caps_free(ids);
    if (!id[0]) {
        ESP_LOGW(TAG, "no photos on card");
        return false;
    }
    return display_show(id);
}

static void display_task(void *arg) {
    char id[PHOTO_ID_MAX];
    for (;;) {
        xQueueReceive(queue, id, portMAX_DELAY);
        if (id[0]) display_show(id);
        else display_next();
        atomic_fetch_sub(&pending, 1);
    }
}

void display_task_start(void) {
    queue = xQueueCreate(4, PHOTO_ID_MAX);
    xTaskCreate(display_task, "display", 6144, NULL, 4, NULL);
}

void display_request(const char *id) {
    char msg[PHOTO_ID_MAX] = {0};
    if (id) snprintf(msg, sizeof(msg), "%s", id);
    atomic_fetch_add(&pending, 1); // counted before queueing so the sleep check can't race it
    if (xQueueSend(queue, msg, 0) != pdTRUE) {
        atomic_fetch_sub(&pending, 1);
        ESP_LOGW(TAG, "display queue full, dropped request");
    }
}

bool display_busy(void) { return atomic_load(&pending) > 0; }

const char *display_current(void) { return current_id; }
