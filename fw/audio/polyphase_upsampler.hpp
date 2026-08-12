#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace audio {

// The phase accumulator remains rational (input_rate/output_rate); the
// 64-entry bank only quantizes the fractional coefficient selection. The
// coefficient bank is shared by both external DAC clock families.
class PolyphaseUpsampler {
public:
    static constexpr std::uint32_t default_output_rate_hz = 192'000U;
    static constexpr std::size_t phase_count = 64U;
    static constexpr std::size_t taps_per_phase = 128U;
    // A one-window ring is mirrored in each channel so every 128-tap window
    // is contiguous, including the window that crosses the ring boundary.
    static constexpr std::size_t history_capacity_frames = taps_per_phase;
    static_assert((history_capacity_frames & (history_capacity_frames - 1U)) == 0U);
    static_assert((taps_per_phase & 3U) == 0U);

    void reset(std::uint32_t input_rate_hz,
               std::uint32_t output_rate_hz = default_output_rate_hz) noexcept;
    bool configure(std::uint32_t input_rate_hz,
                   std::uint32_t output_rate_hz = default_output_rate_hz) noexcept;

    std::uint32_t input_rate_hz() const noexcept { return input_rate_hz_; }
    std::uint32_t output_rate_hz() const noexcept { return output_rate_hz_; }
    std::uint64_t position_numerator() const noexcept { return position_numerator_; }
    std::uint32_t phase_remainder() const noexcept { return phase_remainder_; }

    // Number of source frames that must be popped before process() can make
    // output_frames outputs.  Existing history is included in this count.
    std::size_t input_frames_needed(std::size_t output_frames) const noexcept;

    // Converts 24-bit stereo PCM in 32-bit little-endian slots to the same
    // container at the configured output rate. Equal-rate configurations use
    // a direct copy and do not spend cycles in the FIR. Returns output frames
    // produced; output is complete when the return value equals output_frames.
    std::size_t process(const std::uint8_t* input, std::size_t input_frames,
                        std::uint8_t* output, std::size_t output_frames) noexcept;

private:
    static float decode_sample(const std::uint8_t* bytes) noexcept;
    static std::int32_t saturate_sample(float value) noexcept;

#if defined(DYKODAC_HOST_TEST)
    // Host tests construct independent converters. The target has one audio
    // engine and keeps its much larger histories in a dedicated DTCM section.
    std::array<float, history_capacity_frames * 2U> left_history_{};
    std::array<float, history_capacity_frames * 2U> right_history_{};
#endif
    std::uint32_t input_rate_hz_{0U};
    std::uint32_t output_rate_hz_{default_output_rate_hz};
    std::uint64_t source_next_index_{0U};
    std::uint64_t source_position_{0U};
    std::uint32_t phase_remainder_{0U};
    std::uint64_t position_numerator_{0U};
};
} // namespace audio
