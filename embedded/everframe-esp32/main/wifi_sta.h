#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * Bring up station Wi-Fi once and wait at most timeout_ms for an IP address.
 *
 * This is intended for the provisioning flow after it has stopped and
 * deinitialized its SoftAP/DNS services. The caller must provide non-empty,
 * NUL-terminated SSID and password strings that fit the ESP-IDF limits.
 *
 * On success the station remains running for the caller to use (for example,
 * to register a device). On failure it is stopped and deinitialized, making
 * it safe for the caller to restore the provisioning AP. It returns
 * ESP_ERR_TIMEOUT when the AP disconnects or no IP address arrives before
 * timeout_ms.
 */
esp_err_t wifi_sta_try_once(const char *ssid, const char *password,
                            uint32_t timeout_ms);

/**
 * Connect to the Wi-Fi network stored in the "everframe" NVS namespace.
 *
 * On successful startup this function manages reconnects forever and does not
 * return. It only returns when initialization or credential loading fails.
 */
esp_err_t wifi_sta_start(void);
