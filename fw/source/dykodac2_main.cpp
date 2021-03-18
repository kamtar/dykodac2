/*
 * Copyright 2016-2020 NXP
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of NXP Semiconductor, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
 
/**
 * @file    MIMXRT1011xxxxx_Project.cpp
 * @brief   Application entry point.
 */
#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "MIMXRT1011.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "BoardManager.hpp"
#include "config_global.h"
#include "fsl_sai.h"
#include "fsl_gpt.h"
#include "LedManager.hpp"
#include "usb/audio/uac2/include/UAC2_structs.hpp"
BoardManager m_board;

TaskBase* tasks[]	//array of pointers to all objects which implement TaskBase
				{
					&m_board,
				};

const size_t tasks_size = sizeof(tasks)/sizeof(tasks[0]);

volatile uint32_t g_systickCounter;

extern "C" void SysTick_Handler(void)
{
	for(uint8_t i=0; i<tasks_size; i++)
	{
		tasks[i]->Tick_ms(1);
	}

    if (g_systickCounter != 0U)
    {
        g_systickCounter--;
    }
}

void SysTick_DelayTicks(uint32_t n)
{
    g_systickCounter = n;
    while (g_systickCounter != 0U)
    {
    }
}


int main(void)
{

	sai_transceiver_t config;

    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
    BOARD_InitDebugConsole();

	CLOCK_EnableClock(kCLOCK_Iomuxc);
	NVIC_EnableIRQ(SysTick_IRQn);
	SysTick_Config(SystemCoreClock / 1000U);

	m_board.Init();
	m_board.Set_OutputSafety(false);
	m_board.m_leds.set_led(Green, 100, 1000);


	m_board.Set_InitLevel(BoardInitLevel::AnalogInit);
	while(m_board.isBusy());

	IOMUXC_SetSaiMClkClockSource(IOMUXC_GPR, kIOMUXC_GPR_SAI1MClk1Sel, 3);
	SAI_Init(SAI1);
	SAI_GetClassicI2SConfig(&config, kSAI_WordWidth32bits, kSAI_Stereo, 1);
	SAI_TxSetConfig(SAI1, &config);
	SAI_TxSetBitClockRate(SAI1, 24576000, 48000, 32, 2);

	uint32_t val  = SAI1->TCR2 & (~I2S_TCR2_MSEL_MASK);
	SAI1->TCR2 = (val | I2S_TCR2_MSEL(kSAI_BclkSourceMclkOption1));
	SAI_TxEnable(SAI1, true);

	m_board.Set_InitLevel(BoardInitLevel::AnalogReady);
	while(m_board.isBusy());

	int dat = 0;
    volatile int i =0;
    while(1) {

    	if(i>20000){
    		if(dat > 0)
    			dat = -4000000;
    		else
    			dat = 4000000;
    	i = 0;
    	}

        SAI_WriteData(SAI1, 0, ((uint32_t)dat)<<11);
        SAI_WriteData(SAI1, 1, ((uint32_t)dat)<<11);
        i++;

    }
    return 0 ;
}
