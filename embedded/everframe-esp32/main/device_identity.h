#pragma once

#include <stddef.h>

#include "esp_err.h"

/* Hex-encoded, persistent device credentials. Do not log device_secret. */
#define DEVICE_ID_HEX_LENGTH 32
#define DEVICE_SECRET_HEX_LENGTH 64
#define DEVICE_TOKEN_HASH_HEX_LENGTH 64
#define DEVICE_ACTIVATION_CODE_LENGTH 6

typedef struct {
    char device_id[DEVICE_ID_HEX_LENGTH + 1];
    char device_secret[DEVICE_SECRET_HEX_LENGTH + 1];
    char activation_code[DEVICE_ACTIVATION_CODE_LENGTH + 1];
} device_identity_t;

/**
 * Load the persistent identity from NVS, creating it if it does not exist or
 * is malformed. NVS must have been initialized before this function is called.
 * The returned secret is intended only for TLS Bearer authentication.
 */
esp_err_t device_ensure_identity(device_identity_t *identity);

/** Load an already-created identity without changing NVS. */
esp_err_t device_get_identity(device_identity_t *identity);

/**
 * Compute lower-case SHA-256(device_secret) as a 64-character hex string.
 * token_hash must provide DEVICE_TOKEN_HASH_HEX_LENGTH + 1 bytes.
 */
esp_err_t device_get_token_hash(char *token_hash, size_t token_hash_size);
