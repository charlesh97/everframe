#include "ePaper_Config.h"
#include "esp_err.h"
#include "psa/crypto.h"
#include <stdlib.h>
#include <string.h>

#define EPD_FRAME_BYTES ((size_t)EPD_7IN3E_WIDTH * EPD_7IN3E_HEIGHT / 2U)

#define EPD_SPI_CLOCK_HZ 20000000

// SPI device handle
spi_device_handle_t spi_handle = NULL;
static bool epd_hardware_initialized;

esp_err_t EPD_Hardware_Init(void)
{
    if (epd_hardware_initialized) {
        return ESP_OK;
    }

    // Setup all the GPIO outputs (RST, CS, DC, PWR)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << EPD_RST_PIN) | (1ULL << EPD_CS_PIN) | (1ULL << EPD_DC_PIN) | (1ULL << EPD_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    // Configure BUSY pin as input
    gpio_config_t busy_conf = {
        .pin_bit_mask = (1ULL << EPD_BUSY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&busy_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_MOSI_PIN,
        .miso_io_num = EPD_MISO_PIN,  // May not be needed for e-paper, but included for completeness
        .sclk_io_num = EPD_CLK_PIN,
        .quadwp_io_num = -1,  // Not used
        .quadhd_io_num = -1,  // Not used
        .max_transfer_sz = EPD_SPI_MAX_TRANSFER,  // S3 DMA cap: SPI_LL_DMA_MAX_BIT_LEN = 1<<18 bits = 32 KB
                                                  // Frame buffer (192,000 B) is sent in 6 chunks, CS held low
    };

    // Initialize SPI bus (SPI2_HOST is typically used for general purpose SPI)
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(EPD_TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure SPI device for e-paper display
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_CLOCK_HZ,
        .mode = 0,  // SPI_MODE_0 (CPOL=0, CPHA=0)
        .spics_io_num = -1,  // CS is manually driven by EPD_Digital_Write in EPD_7in3e.c
                            // (matches Waveshare reference; avoids double-driving the pin
                            //  against the SPI peripheral's hardware CS)
        .queue_size = 1,
        .flags = 0,
        .pre_cb = NULL,
    };

    // Add device to SPI bus
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(EPD_TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return ret;
    }

    epd_hardware_initialized = true;
    ESP_LOGI(EPD_TAG, "SPI bus initialized successfully at %u Hz",
             (unsigned)EPD_SPI_CLOCK_HZ);
    return ESP_OK;
}

void EPD_Task(void *pvParameter)
{
    (void)pvParameter;
    if (EPD_Hardware_Init() != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }

    // Display image data
    EPD_Module_Init();

    printf("e-Paper Init and Clear...\n");
    EPD_7IN3E_Init();
    EPD_7IN3E_Clear(EPD_7IN3E_BLUE); // WHITE 
    vTaskDelay(pdMS_TO_TICKS(1000));

    printf("Displaying color bands...\n");
    uint8_t *frame = (uint8_t *)malloc(EPD_7IN3E_WIDTH * EPD_7IN3E_HEIGHT / 2);
    if (frame == NULL) {
        ESP_LOGE(EPD_TAG, "Failed to allocate frame buffer");
        vTaskDelete(NULL);
        return;
    }
    // Full frame: 6 horizontal color bands (E palette, 800x480, 4bpp)
    const uint8_t colors[6] = {
        EPD_7IN3E_BLACK, EPD_7IN3E_YELLOW, EPD_7IN3E_RED,
        EPD_7IN3E_BLUE,  EPD_7IN3E_GREEN,  EPD_7IN3E_WHITE
    };
    uint16_t row_bytes = EPD_7IN3E_WIDTH / 2;   // 400 bytes/row
    uint16_t band_rows = EPD_7IN3E_HEIGHT / 6;  // 80 rows per band
    for (uint8_t b = 0; b < 6; b++) {
        for (uint16_t r = 0; r < band_rows; r++) {
            memset(frame + (b * band_rows + r) * row_bytes,
                   (colors[b] << 4) | colors[b], row_bytes);
        }
    }
    esp_err_t display_error = EPD_7IN3E_Display(frame);
    free(frame);
    if (display_error != ESP_OK) {
        ESP_LOGE(EPD_TAG, "Self-test frame transfer failed: %s",
                 esp_err_to_name(display_error));
    }

    printf("Sleeping...\n");
    EPD_7IN3E_Sleep();

    printf("Exiting...\r\n");
    EPD_Module_Exit();

    // Log
    ESP_LOGI(EPD_TAG, "Epaper peripheral driver initialized");

    ESP_LOGI(EPD_TAG, "E-paper self-test completed; deleting task");
    vTaskDelete(NULL);
}

void EPD_SPI_WriteByte(uint8_t value)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &value,
        .rx_buffer = NULL,
    };
    // Polling transmit: no ISR/queue hop, ~2 us overhead for single bytes.
    spi_device_polling_transmit(spi_handle, &t);
}

// Bulk write: len bytes in one CS frame (caller asserts CS). Chunks at the
// S3 DMA cap (EPD_SPI_MAX_TRANSFER) — 192 KB frame = 6 transactions, not 192,000.
esp_err_t EPD_SPI_WriteData(const uint8_t *data, size_t len)
{
    const bool is_frame = len == EPD_FRAME_BYTES;
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    if (is_frame &&
        (psa_crypto_init() != PSA_SUCCESS ||
         psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS)) {
        psa_hash_abort(&sha);
        ESP_LOGE(EPD_TAG, "Could not initialize SPI frame verification hash");
        return ESP_FAIL;
    }

    const size_t original_len = len;
    while (len > 0) {
        size_t chunk = (len > EPD_SPI_MAX_TRANSFER) ? EPD_SPI_MAX_TRANSFER : len;
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = data,
            .rx_buffer = NULL,
        };
        esp_err_t ret = spi_device_polling_transmit(spi_handle, &t);
        if (ret != ESP_OK) {
            if (is_frame) {
                psa_hash_abort(&sha);
            }
            return ret;
        }
        if (is_frame && psa_hash_update(&sha, data, chunk) != PSA_SUCCESS) {
            psa_hash_abort(&sha);
            return ESP_FAIL;
        }
        data += chunk;
        len -= chunk;
    }

    if (is_frame) {
        uint8_t digest[32];
        size_t digest_len = 0U;
        if (psa_hash_finish(&sha, digest, sizeof(digest), &digest_len) != PSA_SUCCESS ||
            digest_len != sizeof(digest)) {
            psa_hash_abort(&sha);
            return ESP_FAIL;
        }

        static const char hex[] = "0123456789abcdef";
        char digest_hex[sizeof(digest) * 2U + 1U];
        for (size_t i = 0; i < sizeof(digest); ++i) {
            digest_hex[i * 2U] = hex[digest[i] >> 4];
            digest_hex[i * 2U + 1U] = hex[digest[i] & 0x0fU];
        }
        digest_hex[sizeof(digest) * 2U] = '\0';
        ESP_LOGI(EPD_TAG,
                 "Submitted SPI frame: %u bytes across DMA transactions, SHA-256 %s",
                 (unsigned)original_len, digest_hex);
    }
    return ESP_OK;
}

int EPD_Module_Init(void)
{
    EPD_Digital_Write(EPD_DC_PIN, 0);
    EPD_Digital_Write(EPD_CS_PIN, 0);
	EPD_Digital_Write(EPD_PWR_PIN, 1);
    EPD_Digital_Write(EPD_RST_PIN, 1);
    return 0;
}

void EPD_Module_Exit(void)
{
    EPD_Digital_Write(EPD_DC_PIN, 0);
    EPD_Digital_Write(EPD_CS_PIN, 0);

    //close 5V
	EPD_Digital_Write(EPD_PWR_PIN, 0);
    EPD_Digital_Write(EPD_RST_PIN, 0);
}

