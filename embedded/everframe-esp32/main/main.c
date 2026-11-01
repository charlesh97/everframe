#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "button.h"
#include "device_identity.h"
#include "device_runtime.h"
#include "display_provisioning.h"
#include "EPD_7in3e.h"
#include "ePaper_Config.h"
#include "wifi_prov.h"
#include "wifi_sta.h"

#define EVERFRAME_NAMESPACE "everframe"

#define LED_GPIO GPIO_NUM_21
#define LED_OFF_LEVEL 1
static const char *TAG = "everframe";

#if CONFIG_EVERFRAME_EMBEDDED_HALF_FRAME_TEST
extern const uint8_t dither_option_1_start[]
    asm("_binary_option_1_no_dither_bin_start");
extern const uint8_t dither_option_1_end[]
    asm("_binary_option_1_no_dither_bin_end");
extern const uint8_t dither_option_2_start[]
    asm("_binary_option_2_blue_noise_bin_start");
extern const uint8_t dither_option_2_end[]
    asm("_binary_option_2_blue_noise_bin_end");
extern const uint8_t dither_option_3_start[]
    asm("_binary_option_3_burkes_bin_start");
extern const uint8_t dither_option_3_end[]
    asm("_binary_option_3_burkes_bin_end");
extern const uint8_t dither_option_4_start[]
    asm("_binary_option_4_atkinson_bin_start");
extern const uint8_t dither_option_4_end[]
    asm("_binary_option_4_atkinson_bin_end");

typedef struct {
    const char *name;
    const char *sha256;
    const uint8_t *start;
    const uint8_t *end;
} dither_test_frame_t;

static const dither_test_frame_t DITHER_TEST_FRAMES[] = {
    {"1 no dither", "f7d001c299df566a499878a2a52faf6f3ef984fbcaf678679e82dde46706e94a",
     dither_option_1_start, dither_option_1_end},
    {"2 blue noise", "2a7f7a3f8ec2ed8ecbfe237530cf169b5a43f46b93f3863e33999eeca1ab1a17",
     dither_option_2_start, dither_option_2_end},
    {"3 serpentine Burkes", "fecaae256b6919884f4619bbcadf61e4da34e865d398d5e8be0c275c43b61203",
     dither_option_3_start, dither_option_3_end},
    {"4 serpentine Atkinson", "1918d55e1d154d2fa9716dae72b368684a6b88338f32621e4be932afa5e1e7ee",
     dither_option_4_start, dither_option_4_end},
};
#endif

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

#if CONFIG_EVERFRAME_EMBEDDED_HALF_FRAME_TEST
    ESP_LOGW(TAG,
             "Running option 4 zero-delay transport test; cloud runtime is bypassed");

    ESP_ERROR_CHECK(EPD_Hardware_Init());
    EPD_Module_Init();
    EPD_7IN3E_Init();
    const dither_test_frame_t *frame = &DITHER_TEST_FRAMES[3];
    const size_t frame_size = (size_t)(frame->end - frame->start);
    ESP_LOGW(TAG, "Displaying option %s with zero row delay", frame->name);
    ESP_LOGI(TAG, "Frame bytes=%u SHA-256=%s",
             (unsigned)frame_size, frame->sha256);
    error = EPD_7IN3E_DisplayRowPaced(frame->start,
                                     frame_size,
                                     EPD_7IN3E_HEIGHT, 1U, 0U,
                                     0U, EPD_7IN3E_WIDTH);
    if (error == ESP_OK) {
        EPD_7IN3E_Sleep();
    }
    EPD_Module_Exit();
    ESP_ERROR_CHECK(error);
    ESP_LOGW(TAG, "Option 4 zero-delay transport test complete");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    device_identity_t identity;
    ESP_ERROR_CHECK(device_ensure_identity(&identity));
    ESP_LOGI(TAG, "Device identity ready: id=%s activation=%s",
             identity.device_id, identity.activation_code);

    ESP_ERROR_CHECK(button_init());
    const bool woke_by_button = button_woke_from_deep_sleep();
    const bool force_provisioning =
        button_was_held_on_boot(BUTTON_PROVISION_HOLD_MS);
    if (force_provisioning &&
        button_poll_held(BUTTON_FACTORY_RESET_HOLD_MS -
                         BUTTON_PROVISION_HOLD_MS)) {
        ESP_LOGW(TAG, "Factory reset requested; erasing NVS and restarting");
        ESP_ERROR_CHECK(nvs_flash_erase());
        esp_restart();
    }
    if (woke_by_button && !force_provisioning) {
        ESP_LOGI(TAG, "Short button press woke the frame; checking cloud now");
    } else if (force_provisioning) {
        ESP_LOGI(TAG, "Button held for provisioning");
    }
    device_runtime_set_button_wake(woke_by_button && !force_provisioning);

    size_t psram_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "External RAM available: %u bytes", (unsigned)psram_bytes);

    /* Both provisioning and station mode use the common TCP/IP/event base. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_EVERFRAME_DISPLAY_SELF_TEST
    ESP_LOGW(TAG, "Provisioning QR display self-test is enabled for this build");
    error = display_show_qr_ap("EverFrame-42D4");
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "E-paper self-test failed: %s", esp_err_to_name(error));
    }
#endif

    if (provisioning_is_complete() && !force_provisioning) {
        ESP_LOGI(TAG, "Provisioning is complete; starting station mode");
        error = wifi_sta_start();
        if (error == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Saved Wi-Fi could not connect; starting setup portal");
            error = wifi_prov_start();
        }
    } else {
        ESP_LOGI(TAG, "%s; starting setup portal",
                 force_provisioning ? "Button requested provisioning"
                                    : "No saved provisioning flag");
        error = wifi_prov_start();
    }

    /* Successful mode startup never returns. Keep a startup failure visible. */
    ESP_LOGE(TAG, "Wi-Fi mode stopped unexpectedly: %s", esp_err_to_name(error));
    while (true) {
        gpio_set_level(LED_GPIO, LED_OFF_LEVEL);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
