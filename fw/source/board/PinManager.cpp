/*
 * BoardManager.cpp
 *
 *  Created on: 9 Dec 2020
 *      Author: Kamtar
 */

#include <PinManager.hpp>

PinManager::PinManager() :
		/*------- GpioPort | GpioNum | Direction | DefaultState | Mux | PadSettings --------*/
_board_pins {
	&(DCDC_EN = 		{ GPIO1, 14, PinDir::Output,  PinSt::Low, IOMUXC_GPIO_AD_00_GPIOMUX_IO14, PadSettings::DefOut }),
	&(DCDC_CLK = 		{ nullptr, 0, PinDir::Output, PinSt::Low, IOMUXC_GPIO_AD_04_GPT2_COMPARE1, PadSettings::DefOut }),
	&(TIMER_MCK_IN = 	{ nullptr, 0, PinDir::Input,  PinSt::Low, IOMUXC_GPIO_AD_03_GPT2_CLK, PadSettings::ClkHysteresis }),

	&(OSC_24MHZ_ON = 	{ GPIO1, 5, PinDir::Output, PinSt::High, IOMUXC_GPIO_05_GPIOMUX_IO05, PadSettings::DefOut }),
	&(OUTPUT_RELAY = 	{ GPIO1, 3, PinDir::Output, PinSt::Low, IOMUXC_GPIO_03_GPIOMUX_IO03, PadSettings::DefOut }),

	&(LED_G = 			{ GPIO1, 1, PinDir::Output, PinSt::Low, IOMUXC_GPIO_01_GPIOMUX_IO01, PadSettings::DefOut }),
	&(LED_R = 			{ GPIO1, 0, PinDir::Output, PinSt::Low, IOMUXC_GPIO_00_GPIOMUX_IO00, PadSettings::DefOut }),

	&(DAC_RST = 		{ GPIO1, 13, PinDir::Output, PinSt::High, IOMUXC_GPIO_13_GPIOMUX_IO13, PadSettings::DefOut }),
	&(DAC_CS = 			{ GPIO1, 12, PinDir::Output, PinSt::High, IOMUXC_GPIO_12_GPIOMUX_IO12, PadSettings::DefOut }),
	&(DAC_MOSI = 		{ GPIO1, 10, PinDir::Output, PinSt::Low, IOMUXC_GPIO_10_GPIOMUX_IO10, PadSettings::DefOut }),
	&(DAC_MISO = 		{ GPIO1, 9,  PinDir::Input,  PinSt::Low, IOMUXC_GPIO_09_GPIOMUX_IO09, PadSettings::DefIn }),
	&(DAC_SCK = 		{ GPIO1, 11, PinDir::Output, PinSt::Low, IOMUXC_GPIO_11_GPIOMUX_IO11, PadSettings::DefOut }),

	&(I2S_MCK_IN = 		{ nullptr, 0, PinDir::Input,  PinSt::Low, IOMUXC_GPIO_08_SAI1_MCLK, PadSettings::DefIn }),
	&(I2S_FSK = 		{ nullptr, 0, PinDir::Output, PinSt::Low, IOMUXC_GPIO_07_SAI1_TX_SYNC, PadSettings::DefOut }),
	&(I2S_SCK = 		{ nullptr, 0, PinDir::Output, PinSt::Low, IOMUXC_GPIO_06_SAI1_TX_BCLK, PadSettings::DefOut }),
	&(I2S_DATA = 		{ nullptr, 0, PinDir::Output, PinSt::Low, IOMUXC_GPIO_04_SAI1_TX_DATA00, PadSettings::DefOut }),
}

{
	// TODO Auto-generated constructor stub

}

void PinManager::Init()
{
	for (int i = 0; i < PinLen; i++) {
		ImxRT_Pin &p = *_board_pins[i];

		assert(p.muxRegister != 0);
		IOMUXC_SetPinMux(p.muxRegister, p.muxMode, p.inputRegister, p.inputDaisy, p.configRegister, 0);
		IOMUXC_SetPinConfig(p.muxRegister, p.muxMode, p.inputRegister, p.inputDaisy, p.configRegister, p.cfgVal);

		if (p.base != nullptr) {
			_gpio_pin_config pin = { static_cast<gpio_pin_direction_t>(p.direction), p.default_state,
					static_cast<gpio_interrupt_mode_t>(0) };
			GPIO_PinInit(p.base, p.pin_num, &pin);
		}
	}
}
