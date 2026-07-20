#pragma once

#include <cstdint>
#include "audio/format_values.h"

namespace usb::descriptor_model {
inline constexpr std::uint16_t vid = 0x1209U;
inline constexpr std::uint16_t pid = 0xD2A3U;
inline constexpr std::uint8_t channels = DYKODAC_CHANNELS;
inline constexpr std::uint8_t subslot_bytes = DYKODAC_SUBSLOT_BYTES;
inline constexpr std::uint8_t valid_bits = DYKODAC_VALID_BITS;
inline constexpr std::uint8_t data_interval = 1U;
inline constexpr std::uint8_t feedback_interval = 4U;
inline constexpr std::uint16_t maximum_packet_bytes = DYKODAC_USB_HS_MAX_PACKET;
} // namespace usb::descriptor_model
