#ifndef _EPAPER_CONFIG_H_
#define _EPAPER_CONFIG_H_

#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "EPD_7in3e.h"

#define EPD_TAG "EPD"


/* Function Prototypes */
void EPD_Task(void *pvParameter);

/**
 * e-Paper GPIO
**/
#define EPD_RST_PIN     GPIO_NUM_6
#define EPD_BUSY_PIN    GPIO_NUM_5
#define EPD_CS_PIN      GPIO_NUM_43
#define EPD_DC_PIN      GPIO_NUM_44
#define EPD_PWR_PIN     GPIO_NUM_4

/**
 * SPI GPIO Pins
**/
#define EPD_MOSI_PIN    GPIO_NUM_9   // SPI MOSI (D10 on XIAO ESP32S3)
#define EPD_MISO_PIN    GPIO_NUM_8   // SPI MISO (D9 on XIAO ESP32S3) - may not be needed for e-paper
#define EPD_CLK_PIN     GPIO_NUM_7   // SPI Clock (D8 on XIAO ESP32S3)

/**
 * GPIO read and write
**/
#define EPD_Digital_Write(_pin, _value) gpio_set_level(_pin, _value);
#define EPD_Digital_Read(_pin) gpio_get_level(_pin)

/**
 * SPI handle
**/
extern spi_device_handle_t spi_handle;

/**
 * SPI transaction size cap
 * ESP32-S3 DMA: SPI_LL_DMA_MAX_BIT_LEN = 1<<18 bits = 32768 B per transaction.
 * Bulk writes chunk at this size; CS is framed by the caller around the whole burst.
**/
#define EPD_SPI_MAX_TRANSFER  32768

/**
 * delay x ms
**/

#define EPD_Delay_ms(__xms) vTaskDelay(pdMS_TO_TICKS(__xms));

void EPD_SPI_WriteByte(uint8_t value);
esp_err_t EPD_SPI_WriteData(const uint8_t *data, size_t len);

int EPD_Module_Init(void);
void EPD_Module_Exit(void);
#endif
