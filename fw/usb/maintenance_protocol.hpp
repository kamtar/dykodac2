#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace usb::maintenance {
inline constexpr std::array<std::uint8_t, 16> enter_rom{{
    'D','Y','K','O','-','R','O','M','-','B','O','O','T','-','0','1'
}};
inline constexpr std::array<std::uint8_t, 16> enter_updater{{
    'D','Y','K','O','-','F','W','-','U','P','D','A','T','E','0','1'
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
inline bool is_enter_updater(const std::uint8_t* data, std::size_t size) noexcept {
    return is_command(data, size, enter_updater);
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
} // namespace usb::maintenance
