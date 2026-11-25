#include "platform/nxp/rom_bootloader.hpp"

#include "tusb.h"
#include <cstring>

extern "C" {
#include "MIMXRT1011.h"
}

// No flash is ever written by this firmware; erase/program is done by NXP's
// RAM flashloader over USB, so the XIP-execution-during-erase hazard that
// broke the earlier custom bootloader cannot occur here.

namespace platform::nxp {
namespace {
// RT1011 layout, confirmed on hardware: version, copyright, runBootloader, ...
// (differs from RT1060, where runBootloader comes first).
struct BootloaderApi {
    std::uint32_t version;
    const char* copyright;
    void (*run_bootloader)(void* argument);
};
// RT1011 ROM API tree pointer (g_bootloaderTree).
constexpr std::uintptr_t bootloader_api_pointer = 0x0020001CU;
constexpr std::uintptr_t rom_start = 0x00200000U;
constexpr std::uintptr_t rom_end = 0x00240000U;

// Survives warm resets (SNVS LPGPR did not on hardware: all zero after
// SYSRESETREQ). Reported to the host as lpgpr_at_boot[0..3] for continuity.
struct Mailbox {
    std::uint32_t argument;
    std::uint32_t breadcrumb;
    std::uint32_t magic;
    std::uint32_t magic_inverse;
};
__attribute__((section(".dtcm_mailbox"))) volatile Mailbox mailbox;
constexpr std::uint32_t request_magic = 0x524F4D42U; // "ROMB"
enum Breadcrumb : std::uint32_t {
    RequestStored = 0xB0000001U,
    RequestSeen = 0xB0000002U,
    CallingRom = 0xB0000003U,
    RomReturned = 0xB0000004U,
    DirectCallingRom = 0xB0000013U,
    DirectRomReturned = 0xB0000014U,
};

BootInfoReport captured{};

void breadcrumb(std::uint32_t value) noexcept {
    mailbox.breadcrumb = value;
    __DSB();
}

void busy_wait_cycles(std::uint32_t cycles) noexcept {
    for (volatile std::uint32_t i = 0U; i < cycles; ++i) {}
}

bool in_rom(std::uintptr_t address) noexcept { return address >= rom_start && address < rom_end; }

const BootloaderApi* api() noexcept {
    const auto pointer = *reinterpret_cast<const std::uintptr_t*>(bootloader_api_pointer);
    return in_rom(pointer) ? reinterpret_cast<const BootloaderApi*>(pointer) : nullptr;
}

[[noreturn]] void call_rom(std::uint32_t argument, Breadcrumb before, Breadcrumb after) noexcept {
    __disable_irq();
    SCB_DisableDCache();
    SCB_DisableICache();
    __DSB();
    __ISB();
    static std::uint32_t rom_argument;
    rom_argument = argument;
    const auto* entry = api();
    if (entry && in_rom(reinterpret_cast<std::uintptr_t>(entry->run_bootloader) & ~1U)) {
        breadcrumb(before);
        entry->run_bootloader(&rom_argument);
    }
    breadcrumb(after);
    NVIC_SystemReset();
}

void stop_usb_controller() noexcept {
    tud_disconnect();
    busy_wait_cycles(20'000'000U); // roughly 50-100 ms: let the host see the detach
    __disable_irq();
    NVIC_DisableIRQ(USB_OTG1_IRQn);
    USB->USBCMD &= ~USB_USBCMD_RS_MASK;
    USB->USBCMD |= USB_USBCMD_RST_MASK;
    for (std::uint32_t n = 0U; n < 1'000'000U && (USB->USBCMD & USB_USBCMD_RST_MASK) != 0U; ++n) {}
}
} // namespace

[[noreturn]] void reboot_to_rom_bootloader(std::uint32_t argument, RomEntryMethod method) noexcept {
    stop_usb_controller();
    SysTick->CTRL = 0U;
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }
    if (method == RomEntryMethod::DirectCall) call_rom(argument, DirectCallingRom, DirectRomReturned);
    mailbox.argument = argument;
    mailbox.magic = request_magic;
    mailbox.magic_inverse = ~request_magic;
    breadcrumb(RequestStored);
    NVIC_SystemReset();
}

void enter_rom_bootloader_if_requested() noexcept {
    captured.srsr_at_boot = SRC->SRSR;
    SRC->SRSR = captured.srsr_at_boot; // write-1-to-clear: next boot reports only its own cause
    captured.lpgpr_at_boot[0] = mailbox.argument;
    captured.lpgpr_at_boot[1] = mailbox.breadcrumb;
    captured.lpgpr_at_boot[2] = mailbox.magic;
    captured.lpgpr_at_boot[3] = mailbox.magic_inverse;

    const bool requested = mailbox.magic == request_magic && mailbox.magic_inverse == ~request_magic;
    // Consume first: a failure inside the ROM must not loop back here.
    mailbox.magic = 0U;
    mailbox.magic_inverse = 0U;
    if (!requested) {
        mailbox.breadcrumb = 0U;
        return;
    }
    breadcrumb(RequestSeen);
    call_rom(mailbox.argument, CallingRom, RomReturned);
}

BootInfoReport boot_info() noexcept {
    BootInfoReport report = captured;
    report.api_tree_pointer = *reinterpret_cast<const std::uint32_t*>(bootloader_api_pointer);
    if (in_rom(report.api_tree_pointer)) {
        const auto* words = reinterpret_cast<const std::uint32_t*>(report.api_tree_pointer);
        for (std::size_t i = 0U; i < 4U; ++i) report.api_words[i] = words[i];
        if (in_rom(report.api_words[1])) {
            const auto* text = reinterpret_cast<const char*>(report.api_words[1]);
            for (std::size_t i = 0U; i < sizeof(report.copyright) && in_rom(report.api_words[1] + i); ++i)
                report.copyright[i] = text[i];
        }
    }
    return report;
}
} // namespace platform::nxp
