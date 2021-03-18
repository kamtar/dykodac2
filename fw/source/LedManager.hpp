/*
 * LedManager.hpp
 *
 *  Created on: 11 Dec 2020
 *      Author: Kamtar
 */

#ifndef LEDMANAGER_HPP_
#define LEDMANAGER_HPP_
#include "TaskBase.hpp"
#include "PinManager.hpp"
#include "hw_specs.h"

enum Colour {
	Green = 0,
	Red = 1
};

enum LedSt {
	ForceOff = 0,
	ForceOn = 1,
	Normal = 2
};

struct LedBlinker
{
	LedSt st;
	uint16_t on_time;
	uint16_t off_time;
	int16_t repeats;
	uint16_t timer;
	MamaGpio& led;
};

class LedManager : public TaskBase {
public:
	LedManager(MamaGpio& green, MamaGpio& red);


	virtual bool Init();
	virtual void Tick_ms(uint32_t elapsed_ms);

	void set_led(Colour clr, uint16_t on_time_ms, uint16_t period_ms, uint16_t repeat = -1);
	void set_led(Colour clr, LedSt state);


private:

	void HandleLed(LedBlinker& led);

	LedBlinker m_green;
	LedBlinker m_red;

};

#endif /* LEDMANAGER_HPP_ */
