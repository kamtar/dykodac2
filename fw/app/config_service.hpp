#pragma once

// Owns the active / staging / last-flash copies of the user settings and
// implements the HID configuration opcodes. Hardware effects (DSP, DAC,
// stream restarts, flash) go through ConfigBackend so this is host-testable.

#include "config/user_settings.hpp"
#include "usb/config_protocol.hpp"

#include <cstddef>
#include <cstdint>

namespace app {

enum class FlashResult : std::uint8_t {
    Ok = 0U, NoRecord, Unavailable, InitFailed, EraseFailed, ProgramFailed, VerifyFailed, BadArgument,
};

class ConfigBackend {
public:
    virtual ~ConfigBackend() = default;
    // Make `s` (already validated) the running configuration.
    virtual void apply(const settings::Settings& s) noexcept = 0;
    virtual bool flash_available() const noexcept = 0;
    virtual FlashResult flash_load(settings::Settings& out, std::uint32_t& sequence) noexcept = 0;
    virtual FlashResult flash_save(const settings::Settings& s, std::uint32_t& sequence) noexcept = 0;
    // Runtime fields of the Info block (rates, volume, boost, state...).
    virtual void fill_runtime_info(usb::config::Info& info) noexcept = 0;
    virtual bool peek_rom(std::uint32_t, std::uint8_t*, std::size_t) noexcept { return false; }
};

class ConfigService {
public:
    // Load flash (or defaults) and apply. Call once the audio path exists.
    void boot(ConfigBackend& backend) noexcept;
    usb::config::Response handle(const std::uint8_t* packet, std::size_t size,
                                 ConfigBackend& backend) noexcept;
    const settings::Settings& active() const noexcept { return active_; }
    bool flash_record_valid() const noexcept { return flash_valid_; }

private:
    void make_active(const settings::Settings& s, ConfigBackend& backend) noexcept;
    settings::Settings active_{settings::defaults()};
    settings::Settings staging_{settings::defaults()};
    settings::Settings flash_copy_{settings::defaults()};
    std::uint32_t flash_sequence_{0U};
    bool flash_valid_{false};
    FlashResult last_flash_{FlashResult::NoRecord};
};

} // namespace app
