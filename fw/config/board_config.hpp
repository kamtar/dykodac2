#pragma once

#include <cstdint>

namespace config::board {

// These values intentionally reproduce the 2022 firmware. They remain
// provisional until polarity is measured on the assembled board.
inline constexpr bool relay_disconnected_level = false;
inline constexpr bool relay_connected_level = true;
inline constexpr bool oscillator_44k1_level = false;
inline constexpr bool oscillator_48k_level = true;
inline constexpr bool dac_reset_asserted_level = false;
inline constexpr bool dac_chip_select_asserted_level = false;

inline constexpr std::uint32_t relay_release_us = 20'000U;
inline constexpr std::uint32_t oscillator_settle_us = 50'000U;
inline constexpr std::uint32_t dac_settle_us = 20'000U;
inline constexpr std::uint32_t stable_audio_us = 100'000U;

} // namespace config::board

