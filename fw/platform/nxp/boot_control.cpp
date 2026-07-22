#include "platform/nxp/boot_control.hpp"
#include "platform/nxp/update_layout.hpp"

extern "C" {
#include "MIMXRT1011.h"
}

namespace platform::nxp::boot_control {
namespace {
bool watchdog_enabled = false;

volatile update::ResetMailbox& mailbox() noexcept {
    return *reinterpret_cast<volatile update::ResetMailbox*>(update::reset_mailbox_address);
}

void reset_with_token(std::uint32_t token) noexcept {
    mailbox().token = token;
    mailbox().token_inverse = ~token;
    SCB_CleanDCache_by_Addr(reinterpret_cast<std::uint32_t*>(update::reset_mailbox_address),
                           sizeof(update::ResetMailbox));
    SNVS->LPGPR[0] = token;
    __DSB();
    NVIC_SystemReset();
}

void enable_trial_watchdog() noexcept {
    if (watchdog_enabled) return;
    __disable_irq();
    RTWDOG->CNT = 0xD928C520U;
    while ((RTWDOG->CS & RTWDOG_CS_ULK_MASK) == 0U) {}
    // LPO is nominally 1 kHz. Five seconds catches a dead main loop while
    // leaving ample margin during the cooperative hardware startup.
    RTWDOG->TOVAL = 5000U;
    RTWDOG->WIN = 0U;
    RTWDOG->CS = RTWDOG_CS_EN_MASK | RTWDOG_CS_UPDATE_MASK |
                 RTWDOG_CS_CMD32EN_MASK | RTWDOG_CS_CLK(1U);
    __enable_irq();
    watchdog_enabled = true;
}
}

bool trial_boot() noexcept {
    const bool trial = (mailbox().trial == update::trial_marker &&
                        mailbox().trial_inverse == ~update::trial_marker) ||
                       SNVS->LPGPR[1] == update::trial_marker;
    if (trial) enable_trial_watchdog();
    return trial;
}

void request_updater() noexcept { reset_with_token(update::enter_updater_token); }

void watchdog_refresh() noexcept {
    if (!watchdog_enabled) return;
    const std::uint32_t primask = __get_PRIMASK();
    __disable_irq();
    RTWDOG->CNT = 0xB480A602U;
    if (primask == 0U) __enable_irq();
}

void poll_trial_health(std::uint32_t uptime_ms, bool healthy) noexcept {
    if (!watchdog_enabled) return;
    if (healthy) watchdog_refresh();
    if (healthy && uptime_ms >= 60'000U) reset_with_token(update::confirm_trial_token);
}
} // namespace platform::nxp::boot_control
