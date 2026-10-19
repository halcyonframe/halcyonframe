// AXP2101 setup. Rail and button settings mirror axp_cmd_init() in the Waveshare reference firmware.
#include "pmic.h"
#include "board.h"
#include "XPowersLib.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pmic";
static XPowersPMU pmu;
static bool pmu_ok;
static i2c_master_bus_handle_t bus; // shared with the RTC and temperature sensor (clock.c)

extern "C" i2c_master_bus_handle_t board_i2c_bus(void) { return bus; }

extern "C" void pmic_init(void) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = (gpio_num_t) PIN_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t) PIN_I2C_SCL;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    // Which of the board's I2C chips answer (addresses from the Waveshare schematic). Diagnostic only:
    // helps tell a bus problem from a single misbehaving chip.
    const struct { uint8_t addr; const char *name; } chips[] = {
        {0x18, "ES8311"}, {0x34, "AXP2101"}, {0x40, "ES7210"}, {0x51, "PCF85063"}, {0x70, "SHTC3"}};
    for (auto &chip : chips)
        ESP_LOGI(TAG, "i2c 0x%02x %-8s %s", chip.addr, chip.name,
                 i2c_master_probe(bus, chip.addr, 50) == ESP_OK ? "ok" : "no answer");

    pmu_ok = pmu.begin(bus, AXP2101_SLAVE_ADDRESS);
    if (!pmu_ok) {
        ESP_LOGE(TAG, "AXP2101 not found");
        return;
    }
    // Why the power chip last switched the rails on and off. A silent mid-upload reset (power-on reset reason, no
    // panic or brownout log) points at the AXP2101 cutting power itself; this names the protection that fired.
    ESP_LOGI(TAG, "AXP2101 power-off source: temp %d dc-over %d dc-under %d vbus-over %d vsys-under %d pwron-low %d sw %d",
             pmu.isOverTemperatureOffSource(), pmu.isDcOverVoltageOffSource(), pmu.isDcUnderVoltageOffSource(),
             pmu.isVbusOverVoltageOffSource(), pmu.isVsysUnderVoltageOffSource(), pmu.isPwronAlwaysLowOffSource(),
             pmu.isSwConfigOffSource());
    ESP_LOGI(TAG, "AXP2101 power-on source: always-high %d batt-insert %d batt-normal %d vbus-insert %d irq-low %d pwron-low %d",
             pmu.isPoweronAlwaysHighSource(), pmu.isBattInsertOnSource(), pmu.isBattNormalOnSource(),
             pmu.isVbusInsertOnSource(), pmu.isIrqLowOnSource(), pmu.isPwronLowOnSource());
    // Only write registers that differ, as the reference does, to avoid glitching live rails.
    if (pmu.getPowerKeyPressOffTime() != XPOWERS_POWEROFF_4S) pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    if (pmu.getPowerKeyPressOnTime() != XPOWERS_POWERON_128MS) pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
    if (pmu.getChargingLedMode() != XPOWERS_CHG_LED_OFF) pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
    if (pmu.getChargeTargetVoltage() != XPOWERS_AXP2101_CHG_VOL_4V1) pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1);
    if (pmu.getButtonBatteryVoltage() != 3300) pmu.setButtonBatteryChargeVoltage(3300);
    if (!pmu.isEnableButtonBatteryCharge()) pmu.enableButtonBatteryCharge();
    // 3V3 rails feeding the ESP32, TF card and panel
    if (pmu.getDC1Voltage() != 3300) pmu.setDC1Voltage(3300);
    if (pmu.getALDO3Voltage() != 3300) pmu.setALDO3Voltage(3300);
    if (pmu.getALDO4Voltage() != 3300) pmu.setALDO4Voltage(3300);

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();

    pmic_status_t st;
    pmic_status(&st);
    ESP_LOGI(TAG, "battery %s %d mV %d%%, usb %d, charging %d", st.battery ? "yes" : "no", st.battery_mv,
             st.percent, st.usb, st.charging);
}

// The web server (status polls) and the display task (battery check per photo) both ask for this.
// XPowersLib isn't thread-safe, so reads are serialized, and cached briefly to keep I2C traffic down.
#define STATUS_CACHE_US (5 * 1000 * 1000)

extern "C" void pmic_status(pmic_status_t *out) {
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex(); // C++ function-static: created once, thread-safe
    static pmic_status_t cache;
    static int64_t cached_at;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!cached_at || esp_timer_get_time() - cached_at > STATUS_CACHE_US) {
        cache = {};
        cache.percent = -1;
        if (pmu_ok) {
            cache.usb = pmu.isVbusIn();
            cache.battery = pmu.isBatteryConnect();
            cache.charging = pmu.isCharging();
            if (cache.battery) {
                cache.battery_mv = pmu.getBattVoltage();
                cache.percent = pmu.getBatteryPercent();
            }
        }
        cached_at = esp_timer_get_time();
    }
    *out = cache;
    xSemaphoreGive(lock);
}

extern "C" batt_level_t pmic_battery_level(void) {
    pmic_status_t p;
    pmic_status(&p);
    if (!p.battery || p.usb || p.charging || p.percent < 0) return BATT_OK;
    // The gauge percentage is uncalibrated on this board; the voltage floor is a backstop in case it reads
    // high near empty. 3.45 V under light load is close to empty for a Li-ion cell.
    if (p.percent <= 2 || (p.battery_mv > 0 && p.battery_mv < 3450)) return BATT_CRITICAL;
    return p.percent <= 15 ? BATT_LOW : BATT_OK;
}
