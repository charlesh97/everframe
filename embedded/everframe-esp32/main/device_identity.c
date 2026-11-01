#include "device_identity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "nvs.h"

#define EVERFRAME_NAMESPACE "everframe"

#define DEVICE_ID_KEY "device_id"
#define DEVICE_SECRET_KEY "device_secret"
#define ACTIVATION_CODE_KEY "activation_code"

static const char *TAG = "device_identity";

static bool is_lower_hex(const char *value, size_t expected_length)
{
    if (value == NULL || strlen(value) != expected_length) {
        return false;
    }

    for (size_t index = 0; index < expected_length; index++) {
        const char character = value[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }

    return true;
}

static void bytes_to_hex(const uint8_t *bytes, size_t byte_count, char *hex)
{
    static const char HEX[] = "0123456789abcdef";

    for (size_t index = 0; index < byte_count; index++) {
        hex[index * 2] = HEX[bytes[index] >> 4];
        hex[index * 2 + 1] = HEX[bytes[index] & 0x0fU];
    }
    hex[byte_count * 2] = '\0';
}

static void generate_hex(char *hex, size_t hex_length)
{
    const size_t byte_count = hex_length / 2;
    uint8_t random_bytes[DEVICE_SECRET_HEX_LENGTH / 2];

    for (size_t offset = 0; offset < byte_count; offset += sizeof(uint32_t)) {
        const uint32_t random_value = esp_random();
        const size_t bytes_this_word = byte_count - offset < sizeof(uint32_t)
                                           ? byte_count - offset
                                           : sizeof(uint32_t);
        for (size_t byte_index = 0; byte_index < bytes_this_word; byte_index++) {
            random_bytes[offset + byte_index] =
                (uint8_t)(random_value >> (byte_index * 8));
        }
    }

    bytes_to_hex(random_bytes, byte_count, hex);
}

static void derive_activation_code(const char *device_id,
                                   char activation_code[DEVICE_ACTIVATION_CODE_LENGTH + 1])
{
    for (size_t index = 0; index < DEVICE_ACTIVATION_CODE_LENGTH; index++) {
        const char character = device_id[index];
        activation_code[index] = (character >= 'a' && character <= 'f')
                                     ? (char)(character - 'a' + 'A')
                                     : character;
    }
    activation_code[DEVICE_ACTIVATION_CODE_LENGTH] = '\0';
}

/* Returns ESP_OK for a valid value, ESP_ERR_NVS_NOT_FOUND if absent. */
static esp_err_t get_nvs_string(nvs_handle_t nvs_handle, const char *key,
                                char *value, size_t value_size)
{
    size_t stored_size = value_size;
    esp_err_t error = nvs_get_str(nvs_handle, key, value, &stored_size);
    if (error != ESP_OK) {
        return error;
    }

    /* Exact sizing rejects truncated, non-terminated, and overlong values. */
    if (stored_size != value_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t load_identity(nvs_handle_t nvs_handle, device_identity_t *identity)
{
    esp_err_t error = get_nvs_string(nvs_handle, DEVICE_ID_KEY,
                                     identity->device_id,
                                     sizeof(identity->device_id));
    if (error != ESP_OK) {
        return error;
    }

    error = get_nvs_string(nvs_handle, DEVICE_SECRET_KEY,
                           identity->device_secret,
                           sizeof(identity->device_secret));
    if (error != ESP_OK) {
        return error;
    }

    if (!is_lower_hex(identity->device_id, DEVICE_ID_HEX_LENGTH) ||
        !is_lower_hex(identity->device_secret, DEVICE_SECRET_HEX_LENGTH)) {
        return ESP_ERR_INVALID_STATE;
    }

    derive_activation_code(identity->device_id, identity->activation_code);
    return ESP_OK;
}

static esp_err_t store_identity(nvs_handle_t nvs_handle,
                                const device_identity_t *identity)
{
    esp_err_t error = nvs_set_str(nvs_handle, DEVICE_ID_KEY, identity->device_id);
    if (error == ESP_OK) {
        error = nvs_set_str(nvs_handle, DEVICE_SECRET_KEY, identity->device_secret);
    }
    if (error == ESP_OK) {
        error = nvs_set_str(nvs_handle, ACTIVATION_CODE_KEY,
                            identity->activation_code);
    }
    if (error == ESP_OK) {
        error = nvs_commit(nvs_handle);
    }
    return error;
}

esp_err_t device_ensure_identity(device_identity_t *identity)
{
    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (error != ESP_OK) {
        return error;
    }

    error = load_identity(nvs_handle, identity);
    if (error == ESP_OK) {
        /* Keep the stored public activation code in sync with the device ID. */
        char stored_code[DEVICE_ACTIVATION_CODE_LENGTH + 1];
        const esp_err_t code_error = get_nvs_string(nvs_handle, ACTIVATION_CODE_KEY,
                                                     stored_code, sizeof(stored_code));
        if (code_error != ESP_OK ||
            strcmp(stored_code, identity->activation_code) != 0) {
            error = nvs_set_str(nvs_handle, ACTIVATION_CODE_KEY,
                                identity->activation_code);
            if (error == ESP_OK) {
                error = nvs_commit(nvs_handle);
            }
        }
        nvs_close(nvs_handle);
        return error;
    }

    /* A partial or malformed identity must not be paired with a new secret. */
    generate_hex(identity->device_id, DEVICE_ID_HEX_LENGTH);
    generate_hex(identity->device_secret, DEVICE_SECRET_HEX_LENGTH);
    derive_activation_code(identity->device_id, identity->activation_code);

    error = store_identity(nvs_handle, identity);
    nvs_close(nvs_handle);
    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Created device identity: deviceId=%s activationCode=%s",
                 identity->device_id, identity->activation_code);
    }
    return error;
}

esp_err_t device_get_identity(device_identity_t *identity)
{
    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t error = nvs_open(EVERFRAME_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (error != ESP_OK) {
        return error;
    }

    error = load_identity(nvs_handle, identity);
    nvs_close(nvs_handle);
    return error;
}

esp_err_t device_get_token_hash(char *token_hash, size_t token_hash_size)
{
    if (token_hash == NULL ||
        token_hash_size < DEVICE_TOKEN_HASH_HEX_LENGTH + 1) {
        return ESP_ERR_INVALID_ARG;
    }

    device_identity_t identity;
    esp_err_t error = device_get_identity(&identity);
    if (error != ESP_OK) {
        return error;
    }

    uint8_t secret_bytes[DEVICE_SECRET_HEX_LENGTH / 2];
    for (size_t index = 0; index < sizeof(secret_bytes); ++index) {
        unsigned int byte = 0;
        if (sscanf(identity.device_secret + index * 2U, "%2x", &byte) != 1) {
            memset(&identity, 0, sizeof(identity));
            return ESP_ERR_INVALID_STATE;
        }
        secret_bytes[index] = (uint8_t)byte;
    }

    uint8_t digest[32];
    const mbedtls_md_info_t *sha256 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (sha256 == NULL) {
        memset(secret_bytes, 0, sizeof(secret_bytes));
        memset(&identity, 0, sizeof(identity));
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int hash_error = mbedtls_md(sha256, secret_bytes,
                                      sizeof(secret_bytes), digest);
    memset(secret_bytes, 0, sizeof(secret_bytes));
    memset(&identity, 0, sizeof(identity));
    if (hash_error != 0) {
        return ESP_FAIL;
    }

    bytes_to_hex(digest, sizeof(digest), token_hash);
    memset(digest, 0, sizeof(digest));
    return ESP_OK;
}
