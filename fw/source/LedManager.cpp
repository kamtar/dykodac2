/*
 * LedManager.cpp
 *
 *  Created on: 11 Dec 2020
 *      Author: Kamtar
 */

#include <LedManager.hpp>

LedManager::LedManager(ImxRT_Pin& green, ImxRT_Pin& red) :
m_green { Normal, 0, 0, 0, 0, green },
m_red { Normal, 0, 0, 0, 0, red }
{
	// TODO Auto-generated constructor stub

}

bool LedManager::Init()
{

}

void LedManager::Tick_ms(uint32_t elapsed_ms)
{
	HandleLed(m_green);
	HandleLed(m_red);
}

void LedManager::HandleLed(LedBlinker& led)
{
	switch(led.st)
	{
	case Normal:
		break;
	case ForceOff:
		led.led.Write(Low);
		return;
	case ForceOn:
		led.led.Write(High);
		return;
	}

	if(led.repeats != 0)
	{
		if(led.timer == 0 )
		{
			led.led.Toggle();

			if(led.led.Read() == true)
				led.timer  = led.on_time;
			else
				led.timer  = led.off_time;

			if(led.repeats > 0)
				led.repeats--;
		}

		led.timer--;
	} else
	{
		led.led.Write(Low);
	}
}

void LedManager::set_led(Colour clr, uint16_t on_time_ms, uint16_t period_ms, uint16_t repeat)
{
	LedBlinker* _led;

	if(clr == Green)
		_led = &m_green;
	else
		_led = &m_red;

	_led->on_time = on_time_ms;
	_led->off_time = period_ms - on_time_ms;
	_led->repeats = repeat;
}

void LedManager::set_led(Colour clr, LedSt state)
{
	LedBlinker* _led;

	if(clr == Green)
		_led = &m_green;
	else
		_led = &m_red;

	_led->st = state;
}
