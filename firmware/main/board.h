// Pin map for the Waveshare ESP32-S3-PhotoPainter (taken from the vendor reference firmware).
#pragma once

// 7.3" Spectra 6 e-paper (SPI)
#define PIN_EPD_DC    8
#define PIN_EPD_CS    9
#define PIN_EPD_SCK   10
#define PIN_EPD_MOSI  11
#define PIN_EPD_RST   12
#define PIN_EPD_BUSY  13

// TF card (SDMMC 4-bit)
#define PIN_SD_CLK    39
#define PIN_SD_CMD    41
#define PIN_SD_D0     40
#define PIN_SD_D1     1
#define PIN_SD_D2     2
#define PIN_SD_D3     38

// Shared I2C bus (AXP2101 PMIC, RTC, SHTC3, audio codecs)
#define PIN_I2C_SDA   47
#define PIN_I2C_SCL   48

// Buttons. BOOT and KEY are active-low; PWR is active-high (pulled down).
#define PIN_BTN_BOOT  0
#define PIN_BTN_KEY   4
#define PIN_BTN_PWR   5

// LEDs are active-low
#define PIN_LED_RED   45
#define PIN_LED_GREEN 42

#define EPD_WIDTH     800
#define EPD_HEIGHT    480
// 4 bits per pixel, two pixels per byte, left pixel in the high nibble
#define EPD_BUF_SIZE  (EPD_WIDTH * EPD_HEIGHT / 2)
