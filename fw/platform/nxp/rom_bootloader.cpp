#include "platform/nxp/rom_bootloader.hpp"
#include <cstdint>

extern "C" {
#include "MIMXRT1011.h"
}

namespace platform::nxp {
namespace {
struct BootloaderApi {
    void (*run_bootloader)(const std::uint32_t* argument);
    std::uint32_t version;
    const char* copyright;
};
constexpr std::uintptr_t bootloader_api_pointer = 0x0020001CU;
constexpr std::uint32_t serial_downloader_argument = 0xEB100000U;
}

[[noreturn]] void enter_rom_serial_downloader() noexcept {
    __disable_irq();
    SCB_CleanInvalidateDCache();
    __DSB();
    __ISB();
    const auto api = *reinterpret_cast<BootloaderApi* const*>(bootloader_api_pointer);
    api->run_bootloader(&serial_downloader_argument);
    NVIC_SystemReset();
    for (;;) __asm volatile("wfi");
}
} // namespace platform::nxp
