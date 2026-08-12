#include "audio/polyphase_upsampler.hpp"

#include "audio/polyphase_coefficients.hpp"

#include <cstring>
#include <limits>

namespace audio {
namespace {
constexpr std::size_t pcm_bytes_per_frame = 8U;
constexpr std::int64_t first_tap_offset =
    1 - static_cast<std::int64_t>(PolyphaseUpsampler::taps_per_phase / 2U);
constexpr std::int64_t last_tap_offset =
    static_cast<std::int64_t>(PolyphaseUpsampler::taps_per_phase / 2U);
constexpr std::size_t history_mask = PolyphaseUpsampler::history_capacity_frames - 1U;
using PcmWord = std::uint32_t __attribute__((may_alias));

#if defined(DYKODAC_HOST_TEST)
#define DYKODAC_HOT
#else
#define DYKODAC_HOT __attribute__((section(".itcm_text"), noinline))
__attribute__((section(".dtcm_history"), aligned(32)))
std::array<float, PolyphaseUpsampler::history_capacity_frames * 2U> target_left_history{};
__attribute__((section(".dtcm_history"), aligned(32)))
std::array<float, PolyphaseUpsampler::history_capacity_frames * 2U> target_right_history{};
#endif

__attribute__((always_inline)) inline void convolve_stereo(
    const float* __restrict left, const float* __restrict right,
    const float* __restrict coefficients, float& left_result,
    float& right_result) noexcept {
    float left0 = 0.0F, left1 = 0.0F, left2 = 0.0F, left3 = 0.0F;
    float right0 = 0.0F, right1 = 0.0F, right2 = 0.0F, right3 = 0.0F;
    const float* const coefficients_end = coefficients + PolyphaseUpsampler::taps_per_phase;
    do {
        const float coefficient0 = coefficients[0];
        const float coefficient1 = coefficients[1];
        const float coefficient2 = coefficients[2];
        const float coefficient3 = coefficients[3];
        left0 += left[0] * coefficient0;
        right0 += right[0] * coefficient0;
        left1 += left[1] * coefficient1;
        right1 += right[1] * coefficient1;
        left2 += left[2] * coefficient2;
        right2 += right[2] * coefficient2;
        left3 += left[3] * coefficient3;
        right3 += right[3] * coefficient3;
        coefficients += 4;
        left += 4;
        right += 4;
    } while (coefficients != coefficients_end);
    left_result = (left0 + left1) + (left2 + left3);
    right_result = (right0 + right1) + (right2 + right3);
}

__attribute__((always_inline)) inline void convolve_stereo_reversed(
    const float* __restrict left, const float* __restrict right,
    const float* __restrict coefficients, float& left_result,
    float& right_result) noexcept {
    float left0 = 0.0F, left1 = 0.0F, left2 = 0.0F, left3 = 0.0F;
    float right0 = 0.0F, right1 = 0.0F, right2 = 0.0F, right3 = 0.0F;
    std::size_t remaining = PolyphaseUpsampler::taps_per_phase;
    do {
        const float coefficient0 = coefficients[0];
        const float coefficient1 = coefficients[-1];
        const float coefficient2 = coefficients[-2];
        const float coefficient3 = coefficients[-3];
        left0 += left[0] * coefficient0;
        right0 += right[0] * coefficient0;
        left1 += left[1] * coefficient1;
        right1 += right[1] * coefficient1;
        left2 += left[2] * coefficient2;
        right2 += right[2] * coefficient2;
        left3 += left[3] * coefficient3;
        right3 += right[3] * coefficient3;
        coefficients -= 4;
        left += 4;
        right += 4;
        remaining -= 4U;
    } while (remaining != 0U);
    left_result = (left0 + left1) + (left2 + left3);
    right_result = (right0 + right1) + (right2 + right3);
}

__attribute__((always_inline)) inline void store_sample(std::uint8_t* output,
                                                         std::int32_t sample) noexcept {
    const std::uint32_t packed = (static_cast<std::uint32_t>(sample) & 0x00FF'FFFFU) << 8U;
    *reinterpret_cast<PcmWord*>(output) = packed;
}
}

void PolyphaseUpsampler::reset(std::uint32_t input_rate_hz,
                               std::uint32_t output_rate_hz) noexcept {
    input_rate_hz_ = input_rate_hz;
    output_rate_hz_ = output_rate_hz;
    source_next_index_ = 0U;
    source_position_ = 0U;
    phase_remainder_ = 0U;
    position_numerator_ = 0U;
#if defined(DYKODAC_HOST_TEST)
    left_history_.fill(0.0F);
    right_history_.fill(0.0F);
#else
    target_left_history.fill(0.0F);
    target_right_history.fill(0.0F);
#endif
}

bool PolyphaseUpsampler::configure(std::uint32_t input_rate_hz,
                                   std::uint32_t output_rate_hz) noexcept {
    if (input_rate_hz == 0U || output_rate_hz == 0U || input_rate_hz > output_rate_hz)
        return false;
    reset(input_rate_hz, output_rate_hz);
    return true;
}

std::size_t PolyphaseUpsampler::input_frames_needed(std::size_t output_frames) const noexcept {
    if (output_frames == 0U || input_rate_hz_ == 0U) return 0U;
    if (input_rate_hz_ == output_rate_hz_) return output_frames;
    std::uint64_t source_advance = 0U;
    const auto output_count = static_cast<std::uint64_t>(output_frames - 1U);
    source_advance = (static_cast<std::uint64_t>(phase_remainder_) +
                      output_count * input_rate_hz_) / output_rate_hz_;
    const std::uint64_t last_source_index = source_position_ + source_advance;
    const std::uint64_t required_end = last_source_index + static_cast<std::uint64_t>(last_tap_offset);
    if (required_end < source_next_index_) return 0U;
    const std::uint64_t needed = required_end - source_next_index_ + 1U;
    return needed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
        ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(needed);
}

inline __attribute__((always_inline)) float PolyphaseUpsampler::decode_sample(const std::uint8_t* bytes) noexcept {
    const auto left_justified = static_cast<std::int32_t>(
        *reinterpret_cast<const PcmWord*>(bytes));
    // Every signed 24-bit PCM integer is exactly representable by binary32.
    return static_cast<float>(left_justified >> 8U);
}

inline __attribute__((always_inline)) std::int32_t PolyphaseUpsampler::saturate_sample(float value) noexcept {
    if (value >= 8'388'607.0F) return 0x007F'FFFF;
    if (value <= -8'388'608.0F) return -0x0080'0000;
    return static_cast<std::int32_t>(value + (value < 0.0F ? -0.5F : 0.5F));
}

DYKODAC_HOT std::size_t PolyphaseUpsampler::process(
    const std::uint8_t* input, std::size_t input_frames,
    std::uint8_t* output, std::size_t output_frames) noexcept {
    if (input_rate_hz_ == 0U || output == nullptr || output_frames == 0U) return 0U;

    if (input_rate_hz_ == output_rate_hz_) {
        const std::size_t produced = input_frames < output_frames ? input_frames : output_frames;
        if (produced == 0U || input == nullptr) return 0U;
        std::memmove(output, input, produced * pcm_bytes_per_frame);
        source_next_index_ += produced;
        source_position_ += produced;
        position_numerator_ += static_cast<std::uint64_t>(produced) * input_rate_hz_;
        return produced;
    }

    std::uint64_t source_next_index = source_next_index_;
    std::uint64_t source_position = source_position_;
    std::uint32_t phase_remainder = phase_remainder_;
    std::size_t consumed = 0U;
    std::size_t produced = 0U;
#if defined(DYKODAC_HOST_TEST)
    auto* const left_history = left_history_.data();
    auto* const right_history = right_history_.data();
#else
    auto* const left_history = target_left_history.data();
    auto* const right_history = target_right_history.data();
#endif

    // Stage a complete block only when doing so cannot overwrite history still
    // needed by its first output. The 128-frame target ring intentionally uses
    // the bounded path; its per-output availability check is tiny beside the
    // 128-tap convolution and avoids doubling the history allocation.
    std::uint64_t last_source_advance = 0U;
    if (output_frames != 0U) {
        last_source_advance = (static_cast<std::uint64_t>(phase_remainder) +
            static_cast<std::uint64_t>(output_frames - 1U) * input_rate_hz_) / output_rate_hz_;
    }
    const bool contiguous_block = last_source_advance + taps_per_phase <= history_capacity_frames;

    if (contiguous_block) {
        const std::uint64_t required_end = source_position + last_source_advance +
                                           static_cast<std::uint64_t>(last_tap_offset);
        const std::uint64_t required = required_end < source_next_index ? 0U :
                                       required_end - source_next_index + 1U;
        const std::size_t append_count = required < input_frames ?
            static_cast<std::size_t>(required) : input_frames;
        while (consumed < append_count) {
            const std::size_t slot = static_cast<std::size_t>(source_next_index) & history_mask;
            const auto* frame = input + consumed * pcm_bytes_per_frame;
            const float left = decode_sample(frame);
            const float right = decode_sample(frame + 4U);
            left_history[slot] = left;
            left_history[slot + history_capacity_frames] = left;
            right_history[slot] = right;
            right_history[slot + history_capacity_frames] = right;
            ++source_next_index;
            ++consumed;
        }
    }

    auto render_output = [&](std::uint8_t* frame) __attribute__((always_inline)) {
        // Select one ready-to-use fractional-delay row.  Rounding, including
        // the phase-1 endpoint, limits the phase error to 1/128 input sample
        // without doing another interpolation FIR inside the audio FIR.
        const std::uint64_t phase_position = static_cast<std::uint64_t>(phase_remainder) * phase_count;
        const auto phase = static_cast<std::size_t>(
            (phase_position + output_rate_hz_ / 2U) / output_rate_hz_);
        const bool reverse_coefficients = phase > phase_count / 2U;
        const std::size_t stored_phase = reverse_coefficients ? phase_count - phase : phase;
        const auto* coefficients = &detail::polyphase_coefficients[stored_phase * taps_per_phase];
        const std::size_t history_slot = (static_cast<std::size_t>(source_position) +
                                          history_capacity_frames +
                                          static_cast<std::size_t>(first_tap_offset)) & history_mask;
        float left = 0.0F;
        float right = 0.0F;
        if (reverse_coefficients) {
            convolve_stereo_reversed(left_history + history_slot,
                                     right_history + history_slot,
                                     coefficients + taps_per_phase - 1U, left, right);
        } else {
            convolve_stereo(left_history + history_slot,
                            right_history + history_slot,
                            coefficients, left, right);
        }
        store_sample(frame, saturate_sample(left));
        store_sample(frame + 4U, saturate_sample(right));
        phase_remainder += input_rate_hz_;
        if (phase_remainder >= output_rate_hz_) {
            phase_remainder -= output_rate_hz_;
            ++source_position;
        }
    };

    const std::uint64_t block_required_end = source_position + last_source_advance +
                                             static_cast<std::uint64_t>(last_tap_offset);
    if (contiguous_block && source_next_index > block_required_end) {
        // Normal DMA refills take this path: all input was staged above, so
        // the output loop contains no source-availability or append work.
        auto* frame = output;
        while (produced < output_frames) {
            render_output(frame);
            frame += pcm_bytes_per_frame;
            ++produced;
        }
    } else {
        while (produced < output_frames) {
            const auto required_end = source_position + static_cast<std::uint64_t>(last_tap_offset);
            while (source_next_index <= required_end && consumed < input_frames) {
                const std::size_t slot = static_cast<std::size_t>(source_next_index) & history_mask;
                const auto* frame = input + consumed * pcm_bytes_per_frame;
                const float left = decode_sample(frame);
                const float right = decode_sample(frame + 4U);
                left_history[slot] = left;
                left_history[slot + history_capacity_frames] = left;
                right_history[slot] = right;
                right_history[slot + history_capacity_frames] = right;
                ++source_next_index;
                ++consumed;
            }
            if (source_next_index <= required_end) break;
            render_output(output + produced * pcm_bytes_per_frame);
            ++produced;
        }
    }

    source_next_index_ = source_next_index;
    source_position_ = source_position;
    phase_remainder_ = phase_remainder;
    position_numerator_ += static_cast<std::uint64_t>(produced) * input_rate_hz_;
    return produced;
}
} // namespace audio
