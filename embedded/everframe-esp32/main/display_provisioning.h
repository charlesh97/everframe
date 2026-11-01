#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DISPLAY_FRAME_BYTES 192000U

/** Show the portrait captive-portal Wi-Fi join QR and SoftAP SSID. */
esp_err_t display_show_qr_ap(const char *softap_ssid);

/** Show the owner activation code and web activation QR. */
esp_err_t display_show_connected(const char *activation_code);

/** Show a validated native 800x480 4-bpp frame and sleep the panel. */
esp_err_t display_show_frame(const uint8_t *frame, size_t frame_size);
