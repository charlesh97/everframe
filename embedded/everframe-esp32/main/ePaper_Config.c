#include "ePaper_Config.h"
#include "esp_err.h"

// SPI device handle
spi_device_handle_t spi_handle = NULL;

void EPD_Task(void *pvParameter)
{
    // Setup all the GPIO outputs (RST, CS, DC, PWR)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << EPD_RST_PIN) | (1ULL << EPD_CS_PIN) | (1ULL << EPD_DC_PIN) | (1ULL << EPD_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Configure BUSY pin as input
    gpio_config_t busy_conf = {
        .pin_bit_mask = (1ULL << EPD_BUSY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy_conf);

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_MOSI_PIN,
        .miso_io_num = EPD_MISO_PIN,  // May not be needed for e-paper, but included for completeness
        .sclk_io_num = EPD_CLK_PIN,
        .quadwp_io_num = -1,  // Not used
        .quadhd_io_num = -1,  // Not used
        .max_transfer_sz = 1,  // Max size for 480x800 image (192000 bytes)
    };

    // Initialize SPI bus (SPI2_HOST is typically used for general purpose SPI)
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(EPD_TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return;
    }

    // Configure SPI device for e-paper display
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,  // 1 MHz clock speed
        .mode = 0,  // SPI_MODE_0 (CPOL=0, CPHA=0)
        .spics_io_num = EPD_CS_PIN,
        .queue_size = 1,
        .flags = 0,
        .pre_cb = NULL,
    };

    // Add device to SPI bus
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(EPD_TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(EPD_TAG, "SPI bus initialized successfully");

    // Display image data
    EPD_Module_Init();

    printf("e-Paper Init and Clear...\r\n");
    EPD_7IN3F_Init();
    EPD_7IN3F_Clear(EPD_7IN3F_WHITE); // WHITE 
    vTaskDelay(pdMS_TO_TICKS(1000));

    printf("Displaying image data...\r\n");
    EPD_7IN3F_Display(gImage_7in3f);

    printf("Sleeping...\r\n");
    EPD_7IN3F_Sleep();

    printf("Exiting...\r\n");
    EPD_Module_Exit();

    // Log
    ESP_LOGI(EPD_TAG, "Epaper peripheral driver initialized");

    // while (1)
    // {
    //     // Wait for image data from queue
    //     uint32_t *image_data = NULL;
    //     if (xQueueReceive(xQueue, &image_data, portMAX_DELAY) == pdPASS) {
    //         // Display image data
    //         EPD_Module_Init();

    //         printf("e-Paper Init and Clear...\r\n");
	//         EPD_7IN3F_Init();
    //         EPD_7IN3F_Clear(EPD_7IN3F_WHITE); // WHITE 
    //         vTaskDelay(pdMS_TO_TICKS(1000));

    //         printf("Displaying image data...\r\n");
    //         EPD_7IN3F_Display(image_data);
    //         printf("Sleeping...\r\n");
    //         EPD_7IN3F_Sleep();

    //         printf("Exiting...\r\n");
    //         EPD_Module_Exit();
    //     }
    // }
}

void EPD_SPI_WriteByte(uint8_t value)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &value,
        .rx_buffer = NULL,
    };
    spi_device_transmit(spi_handle, &t);
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

