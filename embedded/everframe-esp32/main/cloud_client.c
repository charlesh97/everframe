#include "cloud_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "psa/crypto.h"

#define CLOUD_JSON_RESPONSE_MAX_LEN (4096U)
#define CLOUD_SIGNED_REQUEST_BUFFER_LEN (4096U)
#define CLOUD_HTTP_STATUS_CONFLICT (409)
#define CLOUD_HTTP_STATUS_UNAUTHORIZED (401)

static const char *TAG = "cloud_client";

typedef struct {
    char register_url[CLOUD_ENDPOINT_URL_MAX_LEN];
    char poll_url[CLOUD_ENDPOINT_URL_MAX_LEN];
    uint32_t timeout_ms;
    bool configured;
} cloud_client_state_t;

typedef struct {
    char body[CLOUD_JSON_RESPONSE_MAX_LEN + 1];
    size_t length;
    bool overflow;
} response_buffer_t;

static cloud_client_state_t s_client;

static esp_err_t set_firmware_headers(esp_http_client_handle_t client)
{
    const esp_app_desc_t *description = esp_app_get_description();
    if (description == NULL) {
        return ESP_FAIL;
    }

    char build_id[(sizeof(description->app_elf_sha256) * 2U) + 1U];
    for (size_t index = 0; index < sizeof(description->app_elf_sha256); ++index) {
        snprintf(build_id + (index * 2U), 3U, "%02x",
                 description->app_elf_sha256[index]);
    }

    esp_err_t error = esp_http_client_set_header(
        client, "X-Firmware-Version", description->version);
    if (error == ESP_OK) {
        error = esp_http_client_set_header(client, "X-Firmware-Build", build_id);
    }
    if (error == ESP_OK) {
        error = esp_http_client_set_header(
            client, "X-Firmware-Idf", description->idf_ver);
    }
    return error;
}

static bool is_hex_string(const char *value, size_t length)
{
    if (value == NULL || strlen(value) != length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!isxdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool is_activation_code(const char *value)
{
    if (value == NULL || strlen(value) != 6U) {
        return false;
    }
    for (size_t i = 0; i < 6U; ++i) {
        if (!isalnum((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

/* Cloud credentials must not be sent to an accidentally configured HTTP URL. */
static bool is_https_url(const char *url, size_t max_length)
{
    if (url == NULL || strncmp(url, "https://", 8U) != 0) {
        return false;
    }
    size_t length = strnlen(url, max_length);
    if (length == 8U || length == max_length) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (isspace((unsigned char)url[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t response_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL ||
        event->data_len <= 0 || event->user_data == NULL) {
        return ESP_OK;
    }

    response_buffer_t *response = event->user_data;
    size_t data_length = (size_t)event->data_len;
    if (data_length > CLOUD_JSON_RESPONSE_MAX_LEN - response->length) {
        response->overflow = true;
        return ESP_OK;
    }
    memcpy(response->body + response->length, event->data, data_length);
    response->length += data_length;
    response->body[response->length] = '\0';
    return ESP_OK;
}

/* Execute a small JSON response endpoint. No credentials or URLs are logged. */
static esp_err_t execute_json(const char *url, esp_http_client_method_t method,
                              const char *post_body, const char *authorization,
                              const char *device_id, const char *current_hash,
                              bool button_wake_window,
                              const char *schedule_version,
                              response_buffer_t *response, int *status_code)
{
    if (url == NULL || response == NULL || status_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = (int)s_client.timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = response_event_handler,
        .user_data = response,
        /* Signed URLs and API calls must never be redirected to another host. */
        .disable_auto_redirect = true,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = ESP_OK;
    if (method == HTTP_METHOD_POST) {
        error = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (error == ESP_OK) {
            error = esp_http_client_set_post_field(client, post_body, strlen(post_body));
        }
    }
    if (error == ESP_OK && authorization != NULL) {
        error = esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (error == ESP_OK && device_id != NULL) {
        error = esp_http_client_set_header(client, "X-Device-Id", device_id);
    }
    if (error == ESP_OK && current_hash != NULL && current_hash[0] != '\0') {
        error = esp_http_client_set_header(client, "X-Current-Image-Hash", current_hash);
    }
    if (error == ESP_OK && button_wake_window) {
        error = esp_http_client_set_header(client, "X-Wake-Window", "button");
    }
    if (error == ESP_OK && schedule_version != NULL && schedule_version[0] != '\0') {
        error = esp_http_client_set_header(client, "X-Schedule-Version",
                                           schedule_version);
    }
    if (error == ESP_OK) {
        error = set_firmware_headers(client);
    }
    if (error == ESP_OK) {
        error = esp_http_client_perform(client);
    }
    if (error == ESP_OK) {
        *status_code = esp_http_client_get_status_code(client);
        if (response->overflow) {
            error = ESP_ERR_INVALID_SIZE;
        }
    }

    esp_http_client_cleanup(client);
    return error;
}

/*
 * ESP-IDF 6 no longer ships cJSON as an IDF component.  The two functions
 * below deliberately accept only the small, known device_auth.py schema. They
 * reject escapes and control characters because none are valid in its hex
 * values or Google signed URLs. This avoids carrying a second JSON parser in
 * the firmware just to parse three booleans and two strings.
 */
static const char *json_member_value(const response_buffer_t *response,
                                     const char *name)
{
    if (response == NULL || name == NULL || response->overflow) {
        return NULL;
    }
    char key[40];
    int written = snprintf(key, sizeof(key), "\"%s\"", name);
    if (written < 0 || (size_t)written >= sizeof(key)) {
        return NULL;
    }
    const char *member = strstr(response->body, key);
    if (member == NULL) {
        return NULL;
    }
    member += strlen(key);
    while (*member == ' ' || *member == '\t' || *member == '\r' || *member == '\n') {
        ++member;
    }
    if (*member != ':') {
        return NULL;
    }
    ++member;
    while (*member == ' ' || *member == '\t' || *member == '\r' || *member == '\n') {
        ++member;
    }
    return member;
}

static bool json_bool(const response_buffer_t *response, const char *name,
                      bool *value)
{
    const char *member = json_member_value(response, name);
    if (member == NULL || value == NULL) {
        return false;
    }
    if (strncmp(member, "true", 4U) == 0 &&
        (member[4] == ',' || member[4] == '}' || isspace((unsigned char)member[4]))) {
        *value = true;
        return true;
    }
    if (strncmp(member, "false", 5U) == 0 &&
        (member[5] == ',' || member[5] == '}' || isspace((unsigned char)member[5]))) {
        *value = false;
        return true;
    }
    return false;
}

static bool json_string(const response_buffer_t *response, const char *name,
                        char *out, size_t out_size)
{
    const char *member = json_member_value(response, name);
    if (member == NULL || out == NULL || out_size < 2U || *member != '\"') {
        return false;
    }
    ++member;
    size_t length = 0U;
    while (*member != '\0' && *member != '\"') {
        if (*member == '\\' || (unsigned char)*member < 0x20U ||
            length + 1U >= out_size) {
            return false;
        }
        out[length++] = *member++;
    }
    if (*member != '\"' || length == 0U) {
        return false;
    }
    out[length] = '\0';
    return true;
}

static bool json_uint64(const response_buffer_t *response, const char *name,
                        uint64_t *value)
{
    const char *member = json_member_value(response, name);
    if (member == NULL || value == NULL || !isdigit((unsigned char)*member)) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(member, &end, 10);
    if (errno != 0 || end == member ||
        (*end != ',' && *end != '}' && !isspace((unsigned char)*end))) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool valid_schedule(const char *value)
{
    return value != NULL &&
        (strcmp(value, "manual") == 0 || strcmp(value, "12h") == 0 ||
         strcmp(value, "daily") == 0 || strcmp(value, "weekly") == 0);
}

esp_err_t cloud_client_configure(const cloud_client_config_t *config)
{
    if (config == NULL || !is_https_url(config->register_url, CLOUD_ENDPOINT_URL_MAX_LEN) ||
        !is_https_url(config->poll_url, CLOUD_ENDPOINT_URL_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t register_length = strlen(config->register_url);
    size_t poll_length = strlen(config->poll_url);
    if (register_length >= sizeof(s_client.register_url) ||
        poll_length >= sizeof(s_client.poll_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_client.register_url, config->register_url, register_length + 1U);
    memcpy(s_client.poll_url, config->poll_url, poll_length + 1U);
    s_client.timeout_ms = config->timeout_ms == 0U ?
        CLOUD_CLIENT_DEFAULT_TIMEOUT_MS : config->timeout_ms;
    s_client.configured = true;
    return ESP_OK;
}

bool cloud_client_is_configured(void)
{
    return s_client.configured;
}

esp_err_t cloud_register_device(const char *device_id, const char *token_hash,
                                const char *activation_code,
                                cloud_register_result_t *result)
{
    if (!s_client.configured || result == NULL || !is_hex_string(device_id, 32U) ||
        !is_hex_string(token_hash, CLOUD_SHA256_HEX_LENGTH) ||
        !is_activation_code(activation_code)) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = CLOUD_REGISTERED;

    char request[192];
    int written = snprintf(request, sizeof(request),
                           "{\"deviceId\":\"%s\",\"tokenHash\":\"%s\","
                           "\"activationCode\":\"%s\"}",
                           device_id, token_hash, activation_code);
    if (written < 0 || (size_t)written >= sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }

    response_buffer_t response = { 0 };
    int status_code = 0;
    esp_err_t error = execute_json(s_client.register_url, HTTP_METHOD_POST,
                                   request, NULL, NULL, NULL, false, NULL, &response,
                                   &status_code);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Device registration request failed: %s", esp_err_to_name(error));
        return error;
    }
    if (status_code == CLOUD_HTTP_STATUS_CONFLICT) {
        *result = CLOUD_REGISTER_IDENTITY_CONFLICT;
        return ESP_OK;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "Device registration returned HTTP %d", status_code);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool ok = false;
    if (!json_bool(&response, "ok", &ok) || !ok) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t cloud_poll_device(const char *device_id, const char *secret,
                            const char *current_image_hash,
                            bool button_wake_window,
                            const char *schedule_version,
                            cloud_poll_result_t *result)
{
    if (!s_client.configured || result == NULL || !is_hex_string(device_id, 32U) ||
        !is_hex_string(secret, CLOUD_SHA256_HEX_LENGTH) ||
        (current_image_hash != NULL && current_image_hash[0] != '\0' &&
         !is_hex_string(current_image_hash, CLOUD_SHA256_HEX_LENGTH))) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    char authorization[80];
    int written = snprintf(authorization, sizeof(authorization), "Bearer %s", secret);
    if (written < 0 || (size_t)written >= sizeof(authorization)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char canonical_current_hash[CLOUD_SHA256_HEX_LENGTH + 1] = { 0 };
    if (current_image_hash != NULL && current_image_hash[0] != '\0') {
        for (size_t i = 0; i < CLOUD_SHA256_HEX_LENGTH; ++i) {
            canonical_current_hash[i] =
                (char)tolower((unsigned char)current_image_hash[i]);
        }
    }

    response_buffer_t response = { 0 };
    int status_code = 0;
    esp_err_t error = execute_json(s_client.poll_url, HTTP_METHOD_GET, NULL,
                                   authorization, device_id, canonical_current_hash,
                                   button_wake_window, schedule_version, &response,
                                   &status_code);
    /* Secret is only kept in this stack buffer; do not log it or the URL. */
    memset(authorization, 0, sizeof(authorization));
    memset(canonical_current_hash, 0, sizeof(canonical_current_hash));
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Device poll request failed: %s", esp_err_to_name(error));
        return error;
    }
    if (status_code == CLOUD_HTTP_STATUS_UNAUTHORIZED) {
        result->state = CLOUD_POLL_UNAUTHORIZED;
        return ESP_OK;
    }
    if (status_code != 200) {
        ESP_LOGW(TAG, "Device poll returned HTTP %d", status_code);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool claimed = false;
    if (!json_bool(&response, "claimed", &claimed)) {
        error = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    if (!claimed) {
        result->state = CLOUD_POLL_PENDING;
        return ESP_OK;
    }

    if (!json_string(&response, "schedule", result->schedule,
                     sizeof(result->schedule)) ||
        !valid_schedule(result->schedule) ||
        !json_string(&response, "scheduleVersion", result->schedule_version,
                     sizeof(result->schedule_version)) ||
        !json_uint64(&response, "serverTime", &result->server_time)) {
        error = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    if (strcmp(result->schedule, "manual") == 0) {
        result->next_wake_at = 0U;
    } else if (!json_uint64(&response, "nextWakeAt", &result->next_wake_at) ||
               result->next_wake_at <= result->server_time) {
        error = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    result->has_schedule = true;

    bool flag = false;
    if (json_bool(&response, "processing", &flag) && flag) {
        result->state = CLOUD_POLL_PROCESSING;
        return ESP_OK;
    }
    if (json_bool(&response, "hasImage", &flag) && !flag) {
        result->state = CLOUD_POLL_NO_IMAGE;
        return ESP_OK;
    }
    if (json_bool(&response, "upToDate", &flag) && flag) {
        if (!json_string(&response, "hash", result->image_hash,
                         sizeof(result->image_hash)) ||
            !is_hex_string(result->image_hash, CLOUD_SHA256_HEX_LENGTH)) {
            error = ESP_ERR_INVALID_RESPONSE;
        } else {
            result->state = CLOUD_POLL_UP_TO_DATE;
            return ESP_OK;
        }
        goto fail;
    }

    if (!json_string(&response, "hash", result->image_hash, sizeof(result->image_hash)) ||
        !is_hex_string(result->image_hash, CLOUD_SHA256_HEX_LENGTH) ||
        !json_string(&response, "url", result->signed_url, sizeof(result->signed_url)) ||
        !is_https_url(result->signed_url, sizeof(result->signed_url))) {
        error = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }
    result->state = CLOUD_POLL_IMAGE_READY;
    return ESP_OK;

fail:
    memset(result, 0, sizeof(*result));
    return error;
}

esp_err_t cloud_fetch_image(const char *signed_url,
                            const char *expected_image_hash,
                            uint8_t *out_buffer,
                            size_t out_buffer_size)
{
    if (!is_https_url(signed_url, CLOUD_SIGNED_URL_MAX_LEN) ||
        !is_hex_string(expected_image_hash, CLOUD_SHA256_HEX_LENGTH) ||
        out_buffer == NULL || out_buffer_size < CLOUD_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t config = {
        .url = signed_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)(s_client.configured ? s_client.timeout_ms :
                            CLOUD_CLIENT_DEFAULT_TIMEOUT_MS),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        /* The V4 signature lives in the request target. ESP-IDF's default
         * transmit buffer is too small for the complete signed URL. */
        .buffer_size_tx = CLOUD_SIGNED_REQUEST_BUFFER_LEN,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = esp_http_client_open(client, 0);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Image download connection failed: %s", esp_err_to_name(error));
        esp_http_client_cleanup(client);
        return error;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200 || content_length != (int64_t)CLOUD_FRAME_BYTES) {
        ESP_LOGW(TAG, "Image download rejected (HTTP %d, length %lld)",
                 status_code, (long long)content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        psa_hash_abort(&sha);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t received = 0U;
    while (received < CLOUD_FRAME_BYTES) {
        int read = esp_http_client_read(client, (char *)out_buffer + received,
                                        CLOUD_FRAME_BYTES - received);
        if (read <= 0) {
            error = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if ((size_t)read > CLOUD_FRAME_BYTES - received) {
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (psa_hash_update(&sha, out_buffer + received, (size_t)read) != PSA_SUCCESS) {
            error = ESP_FAIL;
            break;
        }
        received += (size_t)read;
    }

    unsigned char actual_hash[32];
    if (error == ESP_OK && received == CLOUD_FRAME_BYTES) {
        size_t actual_hash_length = 0U;
        if (psa_hash_finish(&sha, actual_hash, sizeof(actual_hash),
                            &actual_hash_length) != PSA_SUCCESS ||
            actual_hash_length != sizeof(actual_hash)) {
            error = ESP_FAIL;
        }
    } else {
        psa_hash_abort(&sha);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK || received != CLOUD_FRAME_BYTES) {
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }

    unsigned char expected_hash[32];
    for (size_t i = 0; i < sizeof(expected_hash); ++i) {
        unsigned int byte = 0;
        if (sscanf(expected_image_hash + (i * 2U), "%2x", &byte) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        expected_hash[i] = (unsigned char)byte;
    }
    if (memcmp(actual_hash, expected_hash, sizeof(actual_hash)) != 0) {
        ESP_LOGW(TAG, "Image download SHA-256 did not match manifest");
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "Verified HTTPS frame: %u bytes, SHA-256 %s",
             (unsigned)CLOUD_FRAME_BYTES, expected_image_hash);
    return ESP_OK;
}
