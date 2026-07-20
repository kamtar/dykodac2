#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio {
class StreamBuffer {
public:
    static constexpr std::size_t capacity_bytes = 8U * 1024U;
    bool push(const std::uint8_t* data, std::size_t size) noexcept;
    std::size_t pop(std::uint8_t* data, std::size_t size) noexcept;
    void clear() noexcept;
    std::size_t size() const noexcept;
    std::size_t free() const noexcept;
    std::uint32_t overflows() const noexcept { return overflows_; }
    std::uint32_t underruns() const noexcept { return underruns_; }
private:
    std::array<std::uint8_t, capacity_bytes> storage_{};
    std::size_t read_{0U};
    std::size_t write_{0U};
    std::size_t used_{0U};
    std::uint32_t overflows_{0U};
    std::uint32_t underruns_{0U};
};
} // namespace audio
