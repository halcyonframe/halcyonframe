// Driver for the 7.3" Spectra 6 (E6) panel. Command sequence copied from the Waveshare reference.
#include "epd.h"
#include "board.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";
#define CHUNK 4096

static spi_device_handle_t spi;
static uint8_t *dma_chunk; // internal RAM bounce buffer; frame buffers live in PSRAM

static void spi_tx(const uint8_t *data, size_t len) {
    while (len) {
        size_t n = len > CHUNK ? CHUNK : len;
        memcpy(dma_chunk, data, n);
        spi_transaction_t t = {.length = n * 8, .tx_buffer = dma_chunk};
        ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
        data += n;
        len -= n;
    }
}

static void cmd(uint8_t c) {
    gpio_set_level(PIN_EPD_DC, 0);
    gpio_set_level(PIN_EPD_CS, 0);
    spi_tx(&c, 1);
    gpio_set_level(PIN_EPD_CS, 1);
}

static void data(const uint8_t *d, size_t len) {
    gpio_set_level(PIN_EPD_DC, 1);
    gpio_set_level(PIN_EPD_CS, 0);
    spi_tx(d, len);
    gpio_set_level(PIN_EPD_CS, 1);
}

#define CMD(c, ...) do { static const uint8_t _d[] = {__VA_ARGS__}; cmd(c); data(_d, sizeof(_d)); } while (0)

// BUSY is low while the controller works. A full refresh takes ~20 s; give up well after that so a
// disconnected or failed panel can't keep the device awake and drain the battery.
#define BUSY_TIMEOUT_MS 90000
static bool timed_out;

static void wait_idle(void) {
    for (int waited = 0; !gpio_get_level(PIN_EPD_BUSY); waited += 10) {
        if (timed_out || waited >= BUSY_TIMEOUT_MS) {
            if (!timed_out) ESP_LOGE(TAG, "panel stuck busy, aborting refresh");
            timed_out = true; // skip the remaining waits of this refresh
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void reset(void) {
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    wait_idle();
}

// The panel is put into deep sleep after every refresh, so each refresh starts with a reset + init.
static void panel_init(void) {
    timed_out = false;
    reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    CMD(0xAA, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18); // CMDH
    CMD(0x01, 0x3F);
    CMD(0x00, 0x5F, 0x69);
    CMD(0x03, 0x00, 0x54, 0x00, 0x44);
    CMD(0x05, 0x40, 0x1F, 0x1F, 0x2C);
    CMD(0x06, 0x6F, 0x1F, 0x17, 0x49);
    CMD(0x08, 0x6F, 0x1F, 0x1F, 0x22);
    CMD(0x30, 0x03);
    CMD(0x50, 0x3F);
    CMD(0x60, 0x02, 0x00);
    CMD(0x61, 0x03, 0x20, 0x01, 0xE0); // 800 x 480
    CMD(0x84, 0x01);
    CMD(0xE3, 0x2F);
    cmd(0x04); // power on
    wait_idle();
}

static void refresh_and_sleep(void) {
    cmd(0x04); // power on
    wait_idle();
    CMD(0x06, 0x6F, 0x1F, 0x17, 0x49);
    CMD(0x12, 0x00); // display refresh
    wait_idle();
    CMD(0x02, 0x00); // power off
    wait_idle();
    CMD(0x07, 0xA5); // deep sleep (as in Waveshare's EPD_7in3e driver); next init resets the panel
}

void epd_init(void) {
    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_EPD_RST) | BIT64(PIN_EPD_DC) | BIT64(PIN_EPD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    // Pull-up as in the vendor driver
    gpio_config_t in = {.pin_bit_mask = BIT64(PIN_EPD_BUSY), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&in));
    gpio_set_level(PIN_EPD_CS, 1);
    gpio_set_level(PIN_EPD_RST, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_EPD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1, // CS is driven manually so a command and its data share one CS window
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &spi));
    dma_chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(dma_chunk);
}

void epd_show(const uint8_t *buf, bool rotate180) {
    panel_init();
    cmd(0x10);
    if (!rotate180) {
        data(buf, EPD_BUF_SIZE);
    } else {
        // Rotating 180° on a 4bpp row-major buffer = reverse byte order and swap nibbles.
        uint8_t row[EPD_WIDTH / 2];
        gpio_set_level(PIN_EPD_DC, 1);
        gpio_set_level(PIN_EPD_CS, 0);
        for (int r = 0; r < EPD_HEIGHT; r++) {
            const uint8_t *src = buf + EPD_BUF_SIZE - 1 - r * sizeof(row);
            for (int i = 0; i < sizeof(row); i++) {
                uint8_t b = *(src - i);
                row[i] = (uint8_t) ((b << 4) | (b >> 4));
            }
            spi_tx(row, sizeof(row));
        }
        gpio_set_level(PIN_EPD_CS, 1);
    }
    refresh_and_sleep();
}

void epd_clear(uint8_t color) {
    uint8_t row[EPD_WIDTH / 2];
    memset(row, (color << 4) | color, sizeof(row));
    panel_init();
    cmd(0x10);
    gpio_set_level(PIN_EPD_DC, 1);
    gpio_set_level(PIN_EPD_CS, 0);
    for (int r = 0; r < EPD_HEIGHT; r++) spi_tx(row, sizeof(row));
    gpio_set_level(PIN_EPD_CS, 1);
    refresh_and_sleep();
}
