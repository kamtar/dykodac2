#pragma once

#include "config/board_config.hpp"

#include <cmath>
#include <cstdint>

namespace audio {
// Host volume range accepted by the MCU gain, in UAC2 1/256 dB units. It
// matches the range reported to the host (-127 dB .. 0 dB).
inline constexpr std::int16_t minimum_volume_db_8_8 = -127 * 256;
inline constexpr std::int16_t maximum_volume_db_8_8 = 0;

inline std::int16_t clamp_volume_db_8_8(std::int16_t db_8_8) noexcept {
    if (db_8_8 > maximum_volume_db_8_8) return maximum_volume_db_8_8;
    if (db_8_8 < minimum_volume_db_8_8) return minimum_volume_db_8_8;
    return db_8_8;
}

// Linear MCU gain for a host volume: 10^((db + mcu_headroom_db) / 20).
// Control-rate float math in main context only.
inline float mcu_gain_from_uac2_db(std::int16_t db_8_8) noexcept {
    const auto total_8_8 = static_cast<std::int32_t>(clamp_volume_db_8_8(db_8_8)) +
                           config::board::mcu_headroom_db_8_8;
    const float db = static_cast<float>(total_8_8) * (1.0F / 256.0F);
    // 10^(x/20) = e^(x * ln(10) / 20)
    return std::exp(db * 0.115129254649702284F);
}
} // namespace audio
