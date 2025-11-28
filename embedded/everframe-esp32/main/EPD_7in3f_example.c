/*****************************************************************************
* | File        :   EPD_7in3f_example.c
* | Author      :   Waveshare team
* | Function    :   7.3inch e-Paper (F) Demo
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
#include "EPD_7in3f.h"
#include "ePaper_Config.h"
#include "GUI_Paint.h"
#include <stdlib.h> // malloc() free()

int EPD_test(void)
{
	printf("EPD_7IN3F_test Demo\r\n");
	if(DEV_Module_Init()!=0){
		return -1;
	}

	printf("e-Paper Init and Clear...\r\n");
	EPD_7IN3F_Init();

	EPD_7IN3F_Clear(EPD_7IN3F_WHITE); // WHITE 
	DEV_Delay_ms(1000);

	//Create a new image cache
	uint8_t *BlackImage;
	uint32_t Imagesize = ((EPD_7IN3F_WIDTH % 2 == 0)? (EPD_7IN3F_WIDTH / 2 ): (EPD_7IN3F_WIDTH / 2 + 1)) * EPD_7IN3F_HEIGHT;
	if((BlackImage = (uint8_t *)malloc(Imagesize/4)) == NULL) {
		printf("Failed to apply for block memory...\r\n");
		return -1;
	}
	printf("Paint_NewImage\r\n");
	Paint_NewImage(BlackImage, EPD_7IN3F_WIDTH/2, EPD_7IN3F_HEIGHT/2, 0, EPD_7IN3F_WHITE);
	Paint_SetScale(7);

	EPD_7IN3F_Display(gImage_7in3f);
	DEV_Delay_ms(5000); 

	printf("Goto Sleep...\r\n");
	EPD_7IN3F_Sleep();
	free(BlackImage);
	BlackImage = NULL;
	DEV_Delay_ms(2000); // important, at least 2s
	// close 5V
	printf("close 5V, Module enters 0 power consumption ...\r\n");
	DEV_Module_Exit();
	
	return 0;
}

