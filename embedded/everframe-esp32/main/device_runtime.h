#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Set before Wi-Fi starts when this boot came from a short button press. */
void device_runtime_set_button_wake(bool button_wake);

/**
 * Start the single device/cloud synchronization task after station Wi-Fi has
 * obtained an IP address. Repeated calls are harmless.
 */
esp_err_t device_runtime_start(void);
