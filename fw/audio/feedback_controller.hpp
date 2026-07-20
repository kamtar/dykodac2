#pragma once

#include "audio/audio_format.hpp"
#include <cstddef>
#include <cstdint>

namespace audio {
class FeedbackController {
public:
    void reset(const FormatConfig& format, std::size_t target_fill) noexcept;
    std::uint32_t update(std::size_t fill) noexcept;
    std::uint32_t value() const noexcept { return value_; }
    std::uint32_t nominal() const noexcept { return nominal_; }
    std::uint32_t minimum() const noexcept { return minimum_; }
    std::uint32_t maximum() const noexcept { return maximum_; }
private:
    std::uint32_t nominal_{0U};
    std::uint32_t value_{0U};
    std::uint32_t minimum_{0U};
    std::uint32_t maximum_{0U};
    std::size_t target_{0U};
    std::int32_t filtered_error_{0};
};
} // namespace audio
