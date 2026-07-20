#pragma once

#include <cstdint>

namespace board {
class MonotonicTimer {
public:
    static bool initialize(std::uint32_t core_clock_hz) noexcept;
    static std::uint32_t now_ms() noexcept;
    static bool reached(std::uint32_t deadline_ms) noexcept;
    static void interrupt() noexcept;
};
} // namespace board

