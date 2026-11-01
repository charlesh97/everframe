#include "button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"

#define BUTTON_POLL_INTERVAL_MS 10U

static const char *TAG = "button";
static bool button_initialized;

static TickType_t milliseconds_to_ticks(uint32_t milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (milliseconds > 0 && ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

static bool button_is_active_raw(void)
{
    return gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL;
}

esp_err_t button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    const esp_err_t error = gpio_config(&config);
    if (error == ESP_OK) {
        button_initialized = true;
    } else {
        ESP_LOGE(TAG, "Could not initialize button GPIO: %s", esp_err_to_name(error));
    }
    return error;
}

bool button_is_pressed(void)
{
    if (!button_initialized && button_init() != ESP_OK) {
        return false;
    }
    if (!button_is_active_raw()) {
        return false;
    }

    vTaskDelay(milliseconds_to_ticks(BUTTON_DEBOUNCE_MS));
    return button_is_active_raw();
}

bool button_poll_held(uint32_t hold_ms)
{
    if (!button_is_pressed()) {
        return false;
    }

    const TickType_t hold_ticks = milliseconds_to_ticks(hold_ms);
    const TickType_t poll_ticks = milliseconds_to_ticks(BUTTON_POLL_INTERVAL_MS);
    const TickType_t start_tick = xTaskGetTickCount();

    while ((TickType_t)(xTaskGetTickCount() - start_tick) < hold_ticks) {
        if (!button_is_active_raw()) {
            return false;
        }
        vTaskDelay(poll_ticks);
    }

    return button_is_active_raw();
}

bool button_was_held_on_boot(uint32_t hold_ms)
{
    return button_poll_held(hold_ms);
}

bool button_woke_from_deep_sleep(void)
{
    return (esp_sleep_get_wakeup_causes() & (1U << ESP_SLEEP_WAKEUP_EXT0)) != 0;
}

esp_err_t button_enable_wakeup(void)
{
    if (!button_initialized) {
        const esp_err_t error = button_init();
        if (error != ESP_OK) {
            return error;
        }
    }

    if (!rtc_gpio_is_valid_gpio(BUTTON_GPIO)) {
        ESP_LOGE(TAG, "Button GPIO does not support EXT0 wakeup");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error;
    if (BUTTON_ACTIVE_LEVEL == 0) {
        error = rtc_gpio_pullup_en(BUTTON_GPIO);
        if (error == ESP_OK) {
            error = rtc_gpio_pulldown_dis(BUTTON_GPIO);
        }
    } else {
        error = rtc_gpio_pullup_dis(BUTTON_GPIO);
        if (error == ESP_OK) {
            error = rtc_gpio_pulldown_en(BUTTON_GPIO);
        }
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure button wake pull: %s", esp_err_to_name(error));
        return error;
    }

    error = esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, BUTTON_ACTIVE_LEVEL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable button wakeup: %s", esp_err_to_name(error));
    }
    return error;
}
