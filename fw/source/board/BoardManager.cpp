/*
 * BoardManager.cpp
 *
 *  Created on: 11 Dec 2020
 *      Author: Kamtar
 */

#include <BoardManager.hpp>

BoardManager::BoardManager() :
	m_init_level(Off),
	m_desired_level(Off),
	m_pins(),
	m_leds(m_pins.LED_G, m_pins.LED_R)
{
	// TODO Auto-generated constructor stub

}

bool BoardManager::Init()
{
	m_pins.Init();
	m_leds.Init();
	return true;
}

void BoardManager::Tick_ms(uint32_t ms_elapssed)
{
	m_leds.Tick_ms(ms_elapssed);

	if(m_init_block_timer > ms_elapssed)
		m_init_block_timer -= ms_elapssed;
	else
		m_init_block_timer = 0;
}

bool BoardManager::Task()
{
	if(m_desired_level > m_init_level)
	{
		if(isWaiting() == false)
		{
			Set_InitLevel((BoardInitLevel)(m_init_level+1));
		}
	}
}

void BoardManager::Set_InitLevel(BoardInitLevel level)
{
	if( level < m_init_level || isWaiting() == true )
	{	//forbidden states
		assert(false);
		return;
	}

	if(level == m_init_level)
		return;

	assert(level > m_init_level);

	BoardInitLevel _level = (BoardInitLevel)(m_init_level+1);	//We can increase init level only by one

	if (level > m_desired_level)
		m_desired_level = level; //If caller wanted higher level save it for later to be handled by Task()

	switch(_level)
	{
	case DigitalInit:
		break;
	case AnalogPowerOn:
		m_pins.DCDC_EN.Write(High);
		m_init_block_timer = AnalogPower_OnDelay;
		break;
	case AnalogInit:
		m_pins.OSC_24MHZ_ON.Write(High);
		m_pins.DAC_RST.Write(Low);
		break;
	case AnalogReady:
		m_pins.DAC_RST.Write(High);
		break;
	default:
		assert(false);
		return;
	}

	m_init_level = _level;
}

void BoardManager::Set_OutputSafety(bool safe)
{
	if(safe == true)
	{
		m_pins.OUTPUT_RELAY.Write(High);
	}else{
		m_pins.OUTPUT_RELAY.Write(Low);
	}
}

