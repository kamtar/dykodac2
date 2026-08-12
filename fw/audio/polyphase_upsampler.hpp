#pragma once

#include <cstddef>
#include <cstdint>

namespace audio {

// One production interpolation mode: an 8192-tap linear-phase prototype,
// decomposed into four exact 2048-tap phases and evaluated with uniform
// partitioned FFT convolution. Equal-rate native streams bypass all FFT work.
class PolyphaseUpsampler {
public:
    static constexpr std::uint32_t default_output_rate_hz = 192'000U;
    static constexpr std::size_t phase_count = 4U;
    static constexpr std::size_t prototype_taps = 8192U;
    static constexpr std::size_t taps_per_phase = prototype_taps / phase_count;
    static constexpr std::size_t partition_size = 256U;
    static constexpr std::size_t fft_size = 2U * partition_size;
    static constexpr std::size_t partition_count = taps_per_phase / partition_size;
    // The exact worst case is 1920 frames at 48 kHz: 896 queued frames when
    // the next phase-0 slice reserves a 1024-frame result. 44.1 kHz needs 1764.
    static constexpr std::size_t ocram_output_queue_frames =
        partition_size * (phase_count + 1U); // 1280 packed frames in OCRAM
    static constexpr std::size_t output_queue_frames = 1920U;
    static constexpr std::uint32_t startup_latency_us = 10'000U;

    static_assert(taps_per_phase == 2048U);
    static_assert(partition_count == 8U);
    static_assert(fft_size == 512U);

    void reset(std::uint32_t input_rate_hz,
               std::uint32_t output_rate_hz = default_output_rate_hz) noexcept;
    bool configure(std::uint32_t input_rate_hz,
                   std::uint32_t output_rate_hz = default_output_rate_hz) noexcept;

    std::uint32_t input_rate_hz() const noexcept { return input_rate_hz_; }
    std::uint32_t output_rate_hz() const noexcept { return output_rate_hz_; }
    std::uint64_t position_numerator() const noexcept { return position_numerator_; }
    std::uint32_t phase_remainder() const noexcept { return 0U; }
    std::size_t input_frames_needed(std::size_t output_frames) const noexcept;

    // Converts signed 24-bit stereo PCM in 32-bit left-justified slots. Exact
    // 4x configurations always return output_frames; deterministic startup
    // latency is represented by digital zero until the first FFT block is due.
    std::size_t process(const std::uint8_t* input, std::size_t input_frames,
                        std::uint8_t* output, std::size_t output_frames) noexcept;

    static constexpr std::size_t fir_group_delay_output_samples_numerator() noexcept {
        return prototype_taps - 1U; // divide by two
    }
    static constexpr std::uint32_t algorithmic_latency_us() noexcept {
        return startup_latency_us;
    }

#if defined(DYKODAC_HOST_TEST)
    // Float results before 24-bit quantization from the most recent process()
    // call. This keeps numerical verification on the actual streaming core.
    std::size_t copy_last_float_output(float* output,
                                       std::size_t capacity_frames) const noexcept;
#endif

private:
    bool begin_fft_block() noexcept;
    void process_one_phase() noexcept;
    void append_input(const std::uint8_t* input, std::size_t input_frames) noexcept;
    void write_output(std::uint8_t* output, std::size_t output_frames) noexcept;

    std::uint32_t input_rate_hz_{0U};
    std::uint32_t output_rate_hz_{default_output_rate_hz};
    std::uint64_t position_numerator_{0U};
    std::uint32_t startup_frames_remaining_{0U};
    std::size_t input_fill_{0U};
    std::size_t fft_history_head_{0U};
    std::size_t job_history_head_{0U};
    std::size_t active_phase_{phase_count};
    std::size_t output_read_{0U};
    std::size_t output_write_{0U};
    std::size_t output_available_{0U};
    bool exact_4x_{false};
#if defined(DYKODAC_HOST_TEST)
    std::size_t last_float_frames_{0U};
#endif
};
} // namespace audio
