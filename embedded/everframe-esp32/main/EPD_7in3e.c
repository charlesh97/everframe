/*****************************************************************************
* | File        :   EPD_7in3e.c
* | Author      :   Waveshare team
* | Function    :   7.3inch e-Paper (E) Driver
* | Info        :
*----------------
* | This version:   V1.0
* | Date        :   2022-10-20
* | Info        :
* -----------------------------------------------------------------------------
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "EPD_7in3e.h"

#define EPD_BUSY_TIMEOUT_MS 60000U

/******************************************************************************
function :  Software reset
parameter:
******************************************************************************/
static void EPD_7IN3E_Reset(void)
{
    EPD_Digital_Write(EPD_RST_PIN, 1);
    EPD_Delay_ms(20);
    EPD_Digital_Write(EPD_RST_PIN, 0);
    EPD_Delay_ms(2);
    EPD_Digital_Write(EPD_RST_PIN, 1);
    EPD_Delay_ms(20);
}

/******************************************************************************
function :  send command
parameter:
     Reg : Command register
******************************************************************************/
static void EPD_7IN3E_SendCommand(uint8_t Reg)
{
    EPD_Digital_Write(EPD_DC_PIN, 0);
    EPD_Digital_Write(EPD_CS_PIN, 0);
    EPD_SPI_WriteByte(Reg);
    EPD_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :  send data
parameter:
    Data : Write data
******************************************************************************/
static void EPD_7IN3E_SendData(uint8_t Data)
{
    EPD_Digital_Write(EPD_DC_PIN, 1);
    EPD_Digital_Write(EPD_CS_PIN, 0);
    EPD_SPI_WriteByte(Data);
    EPD_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :  send a buffer of data bytes with CS held low for the whole burst
parameter:
    Data : buffer pointer
    Len  : byte count
******************************************************************************/
static esp_err_t EPD_7IN3E_SendDataN(const uint8_t *Data, uint32_t Len)
{
    EPD_Digital_Write(EPD_DC_PIN, 1);
    EPD_Digital_Write(EPD_CS_PIN, 0);
    esp_err_t error = EPD_SPI_WriteData(Data, Len);
    EPD_Digital_Write(EPD_CS_PIN, 1);
    return error;
}

/******************************************************************************
function :  Wait until the busy_pin goes LOW
parameter:
******************************************************************************/
static esp_err_t EPD_7IN3E_ReadBusyH(void)
{
    ESP_LOGI(EPD_TAG, "e-Paper busy H\r\n");
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(EPD_BUSY_TIMEOUT_MS);
    while(!EPD_Digital_Read(EPD_BUSY_PIN)) {      //LOW: busy, HIGH: idle
        if (xTaskGetTickCount() - start >= timeout) {
            ESP_LOGE(EPD_TAG, "e-Paper BUSY remained low for %u ms",
                     (unsigned)EPD_BUSY_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        EPD_Delay_ms(1);
    }
    ESP_LOGI(EPD_TAG, "e-Paper busy H release\r\n");
    return ESP_OK;
}

/******************************************************************************
function :  Turn On Display
parameter:
******************************************************************************/
static esp_err_t EPD_7IN3E_TurnOnDisplay(void)
{
    
    EPD_7IN3E_SendCommand(0x04); // POWER_ON
    esp_err_t error = EPD_7IN3E_ReadBusyH();
    if (error != ESP_OK) {
        return error;
    }

    //Second setting 
    EPD_7IN3E_SendCommand(0x06);
    EPD_7IN3E_SendData(0x6F);
    EPD_7IN3E_SendData(0x1F);
    EPD_7IN3E_SendData(0x17);
    EPD_7IN3E_SendData(0x49);

    EPD_7IN3E_SendCommand(0x12); // DISPLAY_REFRESH
    EPD_7IN3E_SendData(0x00);
    error = EPD_7IN3E_ReadBusyH();
    if (error != ESP_OK) {
        return error;
    }
    
    EPD_7IN3E_SendCommand(0x02); // POWER_OFF
    EPD_7IN3E_SendData(0X00);
    return EPD_7IN3E_ReadBusyH();
}

/******************************************************************************
function :  Initialize the e-Paper register
parameter:
******************************************************************************/
esp_err_t EPD_7IN3E_Init(void)
{
    EPD_7IN3E_Reset();
    esp_err_t error = EPD_7IN3E_ReadBusyH();
    if (error != ESP_OK) {
        return error;
    }
    EPD_Delay_ms(30);

    EPD_7IN3E_SendCommand(0xAA);    // CMDH
    EPD_7IN3E_SendData(0x49);
    EPD_7IN3E_SendData(0x55);
    EPD_7IN3E_SendData(0x20);
    EPD_7IN3E_SendData(0x08);
    EPD_7IN3E_SendData(0x09);
    EPD_7IN3E_SendData(0x18);

    EPD_7IN3E_SendCommand(0x01);//
    EPD_7IN3E_SendData(0x3F);

    EPD_7IN3E_SendCommand(0x00);  
    EPD_7IN3E_SendData(0x5F);
    EPD_7IN3E_SendData(0x69);

    EPD_7IN3E_SendCommand(0x03);
    EPD_7IN3E_SendData(0x00);
    EPD_7IN3E_SendData(0x54);
    EPD_7IN3E_SendData(0x00);
    EPD_7IN3E_SendData(0x44); 

    EPD_7IN3E_SendCommand(0x05);
    EPD_7IN3E_SendData(0x40);
    EPD_7IN3E_SendData(0x1F);
    EPD_7IN3E_SendData(0x1F);
    EPD_7IN3E_SendData(0x2C);

    EPD_7IN3E_SendCommand(0x06);
    EPD_7IN3E_SendData(0x6F);
    EPD_7IN3E_SendData(0x1F);
    EPD_7IN3E_SendData(0x17);
    EPD_7IN3E_SendData(0x49);

    EPD_7IN3E_SendCommand(0x08);
    EPD_7IN3E_SendData(0x6F);
    EPD_7IN3E_SendData(0x1F);
    EPD_7IN3E_SendData(0x1F);
    EPD_7IN3E_SendData(0x22);

    EPD_7IN3E_SendCommand(0x30);
    EPD_7IN3E_SendData(0x03);
    
    EPD_7IN3E_SendCommand(0x50);
    EPD_7IN3E_SendData(0x3F);

    EPD_7IN3E_SendCommand(0x60);
    EPD_7IN3E_SendData(0x02);
    EPD_7IN3E_SendData(0x00);

    EPD_7IN3E_SendCommand(0x61);
    EPD_7IN3E_SendData(0x03);
    EPD_7IN3E_SendData(0x20);
    EPD_7IN3E_SendData(0x01); 
    EPD_7IN3E_SendData(0xE0);

    EPD_7IN3E_SendCommand(0x84);
    EPD_7IN3E_SendData(0x01);

    EPD_7IN3E_SendCommand(0xE3);
    EPD_7IN3E_SendData(0x2F);

    EPD_7IN3E_SendCommand(0x04);     //PWR on  
    return EPD_7IN3E_ReadBusyH(); //waiting for the electronic paper IC to release the idle signal

}

/******************************************************************************
function :  Clear screen
parameter:
******************************************************************************/
void EPD_7IN3E_Clear(uint8_t color)
{
    // 192,000 bytes of (color<<4)|color, sent in 4 KB stack chunks with
    // CS held low for the entire burst (6 DMA transactions, ~80 ms @ 20 MHz).
    uint32_t Total = (uint32_t)EPD_7IN3E_WIDTH * EPD_7IN3E_HEIGHT / 2;
    uint8_t chunk[4096];
    memset(chunk, (color << 4) | color, sizeof(chunk));

    EPD_7IN3E_SendCommand(0x10);
    EPD_Digital_Write(EPD_DC_PIN, 1);
    EPD_Digital_Write(EPD_CS_PIN, 0);
    while (Total > 0) {
        uint32_t n = (Total > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : Total;
        EPD_SPI_WriteData(chunk, n);
        Total -= n;
    }
    EPD_Digital_Write(EPD_CS_PIN, 1);

    (void)EPD_7IN3E_TurnOnDisplay();
}

/******************************************************************************
function :  show 7 kind of color block
parameter:
******************************************************************************/
void EPD_7IN3E_Show7Block(void)
{
    uint8_t i;
    unsigned long j;
    unsigned char const Color_seven[6] = 
    {EPD_7IN3E_BLACK, EPD_7IN3E_YELLOW, EPD_7IN3E_RED, EPD_7IN3E_BLUE, EPD_7IN3E_GREEN, EPD_7IN3E_WHITE};
    uint8_t chunk[4096];

    EPD_7IN3E_SendCommand(0x10);
    EPD_Digital_Write(EPD_DC_PIN, 1);
    EPD_Digital_Write(EPD_CS_PIN, 0);
    for (i = 0; i < 6; i++) {
        memset(chunk, (Color_seven[i] << 4) | Color_seven[i], sizeof(chunk));
        for (j = 0; j < 20000; j += sizeof(chunk)) {
            uint32_t n = (20000 - j) > sizeof(chunk) ? (uint32_t)sizeof(chunk) : (uint32_t)(20000 - j);
            EPD_SPI_WriteData(chunk, n);
        }
    }
    EPD_Digital_Write(EPD_CS_PIN, 1);
    (void)EPD_7IN3E_TurnOnDisplay();
}

void EPD_7IN3E_Show(void)
{
    unsigned long k,o;
    unsigned char const Color_seven[6] = 
    {EPD_7IN3E_BLACK, EPD_7IN3E_YELLOW, EPD_7IN3E_RED, EPD_7IN3E_BLUE, EPD_7IN3E_GREEN, EPD_7IN3E_WHITE};

    uint16_t Width, Height;
    Width = (EPD_7IN3E_WIDTH % 2 == 0)? (EPD_7IN3E_WIDTH / 2 ): (EPD_7IN3E_WIDTH / 2 + 1);
    Height = EPD_7IN3E_HEIGHT;
    k = 0;
    o = 0;

    EPD_7IN3E_SendCommand(0x10);
    for (uint16_t j = 0; j < Height; j++) {
        if((j > 10) && (j<50))
        for (uint16_t i = 0; i < Width; i++) {
                EPD_7IN3E_SendData((Color_seven[0]<<4) |Color_seven[0]);
            }
        else if(o < Height/2)
        for (uint16_t i = 0; i < Width; i++) {
                EPD_7IN3E_SendData((Color_seven[0]<<4) |Color_seven[0]);
            }
        
        else
        {
            for (uint16_t i = 0; i < Width; i++) {
                EPD_7IN3E_SendData((Color_seven[k]<<4) |Color_seven[k]);
                
            }
            k++ ;
            if(k >= 6)
                k = 0;
        }
            
        o++ ;
        if(o >= Height)
            o = 0;
    }
    (void)EPD_7IN3E_TurnOnDisplay();
}

/******************************************************************************
function :  Sends the image buffer in RAM to e-Paper and displays
parameter:
******************************************************************************/
esp_err_t EPD_7IN3E_Display(const uint8_t *Image)
{
    EPD_7IN3E_SendCommand(0x10);
    esp_err_t error = EPD_7IN3E_SendDataN(
        Image, (uint32_t)EPD_7IN3E_WIDTH * EPD_7IN3E_HEIGHT / 2);
    if (error != ESP_OK) {
        ESP_LOGE(EPD_TAG, "Frame SPI transfer failed: %s", esp_err_to_name(error));
        return error;
    }
    return EPD_7IN3E_TurnOnDisplay();
}

/******************************************************************************
function :  Send an embedded frame one scanline at a time with pacing
parameter:
    Image        : exact 192 KB native panel frame
    image_len    : must equal the native frame size
    image_rows   : number of source rows to send; remaining rows are white
    source_row_step : 1 sends every source row; 2 sends alternating rows
    row_delay_ms : delay after each 400-byte scanline
    active_x_start/end : native pixel columns copied from Image; others white
******************************************************************************/
esp_err_t EPD_7IN3E_DisplayRowPaced(const uint8_t *Image, size_t image_len,
                                    uint16_t image_rows,
                                    uint8_t source_row_step,
                                    uint32_t row_delay_ms,
                                    uint16_t active_x_start,
                                    uint16_t active_x_end)
{
    const size_t row_bytes = EPD_7IN3E_WIDTH / 2U;
    const size_t frame_bytes = row_bytes * EPD_7IN3E_HEIGHT;
    uint8_t row[EPD_7IN3E_WIDTH / 2U];

    if (Image == NULL || image_len != frame_bytes ||
        image_rows > EPD_7IN3E_HEIGHT || source_row_step == 0U ||
        active_x_start >= active_x_end || active_x_end > EPD_7IN3E_WIDTH ||
        (active_x_start % 2U) != 0U || (active_x_end % 2U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGW(EPD_TAG,
             "Row-paced test: %u rows, step %u, %lu ms/row, native x [%u,%u)",
             (unsigned)image_rows,
             (unsigned)source_row_step,
             (unsigned long)row_delay_ms,
             (unsigned)active_x_start,
             (unsigned)active_x_end);

    EPD_7IN3E_SendCommand(0x10);
    for (uint16_t y = 0; y < EPD_7IN3E_HEIGHT; ++y) {
        memset(row, (EPD_7IN3E_WHITE << 4) | EPD_7IN3E_WHITE, row_bytes);
        if (y < image_rows && (y % source_row_step) == 0U) {
            const size_t first_byte = active_x_start / 2U;
            const size_t active_bytes = (active_x_end - active_x_start) / 2U;
            memcpy(row + first_byte,
                   Image + ((size_t)y * row_bytes) + first_byte,
                   active_bytes);
        }

        esp_err_t error = EPD_7IN3E_SendDataN(row, row_bytes);
        if (error != ESP_OK) {
            ESP_LOGE(EPD_TAG, "Slow transfer failed at row %u: %s",
                     (unsigned)y, esp_err_to_name(error));
            return error;
        }
        if (((y + 1U) % 40U) == 0U || y + 1U == EPD_7IN3E_HEIGHT) {
            ESP_LOGI(EPD_TAG, "Slow transfer: %u/%u rows submitted",
                     (unsigned)(y + 1U), (unsigned)EPD_7IN3E_HEIGHT);
        }
        if (row_delay_ms > 0U) {
            EPD_Delay_ms(row_delay_ms);
        }
    }

    ESP_LOGI(EPD_TAG, "Row-paced transfer complete; refreshing panel");
    return EPD_7IN3E_TurnOnDisplay();
}

/******************************************************************************
function :  Enter sleep mode
parameter:
******************************************************************************/
esp_err_t EPD_7IN3E_Sleep(void)
{
    EPD_7IN3E_SendCommand(0X02); // DEEP_SLEEP
    EPD_7IN3E_SendData(0x00);
    esp_err_t error = EPD_7IN3E_ReadBusyH();
    if (error != ESP_OK) {
        return error;
    }

    EPD_7IN3E_SendCommand(0x07); // DEEP_SLEEP
    EPD_7IN3E_SendData(0XA5);
    return ESP_OK;
}
