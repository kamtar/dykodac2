#include "platform/nxp/settings_flash.hpp"

#include <cstring>

extern "C" {
#include "MIMXRT1011.h"
}

// The firmware only ever READS the settings sectors (XIP). They are written
// by the host configurator through the ROM serial downloader and NXP's RAM
// flashloader, the same proven path as a firmware update, so nothing here
// can hit the erase-while-executing-in-place hazard. (On hardware the RT1011
// ROM FlexSPI driver table also turned out not to match the SDK layout.)

namespace platform::nxp::settings_flash {
namespace {
constexpr std::uintptr_t rom_start = 0x00200000U;
constexpr std::uintptr_t rom_end = 0x00240000U;

bool in_rom(std::uintptr_t address) noexcept { return address >= rom_start && address < rom_end; }

const Record* record_at(std::uint32_t sector) noexcept {
    return reinterpret_cast<const Record*>(flash_base + region_offset + sector * sector_size);
}

bool record_valid(const Record& r) noexcept {
    // Records of the 192-byte layout stay readable (settings::from_record).
    return r.magic == record_magic &&
           (r.length == sizeof(settings::Settings) || r.length == settings::legacy_size) &&
           r.crc == settings::crc32(&r.settings, r.length);
}
} // namespace

bool peek_rom(std::uint32_t address, std::uint8_t* out, std::size_t length) noexcept {
    if (length == 0U || !in_rom(address) || !in_rom(address + length - 1U)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), length);
    return true;
}

app::FlashResult load(settings::Settings& out, std::uint32_t& sequence) noexcept {
    // The sectors change underneath the cache only across a reset, but a
    // stale line after a host write + warm reset must never be trusted.
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<void*>(flash_base + region_offset),
                                 static_cast<std::int32_t>(sector_count * sector_size));
    const Record* best = nullptr;
    for (std::uint32_t sector = 0U; sector < sector_count; ++sector) {
        const Record* r = record_at(sector);
        if (!record_valid(*r)) continue;
        if (!best || static_cast<std::int32_t>(r->sequence - best->sequence) > 0) best = r;
    }
    if (!best) return app::FlashResult::NoRecord;
    if (!settings::from_record(out, &best->settings, best->length)) return app::FlashResult::NoRecord;
    sequence = best->sequence;
    return app::FlashResult::Ok;
}

} // namespace platform::nxp::settings_flash
