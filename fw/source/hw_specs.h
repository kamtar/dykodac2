/*
 * hw_specs.h
 *
 *  Created on: 2 Mar 2021
 *      Author: Kamtar
 */

#ifndef SOURCE_HW_SPECS_H_
#define SOURCE_HW_SPECS_H_

#include <stdio.h>
#include "fsl_gpio.h"

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

typedef ImxRT_Pin MamaGpio;


#endif /* SOURCE_HW_SPECS_H_ */
