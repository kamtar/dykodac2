#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace usb::maintenance {
// Reboots into the RT1011 ROM USB serial downloader for flashing.
inline constexpr std::array<std::uint8_t, 16> enter_rom{{
    'D','Y','K','O','-','R','O','M','-','B','O','O','T','-','0','1'
}};
// Experimental variant: bytes 16..19 = little-endian ROM argument, byte 20 =
// entry method (0 reset-then-call, 1 direct call), remaining bytes zero.
inline constexpr std::array<std::uint8_t, 16> enter_rom_custom{{
    'D','Y','K','O','-','R','O','M','-','B','O','O','T','-','X','X'
}};
inline constexpr std::array<std::uint8_t, 16> get_boot_info{{
    'D','Y','K','O','-','B','O','O','T','-','I','N','F','O','-','1'
}};
inline constexpr std::array<std::uint8_t, 16> get_diagnostics{{
    'D','Y','K','O','-','D','I','A','G','-','G','E','T','-','0','1'
}};
inline constexpr std::array<std::uint8_t, 16> get_events{{
    'D','Y','K','O','-','E','V','E','N','T','-','G','E','T','-','1'
}};
inline constexpr std::array<std::uint8_t, 16> get_dma_diagnostics{{
    'D','Y','K','O','-','D','M','A','-','G','E','T','-','0','0','1'
}};
inline constexpr std::array<std::uint8_t, 16> get_refill_timing{{
    'D','Y','K','O','-','P','E','R','F','-','G','E','T','-','0','1'
}};

template <std::size_t N>
inline bool is_command(const std::uint8_t* data, std::size_t size,
                       const std::array<std::uint8_t, N>& command) noexcept {
    if (size < command.size()) return false;
    for (std::size_t i = 0; i < command.size(); ++i) if (data[i] != command[i]) return false;
    for (std::size_t i = command.size(); i < size; ++i) if (data[i] != 0U) return false;
    return true;
}
inline bool is_enter_rom(const std::uint8_t* data, std::size_t size) noexcept {
    return is_command(data, size, enter_rom);
}
inline bool is_get_boot_info(const std::uint8_t* data, std::size_t size) noexcept {
    return is_command(data, size, get_boot_info);
}
struct RomRequest { bool valid; std::uint32_t argument; std::uint8_t method; };
inline RomRequest parse_enter_rom_custom(const std::uint8_t* data, std::size_t size) noexcept {
    if (size < 21U) return {false, 0U, 0U};
    for (std::size_t i = 0; i < enter_rom_custom.size(); ++i) if (data[i] != enter_rom_custom[i]) return {false, 0U, 0U};
    for (std::size_t i = 21U; i < size; ++i) if (data[i] != 0U) return {false, 0U, 0U};
    const std::uint32_t argument = static_cast<std::uint32_t>(data[16]) | (static_cast<std::uint32_t>(data[17]) << 8U) |
                                   (static_cast<std::uint32_t>(data[18]) << 16U) | (static_cast<std::uint32_t>(data[19]) << 24U);
    if ((argument >> 24U) != 0xEBU || data[20] > 1U) return {false, 0U, 0U};
    return {true, argument, data[20]};
}
inline bool is_get_diagnostics(const std::uint8_t* data, std::size_t size) noexcept {
    return is_command(data, size, get_diagnostics);
}
inline bool is_get_events(const std::uint8_t* data, std::size_t size) noexcept {
    return is_command(data, size, get_events);
}
inline bool is_get_dma_diagnostics(const std::uint8_t* data, std::size_t size) noexcept {
    return is_command(data, size, get_dma_diagnostics);
}
inline bool is_get_refill_timing(const std::uint8_t* data, std::size_t size) noexcept {
    return is_command(data, size, get_refill_timing);
}
} // namespace usb::maintenance
