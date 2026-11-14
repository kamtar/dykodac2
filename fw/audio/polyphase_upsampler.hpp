#pragma once

#include "audio/fft_filter_spectra.hpp"
#include "audio/halfband_coefficients.hpp"

#include <cstddef>
#include <cstdint>

namespace audio {

// One production interpolation mode: exact 4x in two stages.
//
// Stage 1 (source rate -> 2x): a long linear-phase prototype decomposed into
// two exact phases and evaluated with uniform partitioned FFT convolution.
// Stage 2 (2x -> 4x): a short time-domain half-band whose second branch is a
// pure delay, so every odd output sample is a stage-1 sample passed through
// unchanged.
//
// The final output is the only quantisation in the path:
//   round_half_away(value * gain + tpdf_dither) saturated to signed 24 bit.
// Equal-rate native streams bypass all filter work byte for byte.
class PolyphaseUpsampler {
public:
    static constexpr std::uint32_t default_output_rate_hz = 192'000U;
    // Overall interpolation factor.
    static constexpr std::size_t phase_count = 4U;

    // Every size below is derived from the generated coefficient headers so a
    // regenerated design (for example 512 stage-1 taps, one partition) only
    // needs a rebuild.
    static constexpr std::size_t stage1_phase_count = detail::stage1_phases;
    static constexpr std::size_t stage2_factor = phase_count / stage1_phase_count;
    static constexpr std::size_t stage1_prototype_taps = detail::stage1_prototype_taps;
    static constexpr std::size_t stage1_taps_per_phase = stage1_prototype_taps / stage1_phase_count;
    static constexpr std::size_t partition_size = detail::stage1_partition_size;
    static constexpr std::size_t fft_size = detail::stage1_fft_size;
    static constexpr std::size_t partition_count = detail::stage1_partitions;
    static constexpr std::size_t halfband_prototype_taps = detail::halfband_prototype_taps;
    static constexpr std::size_t halfband_branch_taps = detail::halfband_branch_taps;
    static constexpr std::size_t halfband_delay_frames = detail::halfband_delay_frames;
    // Stage-2 needs the previous branch_taps-1 stage-1 samples per channel.
    static constexpr std::size_t halfband_history_frames = halfband_branch_taps - 1U;
    // Stage-1 samples per channel produced from one source partition.
    static constexpr std::size_t stage1_block_frames = partition_size * stage1_phase_count;
    // Output frames produced from one source partition.
    static constexpr std::size_t block_output_frames = partition_size * phase_count;
    // One forward-FFT job plus four slices per source block: stage-1 phase 0,
    // stage-1 phase 1, stage 2 for the first half, stage 2 for the second half.
    static constexpr std::size_t slices_per_block = 4U;
    static constexpr std::size_t stage2_slice_frames = stage1_block_frames / 2U;
    // Equivalent single 4x prototype: stage 1 zero-stuffed by two, convolved
    // with the half-band.
    static constexpr std::size_t composite_taps =
        stage2_factor * (stage1_prototype_taps - 1U) + halfband_prototype_taps;

    // The exact worst case is 1920 frames at 48 kHz: 896 queued frames when
    // the next block reserves its 1024-frame result. 44.1 kHz needs 1764.
    // Stage 2 writes the reservation two slices later than the old single
    // stage design and the 1024 frames are still published only when the
    // fourth slice finishes, so the reservation can only be smaller.
    static constexpr std::size_t ocram_output_queue_frames =
        partition_size * (phase_count + 1U); // 1280 packed frames in OCRAM
    static constexpr std::size_t output_queue_frames = 1920U;
    static constexpr std::uint32_t startup_latency_us = 10'000U;
    // Gain moves 0 -> 1 in this time at most (per-output-frame linear slew).
    static constexpr float gain_ramp_seconds = 0.020F;

    static_assert(stage1_phase_count == 2U, "stage 2 is a fixed 2x half-band");
    static_assert(stage2_factor == 2U);
    static_assert(stage1_prototype_taps % stage1_phase_count == 0U);
    static_assert(fft_size == 2U * partition_size);
    // The CMSIS instance is initialised for the fixed 512-point real FFT.
    static_assert(fft_size == 512U, "fft_instance uses the 512-point CMSIS tables");
    static_assert(partition_count >= 1U && (partition_count & (partition_count - 1U)) == 0U,
                  "the FFT history ring index is masked");
    static_assert(stage1_taps_per_phase == partition_count * partition_size);
    static_assert(detail::fft_filter_spectra.size() ==
                  stage1_phase_count * partition_count * fft_size);
    static_assert(halfband_prototype_taps == 2U * halfband_branch_taps - 1U);
    static_assert((halfband_prototype_taps - 1U) / 2U == 2U * halfband_delay_frames + 1U,
                  "pass-through branch is the half-band centre tap");
    static_assert(halfband_delay_frames <= halfband_history_frames);
    static_assert(detail::halfband_branch.size() == halfband_branch_taps);
    static_assert(stage1_block_frames % 2U == 0U);
    static_assert(2U * stage2_slice_frames * stage2_factor == block_output_frames);
    static_assert(halfband_history_frames <= stage1_block_frames);
    static_assert(ocram_output_queue_frames <= output_queue_frames);
    static_assert(output_queue_frames >= block_output_frames);

    void reset(std::uint32_t input_rate_hz,
               std::uint32_t output_rate_hz = default_output_rate_hz) noexcept;
    bool configure(std::uint32_t input_rate_hz,
                   std::uint32_t output_rate_hz = default_output_rate_hz) noexcept;

    std::uint32_t input_rate_hz() const noexcept { return input_rate_hz_; }
    std::uint32_t output_rate_hz() const noexcept { return output_rate_hz_; }
    std::uint64_t position_numerator() const noexcept { return position_numerator_; }
    std::uint32_t phase_remainder() const noexcept { return 0U; }
    std::size_t input_frames_needed(std::size_t output_frames) const noexcept;

    // Linear gain target, clamped to [0, 1]. The applied gain slews toward it
    // by at most 1/(gain_ramp_seconds * output rate) per output frame. Both
    // the target and the current gain survive configure()/reset(); reset
    // snaps the current gain to the target. Main context only (same context
    // as process()).
    void set_gain_target(float linear) noexcept;
    float gain_target() const noexcept { return gain_target_; }
    float current_gain() const noexcept { return gain_; }

    // Converts signed 24-bit stereo PCM in 32-bit left-justified slots. Exact
    // 4x configurations always return output_frames; deterministic startup
    // latency is represented by digital zero until the first FFT block is due.
    std::size_t process(const std::uint8_t* input, std::size_t input_frames,
                        std::uint8_t* output, std::size_t output_frames) noexcept;

    static constexpr std::size_t fir_group_delay_output_samples_numerator() noexcept {
        return composite_taps - 1U; // divide by two
    }
    static constexpr std::uint32_t algorithmic_latency_us() noexcept {
        return startup_latency_us;
    }

#if defined(DYKODAC_HOST_TEST)
    // Float results after stage 2, before gain, dither and 24-bit
    // quantisation, from the most recent process() call. This keeps numerical
    // verification on the actual streaming core.
    std::size_t copy_last_float_output(float* output,
                                       std::size_t capacity_frames) const noexcept;
    // Number of completed stage-1 blocks and a copy of the most recent one
    // (stage1_block_frames interleaved stereo frames), recorded when stage 1
    // finishes and before stage 2 reads it.
    std::uint64_t stage1_blocks_completed() const noexcept { return stage1_blocks_; }
    void copy_last_stage1_block(float* output) const noexcept;
#endif

private:
    bool begin_fft_block() noexcept;
    void process_one_slice() noexcept;
    void stage1_slice(std::size_t phase) noexcept;
    void stage2_slice(std::size_t half) noexcept;
    float next_gain() noexcept;
    void append_input(const std::uint8_t* input, std::size_t input_frames) noexcept;
    void write_output(std::uint8_t* output, std::size_t output_frames) noexcept;

    std::uint32_t input_rate_hz_{0U};
    std::uint32_t output_rate_hz_{default_output_rate_hz};
    std::uint64_t position_numerator_{0U};
    std::uint32_t startup_frames_remaining_{0U};
    std::size_t input_fill_{0U};
    std::size_t fft_history_head_{0U};
    std::size_t job_history_head_{0U};
    std::size_t active_slice_{slices_per_block};
    std::size_t output_read_{0U};
    std::size_t output_write_{0U};
    std::size_t output_available_{0U};
    float gain_target_{1.0F};
    float gain_{1.0F};
    float gain_step_{1.0F};
    std::uint32_t dither_state_[2]{1U, 2U};
    bool block_queue_ok_{false};
    bool exact_4x_{false};
#if defined(DYKODAC_HOST_TEST)
    std::size_t last_float_frames_{0U};
    std::uint64_t stage1_blocks_{0U};
#endif
};
} // namespace audio
