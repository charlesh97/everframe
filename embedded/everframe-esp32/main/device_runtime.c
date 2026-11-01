#include "device_runtime.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs.h"

#include "button.h"
#include "cloud_client.h"
#include "device_identity.h"
#include "display_provisioning.h"

#define EVERFRAME_NAMESPACE "everframe"
#define LAST_IMAGE_HASH_KEY "last_image_hash"
#define WAKE_PRESET_KEY "wake_preset"
#define WAKE_VERSION_KEY "wake_version"
#define WAKE_SECONDS_KEY "wake_seconds"

#define REGISTER_URL \
    "https://registerdevice-z7xqv7ciuq-uc.a.run.app"
#define POLL_URL \
    "https://devicepoll-z7xqv7ciuq-uc.a.run.app"

#define RUNTIME_TASK_STACK_BYTES (16U * 1024U)
#define RUNTIME_TASK_PRIORITY 5U
#define POLL_INTERVAL_MS 15000U
#define POLL_RETRY_INITIAL_MS 5000U
#define POLL_RETRY_MAX_MS 30000U
#define REGISTER_RETRY_INITIAL_MS 1000U
#define REGISTER_RETRY_MAX_ATTEMPTS 3U
#define POLL_MAX_CONSECUTIVE_FAILURES 3U
#define FRAME_UPDATE_MAX_ATTEMPTS 3U
#define REAUTH_MAX_ATTEMPTS 2U
#define CLAIM_WINDOW_MINUTES 30U
#define BUTTON_UPDATE_WINDOW_MINUTES 5U
#define IDLE_SLEEP_SECONDS (12U * 60U * 60U)
#define RETRY_SLEEP_SECONDS (60U * 60U)
#define MIN_SCHEDULE_SLEEP_SECONDS 60U
#define MAX_SCHEDULE_SLEEP_SECONDS (8U * 24U * 60U * 60U)

static const char *TAG = "device_runtime";
static TaskHandle_t s_runtime_task;
static bool s_button_wake_window;

typedef struct {
    char preset[CLOUD_SCHEDULE_NAME_MAX_LEN + 1];
    char version[CLOUD_SCHEDULE_VERSION_MAX_LEN + 1];
    uint32_t sleep_seconds;
    TickType_t received_tick;
    bool manual;
} wake_schedule_state_t;

void device_runtime_set_button_wake(bool button_wake)
{
    s_button_wake_window = button_wake;
}

static bool keep_awake_for_development(void)
{
#ifdef CONFIG_EVERFRAME_DISABLE_DEEP_SLEEP
    return true;
#else
    return false;
#endif
}

static bool is_hash(const char *value)
{
    if (value == NULL || strlen(value) != CLOUD_SHA256_HEX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < CLOUD_SHA256_HEX_LENGTH; ++i) {
        if (!isxdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t load_last_image_hash(char hash[CLOUD_SHA256_HEX_LENGTH + 1])
{
    hash[0] = '\0';
    nvs_handle_t handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }

    size_t length = CLOUD_SHA256_HEX_LENGTH + 1U;
    error = nvs_get_str(handle, LAST_IMAGE_HASH_KEY, hash, &length);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        hash[0] = '\0';
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (length != CLOUD_SHA256_HEX_LENGTH + 1U || !is_hash(hash)) {
        hash[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < CLOUD_SHA256_HEX_LENGTH; ++i) {
        hash[i] = (char)tolower((unsigned char)hash[i]);
    }
    return ESP_OK;
}

static esp_err_t save_last_image_hash(const char *hash)
{
    if (!is_hash(hash)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, LAST_IMAGE_HASH_KEY, hash);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

/*
 * Any non-photo screen invalidates the cached photo hash.  Otherwise a frame
 * that shows its activation code can be claimed while still reporting the
 * previous photo as current, leaving the activation screen visible forever.
 */
static esp_err_t clear_last_image_hash(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }

    error = nvs_erase_key(handle, LAST_IMAGE_HASH_KEY);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static bool is_schedule_name(const char *value)
{
    return value != NULL &&
        (strcmp(value, "manual") == 0 || strcmp(value, "12h") == 0 ||
         strcmp(value, "daily") == 0 || strcmp(value, "weekly") == 0);
}

static void default_wake_schedule(wake_schedule_state_t *schedule)
{
    memset(schedule, 0, sizeof(*schedule));
    memcpy(schedule->preset, "12h", 4U);
    schedule->sleep_seconds = IDLE_SLEEP_SECONDS;
    schedule->received_tick = xTaskGetTickCount();
}

static esp_err_t load_wake_schedule(wake_schedule_state_t *schedule)
{
    if (schedule == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    default_wake_schedule(schedule);
    nvs_handle_t handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    size_t preset_length = sizeof(schedule->preset);
    size_t version_length = sizeof(schedule->version);
    error = nvs_get_str(handle, WAKE_PRESET_KEY, schedule->preset, &preset_length);
    if (error == ESP_OK) {
        error = nvs_get_str(handle, WAKE_VERSION_KEY, schedule->version,
                            &version_length);
    }
    if (error == ESP_OK) {
        error = nvs_get_u32(handle, WAKE_SECONDS_KEY, &schedule->sleep_seconds);
    }
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        default_wake_schedule(schedule);
        return ESP_OK;
    }
    if (error != ESP_OK || !is_schedule_name(schedule->preset) ||
        schedule->version[0] == '\0' ||
        schedule->sleep_seconds > MAX_SCHEDULE_SLEEP_SECONDS ||
        (strcmp(schedule->preset, "manual") != 0 &&
         schedule->sleep_seconds < MIN_SCHEDULE_SLEEP_SECONDS)) {
        default_wake_schedule(schedule);
        return ESP_ERR_INVALID_STATE;
    }
    schedule->manual = strcmp(schedule->preset, "manual") == 0;
    schedule->received_tick = xTaskGetTickCount();
    return ESP_OK;
}

static esp_err_t save_wake_schedule(const wake_schedule_state_t *schedule)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_str(handle, WAKE_PRESET_KEY, schedule->preset);
    if (error == ESP_OK) {
        error = nvs_set_str(handle, WAKE_VERSION_KEY, schedule->version);
    }
    if (error == ESP_OK) {
        error = nvs_set_u32(handle, WAKE_SECONDS_KEY, schedule->sleep_seconds);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

static esp_err_t apply_poll_schedule(const cloud_poll_result_t *poll,
                                     wake_schedule_state_t *schedule,
                                     bool *version_changed)
{
    if (version_changed != NULL) {
        *version_changed = false;
    }
    if (poll == NULL || schedule == NULL || !poll->has_schedule ||
        !is_schedule_name(poll->schedule) || poll->schedule_version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t sleep_seconds = 0U;
    bool manual = strcmp(poll->schedule, "manual") == 0;
    if (!manual) {
        uint64_t delta = poll->next_wake_at - poll->server_time;
        if (poll->next_wake_at <= poll->server_time ||
            delta < MIN_SCHEDULE_SLEEP_SECONDS ||
            delta > MAX_SCHEDULE_SLEEP_SECONDS) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        sleep_seconds = (uint32_t)delta;
    }
    struct timeval online_time = {
        .tv_sec = (time_t)poll->server_time,
        .tv_usec = 0,
    };
    if (settimeofday(&online_time, NULL) != 0) {
        return ESP_FAIL;
    }
    bool changed = strcmp(schedule->version, poll->schedule_version) != 0 ||
        strcmp(schedule->preset, poll->schedule) != 0;
    memcpy(schedule->preset, poll->schedule, sizeof(schedule->preset));
    memcpy(schedule->version, poll->schedule_version, sizeof(schedule->version));
    schedule->sleep_seconds = sleep_seconds;
    schedule->received_tick = xTaskGetTickCount();
    schedule->manual = manual;
    if (version_changed != NULL) {
        *version_changed = changed;
    }
    if (!changed) {
        return ESP_OK;
    }
    return save_wake_schedule(schedule);
}

static void sleep_device(uint32_t seconds)
{
    if (keep_awake_for_development()) {
        ESP_LOGW(TAG,
                 "Development keep-awake is enabled; skipping %lu-second deep sleep",
                 (unsigned long)seconds);
        return;
    }
    if (seconds == 0U) {
        ESP_LOGI(TAG, "Manual-only schedule active; sleeping until button wake");
    } else {
        ESP_LOGI(TAG, "Sleeping for %lu seconds", (unsigned long)seconds);
    }
    esp_err_t error = button_enable_wakeup();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Button wake could not be enabled: %s",
                 esp_err_to_name(error));
    }
    if (seconds > 0U) {
        ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL));
    } else {
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
    esp_deep_sleep_start();
}

static void sleep_for_schedule(const wake_schedule_state_t *schedule)
{
    if (schedule == NULL || schedule->manual) {
        sleep_device(0U);
        return;
    }
    uint32_t elapsed = (uint32_t)(
        (xTaskGetTickCount() - schedule->received_tick) / configTICK_RATE_HZ
    );
    uint32_t remaining = schedule->sleep_seconds > elapsed ?
        schedule->sleep_seconds - elapsed : MIN_SCHEDULE_SLEEP_SECONDS;
    if (remaining < MIN_SCHEDULE_SLEEP_SECONDS) {
        remaining = MIN_SCHEDULE_SLEEP_SECONDS;
    }
    sleep_device(remaining);
}

static esp_err_t register_identity(const device_identity_t *identity)
{
    char token_hash[DEVICE_TOKEN_HASH_HEX_LENGTH + 1];
    esp_err_t error = device_get_token_hash(token_hash, sizeof(token_hash));
    if (error != ESP_OK) {
        return error;
    }

    cloud_register_result_t result;
    error = cloud_register_device(identity->device_id, token_hash,
                                  identity->activation_code, &result);
    memset(token_hash, 0, sizeof(token_hash));
    if (error != ESP_OK) {
        return error;
    }
    if (result == CLOUD_REGISTER_IDENTITY_CONFLICT) {
        ESP_LOGE(TAG, "Device identity conflicts with cloud; hold the button for 10 seconds to factory reset");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Device registration is ready (activation %s)",
             identity->activation_code);
    return ESP_OK;
}

static uint32_t backoff_delay_ms(uint32_t initial_delay_ms,
                                 uint32_t maximum_delay_ms,
                                 uint8_t failed_attempts)
{
    uint32_t delay = initial_delay_ms;
    while (failed_attempts > 1U && delay < maximum_delay_ms) {
        if (delay > maximum_delay_ms / 2U) {
            return maximum_delay_ms;
        }
        delay *= 2U;
        --failed_attempts;
    }
    return delay > maximum_delay_ms ? maximum_delay_ms : delay;
}

/*
 * Registration is idempotent. Retrying a short, bounded number of transient
 * transport failures avoids stranding a just-connected frame, while a 409
 * identity conflict is deliberately returned immediately for user recovery.
 */
static esp_err_t register_identity_with_retry(const device_identity_t *identity)
{
    esp_err_t error = ESP_FAIL;
    for (uint8_t attempt = 1U; attempt <= REGISTER_RETRY_MAX_ATTEMPTS; ++attempt) {
        error = register_identity(identity);
        if (error == ESP_OK || error == ESP_ERR_INVALID_STATE) {
            return error;
        }
        if (attempt < REGISTER_RETRY_MAX_ATTEMPTS) {
            uint32_t delay = backoff_delay_ms(REGISTER_RETRY_INITIAL_MS,
                                              POLL_RETRY_MAX_MS, attempt);
            ESP_LOGW(TAG, "Registration attempt %u/%u failed: %s; retrying in %lu ms",
                     (unsigned)attempt, (unsigned)REGISTER_RETRY_MAX_ATTEMPTS,
                     esp_err_to_name(error), (unsigned long)delay);
            vTaskDelay(pdMS_TO_TICKS(delay));
        }
    }
    return error;
}

/* displayed is true only after the panel accepted a complete refresh. */
static esp_err_t fetch_and_display(const cloud_poll_result_t *poll,
                                   bool *displayed)
{
    if (displayed != NULL) {
        *displayed = false;
    }
    uint8_t *frame = heap_caps_malloc(CLOUD_FRAME_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = cloud_fetch_image(poll->signed_url, poll->image_hash,
                                        frame, CLOUD_FRAME_BYTES);
    if (error == ESP_OK) {
        error = display_show_frame(frame, CLOUD_FRAME_BYTES);
    }
    heap_caps_free(frame);
    if (error == ESP_OK) {
        if (displayed != NULL) {
            *displayed = true;
        }
        error = save_last_image_hash(poll->image_hash);
    }
    return error;
}

/*
 * Report the newly persisted hash before sleeping so the dashboard reflects
 * the physical panel immediately.  This is deliberately acknowledgment-only:
 * if a newer manifest appears during the long e-paper refresh, leave it for
 * the next wake instead of driving the panel twice in one wake cycle.
 */
static void acknowledge_displayed_frame(const device_identity_t *identity,
                                        const char *current_hash,
                                        const wake_schedule_state_t *schedule)
{
    cloud_poll_result_t acknowledgement;
    esp_err_t error = cloud_poll_device(identity->device_id,
                                        identity->device_secret,
                                        current_hash,
                                        s_button_wake_window,
                                        schedule != NULL ? schedule->version : NULL,
                                        &acknowledgement);
    if (error != ESP_OK) {
        ESP_LOGW(TAG,
                 "Displayed frame could not be acknowledged to cloud: %s; "
                 "will retry on the next wake",
                 esp_err_to_name(error));
        return;
    }

    if (acknowledgement.state == CLOUD_POLL_UP_TO_DATE) {
        ESP_LOGI(TAG, "Displayed frame acknowledged to cloud");
    } else {
        ESP_LOGW(TAG,
                 "Cloud changed while the panel refreshed; deferring the "
                 "new state (%d) until the next wake",
                 (int)acknowledgement.state);
    }
    memset(&acknowledgement, 0, sizeof(acknowledgement));
}

static esp_err_t run_cloud_flow(void)
{
    const cloud_client_config_t config = {
        .register_url = REGISTER_URL,
        .poll_url = POLL_URL,
        .timeout_ms = CLOUD_CLIENT_DEFAULT_TIMEOUT_MS,
    };
    esp_err_t error = cloud_client_configure(&config);
    if (error != ESP_OK) {
        return error;
    }

    device_identity_t identity;
    error = device_get_identity(&identity);
    if (error != ESP_OK) {
        return error;
    }
    error = register_identity_with_retry(&identity);
    if (error != ESP_OK) {
        memset(&identity, 0, sizeof(identity));
        return error;
    }

    char current_hash[CLOUD_SHA256_HEX_LENGTH + 1];
    error = load_last_image_hash(current_hash);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Ignoring invalid last-image state: %s",
                 esp_err_to_name(error));
        current_hash[0] = '\0';
    }

    wake_schedule_state_t wake_schedule;
    error = load_wake_schedule(&wake_schedule);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Using the safe 12-hour schedule after invalid saved state: %s",
                 esp_err_to_name(error));
        default_wake_schedule(&wake_schedule);
    }

    const uint32_t wake_window_minutes = s_button_wake_window ?
        BUTTON_UPDATE_WINDOW_MINUTES : CLAIM_WINDOW_MINUTES;
    TickType_t deadline = xTaskGetTickCount() +
        pdMS_TO_TICKS(wake_window_minutes * 60U * 1000U);
    if (s_button_wake_window) {
        ESP_LOGI(TAG, "Button update window open for %u minutes",
                 (unsigned)BUTTON_UPDATE_WINDOW_MINUTES);
    }
    bool activation_displayed = false;
    uint8_t consecutive_poll_failures = 0U;
    uint8_t frame_update_failures = 0U;
    uint8_t reauthentication_attempts = 0U;
    char failed_frame_hash[CLOUD_SHA256_HEX_LENGTH + 1] = { 0 };
    bool activation_window = false;
    while (keep_awake_for_development() ||
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        cloud_poll_result_t poll;
        error = cloud_poll_device(identity.device_id, identity.device_secret,
                                  current_hash, s_button_wake_window,
                                  wake_schedule.version, &poll);
        if (error != ESP_OK) {
            ++consecutive_poll_failures;
            if (consecutive_poll_failures >= POLL_MAX_CONSECUTIVE_FAILURES &&
                !activation_window) {
                ESP_LOGW(TAG,
                         "Cloud poll failed %u times; preserving the current "
                         "image and sleeping: %s",
                         (unsigned)consecutive_poll_failures,
                         esp_err_to_name(error));
                memset(&identity, 0, sizeof(identity));
                return error;
            }
            uint32_t delay = backoff_delay_ms(POLL_RETRY_INITIAL_MS,
                                              POLL_RETRY_MAX_MS,
                                              consecutive_poll_failures);
            if (activation_window &&
                consecutive_poll_failures >= POLL_MAX_CONSECUTIVE_FAILURES) {
                ESP_LOGW(TAG,
                         "Cloud poll is still failing during activation: %s; "
                         "staying awake and retrying in %lu ms",
                         esp_err_to_name(error), (unsigned long)delay);
            } else {
                ESP_LOGW(TAG,
                         "Cloud poll failed (%u/%u): %s; retrying in %lu ms",
                         (unsigned)consecutive_poll_failures,
                         (unsigned)POLL_MAX_CONSECUTIVE_FAILURES,
                         esp_err_to_name(error), (unsigned long)delay);
            }
            vTaskDelay(pdMS_TO_TICKS(delay));
            continue;
        }
        consecutive_poll_failures = 0U;
        if (poll.state != CLOUD_POLL_UNAUTHORIZED) {
            reauthentication_attempts = 0U;
        }
        bool schedule_changed = false;
        if (poll.state != CLOUD_POLL_PENDING &&
            poll.state != CLOUD_POLL_UNAUTHORIZED) {
            error = apply_poll_schedule(&poll, &wake_schedule, &schedule_changed);
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "Ignoring invalid cloud wake schedule: %s",
                         esp_err_to_name(error));
                memset(&poll, 0, sizeof(poll));
                ++consecutive_poll_failures;
                continue;
            }
            if (schedule_changed) {
                ESP_LOGI(TAG, "Applied %s wake schedule (version %.8s)",
                         wake_schedule.preset, wake_schedule.version);
            }
        }

        switch (poll.state) {
            case CLOUD_POLL_PENDING:
                if (!activation_window) {
                    /*
                     * Showing a pairing code is a promise that the frame is
                     * listening for the claim.  A short-button wake therefore
                     * upgrades from five minutes to a full claim window.
                     */
                    activation_window = true;
                    deadline = xTaskGetTickCount() +
                        pdMS_TO_TICKS(CLAIM_WINDOW_MINUTES * 60U * 1000U);
                    ESP_LOGI(TAG, "Activation window open for %u minutes",
                             (unsigned)CLAIM_WINDOW_MINUTES);
                }
                if (!activation_displayed) {
                    error = display_show_connected(identity.activation_code);
                    if (error != ESP_OK) {
                        ESP_LOGW(TAG, "Could not show activation screen: %s",
                                 esp_err_to_name(error));
                    } else {
                        /* The panel no longer contains the cached photo. */
                        current_hash[0] = '\0';
                        error = clear_last_image_hash();
                        if (error != ESP_OK) {
                            ESP_LOGW(TAG,
                                     "Could not invalidate the previous photo "
                                     "hash after showing activation: %s",
                                     esp_err_to_name(error));
                        }
                    }
                    activation_displayed = true;
                }
                ESP_LOGI(TAG, "Waiting for owner activation code %s",
                         identity.activation_code);
                break;

            case CLOUD_POLL_NO_IMAGE:
                ESP_LOGI(TAG, "Device claimed; waiting for the first processed image");
                break;

            case CLOUD_POLL_PROCESSING:
                ESP_LOGI(TAG, "Cloud is preparing the next image");
                break;

            case CLOUD_POLL_UP_TO_DATE:
                if (s_button_wake_window &&
                    (int32_t)(deadline - xTaskGetTickCount()) > 0) {
                    ESP_LOGI(TAG, "Displayed image is current; waiting for a dashboard send");
                    break;
                }
                ESP_LOGI(TAG, "Displayed image is already current");
                if (schedule_changed) {
                    cloud_poll_result_t schedule_ack;
                    (void)cloud_poll_device(identity.device_id,
                                            identity.device_secret,
                                            current_hash, false,
                                            wake_schedule.version,
                                            &schedule_ack);
                    memset(&schedule_ack, 0, sizeof(schedule_ack));
                }
                if (!keep_awake_for_development()) {
                    memset(&identity, 0, sizeof(identity));
                }
                sleep_for_schedule(&wake_schedule);
                break;

            case CLOUD_POLL_IMAGE_READY:
                ESP_LOGI(TAG, "Downloading a new 192000-byte frame");
                if (strncmp(failed_frame_hash, poll.image_hash,
                            sizeof(failed_frame_hash)) != 0) {
                    memcpy(failed_frame_hash, poll.image_hash,
                           sizeof(failed_frame_hash));
                    frame_update_failures = 0U;
                }
                bool displayed = false;
                error = fetch_and_display(&poll, &displayed);
                if (displayed) {
                    memcpy(current_hash, poll.image_hash,
                           CLOUD_SHA256_HEX_LENGTH + 1U);
                }
                memset(&poll, 0, sizeof(poll));
                if (error == ESP_OK) {
                    ESP_LOGI(TAG, "New frame displayed and committed");
                    acknowledge_displayed_frame(&identity, current_hash,
                                                &wake_schedule);
                    if (!keep_awake_for_development()) {
                        memset(&identity, 0, sizeof(identity));
                    }
                    sleep_for_schedule(&wake_schedule);
                } else if (displayed) {
                    /* The screen is current even if NVS could not retain its hash.
                     * Avoid a second high-power refresh during this wake cycle. */
                    ESP_LOGW(TAG,
                             "Frame displayed but its hash was not persisted: %s; "
                             "will verify again after the next wake",
                             esp_err_to_name(error));
                    if (!keep_awake_for_development()) {
                        memset(&identity, 0, sizeof(identity));
                    }
                    sleep_for_schedule(&wake_schedule);
                } else {
                    ++frame_update_failures;
                    if (frame_update_failures >= FRAME_UPDATE_MAX_ATTEMPTS) {
                        ESP_LOGW(TAG,
                                 "Frame update failed %u times; preserving the "
                                 "current image and sleeping: %s",
                                 (unsigned)frame_update_failures,
                                 esp_err_to_name(error));
                        memset(&identity, 0, sizeof(identity));
                        return error;
                    }
                    ESP_LOGW(TAG, "Frame update failed (%u/%u): %s",
                             (unsigned)frame_update_failures,
                             (unsigned)FRAME_UPDATE_MAX_ATTEMPTS,
                             esp_err_to_name(error));
                }
                break;

            case CLOUD_POLL_UNAUTHORIZED:
                ++reauthentication_attempts;
                if (reauthentication_attempts > REAUTH_MAX_ATTEMPTS) {
                    ESP_LOGW(TAG,
                             "Device remained unauthorized after %u "
                             "re-registration attempts; sleeping",
                             (unsigned)REAUTH_MAX_ATTEMPTS);
                    memset(&identity, 0, sizeof(identity));
                    return ESP_ERR_INVALID_STATE;
                }
                ESP_LOGW(TAG, "Device authorization was rejected; re-registering "
                         "(%u/%u)", (unsigned)reauthentication_attempts,
                         (unsigned)REAUTH_MAX_ATTEMPTS);
                error = register_identity_with_retry(&identity);
                if (error != ESP_OK) {
                    memset(&identity, 0, sizeof(identity));
                    return error;
                }
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }

    memset(&identity, 0, sizeof(identity));
    if (activation_window) {
        ESP_LOGI(TAG, "Activation window expired without a completed link");
        sleep_device(IDLE_SLEEP_SECONDS);
    } else if (s_button_wake_window) {
        ESP_LOGI(TAG, "Button update window expired without a new frame");
        sleep_for_schedule(&wake_schedule);
    } else {
        sleep_device(RETRY_SLEEP_SECONDS);
    }
    return ESP_OK;
}

static void runtime_task(void *argument)
{
    (void)argument;
    while (true) {
        esp_err_t error = run_cloud_flow();
        if (!keep_awake_for_development()) {
            ESP_LOGW(TAG,
                     "Cloud flow stopped: %s; preserving the current image and "
                     "sleeping for retry",
                     esp_err_to_name(error));
            sleep_device(RETRY_SLEEP_SECONDS);
        }
        ESP_LOGE(TAG, "Cloud flow stopped: %s; retrying in 30 seconds",
                 esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(30000U));
    }
}

esp_err_t device_runtime_start(void)
{
    if (s_runtime_task != NULL) {
        return ESP_OK;
    }
    BaseType_t created = xTaskCreate(runtime_task, "everframe_cloud",
                                     RUNTIME_TASK_STACK_BYTES, NULL,
                                     RUNTIME_TASK_PRIORITY, &s_runtime_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
