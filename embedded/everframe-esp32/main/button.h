#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

/* SW1 is wired to XIAO D0 / GPIO1. Override at build time for another board. */
#ifndef BUTTON_GPIO
#define BUTTON_GPIO GPIO_NUM_1
#endif

/* SW1 drives the input high through 1 kOhm when pressed. */
#ifndef BUTTON_ACTIVE_LEVEL
#define BUTTON_ACTIVE_LEVEL 1
#endif

#define BUTTON_DEBOUNCE_MS 50U
#define BUTTON_PROVISION_HOLD_MS 3000U
#define BUTTON_FACTORY_RESET_HOLD_MS 10000U

/** Configure SW1 as an active-level input with the matching internal pull. */
esp_err_t button_init(void);

/** Return whether the button is currently pressed after a 50 ms debounce. */
bool button_is_pressed(void);

/**
 * Check for a continuous hold during early boot. This blocks for at most
 * hold_ms and returns false if the button is released at any point.
 */
bool button_was_held_on_boot(uint32_t hold_ms);

/** Check for a continuous debounced hold while the firmware is running. */
bool button_poll_held(uint32_t hold_ms);

/** True when the current boot was caused by this button's EXT0 wake source. */
bool button_woke_from_deep_sleep(void);

/** Arm active-level EXT0 wakeup before entering deep sleep. */
esp_err_t button_enable_wakeup(void);
