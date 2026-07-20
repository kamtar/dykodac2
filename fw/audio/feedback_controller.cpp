#include "audio/feedback_controller.hpp"

namespace audio {
void FeedbackController::reset(const FormatConfig& format, std::size_t target_fill) noexcept {
    nominal_ = format.feedback_nominal_16_16;
    const std::uint32_t one_frame = 1U << 16U;
    minimum_ = nominal_ > one_frame ? nominal_ - one_frame : 0U;
    maximum_ = nominal_ + one_frame;
    value_ = nominal_;
    target_ = target_fill;
    filtered_error_ = 0;
}

std::uint32_t FeedbackController::update(std::size_t fill) noexcept {
    const std::int32_t error = static_cast<std::int32_t>(target_) - static_cast<std::int32_t>(fill);
    filtered_error_ += (error - filtered_error_) / 16;
    // Low fill requests more samples from the host; high fill requests fewer.
    const std::int32_t correction = filtered_error_ * 4;
    std::int64_t candidate = static_cast<std::int64_t>(nominal_) + correction;
    if (candidate < minimum_) candidate = minimum_;
    if (candidate > maximum_) candidate = maximum_;
    // Additional output smoothing avoids packet-size chatter.
    value_ = static_cast<std::uint32_t>(static_cast<std::int64_t>(value_) +
        (candidate - static_cast<std::int64_t>(value_)) / 8);
    return value_;
}
} // namespace audio
