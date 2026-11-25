#pragma once

// Persistent user settings in the last two 4 KiB sectors of the 1 MiB XIP
// window (A/B records with sequence number and CRC-32). Read-only for the
// firmware; tools/dykodac_config/flash_store.py writes them through the ROM
// serial downloader + RAM flashloader (see docs/configurator.md).

#include "app/config_service.hpp"
#include "config/user_settings.hpp"

#include <cstdint>

namespace platform::nxp::settings_flash {

inline constexpr std::uint32_t flash_base = 0x60000000U;
inline constexpr std::uint32_t sector_size = 4096U;
// Device-relative offsets (serial NOR address), never AHB addresses. The
// linker script ends the image below region_offset and asserts it.
inline constexpr std::uint32_t region_offset = 0x000FE000U;
inline constexpr std::uint32_t sector_count = 2U;

struct Record {
    std::uint32_t magic;    // "DKST"
    std::uint32_t sequence; // newest valid record wins
    std::uint32_t length;   // sizeof(settings::Settings)
    std::uint32_t crc;      // CRC-32 over settings
    settings::Settings settings;
};
inline constexpr std::uint32_t record_magic = 0x54534B44U;
static_assert(sizeof(Record) == 208U, "flash record layout is shared with flash_store.py");

// Read-only copy from the boot ROM (0x00200000..0x0023FFFF), a diagnostic
// for inspecting the ROM API tables from the host.
bool peek_rom(std::uint32_t address, std::uint8_t* out, std::size_t length) noexcept;
app::FlashResult load(settings::Settings& out, std::uint32_t& sequence) noexcept;

} // namespace platform::nxp::settings_flash
