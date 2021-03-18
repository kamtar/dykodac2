/*
 * BoardManager.hpp
 *
 *  Created on: 9 Dec 2020
 *      Author: Kamtar
 */

#ifndef PINMANAGER_HPP_
#define PINMANAGER_HPP_

#include "config_global.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
#include "hw_specs.h"

class PinManager {

	static constexpr uint8_t PinLen = 16;

	enum PadSettings : uint32_t
	{
		DefIn = 0,
		DefOut = 0x10B0,
		ClkHysteresis = (1<<16),
	};


public:
	PinManager();

	void Init();

	ImxRT_Pin DCDC_EN;
	ImxRT_Pin DCDC_CLK;
	ImxRT_Pin TIMER_MCK_IN;

	ImxRT_Pin OSC_24MHZ_ON;

	ImxRT_Pin OUTPUT_RELAY;

	ImxRT_Pin LED_G;
	ImxRT_Pin LED_R;

	/* DAC PINC */
	ImxRT_Pin DAC_RST;
	ImxRT_Pin DAC_CS;
	ImxRT_Pin DAC_MOSI;
	ImxRT_Pin DAC_MISO;
	ImxRT_Pin DAC_SCK;

	ImxRT_Pin I2S_MCK_IN;
	ImxRT_Pin I2S_FSK;
	ImxRT_Pin I2S_SCK;
	ImxRT_Pin I2S_DATA;


private:
	ImxRT_Pin* _board_pins[PinLen];
};

#endif /* PINMANAGER_HPP_ */
