#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool battery;   // battery connected
    bool charging;
    bool usb;       // VBUS present
    int battery_mv;
    int percent;    // -1 when no battery
} pmic_status_t;

typedef enum { BATT_OK, BATT_LOW, BATT_CRITICAL } batt_level_t;

void pmic_init(void);
// LOW (<= 15%) adds a "Charge me" badge to photos; CRITICAL (<= 2%) shows the "Please charge me" screen
// so that is what stays on the panel once the battery dies. Always OK on USB power or while charging.
batt_level_t pmic_battery_level(void);
void pmic_status(pmic_status_t *out);

#ifdef __cplusplus
}
#endif
