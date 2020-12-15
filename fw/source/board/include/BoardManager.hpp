/*
 * BoardManager.hpp
 *
 *  Created on: 11 Dec 2020
 *      Author: Kamtar
 */

#ifndef BOARD_BOARDMANAGER_HPP_
#define BOARD_BOARDMANAGER_HPP_

#include "PinManager.hpp"
#include "TaskBase.hpp"
#include "LedManager.hpp"

enum BoardInitLevel
{
	Off = 0,
	DigitalInit = 1,
	AnalogPowerOn = 2,
	AnalogInit = 3,
	AnalogReady = 4
};

class BoardManager : public TaskBase {
	static constexpr int AnalogPower_OnDelay = 10; //Delay for switching on analog power

public:
	BoardManager();

	void Set_InitLevel(BoardInitLevel level); //Gradual init of a board
	void Set_OutputSafety(bool safe); //Sets if audio output of a board is ready

	/* TaskBase start */
	virtual bool Init() override;
	virtual void Tick_ms(uint32_t ms_elapssed) override;
	virtual bool Task() override;
	/* TaskBase end */

	inline bool isBusy() { return (isWaiting() || m_desired_level!=m_init_level);}

	PinManager m_pins;
	LedManager	m_leds;

private:

	inline bool isWaiting() {  return (m_init_block_timer!=0); }

	BoardInitLevel m_init_level;
	BoardInitLevel m_desired_level;
	uint8_t m_init_block_timer;
};

#endif /* BOARD_BOARDMANAGER_HPP_ */
