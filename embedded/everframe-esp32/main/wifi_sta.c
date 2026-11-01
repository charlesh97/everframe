#include "wifi_sta.h"

#include "device_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"

#define EVERFRAME_NAMESPACE "everframe"

#define LED_GPIO GPIO_NUM_21
#define LED_ON_LEVEL 0
#define LED_OFF_LEVEL 1

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64
#define WIFI_MAX_FAST_RETRIES 5
#define WIFI_SLOW_RETRY_DELAY_MS 5000
#define WIFI_INITIAL_CONNECT_TIMEOUT_MS 30000

#define WIFI_TRY_CONNECTED_BIT BIT0
#define WIFI_TRY_FAILED_BIT BIT1

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

/** Validate caller-owned credentials and build the ESP-IDF station config. */
static esp_err_t make_station_config(const char *ssid, const char *password,
                                     wifi_config_t *wifi_config)
{
    if (ssid == NULL || password == NULL || wifi_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strnlen(ssid, WIFI_SSID_MAX_LEN + 1);
    size_t password_len = strnlen(password, WIFI_PASSWORD_MAX_LEN + 1);
    if (ssid_len == 0 || ssid_len > WIFI_SSID_MAX_LEN || password_len == 0 ||
        password_len > WIFI_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(wifi_config, 0, sizeof(*wifi_config));
    memcpy(wifi_config->sta.ssid, ssid, ssid_len);
    memcpy(wifi_config->sta.password, password, password_len);

    /* Require WPA2 or newer while allowing the normal WPA2/WPA3 transition. */
    wifi_config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return ESP_OK;
}

/**
 * Complete one station connection attempt without scheduling a retry.
 *
 * The bounded provisioning path needs a clear result so it can return to the
 * captive portal after bad credentials instead of remaining in a retry loop.
 */
static void wifi_try_event_handler(void *argument, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    EventGroupHandle_t event_group = argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "One-time connect call failed: %s",
                     esp_err_to_name(error));
            xEventGroupSetBits(event_group, WIFI_TRY_FAILED_BIT);
        }
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(TAG, "One-time Wi-Fi connection disconnected (reason=%d)",
                 event->reason);
        xEventGroupSetBits(event_group, WIFI_TRY_FAILED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "One-time connection received IP " IPSTR,
                 IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(event_group, WIFI_TRY_CONNECTED_BIT);
    }
}

/** Stop and release the resources acquired by wifi_sta_try_once() on failure. */
static void wifi_try_cleanup(esp_netif_t *station_netif,
                             esp_event_handler_instance_t wifi_event_instance,
                             esp_event_handler_instance_t ip_event_instance)
{
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_event_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          ip_event_instance);
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(station_netif);
}

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

esp_err_t wifi_sta_try_once(const char *ssid, const char *password,
                            uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wifi_config;
    esp_err_t error = make_station_config(ssid, password, &wifi_config);
    if (error != ESP_OK) {
        return error;
    }

    EventGroupHandle_t event_group = xEventGroupCreate();
    if (event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* app_main already initializes esp_netif and the default event loop. */
    esp_netif_t *station_netif = esp_netif_create_default_wifi_sta();
    if (station_netif == NULL) {
        vEventGroupDelete(event_group);
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init_config);
    if (error != ESP_OK) {
        esp_netif_destroy_default_wifi(station_netif);
        vEventGroupDelete(event_group);
        return error;
    }

    esp_event_handler_instance_t wifi_event_instance = NULL;
    esp_event_handler_instance_t ip_event_instance = NULL;
    error = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_try_event_handler, event_group,
        &wifi_event_instance);
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_try_event_handler, event_group,
            &ip_event_instance);
    }
    if (error != ESP_OK) {
        if (wifi_event_instance != NULL) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  wifi_event_instance);
        }
        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(station_netif);
        vEventGroupDelete(event_group);
        return error;
    }

    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) {
        /* Credentials are owned by the everframe NVS namespace, not Wi-Fi's. */
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error != ESP_OK) {
        wifi_try_cleanup(station_netif, wifi_event_instance,
                         ip_event_instance);
        vEventGroupDelete(event_group);
        return error;
    }

    ESP_LOGI(TAG, "Trying Wi-Fi SSID '%s' once", ssid);
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    EventBits_t bits = xEventGroupWaitBits(
        event_group, WIFI_TRY_CONNECTED_BIT | WIFI_TRY_FAILED_BIT, pdFALSE,
        pdFALSE, timeout_ticks);

    if ((bits & WIFI_TRY_CONNECTED_BIT) != 0) {
        /* The caller owns the live station connection after success. */
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_instance);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_event_instance);
        vEventGroupDelete(event_group);
        return ESP_OK;
    }

    wifi_try_cleanup(station_netif, wifi_event_instance, ip_event_instance);
    vEventGroupDelete(event_group);
    if ((bits & WIFI_TRY_FAILED_BIT) != 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID '%s' rejected the one-time connection", ssid);
    } else {
        ESP_LOGW(TAG, "Timed out connecting to Wi-Fi SSID '%s'", ssid);
    }
    return ESP_ERR_TIMEOUT;
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

    wifi_config_t wifi_config;
    error = make_station_config(ssid, password, &wifi_config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Saved Wi-Fi credentials are invalid: %s",
                 esp_err_to_name(error));
        return error;
    }

    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error == ESP_OK) {
        /* Credentials are owned by the everframe NVS namespace, not Wi-Fi's. */
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (error == ESP_OK) {
        error = esp_wifi_start();
    }
    if (error == ESP_OK) {
        /*
         * HTTPS transfers are brief and happen only while the frame is awake.
         * Keeping the radio fully awake here avoids intermittent TLS handshake
         * timeouts; deep sleep still provides the normal battery savings.
         */
        error = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI(TAG, "Connecting to saved Wi-Fi SSID '%s'", ssid);

    /*
     * Bound only the initial connection so stale credentials can recover to the
     * captive portal. After the first IP, callbacks retain the existing
     * indefinite reconnect behavior for a normally provisioned frame.
     */
    bool connected_once = false;
    TickType_t initial_start = xTaskGetTickCount();
    TickType_t initial_timeout = pdMS_TO_TICKS(WIFI_INITIAL_CONNECT_TIMEOUT_MS);
    while (true) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (!connected_once) {
            TickType_t elapsed = xTaskGetTickCount() - initial_start;
            if (elapsed >= initial_timeout) {
                ESP_LOGW(TAG, "No IP after %d seconds; returning to setup",
                         WIFI_INITIAL_CONNECT_TIMEOUT_MS / 1000);
                wifi_try_cleanup(station_netif, wifi_event_instance,
                                 ip_event_instance);
                vEventGroupDelete(s_wifi_event_group);
                s_wifi_event_group = NULL;
                return ESP_ERR_TIMEOUT;
            }
            wait_ticks = initial_timeout - elapsed;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_SLOW_RETRY_BIT,
            pdTRUE, pdFALSE, wait_ticks);

        if (bits == 0 && !connected_once) {
            ESP_LOGW(TAG, "Initial Wi-Fi connection timed out; returning to setup");
            wifi_try_cleanup(station_netif, wifi_event_instance,
                             ip_event_instance);
            vEventGroupDelete(s_wifi_event_group);
            s_wifi_event_group = NULL;
            return ESP_ERR_TIMEOUT;
        }

        if ((bits & WIFI_CONNECTED_BIT) != 0) {
            connected_once = true;
            ESP_LOGI(TAG, "Station connection is ready");
            error = device_runtime_start();
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "Could not start device/cloud runtime: %s",
                         esp_err_to_name(error));
            }
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
