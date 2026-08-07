#pragma once

#include "esp_err.h"

/**
 * Connect to the Wi-Fi network stored in the "everframe" NVS namespace.
 *
 * On successful startup this function manages reconnects forever and does not
 * return. It only returns when initialization or credential loading fails.
 */
esp_err_t wifi_sta_start(void);
