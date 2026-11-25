#pragma once

#include <cstdint>

namespace platform::nxp {
inline constexpr std::uint32_t default_rom_argument = 0xEB100000U; // serial downloader, auto interface

enum class RomEntryMethod : std::uint8_t {
    ResetThenCall = 0U, // store request in SNVS, reset, call ROM first thing in main()
    DirectCall = 1U,    // stop USB/IRQs and call the ROM from the running application
};

// Call only after the audio path is safe (relay open, DAC muted, SAI/eDMA stopped).
[[noreturn]] void reboot_to_rom_bootloader(std::uint32_t argument = default_rom_argument,
                                           RomEntryMethod method = RomEntryMethod::ResetThenCall) noexcept;

// Must be the first call in main(). Captures boot evidence (reset reason, SNVS
// registers) and, if a request is pending, consumes it and enters the RT1011
// boot ROM USB serial downloader; otherwise returns immediately.
void enter_rom_bootloader_if_requested() noexcept;

// 64-byte HID report describing the last boot and the ROM API, for debugging
// ROM entry without a debug probe.
struct BootInfoReport {
    std::uint32_t magic{0x49424B44U}; // "DKBI"
    std::uint8_t version{1U};
    std::uint8_t size{64U};
    std::uint16_t reserved{0U};
    std::uint32_t srsr_at_boot{0U};      // SRC reset status before clearing
    std::uint32_t lpgpr_at_boot[4]{};    // DTCM mailbox at boot: argument, breadcrumb, magic, ~magic
    std::uint32_t api_tree_pointer{0U};  // *(0x0020001C)
    std::uint32_t api_words[4]{};        // version, copyright, runBootloader, reserved
    char copyright[16]{};
};
static_assert(sizeof(BootInfoReport) == 64U, "boot info must fit one HID packet");
BootInfoReport boot_info() noexcept;
} // namespace platform::nxp
