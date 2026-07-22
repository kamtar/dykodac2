#pragma once

#include <cstdint>

namespace platform::nxp::boot_control {
bool trial_boot() noexcept;
void request_updater() noexcept;
void poll_trial_health(std::uint32_t uptime_ms, bool healthy) noexcept;
void watchdog_refresh() noexcept;
}
