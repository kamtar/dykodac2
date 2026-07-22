#include "board/target_board.hpp"

#include "config/board_config.hpp"

extern "C" {
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"
}

namespace board::target {
namespace {
constexpr std::uint32_t red_led_pin = 0U;
constexpr std::uint32_t green_led_pin = 1U;

void output(std::uint32_t mux_register, std::uint32_t mux_mode,
            std::uint32_t input_register, std::uint32_t input_daisy,
            std::uint32_t config_register, std::uint32_t pin, bool level) noexcept {
    IOMUXC_SetPinMux(mux_register, mux_mode, input_register, input_daisy, config_register, 0U);
    IOMUXC_SetPinConfig(mux_register, mux_mode, input_register, input_daisy, config_register, 0x10B0U);
    const gpio_pin_config_t config{kGPIO_DigitalOutput, static_cast<std::uint8_t>(level ? 1U : 0U), kGPIO_NoIntmode};
    GPIO_PinInit(GPIO1, pin, &config);
}

void write(std::uint32_t pin, bool level) noexcept {
    GPIO_WritePinOutput(GPIO1, pin, level ? 1U : 0U);
}
} // namespace

void initialize_diagnostic_led_early() noexcept {
    output(IOMUXC_GPIO_01_GPIOMUX_IO01, 1U, false);
}

void initialize_early_safe() noexcept {
    // Called immediately after the verified run-clock transition: establish
    // relay open and DAC reset before any peripheral or analog initialization.
    output(IOMUXC_GPIO_03_GPIOMUX_IO03, 3U, config::board::relay_disconnected_level);
    output(IOMUXC_GPIO_13_GPIOMUX_IO13, 13U, config::board::dac_reset_asserted_level);
    output(IOMUXC_GPIO_12_GPIOMUX_IO12, 12U, !config::board::dac_chip_select_asserted_level);
    // The assembled prototype reroutes !DCDC_EN to package pin 60. Keep it
    // disabled through the clock transition; main enables it once all safe
    // GPIO levels have been established.
    output(IOMUXC_GPIO_AD_00_GPIOMUX_IO14, 14U, config::board::dcdc_disabled_level);
    // Complete indicator setup only after the clock routine has returned.
    output(IOMUXC_GPIO_00_GPIOMUX_IO00, 0U, false);
    output(IOMUXC_GPIO_01_GPIOMUX_IO01, 1U, false);
}

void initialize_digital() noexcept {
    output(IOMUXC_GPIO_05_GPIOMUX_IO05, 5U, config::board::oscillator_44k1_level);
}

void initialize_dac_bus() noexcept {
    IOMUXC_SetPinMux(IOMUXC_GPIO_09_FLEXIO1_IO01, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_10_FLEXIO1_IO02, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_11_FLEXIO1_IO03, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_09_FLEXIO1_IO01, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_10_FLEXIO1_IO02, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_11_FLEXIO1_IO03, 0x10B0U);
}

void initialize_audio_pins() noexcept {
    IOMUXC_SetPinMux(IOMUXC_GPIO_04_SAI1_TX_DATA00, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_06_SAI1_TX_BCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_07_SAI1_TX_SYNC, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_08_SAI1_MCLK, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_04_SAI1_TX_DATA00, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_06_SAI1_TX_BCLK, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_07_SAI1_TX_SYNC, 0x10B0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_08_SAI1_MCLK, 0U);
}

void set_relay_connected(bool connected) noexcept {
    write(3U, connected ? config::board::relay_connected_level : config::board::relay_disconnected_level);
}
void set_dac_reset(bool asserted) noexcept {
    write(13U, asserted ? config::board::dac_reset_asserted_level : !config::board::dac_reset_asserted_level);
}
void set_dac_chip_select(bool asserted) noexcept {
    write(12U, asserted ? config::board::dac_chip_select_asserted_level : !config::board::dac_chip_select_asserted_level);
}
void set_dcdc_enabled(bool enabled) noexcept {
    write(14U, enabled ? config::board::dcdc_enabled_level : config::board::dcdc_disabled_level);
}
void select_clock_family(audio::ClockFamily family) noexcept {
    write(5U, family == audio::ClockFamily::Family48k ? config::board::oscillator_48k_level : config::board::oscillator_44k1_level);
}
bool clock_select_level() noexcept { return GPIO_ReadPinInput(GPIO1, 5U) != 0U; }
void set_red_led(bool on) noexcept { write(red_led_pin, on); }
void set_green_led(bool on) noexcept { write(green_led_pin, on); }
} // namespace board::target
