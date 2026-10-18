// Host-build stand-in for ESP-IDF's esp_err.h (render_screens only).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h> // the real header pulls these in indirectly; the QR component relies on it
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
