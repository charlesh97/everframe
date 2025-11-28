#ifndef _EPAPER_CONFIG_H_
#define _EPAPER_CONFIG_H_

#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "EPD_7in3f.h"

#define EPD_TAG "EPD"


/* Dummy data for testing*/
extern const unsigned char gImage_7in3f[];


/* Function Prototypes */
void EPD_Task(void *pvParameter);

/**
 * e-Paper GPIO
**/
#define EPD_RST_PIN     GPIO_NUM_6
#define EPD_BUSY_PIN    GPIO_NUM_5
#define EPD_CS_PIN      GPIO_NUM_43
#define EPD_DC_PIN      GPIO_NUM_44
#define EPD_PWR_PIN     GPIO_NUM_21

/**
 * SPI GPIO Pins
**/
#define EPD_MOSI_PIN    GPIO_NUM_11  // SPI MOSI (Master Out Slave In)
#define EPD_MISO_PIN    GPIO_NUM_13  // SPI MISO (Master In Slave Out) - may not be needed for e-paper
#define EPD_CLK_PIN     GPIO_NUM_12  // SPI Clock

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
 * delay x ms
**/

#define EPD_Delay_ms(__xms) vTaskDelay(pdMS_TO_TICKS(__xms));

void EPD_SPI_WriteByte(uint8_t value);

int EPD_Module_Init(void);
void EPD_Module_Exit(void);
#endif
