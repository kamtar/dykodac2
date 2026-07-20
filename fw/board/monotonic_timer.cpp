#include "board/monotonic_timer.hpp"

extern "C" {
#include "MIMXRT1011.h"
}

namespace {
volatile std::uint32_t ticks_ms = 0U;
}

namespace board {
bool MonotonicTimer::initialize(std::uint32_t core_clock_hz) noexcept {
    ticks_ms = 0U;
    return SysTick_Config(core_clock_hz / 1'000U) == 0U;
}

std::uint32_t MonotonicTimer::now_ms() noexcept { return ticks_ms; }

bool MonotonicTimer::reached(std::uint32_t deadline_ms) noexcept {
    return static_cast<std::int32_t>(now_ms() - deadline_ms) >= 0;
}

void MonotonicTimer::interrupt() noexcept { ++ticks_ms; }
} // namespace board

extern "C" void SysTick_Handler() { board::MonotonicTimer::interrupt(); }

