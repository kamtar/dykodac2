#pragma once

#include "audio/audio_format.hpp"
#include <cstddef>
#include <array>

namespace audio {
const FormatConfig* find_format(std::uint32_t sample_rate_hz) noexcept;
std::size_t supported_format_count() noexcept;
const FormatConfig& supported_format(std::size_t index) noexcept;
std::uint16_t usb_hs_packet_bytes(const FormatConfig& config, std::uint32_t microframe) noexcept;
} // namespace audio
