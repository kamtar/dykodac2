#include "audio/feedback_controller.hpp"

namespace audio {
void FeedbackController::reset(const FormatConfig& format, std::size_t target_fill) noexcept {
    nominal_ = format.feedback_nominal_16_16;
    // Async feedback corrects oscillator tolerance, not a stalled audio
    // consumer.  Keep it within +/-0.5% of nominal so a full ring cannot drag
    // 44.1 kHz playback down by several percent and change pitch.
    const std::uint32_t tolerance = nominal_ / 200U;
    minimum_ = nominal_ - tolerance;
    maximum_ = nominal_ + tolerance;
    value_ = nominal_;
    target_ = target_fill;
    filtered_error_ = 0;
}

std::uint32_t FeedbackController::update(std::size_t fill) noexcept {
    const std::int32_t error = static_cast<std::int32_t>(target_) - static_cast<std::int32_t>(fill);
    filtered_error_ += (error - filtered_error_) / 16;
    // Low fill requests more samples from the host; high fill requests fewer.
    // One 16.16 feedback LSB per byte of filtered ring error reaches the
    // clamp at sustained high/low fill while remaining quiet near target.
    const std::int32_t correction = filtered_error_;
    std::int64_t candidate = static_cast<std::int64_t>(nominal_) + correction;
    if (candidate < minimum_) candidate = minimum_;
    if (candidate > maximum_) candidate = maximum_;
    // Additional output smoothing avoids packet-size chatter.
    value_ = static_cast<std::uint32_t>(static_cast<std::int64_t>(value_) +
        (candidate - static_cast<std::int64_t>(value_)) / 8);
    return value_;
}
} // namespace audio
