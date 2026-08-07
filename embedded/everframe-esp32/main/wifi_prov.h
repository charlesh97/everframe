#pragma once

#include "esp_err.h"

/**
 * Start EverFrame's open SoftAP, captive DNS server, and setup website.
 *
 * On success this function keeps the provisioning services alive and does not
 * return. It only returns when startup fails.
 */
esp_err_t wifi_prov_start(void);
