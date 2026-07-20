#pragma once

#include "audio/audio_format.hpp"

namespace board::target {
void initialize_diagnostic_led_early() noexcept;
void initialize_early_safe() noexcept;
void initialize_digital() noexcept;
void initialize_dac_bus() noexcept;
void initialize_audio_pins() noexcept;
void set_relay_connected(bool connected) noexcept;
void set_dac_reset(bool asserted) noexcept;
void set_dac_chip_select(bool asserted) noexcept;
void select_clock_family(audio::ClockFamily family) noexcept;
bool clock_select_level() noexcept;
void set_red_led(bool on) noexcept;
void set_green_led(bool on) noexcept;
} // namespace board::target
