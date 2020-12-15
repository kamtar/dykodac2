/*
 * Copyright 2017-2020 NXP
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

/* TEXT BELOW IS USED AS SETTING FOR TOOLS *************************************
!!GlobalInfo
product: Clocks v4.0
* BE CAREFUL MODIFYING THIS COMMENT - IT IS YAML SETTINGS FOR TOOLS **********/

/**
 * @file    clock_config.c
 * @brief   Board clocks initialization file.
 */
 
/* This is a template for board specific configuration created by MCUXpresso IDE Project Wizard.*/

#include "MIMXRT1011.h"
#include "clock_config.h"
#include "fsl_clock.h"


	const clock_sys_pll_config_t sysPllConfig_BOARD_BootClockRUN =
	    {
	        .loopDivider = 1,                         /* PLL loop divider, Fout = Fin * ( 20 + loopDivider*2 + numerator / denominator ) */
	        .numerator = 0,                           /* 30 bit numerator of fractional loop divider */
	        .denominator = 1,                         /* 30 bit denominator of fractional loop divider */
	        .src = 0,                                 /* Bypass clock source, 0 - OSC 24M, 1 - CLK1_P and CLK1_N */
	    };
	const clock_usb_pll_config_t usb1PllConfig_BOARD_BootClockRUN =
	    {
	        .loopDivider = 0,                         /* PLL loop divider, Fout = Fin * 20 */
	        .src = 0,                                 /* Bypass clock source, 0 - OSC 24M, 1 - CLK1_P and CLK1_N */
	    };
	const clock_enet_pll_config_t enetPllConfig_BOARD_BootClockRUN =
	    {
	        .enableClkOutput500M = true,              /* Enable the PLL providing the ENET 500MHz reference clock */
	        .src = 0,                                 /* Bypass clock source, 0 - OSC 24M, 1 - CLK1_P and CLK1_N */
	    };
	/*******************************************************************************
	 * Code for BOARD_BootClockRUN configuration
	 ******************************************************************************/
	void BOARD_InitBootClocks(void)
	{
	    /* Init RTC OSC clock frequency. */

	    /* Enable 1MHz clock output. */
	    XTALOSC24M->OSC_CONFIG2 |= XTALOSC24M_OSC_CONFIG2_ENABLE_1M_MASK;
	    /* Use free 1MHz clock output. */
	    XTALOSC24M->OSC_CONFIG2 &= ~XTALOSC24M_OSC_CONFIG2_MUX_1M_MASK;
	    /* Set XTAL 24MHz clock frequency. */
	    CLOCK_SetXtalFreq(24000000U);
	    /* Enable XTAL 24MHz clock source. */
	    CLOCK_InitExternalClk(0);
	    /* Enable internal RC. */
	    CLOCK_InitRcOsc24M();
	    /* Switch clock source to external OSC. */
	    CLOCK_SwitchOsc(kCLOCK_XtalOsc);
	    /* Set Oscillator ready counter value. */
	    CCM->CCR = (CCM->CCR & (~CCM_CCR_OSCNT_MASK)) | CCM_CCR_OSCNT(127);
	    /* Setting PeriphClk2Mux and PeriphMux to provide stable clock before PLLs are initialed */
	    CLOCK_SetMux(kCLOCK_PeriphClk2Mux, 1); /* Set PERIPH_CLK2 MUX to OSC */
	    CLOCK_SetMux(kCLOCK_PeriphMux, 1);     /* Set PERIPH_CLK MUX to PERIPH_CLK2 */
	    /* Setting the VDD_SOC to 1.5V. It is necessary to config CORE to 500Mhz. */
	    DCDC->REG3 = (DCDC->REG3 & (~DCDC_REG3_TRG_MASK)) | DCDC_REG3_TRG(0x12);
	    /* Waiting for DCDC_STS_DC_OK bit is asserted */
	    while (DCDC_REG0_STS_DC_OK_MASK != (DCDC_REG0_STS_DC_OK_MASK & DCDC->REG0))
	    {
	    }
	    /* Set AHB_PODF. */
	    CLOCK_SetDiv(kCLOCK_AhbDiv, 0);
	    /* Disable IPG clock gate. */
	    CLOCK_DisableClock(kCLOCK_Adc1);
	    CLOCK_DisableClock(kCLOCK_Xbar1);
	    /* Set IPG_PODF. */
	    CLOCK_SetDiv(kCLOCK_IpgDiv, 3);
	    /* Disable PERCLK clock gate. */
	    CLOCK_DisableClock(kCLOCK_Gpt1);
	    CLOCK_DisableClock(kCLOCK_Gpt1S);
	    CLOCK_DisableClock(kCLOCK_Gpt2);
	    CLOCK_DisableClock(kCLOCK_Gpt2S);
	    CLOCK_DisableClock(kCLOCK_Pit);
	    /* Set PERCLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_PerclkDiv, 1);
	    /* In SDK projects, external flash (configured by FLEXSPI) will be initialized by dcd.
	     * With this macro XIP_EXTERNAL_FLASH, usb1 pll (selected to be FLEXSPI clock source in SDK projects) will be left unchanged.
	     * Note: If another clock source is selected for FLEXSPI, user may want to avoid changing that clock as well.*/
	#if !(defined(XIP_EXTERNAL_FLASH) && (XIP_EXTERNAL_FLASH == 1))
	    /* Disable Flexspi clock gate. */
	    CLOCK_DisableClock(kCLOCK_FlexSpi);
	    /* Set FLEXSPI_PODF. */
	    CLOCK_SetDiv(kCLOCK_FlexspiDiv, 3);
	    /* Set Flexspi clock source. */
	    CLOCK_SetMux(kCLOCK_FlexspiMux, 0);
	    CLOCK_SetMux(kCLOCK_FlexspiSrcMux, 0);
	#endif
	    /* Disable ADC_ACLK_EN clock gate. */
	    CCM->CSCMR2 &= ~CCM_CSCMR2_ADC_ACLK_EN_MASK;
	    /* Set ADC_ACLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_AdcDiv, 11);
	    /* Disable LPSPI clock gate. */
	    CLOCK_DisableClock(kCLOCK_Lpspi1);
	    CLOCK_DisableClock(kCLOCK_Lpspi2);
	    /* Set LPSPI_PODF. */
	    CLOCK_SetDiv(kCLOCK_LpspiDiv, 4);
	    /* Set Lpspi clock source. */
	    CLOCK_SetMux(kCLOCK_LpspiMux, 2);
	    /* Disable TRACE clock gate. */
	    CLOCK_DisableClock(kCLOCK_Trace);
	    /* Set TRACE_PODF. */
	    CLOCK_SetDiv(kCLOCK_TraceDiv, 3);
	    /* Set Trace clock source. */
	    CLOCK_SetMux(kCLOCK_TraceMux, 0);
	    /* Disable SAI1 clock gate. */
	    CLOCK_DisableClock(kCLOCK_Sai1);
	    /* Set SAI1_CLK_PRED. */
	    CLOCK_SetDiv(kCLOCK_Sai1PreDiv, 3);
	    /* Set SAI1_CLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_Sai1Div, 1);
	    /* Set Sai1 clock source. */
	    CLOCK_SetMux(kCLOCK_Sai1Mux, 0);
	    /* Disable SAI3 clock gate. */
	    CLOCK_DisableClock(kCLOCK_Sai3);
	    /* Set SAI3_CLK_PRED. */
	    CLOCK_SetDiv(kCLOCK_Sai3PreDiv, 3);
	    /* Set SAI3_CLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_Sai3Div, 1);
	    /* Set Sai3 clock source. */
	    CLOCK_SetMux(kCLOCK_Sai3Mux, 0);
	    /* Disable Lpi2c clock gate. */
	    CLOCK_DisableClock(kCLOCK_Lpi2c1);
	    CLOCK_DisableClock(kCLOCK_Lpi2c2);
	    /* Set LPI2C_CLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_Lpi2cDiv, 0);
	    /* Set Lpi2c clock source. */
	    CLOCK_SetMux(kCLOCK_Lpi2cMux, 0);
	    /* Disable UART clock gate. */
	    CLOCK_DisableClock(kCLOCK_Lpuart1);
	    CLOCK_DisableClock(kCLOCK_Lpuart2);
	    CLOCK_DisableClock(kCLOCK_Lpuart3);
	    CLOCK_DisableClock(kCLOCK_Lpuart4);
	    /* Set UART_CLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_UartDiv, 0);
	    /* Set Uart clock source. */
	    CLOCK_SetMux(kCLOCK_UartMux, 0);
	    /* Disable SPDIF clock gate. */
	    CLOCK_DisableClock(kCLOCK_Spdif);
	    /* Set SPDIF0_CLK_PRED. */
	    CLOCK_SetDiv(kCLOCK_Spdif0PreDiv, 1);
	    /* Set SPDIF0_CLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_Spdif0Div, 7);
	    /* Set Spdif clock source. */
	    CLOCK_SetMux(kCLOCK_SpdifMux, 3);
	    /* Disable Flexio1 clock gate. */
	    CLOCK_DisableClock(kCLOCK_Flexio1);
	    /* Set FLEXIO1_CLK_PRED. */
	    CLOCK_SetDiv(kCLOCK_Flexio1PreDiv, 1);
	    /* Set FLEXIO1_CLK_PODF. */
	    CLOCK_SetDiv(kCLOCK_Flexio1Div, 7);
	    /* Set Flexio1 clock source. */
	    CLOCK_SetMux(kCLOCK_Flexio1Mux, 3);
	    /* Set Pll3 sw clock source. */
	    CLOCK_SetMux(kCLOCK_Pll3SwMux, 0);
	    /* Init System PLL. */
	    CLOCK_InitSysPll(&sysPllConfig_BOARD_BootClockRUN);
	    /* Init System pfd0. */
	    CLOCK_InitSysPfd(kCLOCK_Pfd0, 27);
	    /* Init System pfd1. */
	    CLOCK_InitSysPfd(kCLOCK_Pfd1, 16);
	    /* Init System pfd2. */
	    CLOCK_InitSysPfd(kCLOCK_Pfd2, 18);
	    /* Init System pfd3. */
	    CLOCK_InitSysPfd(kCLOCK_Pfd3, 18);
	    /* In SDK projects, external flash (configured by FLEXSPI) will be initialized by dcd.
	     * With this macro XIP_EXTERNAL_FLASH, usb1 pll (selected to be FLEXSPI clock source in SDK projects) will be left unchanged.
	     * Note: If another clock source is selected for FLEXSPI, user may want to avoid changing that clock as well.*/
	#if !(defined(XIP_EXTERNAL_FLASH) && (XIP_EXTERNAL_FLASH == 1))
	    /* Init Usb1 PLL. */
	    CLOCK_InitUsb1Pll(&usb1PllConfig_BOARD_BootClockRUN);
	    /* Init Usb1 pfd0. */
	    CLOCK_InitUsb1Pfd(kCLOCK_Pfd0, 22);
	    /* Init Usb1 pfd1. */
	    CLOCK_InitUsb1Pfd(kCLOCK_Pfd1, 16);
	    /* Init Usb1 pfd2. */
	    CLOCK_InitUsb1Pfd(kCLOCK_Pfd2, 17);
	    /* Init Usb1 pfd3. */
	    CLOCK_InitUsb1Pfd(kCLOCK_Pfd3, 18);
	    /* Disable Usb1 PLL output for USBPHY1. */
	    CCM_ANALOG->PLL_USB1 &= ~CCM_ANALOG_PLL_USB1_EN_USB_CLKS_MASK;
	#endif
	    /* DeInit Audio PLL. */
	    CLOCK_DeinitAudioPll();
	    /* Bypass Audio PLL. */
	    CLOCK_SetPllBypass(CCM_ANALOG, kCLOCK_PllAudio, 1);
	    /* Set divider for Audio PLL. */
	    CCM_ANALOG->MISC2 &= ~CCM_ANALOG_MISC2_AUDIO_DIV_LSB_MASK;
	    CCM_ANALOG->MISC2 &= ~CCM_ANALOG_MISC2_AUDIO_DIV_MSB_MASK;
	    /* Enable Audio PLL output. */
	    CCM_ANALOG->PLL_AUDIO |= CCM_ANALOG_PLL_AUDIO_ENABLE_MASK;
	    /* Init Enet PLL. */
	    CLOCK_InitEnetPll(&enetPllConfig_BOARD_BootClockRUN);
	    /* Set preperiph clock source. */
	    CLOCK_SetMux(kCLOCK_PrePeriphMux, 3);
	    /* Set periph clock source. */
	    CLOCK_SetMux(kCLOCK_PeriphMux, 0);
	    /* Set periph clock2 clock source. */
	    CLOCK_SetMux(kCLOCK_PeriphClk2Mux, 0);
	    /* Set per clock source. */
	    CLOCK_SetMux(kCLOCK_PerclkMux, 0);
	    /* Set clock out1 divider. */
	    CCM->CCOSR = (CCM->CCOSR & (~CCM_CCOSR_CLKO1_DIV_MASK)) | CCM_CCOSR_CLKO1_DIV(0);
	    /* Set clock out1 source. */
	    CCM->CCOSR = (CCM->CCOSR & (~CCM_CCOSR_CLKO1_SEL_MASK)) | CCM_CCOSR_CLKO1_SEL(1);
	    /* Set clock out2 divider. */
	    CCM->CCOSR = (CCM->CCOSR & (~CCM_CCOSR_CLKO2_DIV_MASK)) | CCM_CCOSR_CLKO2_DIV(0);
	    /* Set clock out2 source. */
	    CCM->CCOSR = (CCM->CCOSR & (~CCM_CCOSR_CLKO2_SEL_MASK)) | CCM_CCOSR_CLKO2_SEL(18);
	    /* Set clock out1 drives clock out1. */
	    CCM->CCOSR &= ~CCM_CCOSR_CLK_OUT_SEL_MASK;
	    /* Disable clock out1. */
	    CCM->CCOSR &= ~CCM_CCOSR_CLKO1_EN_MASK;
	    /* Disable clock out2. */
	    CCM->CCOSR &= ~CCM_CCOSR_CLKO2_EN_MASK;

	    /* Set GPT1 High frequency reference clock source. */
	    IOMUXC_GPR->GPR5 &= ~IOMUXC_GPR_GPR5_VREF_1M_CLK_GPT1_MASK;
	    /* Set GPT2 High frequency reference clock source. */
	    IOMUXC_GPR->GPR5 &= ~IOMUXC_GPR_GPR5_VREF_1M_CLK_GPT2_MASK;
	    /* Set SystemCoreClock variable. */
	    SystemCoreClock = 500000000U;
	}

