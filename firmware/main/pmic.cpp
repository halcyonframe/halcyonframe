// AXP2101 setup. Rail and button settings mirror axp_cmd_init() in the Waveshare reference firmware.
#include "pmic.h"
#include "board.h"
#include "XPowersLib.h"
#include "esp_log.h"

static const char *TAG = "pmic";
static XPowersPMU pmu;
static bool pmu_ok;

extern "C" void pmic_init(void) {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = (gpio_num_t) PIN_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t) PIN_I2C_SCL;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    pmu_ok = pmu.begin(bus, AXP2101_SLAVE_ADDRESS);
    if (!pmu_ok) {
        ESP_LOGE(TAG, "AXP2101 not found");
        return;
    }
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
}

extern "C" void pmic_status(pmic_status_t *out) {
    *out = {};
    out->percent = -1;
    if (!pmu_ok) return;
    out->usb = pmu.isVbusIn();
    out->battery = pmu.isBatteryConnect();
    out->charging = pmu.isCharging();
    if (out->battery) {
        out->battery_mv = pmu.getBattVoltage();
        out->percent = pmu.getBatteryPercent();
    }
}
