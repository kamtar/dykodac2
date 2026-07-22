#pragma once

#include <array>
#include <cstdint>

namespace board {

enum class Direction : std::uint8_t { Input, Output, Peripheral };
enum class InitPhase : std::uint8_t { EarlySafe, Digital, AnalogPower, AudioClock, Runtime };
enum class Owner : std::uint8_t { Relay, ClockManager, Indicators, Dac, Power, Audio, Monitor };

struct Pin {
    const char* net;
    const char* pad;
    std::uint8_t package_pin;
    const char* function;
    Direction direction;
    bool safe_level;
    InitPhase phase;
    Owner owner;
    const char* note;
};

namespace pins {
inline constexpr Pin output_relay{"OPTO_RELAY_JACK", "GPIO_03", 10, "GPIO1_IO03", Direction::Output, false, InitPhase::EarlySafe, Owner::Relay, "Polarity provisional: legacy low at boot, high before USB"};
inline constexpr Pin oscillator_select{"D_OSC_SELECT", "GPIO_05", 8, "GPIO1_IO05", Direction::Output, false, InitPhase::AudioClock, Owner::ClockManager, "Legacy low selects 22.5792 MHz; verify electrically"};
inline constexpr Pin green_status_led{"LED_G_ST", "GPIO_01", 12, "GPIO1_IO01", Direction::Output, false, InitPhase::Digital, Owner::Indicators, "blink pin"};
inline constexpr Pin red_error_led{"LED_R_ERR", "GPIO_00", 13, "GPIO1_IO00", Direction::Output, false, InitPhase::Digital, Owner::Indicators, "blink pin"};
inline constexpr Pin dac_reset{"D_DAC_RST", "GPIO_13", 79, "GPIO1_IO13", Direction::Output, false, InitPhase::EarlySafe, Owner::Dac, "Active low in legacy startup"};
inline constexpr Pin dac_chip_select{"D_DAC_CS", "GPIO_12", 80, "GPIO1_IO12", Direction::Output, true, InitPhase::EarlySafe, Owner::Dac, "Active low"};
inline constexpr Pin dac_spi_miso{"D_SPI_MISO", "GPIO_09", 3, "FLEXIO1_IO01", Direction::Peripheral, false, InitPhase::Digital, Owner::Dac, "Isolator may hold low and confuse ROM boot detection"};
inline constexpr Pin dac_spi_mosi{"D_SPI_MOSI", "GPIO_10", 2, "FLEXIO1_IO02", Direction::Peripheral, false, InitPhase::Digital, Owner::Dac, "FlexIO shifter 0"};
inline constexpr Pin dac_spi_clock{"D_SPI_SCK", "GPIO_11", 1, "FLEXIO1_IO03", Direction::Peripheral, false, InitPhase::Digital, Owner::Dac, "2 MHz in legacy firmware"};
inline constexpr Pin i2s_mclk{"D_MCK", "GPIO_08", 4, "SAI1_MCLK", Direction::Peripheral, false, InitPhase::AudioClock, Owner::Audio, "External oscillator input"};
inline constexpr Pin i2s_bclk{"D_I2S_SCK", "GPIO_06", 6, "SAI1_TX_BCLK", Direction::Peripheral, false, InitPhase::AudioClock, Owner::Audio, "64-bit stereo frame"};
inline constexpr Pin i2s_frame_clock{"D_I2S_FCK", "GPIO_07", 5, "SAI1_TX_SYNC", Direction::Peripheral, false, InitPhase::AudioClock, Owner::Audio, "I2S LR clock"};
inline constexpr Pin i2s_data{"D_I2S_DATA", "GPIO_04", 9, "SAI1_TX_DATA00", Direction::Peripheral, false, InitPhase::AudioClock, Owner::Audio, "Stereo output"};
inline constexpr Pin dcdc_enable{"!DCDC_EN", "GPIO_AD_00", 60, "GPIO1_IO14", Direction::Output, false, InitPhase::EarlySafe, Owner::Power, "As-built prototype hack: low during early boot, then high to enable DC/DC; schematic GPIO_AD_03/GPIO1_IO17 is not used"};
inline constexpr Pin dcdc_clock{"DCDC_CLK", "GPIO_AD_04", 56, "GPT2_COMPARE1", Direction::Peripheral, false, InitPhase::AnalogPower, Owner::Power, "Not implemented in legacy firmware"};
inline constexpr Pin mclk_monitor{"D_MCK", "GPIO_AD_05", 55, "GPT2_CAPTURE1", Direction::Input, false, InitPhase::Runtime, Owner::Monitor, "Schematic mapping; do not enable before board verification"};

inline constexpr std::array<const Pin*, 16> all{{
    &output_relay, &oscillator_select, &green_status_led, &red_error_led,
    &dac_reset, &dac_chip_select, &dac_spi_miso, &dac_spi_mosi,
    &dac_spi_clock, &i2s_mclk, &i2s_bclk, &i2s_frame_clock, &i2s_data,
    &dcdc_enable, &dcdc_clock, &mclk_monitor,
}};
} // namespace pins
} // namespace board
