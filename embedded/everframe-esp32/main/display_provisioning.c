#include "display_provisioning.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "qrcode.h"

#include "EPD_7in3e.h"
#include "fonts.h"
#include "ePaper_Config.h"

#define DISPLAY_WIDTH EPD_7IN3E_WIDTH
#define DISPLAY_HEIGHT EPD_7IN3E_HEIGHT
#define CANVAS_WIDTH DISPLAY_HEIGHT
#define CANVAS_HEIGHT DISPLAY_WIDTH
#define QR_QUIET_MODULES 4

static const char *TAG = "display_provisioning";

static uint8_t *s_qr_frame;
static int s_qr_center_x;
static int s_qr_center_y;
static int s_qr_max_pixels;

static void set_physical_pixel(uint8_t *frame, int x, int y, uint8_t color)
{
    if (frame == NULL || x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) {
        return;
    }
    size_t offset = (size_t)y * (DISPLAY_WIDTH / 2U) + (size_t)x / 2U;
    if ((x & 1) == 0) {
        frame[offset] = (uint8_t)((frame[offset] & 0x0fU) | (color << 4));
    } else {
        frame[offset] = (uint8_t)((frame[offset] & 0xf0U) | color);
    }
}

/* Rotate the logical 480x800 portrait canvas 90 degrees clockwise into the
 * panel's native 800x480 framebuffer. */
static void set_pixel(uint8_t *frame, int x, int y, uint8_t color)
{
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) {
        return;
    }
    set_physical_pixel(frame, DISPLAY_WIDTH - 1 - y, x, color);
}

static void fill_rect(uint8_t *frame, int x, int y, int width, int height,
                      uint8_t color)
{
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            set_pixel(frame, column, row, color);
        }
    }
}

static void draw_text(uint8_t *frame, int x, int y, const char *text,
                      const sFONT *font, int scale, uint8_t color)
{
    const int row_bytes = (font->Width + 7) / 8;
    const int glyph_bytes = font->Height * row_bytes;
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        unsigned char value = (unsigned char)*cursor;
        if (value < ' ' || value > '~') {
            value = '?';
        }
        const uint8_t *glyph = font->table + (value - ' ') * glyph_bytes;
        for (int row = 0; row < font->Height; ++row) {
            for (int column = 0; column < font->Width; ++column) {
                const uint8_t bits = glyph[row * row_bytes + column / 8];
                if ((bits & (0x80U >> (column % 8))) != 0U) {
                    fill_rect(frame, x + column * scale, y + row * scale,
                              scale, scale, color);
                }
            }
        }
        x += font->Width * scale;
    }
}

static int text_width(const char *text, const sFONT *font, int scale)
{
    size_t length = text == NULL ? 0U : strlen(text);
    return (int)length * font->Width * scale;
}

static void draw_centered_text(uint8_t *frame, int y, const char *text,
                               const sFONT *font, int scale, uint8_t color)
{
    draw_text(frame, (CANVAS_WIDTH - text_width(text, font, scale)) / 2, y,
              text, font, scale, color);
}

static void qr_render_callback(esp_qrcode_handle_t qr)
{
    int size = esp_qrcode_get_size(qr);
    int scale = s_qr_max_pixels / (size + QR_QUIET_MODULES * 2);
    int outer_size = (size + QR_QUIET_MODULES * 2) * scale;
    int origin_x = s_qr_center_x - outer_size / 2 + QR_QUIET_MODULES * scale;
    int origin_y = s_qr_center_y - outer_size / 2 + QR_QUIET_MODULES * scale;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (esp_qrcode_get_module(qr, x, y)) {
                fill_rect(s_qr_frame, origin_x + x * scale,
                          origin_y + y * scale, scale, scale,
                          EPD_7IN3E_BLACK);
            }
        }
    }
}

static esp_err_t draw_qr(uint8_t *frame, const char *payload,
                         int center_x, int center_y, int max_pixels)
{
    if (frame == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Version 5 is at most 37 modules; include a four-module quiet zone. */
    if (max_pixels / (37 + QR_QUIET_MODULES * 2) < 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_qr_center_x = center_x;
    s_qr_center_y = center_y;
    s_qr_max_pixels = max_pixels;
    s_qr_frame = frame;

    esp_qrcode_config_t config = ESP_QRCODE_CONFIG_DEFAULT();
    config.display_func = qr_render_callback;
    config.max_qrcode_version = 5;
    config.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    esp_err_t error = esp_qrcode_generate(&config, payload);
    s_qr_frame = NULL;
    return error;
}

esp_err_t display_show_frame(const uint8_t *frame, size_t frame_size)
{
    if (frame == NULL || frame_size != DISPLAY_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = EPD_Hardware_Init();
    if (error != ESP_OK) {
        return error;
    }

    ESP_LOGI(TAG, "Refreshing 800x480 e-paper frame");
    EPD_Module_Init();
    error = EPD_7IN3E_Init();
    bool panel_initialized = error == ESP_OK;
    if (error == ESP_OK) {
        error = EPD_7IN3E_Display(frame);
    }
    esp_err_t sleep_error = panel_initialized ? EPD_7IN3E_Sleep() : ESP_OK;
    EPD_Module_Exit();
    if (error != ESP_OK) {
        return error;
    }
    if (sleep_error != ESP_OK) {
        return sleep_error;
    }
    ESP_LOGI(TAG, "E-paper refresh complete");
    return ESP_OK;
}

static uint8_t *new_white_frame(void)
{
    uint8_t *frame = heap_caps_malloc(DISPLAY_FRAME_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frame != NULL) {
        memset(frame, (EPD_7IN3E_WHITE << 4) | EPD_7IN3E_WHITE,
               DISPLAY_FRAME_BYTES);
    }
    return frame;
}

esp_err_t display_show_qr_ap(const char *softap_ssid)
{
    if (softap_ssid == NULL || softap_ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char payload[80];
    int written = snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;",
                           softap_ssid);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *frame = new_white_frame();
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }
    draw_centered_text(frame, 52, "JOIN WIFI", &Font24, 2,
                       EPD_7IN3E_BLACK);
    draw_centered_text(frame, 126, softap_ssid, &Font24, 1,
                       EPD_7IN3E_BLUE);
    esp_err_t error = draw_qr(frame, payload, 240, 390, 420);
    if (error == ESP_OK) {
        fill_rect(frame, 32, 744, 94, 4, EPD_7IN3E_BLUE);
        draw_centered_text(frame, 730, "EVERFRAME", &Font24, 1,
                           EPD_7IN3E_BLUE);
        fill_rect(frame, 354, 744, 94, 4, EPD_7IN3E_BLUE);
        error = display_show_frame(frame, DISPLAY_FRAME_BYTES);
    }
    heap_caps_free(frame);
    return error;
}

esp_err_t display_show_connected(const char *activation_code)
{
    if (activation_code == NULL || strlen(activation_code) != 6U) {
        return ESP_ERR_INVALID_ARG;
    }
    char payload[96];
    int written = snprintf(payload, sizeof(payload),
                           "https://everframe.hoodcharlesd.me/activate?code=%s",
                           activation_code);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *frame = new_white_frame();
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }
    fill_rect(frame, 0, 0, CANVAS_WIDTH, 64, EPD_7IN3E_GREEN);
    draw_centered_text(frame, 8, "CONNECTED", &Font24, 2,
                       EPD_7IN3E_BLACK);
    esp_err_t error = draw_qr(frame, payload, 240, 320, 410);
    if (error == ESP_OK) {
        draw_centered_text(frame, 560, "ACTIVATION CODE", &Font24, 1,
                           EPD_7IN3E_BLACK);
        draw_centered_text(frame, 610, activation_code, &Font24, 3,
                           EPD_7IN3E_BLUE);
        draw_centered_text(frame, 724, "EVERFRAME.HOODCHARLESD.ME", &Font24, 1,
                           EPD_7IN3E_BLACK);
        error = display_show_frame(frame, DISPLAY_FRAME_BYTES);
    }
    heap_caps_free(frame);
    return error;
}
