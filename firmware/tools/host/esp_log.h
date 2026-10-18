// Host-build stand-in for ESP-IDF's esp_log.h (render_screens only).
#pragma once
#define ESP_LOGI(tag, ...) ((void) 0)
#define ESP_LOGE(tag, ...) ((void) 0)
#define ESP_LOGW(tag, ...) ((void) 0)
#define ESP_LOGD(tag, ...) ((void) 0)
