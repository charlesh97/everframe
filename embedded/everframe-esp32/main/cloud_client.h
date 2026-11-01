#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Native 800 x 480, 4-bits-per-pixel EverFrame panel payload. */
#define CLOUD_FRAME_BYTES (192000U)
#define CLOUD_SHA256_HEX_LENGTH (64U)
#define CLOUD_SCHEDULE_VERSION_MAX_LEN (32U)
#define CLOUD_SCHEDULE_NAME_MAX_LEN (8U)

/*
 * A V4 Google Cloud Storage URL is normally under 1 KiB.  Keep room for
 * future query parameters while avoiding a heap allocation in normal polling.
 */
#define CLOUD_SIGNED_URL_MAX_LEN (2048U)
#define CLOUD_ENDPOINT_URL_MAX_LEN (256U)

/** HTTPS endpoints deployed from cloud/cloud_function/device_auth.py. */
typedef struct {
    const char *register_url; /* POST /registerDevice */
    const char *poll_url;     /* GET  /devicePoll */
    uint32_t timeout_ms;      /* 0 selects CLOUD_CLIENT_DEFAULT_TIMEOUT_MS */
} cloud_client_config_t;

#define CLOUD_CLIENT_DEFAULT_TIMEOUT_MS (15000U)

/** Configure the HTTPS endpoints once after Wi-Fi has an IP address. */
esp_err_t cloud_client_configure(const cloud_client_config_t *config);

/** True after a successful cloud_client_configure(). */
bool cloud_client_is_configured(void);

typedef enum {
    CLOUD_REGISTERED = 0,
    /* HTTP 409: a device ID exists with a different token hash. */
    CLOUD_REGISTER_IDENTITY_CONFLICT,
} cloud_register_result_t;

/*
 * POST the on-device SHA-256(secret), never the secret itself.  A valid
 * server response (including 409) returns ESP_OK; inspect result for 409.
 */
esp_err_t cloud_register_device(const char *device_id,
                                const char *token_hash,
                                const char *activation_code,
                                cloud_register_result_t *result);

typedef enum {
    CLOUD_POLL_PENDING = 0,  /* {"claimed": false} */
    CLOUD_POLL_NO_IMAGE,     /* {"claimed": true, "hasImage": false} */
    CLOUD_POLL_UP_TO_DATE,   /* {"claimed": true, "upToDate": true} */
    CLOUD_POLL_IMAGE_READY,  /* {"claimed": true, "hash": ..., "url": ...} */
    CLOUD_POLL_PROCESSING,   /* {"claimed": true, "processing": true} */
    CLOUD_POLL_UNAUTHORIZED, /* HTTP 401; re-register or factory reset */
} cloud_poll_state_t;

typedef struct {
    cloud_poll_state_t state;
    char image_hash[CLOUD_SHA256_HEX_LENGTH + 1];
    /* Populated only when state == CLOUD_POLL_IMAGE_READY. Treat as secret. */
    char signed_url[CLOUD_SIGNED_URL_MAX_LEN];
    bool has_schedule;
    char schedule[CLOUD_SCHEDULE_NAME_MAX_LEN + 1];
    char schedule_version[CLOUD_SCHEDULE_VERSION_MAX_LEN + 1];
    uint64_t server_time;
    uint64_t next_wake_at; /* 0 for manual-only */
} cloud_poll_result_t;

/*
 * Authenticated poll. current_image_hash may be NULL or empty on first fetch;
 * otherwise it must be a 64-character lowercase/uppercase SHA-256 hex value.
 * button_wake_window tells the server this frame is intentionally reachable
 * for a short interactive send window.
 * A server 401 is represented by CLOUD_POLL_UNAUTHORIZED and ESP_OK.
 */
esp_err_t cloud_poll_device(const char *device_id,
                            const char *secret,
                            const char *current_image_hash,
                            bool button_wake_window,
                            const char *schedule_version,
                            cloud_poll_result_t *result);

/*
 * Download one signed GCS artifact with no Authorization header.  The server
 * must send a non-chunked Content-Length of exactly CLOUD_FRAME_BYTES; bytes
 * are SHA-256 checked against expected_image_hash while copied to out_buffer.
 * out_buffer must point to at least CLOUD_FRAME_BYTES writable bytes.
 */
esp_err_t cloud_fetch_image(const char *signed_url,
                            const char *expected_image_hash,
                            uint8_t *out_buffer,
                            size_t out_buffer_size);
