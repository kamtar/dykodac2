#pragma once

#include <cstdint>
#include "audio/format_values.h"

namespace config::product {
inline constexpr std::uint8_t channels = DYKODAC_CHANNELS;
inline constexpr std::uint8_t container_bits = DYKODAC_SUBSLOT_BYTES * 8U;
inline constexpr std::uint8_t valid_bits = DYKODAC_VALID_BITS;
inline constexpr std::uint32_t initial_sample_rate_hz = DYKODAC_RATE_44K1;
} // namespace config::product
