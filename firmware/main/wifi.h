#pragma once
#include <stdbool.h>

// Joins the configured network; falls back to a SoftAP (192.168.4.1) when none is set or it fails.
void wifi_start(void);
void wifi_stop(void);
bool wifi_is_ap(void);
const char *wifi_ip(void);
