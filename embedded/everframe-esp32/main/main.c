#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_prov.h"
#include "wifi_sta.h"

#define EVERFRAME_NAMESPACE "everframe"

#define LED_GPIO GPIO_NUM_21
#define LED_OFF_LEVEL 1

static const char *TAG = "everframe";

/** Configure the XIAO ESP32S3 onboard LED and leave it off at boot. */
static void led_init_off(void)
{
    gpio_config_t led_config = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&led_config));

    /* The onboard GPIO21 LED is active-low, so a high output is off. */
    ESP_ERROR_CHECK(gpio_set_level(LED_GPIO, LED_OFF_LEVEL));
}

/** Read the one-byte boot decision saved after a successful form submission. */
static bool provisioning_is_complete(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not open EverFrame NVS namespace: %s",
                 esp_err_to_name(error));
        return false;
    }

    uint8_t provisioning_done = 0;
    error = nvs_get_u8(nvs_handle, "prov_done", &provisioning_done);
    nvs_close(nvs_handle);

    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not read prov_done: %s", esp_err_to_name(error));
        return false;
    }

    return provisioning_done == 1;
}

void app_main(void)
{
    /* NVS stores both ESP-IDF Wi-Fi state and EverFrame's submitted settings. */
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    led_init_off();

    /* Both provisioning and station mode use the common TCP/IP/event base. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /*
     * Display task disabled during Wi-Fi bring-up. The display sources remain
     * in CMake and continue to compile, but no panel GPIO or SPI work is started.
     */
    // xTaskCreatePinnedToCore(EPD_Task, "EPD_Task", 1024 * 16,
    //                         NULL, 5, NULL, 1);

    if (provisioning_is_complete()) {
        ESP_LOGI(TAG, "Provisioning is complete; starting station mode");
        error = wifi_sta_start();
    } else {
        ESP_LOGI(TAG, "No saved provisioning flag; starting setup portal");
        error = wifi_prov_start();
    }

    /* Successful mode startup never returns. Keep a startup failure visible. */
    ESP_LOGE(TAG, "Wi-Fi mode stopped unexpectedly: %s", esp_err_to_name(error));
    while (true) {
        gpio_set_level(LED_GPIO, LED_OFF_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
