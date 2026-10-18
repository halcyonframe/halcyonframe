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

void pmic_init(void);
void pmic_status(pmic_status_t *out);

#ifdef __cplusplus
}
#endif
