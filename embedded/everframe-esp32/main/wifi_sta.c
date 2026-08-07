#include "wifi_sta.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

#define EVERFRAME_NAMESPACE "everframe"

#define LED_GPIO GPIO_NUM_21
#define LED_ON_LEVEL 0
#define LED_OFF_LEVEL 1

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64
#define WIFI_MAX_FAST_RETRIES 5
#define WIFI_SLOW_RETRY_DELAY_MS 5000

/*
 * The event handler wakes wifi_sta_start() for two meaningful state changes:
 * an acquired IP address, or a failed connection that needs a delayed retry.
 */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SLOW_RETRY_BIT BIT1

static const char *TAG = "wifi_sta";

static EventGroupHandle_t s_wifi_event_group;
static int s_fast_retry_count;
static bool s_slow_retry_mode;

/** Read and validate the station credentials saved during provisioning. */
static esp_err_t read_wifi_credentials(char *ssid, size_t ssid_size,
                                       char *password, size_t password_size)
{
    nvs_handle_t nvs_handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (error != ESP_OK) {
        return error;
    }

    size_t stored_ssid_size = ssid_size;
    error = nvs_get_str(nvs_handle, "wifi_ssid", ssid, &stored_ssid_size);
    if (error == ESP_OK) {
        size_t stored_password_size = password_size;
        error = nvs_get_str(nvs_handle, "wifi_pass", password,
                            &stored_password_size);
    }

    nvs_close(nvs_handle);

    if (error == ESP_OK && (ssid[0] == '\0' || password[0] == '\0')) {
        return ESP_ERR_INVALID_STATE;
    }
    return error;
}

/**
 * Follow the standard ESP-IDF station event pattern.
 *
 * Start triggers the first connection. Disconnects get five immediate retries;
 * after that, the main station loop spaces attempts five seconds apart. Once an
 * IP is acquired the retry state is reset and the onboard LED becomes solid.
 */
static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Station started; connecting");
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Initial connect call failed: %s",
                     esp_err_to_name(error));
            s_slow_retry_mode = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_SLOW_RETRY_BIT);
        }
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;

        /* The solid connected indication must disappear if the link drops. */
        gpio_set_level(LED_GPIO, LED_OFF_LEVEL);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (!s_slow_retry_mode &&
            s_fast_retry_count < WIFI_MAX_FAST_RETRIES) {
            s_fast_retry_count++;
            ESP_LOGI(TAG, "Disconnected (reason=%d); fast retry %d/%d",
                     event->reason, s_fast_retry_count,
                     WIFI_MAX_FAST_RETRIES);

            esp_err_t error = esp_wifi_connect();
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "Retry call failed: %s", esp_err_to_name(error));
                s_slow_retry_mode = true;
                xEventGroupSetBits(s_wifi_event_group, WIFI_SLOW_RETRY_BIT);
            }
        } else {
            s_slow_retry_mode = true;
            ESP_LOGE(TAG,
                     "Unable to connect after %d fast retries; switching to "
                     "one attempt every %d seconds",
                     WIFI_MAX_FAST_RETRIES, WIFI_SLOW_RETRY_DELAY_MS / 1000);
            xEventGroupSetBits(s_wifi_event_group, WIFI_SLOW_RETRY_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;

        s_fast_retry_count = 0;
        s_slow_retry_mode = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_SLOW_RETRY_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        /* GPIO21 is active-low on the XIAO ESP32S3. */
        gpio_set_level(LED_GPIO, LED_ON_LEVEL);
        ESP_LOGI(TAG, "Connected; IP address: " IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Onboard LED is solid ON");
    }
}

esp_err_t wifi_sta_start(void)
{
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASSWORD_MAX_LEN + 1];
    esp_err_t error = read_wifi_credentials(ssid, sizeof(ssid), password,
                                            sizeof(password));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not read saved Wi-Fi credentials: %s",
                 esp_err_to_name(error));
        return error;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* app_main has already initialized esp_netif and the default event loop. */
    esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
    if (station_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init_config);
    if (error != ESP_OK) {
        return error;
    }

    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;
    error = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
        &wifi_event_instance);
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL,
            &ip_event_instance);
    }
    if (error != ESP_OK) {
        return error;
    }

    wifi_config_t wifi_config = { 0 };
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password, password_len);

    /* Require WPA2 or newer while allowing the normal WPA2/WPA3 transition. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI(TAG, "Connecting to saved Wi-Fi SSID '%s'", ssid);

    /*
     * Wi-Fi callbacks handle fast retries. This loop remains blocked until it
     * either observes a successful connection or needs to schedule a slow one.
     */
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_SLOW_RETRY_BIT,
            pdTRUE, pdFALSE, portMAX_DELAY);

        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            ESP_LOGI(TAG, "Station connection is ready");
        }

        if ((bits & WIFI_SLOW_RETRY_BIT) != 0) {
            ESP_LOGI(TAG, "Waiting %d seconds before reconnecting",
                     WIFI_SLOW_RETRY_DELAY_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(WIFI_SLOW_RETRY_DELAY_MS));

            ESP_LOGI(TAG, "Trying Wi-Fi connection again");
            error = esp_wifi_connect();
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "Slow retry call failed: %s",
                         esp_err_to_name(error));
                xEventGroupSetBits(s_wifi_event_group, WIFI_SLOW_RETRY_BIT);
            }
        }
    }
}
