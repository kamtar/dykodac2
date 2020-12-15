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

enum PinDir
{
	Input = 0,
	Output = 1,
};

enum PinSt
{
	Low = 0,
	High = 1
};

struct ImxRT_Pin
{
	GPIO_Type* base;
	uint8_t pin_num;
	PinDir direction;
	PinSt default_state;
/* --- UGLY TRASH DATA FOR IOMUXC_SetPinMux --*/
	uint32_t muxRegister;
	uint32_t muxMode;
	uint32_t inputRegister;
	uint32_t inputDaisy;
	uint32_t configRegister;

	uint32_t cfgVal;

	inline void Write(PinSt st) { assert(base!=nullptr); GPIO_PinWrite(base, pin_num, st); };
	inline void Toggle() { assert(base!=nullptr); GPIO_PortToggle(base, 1<<pin_num); };
	inline bool Read() { assert(base!=nullptr); return GPIO_PinRead(base, pin_num); };
};

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
