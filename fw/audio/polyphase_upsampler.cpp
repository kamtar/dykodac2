#include "audio/polyphase_upsampler.hpp"

#include "audio/fft_filter_spectra.hpp"

extern "C" {
#include "arm_common_tables.h"
#include "arm_const_structs.h"
#include "dsp/transform_functions.h"
}

#include <array>
#include <cstring>

namespace audio {
namespace {
constexpr std::size_t channels = 2U;
constexpr std::size_t pcm_bytes_per_frame = 8U;
constexpr std::size_t packed_bytes_per_frame = 6U;
using PcmWord = std::uint32_t __attribute__((may_alias));

#if defined(DYKODAC_HOST_TEST)
#define DYKODAC_FFT_HOT
#define DYKODAC_DTCM
#define DYKODAC_OCRAM
#else
#define DYKODAC_FFT_HOT __attribute__((section(".itcm_text"), noinline))
#define DYKODAC_DTCM __attribute__((section(".fft_working"), aligned(32)))
#define DYKODAC_OCRAM __attribute__((section(".fft_state_ocram"), aligned(32)))
#endif

// The two channels have independent X histories, while all four phase filters
// share each channel FFT. Packed RFFT storage keeps the FDL at exactly 32 KiB.
DYKODAC_OCRAM std::array<float, channels * PolyphaseUpsampler::partition_count *
                                      PolyphaseUpsampler::fft_size> fft_history{};
// The main packed queue stays in OCRAM; a small DTCM extension provides the
// full reservation needed while the preceding result block is still draining.
DYKODAC_OCRAM std::array<std::uint8_t, PolyphaseUpsampler::ocram_output_queue_frames *
                                      packed_bytes_per_frame> output_queue{};
DYKODAC_DTCM std::array<std::uint8_t,
    (PolyphaseUpsampler::output_queue_frames - PolyphaseUpsampler::ocram_output_queue_frames) *
    packed_bytes_per_frame> output_queue_extension{};

DYKODAC_DTCM std::array<float, channels * PolyphaseUpsampler::partition_size> input_block{};
DYKODAC_DTCM std::array<float, channels * PolyphaseUpsampler::fft_size> spectral_accumulator{};
DYKODAC_DTCM std::array<float, PolyphaseUpsampler::fft_size> fft_scratch{};
DYKODAC_DTCM std::array<float, PolyphaseUpsampler::phase_count * channels *
                                     PolyphaseUpsampler::partition_size> ola_overlap{};

arm_rfft_fast_instance_f32 fft_instance{};

#if defined(DYKODAC_HOST_TEST)
std::array<float, PolyphaseUpsampler::output_queue_frames * channels> debug_output_queue{};
std::array<float, PolyphaseUpsampler::output_queue_frames * channels> last_float_output{};
#endif

inline float decode_sample(const std::uint8_t* bytes) noexcept {
    const auto left_justified = static_cast<std::int32_t>(
        *reinterpret_cast<const PcmWord*>(bytes));
    // Every signed 24-bit integer is exactly representable by binary32.
    return static_cast<float>(left_justified >> 8U);
}

inline std::int32_t saturate_sample(float value) noexcept {
    if (value >= 8'388'607.0F) return 0x007F'FFFF;
    if (value <= -8'388'608.0F) return -0x0080'0000;
    return static_cast<std::int32_t>(value + (value < 0.0F ? -0.5F : 0.5F));
}

inline std::size_t history_offset(std::size_t channel, std::size_t partition) noexcept {
    return (channel * PolyphaseUpsampler::partition_count + partition) *
           PolyphaseUpsampler::fft_size;
}

inline std::size_t overlap_offset(std::size_t phase, std::size_t channel) noexcept {
    return (phase * channels + channel) * PolyphaseUpsampler::partition_size;
}

inline void store_packed_queue(std::size_t frame, std::size_t channel,
                               std::int32_t sample) noexcept {
    const auto word = static_cast<std::uint32_t>(sample) & 0x00FF'FFFFU;
    auto* const frame_data = frame < PolyphaseUpsampler::ocram_output_queue_frames ?
        output_queue.data() + frame * packed_bytes_per_frame :
        output_queue_extension.data() +
            (frame - PolyphaseUpsampler::ocram_output_queue_frames) * packed_bytes_per_frame;
    auto* const destination = frame_data + channel * 3U;
    destination[0] = static_cast<std::uint8_t>(word);
    destination[1] = static_cast<std::uint8_t>(word >> 8U);
    destination[2] = static_cast<std::uint8_t>(word >> 16U);
}

inline void load_packed_queue(std::size_t frame, std::uint8_t* destination) noexcept {
    const auto* const source = frame < PolyphaseUpsampler::ocram_output_queue_frames ?
        output_queue.data() + frame * packed_bytes_per_frame :
        output_queue_extension.data() +
            (frame - PolyphaseUpsampler::ocram_output_queue_frames) * packed_bytes_per_frame;
    destination[0] = 0U;
    destination[1] = source[0];
    destination[2] = source[1];
    destination[3] = source[2];
    destination[4] = 0U;
    destination[5] = source[3];
    destination[6] = source[4];
    destination[7] = source[5];
}
} // namespace

void PolyphaseUpsampler::reset(std::uint32_t input_rate_hz,
                               std::uint32_t output_rate_hz) noexcept {
    input_rate_hz_ = input_rate_hz;
    output_rate_hz_ = output_rate_hz;
    exact_4x_ = input_rate_hz != 0U &&
                static_cast<std::uint64_t>(input_rate_hz) * phase_count == output_rate_hz;
    position_numerator_ = 0U;
    startup_frames_remaining_ = exact_4x_ ?
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(output_rate_hz) *
                                    startup_latency_us) / 1'000'000U) : 0U;
    input_fill_ = 0U;
    fft_history_head_ = 0U;
    job_history_head_ = 0U;
    active_phase_ = phase_count;
    output_read_ = 0U;
    output_write_ = 0U;
    output_available_ = 0U;
    fft_history.fill(0.0F);
    output_queue.fill(0U);
    output_queue_extension.fill(0U);
    input_block.fill(0.0F);
    spectral_accumulator.fill(0.0F);
    fft_scratch.fill(0.0F);
    ola_overlap.fill(0.0F);
#if defined(DYKODAC_HOST_TEST)
    debug_output_queue.fill(0.0F);
    last_float_output.fill(0.0F);
    last_float_frames_ = 0U;
#endif
    // Fixed-size initialization avoids linking unused FFT-size initializers.
    fft_instance.Sint = arm_cfft_sR_f32_len256;
    fft_instance.fftLenRFFT = static_cast<std::uint16_t>(fft_size);
    fft_instance.pTwiddleRFFT = twiddleCoef_rfft_512;
}

bool PolyphaseUpsampler::configure(std::uint32_t input_rate_hz,
                                   std::uint32_t output_rate_hz) noexcept {
    const bool native = input_rate_hz != 0U && input_rate_hz == output_rate_hz;
    const bool exact = input_rate_hz != 0U &&
        static_cast<std::uint64_t>(input_rate_hz) * phase_count == output_rate_hz;
    if (!native && !exact) return false;
    reset(input_rate_hz, output_rate_hz);
    return true;
}

std::size_t PolyphaseUpsampler::input_frames_needed(std::size_t output_frames) const noexcept {
    if (input_rate_hz_ == 0U || output_frames == 0U) return 0U;
    if (!exact_4x_) return output_frames;
    return (output_frames + phase_count - 1U) / phase_count;
}

DYKODAC_FFT_HOT bool PolyphaseUpsampler::begin_fft_block() noexcept {
    if (input_fill_ != partition_size || active_phase_ != phase_count) return false;
    job_history_head_ = fft_history_head_;
    for (std::size_t channel = 0U; channel < channels; ++channel) {
        const auto input_base = channel * partition_size;
        std::memcpy(fft_scratch.data(), input_block.data() + input_base,
                    partition_size * sizeof(float));
        std::memset(fft_scratch.data() + partition_size, 0,
                    partition_size * sizeof(float));
        arm_rfft_fast_f32(&fft_instance, fft_scratch.data(),
                          fft_history.data() + history_offset(channel, job_history_head_), 0U);
    }
    fft_history_head_ = (fft_history_head_ + 1U) & (partition_count - 1U);
    input_fill_ = 0U;
    active_phase_ = 0U;
    return true;
}

void PolyphaseUpsampler::append_input(const std::uint8_t* input,
                                      std::size_t input_frames) noexcept {
    if (input == nullptr) return;
    for (std::size_t frame = 0U; frame < input_frames; ++frame) {
        if (input_fill_ == partition_size) {
            // With the production 1 ms cadence the four phase slices finish
            // before another 256-sample block can fill. Do not overwrite a
            // full block if a caller violates that bounded schedule.
            if (!begin_fft_block()) break;
        }
        input_block[input_fill_] = decode_sample(input + frame * pcm_bytes_per_frame);
        input_block[partition_size + input_fill_] =
            decode_sample(input + frame * pcm_bytes_per_frame + 4U);
        ++input_fill_;
    }
    if (input_fill_ == partition_size && active_phase_ == phase_count)
        (void)begin_fft_block();
}

DYKODAC_FFT_HOT void PolyphaseUpsampler::process_one_phase() noexcept {
    if (active_phase_ >= phase_count) return;
    const std::size_t phase = active_phase_;
    auto* const left_accumulator = spectral_accumulator.data();
    auto* const right_accumulator = spectral_accumulator.data() + fft_size;
    std::memset(spectral_accumulator.data(), 0, spectral_accumulator.size() * sizeof(float));

    const auto* coefficients = detail::fft_filter_spectra.data() +
        phase * partition_count * fft_size;
    for (std::size_t partition = 0U; partition < partition_count; ++partition) {
        const std::size_t history_partition =
            (job_history_head_ + partition_count - partition) & (partition_count - 1U);
        const auto* const left = fft_history.data() + history_offset(0U, history_partition);
        const auto* const right = fft_history.data() + history_offset(1U, history_partition);
        const auto* const filter = coefficients + partition * fft_size;

        // DC and Nyquist are the two real-only values in CMSIS packed format.
        left_accumulator[0] += left[0] * filter[0];
        right_accumulator[0] += right[0] * filter[0];
        left_accumulator[1] += left[1] * filter[1];
        right_accumulator[1] += right[1] * filter[1];
        for (std::size_t bin = 2U; bin < fft_size; bin += 2U) {
            // Load H once from sequential QSPI and apply it to both channels.
            const float hr = filter[bin];
            const float hi = filter[bin + 1U];
            const float xlr = left[bin];
            const float xli = left[bin + 1U];
            const float xrr = right[bin];
            const float xri = right[bin + 1U];
            left_accumulator[bin] += xlr * hr - xli * hi;
            left_accumulator[bin + 1U] += xlr * hi + xli * hr;
            right_accumulator[bin] += xrr * hr - xri * hi;
            right_accumulator[bin + 1U] += xrr * hi + xri * hr;
        }
    }

    const std::size_t free_frames = output_queue_frames - output_available_;
    const bool queue_has_room = free_frames >= partition_size * phase_count;
    for (std::size_t channel = 0U; channel < channels; ++channel) {
        auto* const accumulator = channel == 0U ? left_accumulator : right_accumulator;
        arm_rfft_fast_f32(&fft_instance, accumulator, fft_scratch.data(), 1U);
        auto* const overlap = ola_overlap.data() + overlap_offset(phase, channel);
        if (queue_has_room) {
            for (std::size_t sample = 0U; sample < partition_size; ++sample) {
                const float value = fft_scratch[sample] + overlap[sample];
                const std::size_t queue_frame =
                    (output_write_ + sample * phase_count + phase) % output_queue_frames;
                store_packed_queue(queue_frame, channel, saturate_sample(value));
#if defined(DYKODAC_HOST_TEST)
                debug_output_queue[queue_frame * channels + channel] = value;
#endif
            }
        }
        std::memcpy(overlap, fft_scratch.data() + partition_size,
                    partition_size * sizeof(float));
    }

    ++active_phase_;
    if (active_phase_ == phase_count && queue_has_room) {
        output_write_ = (output_write_ + partition_size * phase_count) % output_queue_frames;
        output_available_ += partition_size * phase_count;
    }
}

void PolyphaseUpsampler::write_output(std::uint8_t* output,
                                      std::size_t output_frames) noexcept {
#if defined(DYKODAC_HOST_TEST)
    last_float_frames_ = output_frames < output_queue_frames ? output_frames : output_queue_frames;
#endif
    for (std::size_t frame = 0U; frame < output_frames; ++frame) {
        auto* const destination = output + frame * pcm_bytes_per_frame;
        if (startup_frames_remaining_ != 0U) {
            std::memset(destination, 0, pcm_bytes_per_frame);
            --startup_frames_remaining_;
#if defined(DYKODAC_HOST_TEST)
            if (frame < last_float_frames_) {
                last_float_output[frame * channels] = 0.0F;
                last_float_output[frame * channels + 1U] = 0.0F;
            }
#endif
        } else if (output_available_ != 0U) {
            load_packed_queue(output_read_, destination);
#if defined(DYKODAC_HOST_TEST)
            if (frame < last_float_frames_) {
                last_float_output[frame * channels] = debug_output_queue[output_read_ * channels];
                last_float_output[frame * channels + 1U] =
                    debug_output_queue[output_read_ * channels + 1U];
            }
#endif
            output_read_ = (output_read_ + 1U) % output_queue_frames;
            --output_available_;
        } else {
            std::memset(destination, 0, pcm_bytes_per_frame);
#if defined(DYKODAC_HOST_TEST)
            if (frame < last_float_frames_) {
                last_float_output[frame * channels] = 0.0F;
                last_float_output[frame * channels + 1U] = 0.0F;
            }
#endif
        }
    }
}

std::size_t PolyphaseUpsampler::process(const std::uint8_t* input,
                                        std::size_t input_frames,
                                        std::uint8_t* output,
                                        std::size_t output_frames) noexcept {
    if (input_rate_hz_ == 0U || output == nullptr || output_frames == 0U) return 0U;
    if (!exact_4x_) {
        const std::size_t produced = input_frames < output_frames ? input_frames : output_frames;
        if (produced == 0U || input == nullptr) return 0U;
        std::memmove(output, input, produced * pcm_bytes_per_frame);
#if defined(DYKODAC_HOST_TEST)
        last_float_frames_ = produced < output_queue_frames ? produced : output_queue_frames;
        for (std::size_t frame = 0U; frame < last_float_frames_; ++frame) {
            last_float_output[frame * channels] = decode_sample(input + frame * pcm_bytes_per_frame);
            last_float_output[frame * channels + 1U] =
                decode_sample(input + frame * pcm_bytes_per_frame + 4U);
        }
#endif
        position_numerator_ += static_cast<std::uint64_t>(produced) * input_rate_hz_;
        return produced;
    }

    const bool had_active_phase = active_phase_ < phase_count;
    append_input(input, input_frames);
    // Exactly one phase slice (16 KiB of contiguous H spectra) is evaluated
    // per normal 1 ms refill. Four slices finish before the next 256-frame
    // source block, bounding both CPU and FlexSPI demand.
    if (had_active_phase) process_one_phase();
    write_output(output, output_frames);
    position_numerator_ += static_cast<std::uint64_t>(output_frames) * input_rate_hz_;
    return output_frames;
}

#if defined(DYKODAC_HOST_TEST)
std::size_t PolyphaseUpsampler::copy_last_float_output(
    float* output, std::size_t capacity_frames) const noexcept {
    if (output == nullptr) return 0U;
    const std::size_t count = last_float_frames_ < capacity_frames ?
        last_float_frames_ : capacity_frames;
    std::memcpy(output, last_float_output.data(), count * channels * sizeof(float));
    return count;
}
#endif
} // namespace audio
