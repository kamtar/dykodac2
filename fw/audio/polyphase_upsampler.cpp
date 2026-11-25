#include "audio/polyphase_upsampler.hpp"

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
using Upsampler = PolyphaseUpsampler;

#if defined(DYKODAC_HOST_TEST)
#define DYKODAC_FFT_HOT
#define DYKODAC_DTCM
#define DYKODAC_OCRAM
#else
#define DYKODAC_FFT_HOT __attribute__((section(".itcm_text"), noinline))
#define DYKODAC_DTCM __attribute__((section(".fft_working"), aligned(32)))
#define DYKODAC_OCRAM __attribute__((section(".fft_state_ocram"), aligned(32)))
#endif

// The two channels have independent X histories, while both stage-1 phase
// filters share each channel FFT. Packed RFFT storage: 2 x partitions x 512.
DYKODAC_OCRAM std::array<float, channels * Upsampler::partition_count *
                                      Upsampler::fft_size> fft_history{};
// Stage-1 output u[] per channel, preceded by the half-band history:
//   [halfband_history_frames previous u | stage1_block_frames current u]
// u[2k + p] is stage-1 phase p, sample k of the current source block.
constexpr std::size_t stage2_stride =
    Upsampler::halfband_history_frames + Upsampler::stage1_block_frames;
DYKODAC_OCRAM std::array<float, channels * stage2_stride> stage2_buffer{};
// The main packed queue stays in OCRAM; a small DTCM extension provides the
// full reservation needed while the preceding result block is still draining.
DYKODAC_OCRAM std::array<std::uint8_t, Upsampler::ocram_output_queue_frames *
                                      packed_bytes_per_frame> output_queue{};
DYKODAC_DTCM std::array<std::uint8_t,
    (Upsampler::output_queue_frames - Upsampler::ocram_output_queue_frames) *
    packed_bytes_per_frame> output_queue_extension{};

DYKODAC_DTCM std::array<float, channels * Upsampler::partition_size> input_block{};
DYKODAC_DTCM std::array<float, channels * Upsampler::fft_size> spectral_accumulator{};
DYKODAC_DTCM std::array<float, Upsampler::fft_size> fft_scratch{};
DYKODAC_DTCM std::array<float, Upsampler::stage1_phase_count * channels *
                                     Upsampler::partition_size> ola_overlap{};
// Zero-wait copy of the half-band branch (the generated table lives in XIP).
DYKODAC_DTCM std::array<float, Upsampler::halfband_branch_taps> halfband_taps{};

arm_rfft_fast_instance_f32 fft_instance{};

#if defined(DYKODAC_HOST_TEST)
std::array<float, Upsampler::output_queue_frames * channels> debug_output_queue{};
std::array<float, Upsampler::output_queue_frames * channels> last_float_output{};
std::array<float, Upsampler::stage1_block_frames * channels> debug_stage1_block{};
#endif

constexpr std::uint32_t dither_seed_left = 0x9E37'79B9U;
constexpr std::uint32_t dither_seed_right = 0x7F4A'7C15U;

inline float decode_sample(const std::uint8_t* bytes) noexcept {
    const auto left_justified = static_cast<std::int32_t>(
        *reinterpret_cast<const PcmWord*>(bytes));
    // Every signed 24-bit integer is exactly representable by binary32.
    return static_cast<float>(left_justified >> 8U);
}

// round_half_away(value + dither) saturated to signed 24 bit: the only
// quantisation in the 4x path. binary32 resolves only 0.5 LSB above 2^22, so
// adding the dither directly to value would itself quantise the dither. Split
// value into its exact integer part and exact fraction first; the dither is
// added to the small fraction at full precision and the integer offset is
// rounded half away from zero with the sign of the complete sum.
inline std::int32_t quantise_sample(float value, float dither) noexcept {
    constexpr std::int32_t maximum = 0x007F'FFFF;
    constexpr std::int32_t minimum = -0x0080'0000;
    // |dither| < 1, so these bounds always saturate and keep the integer
    // conversion below in range.
    if (value >= 8'388'609.0F) return maximum;
    if (value <= -8'388'610.0F) return minimum;
    const auto whole = static_cast<std::int32_t>(value);          // exact, toward zero
    const float fraction = (value - static_cast<float>(whole)) + dither; // |fraction| < 2
    // IEEE addition never flips the sign of a non-zero exact sum.
    const bool negative = static_cast<float>(whole) + fraction < 0.0F;
    std::int32_t offset;
    if (negative) {
        const float shifted = fraction - 0.5F;
        offset = static_cast<std::int32_t>(shifted);               // toward zero
        if (static_cast<float>(offset) < shifted) ++offset;        // ceil
    } else {
        const float shifted = fraction + 0.5F;
        offset = static_cast<std::int32_t>(shifted);
        if (static_cast<float>(offset) > shifted) --offset;        // floor
    }
    const std::int32_t sample = whole + offset;
    if (sample > maximum) return maximum;
    if (sample < minimum) return minimum;
    return sample;
}

inline std::uint32_t xorshift32(std::uint32_t& state) noexcept {
    std::uint32_t x = state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    state = x;
    return x;
}

// TPDF dither in 24-bit LSB units: the difference of two independent uniform
// [0, 1) values with 24-bit resolution, so the result lies in (-1, 1) and is
// computed exactly (|a - b| < 2^24).
inline float tpdf_dither(std::uint32_t& state) noexcept {
    const auto a = static_cast<std::int32_t>(xorshift32(state) >> 8U);
    const auto b = static_cast<std::int32_t>(xorshift32(state) >> 8U);
    return static_cast<float>(a - b) * (1.0F / 16'777'216.0F);
}

inline std::size_t history_offset(std::size_t channel, std::size_t partition) noexcept {
    return (channel * Upsampler::partition_count + partition) * Upsampler::fft_size;
}

inline std::size_t overlap_offset(std::size_t phase, std::size_t channel) noexcept {
    return (phase * channels + channel) * Upsampler::partition_size;
}

inline void store_packed_queue(std::size_t frame, std::size_t channel,
                               std::int32_t sample) noexcept {
    const auto word = static_cast<std::uint32_t>(sample) & 0x00FF'FFFFU;
    auto* const frame_data = frame < Upsampler::ocram_output_queue_frames ?
        output_queue.data() + frame * packed_bytes_per_frame :
        output_queue_extension.data() +
            (frame - Upsampler::ocram_output_queue_frames) * packed_bytes_per_frame;
    auto* const destination = frame_data + channel * 3U;
    destination[0] = static_cast<std::uint8_t>(word);
    destination[1] = static_cast<std::uint8_t>(word >> 8U);
    destination[2] = static_cast<std::uint8_t>(word >> 16U);
}

inline void load_packed_queue(std::size_t frame, std::uint8_t* destination) noexcept {
    const auto* const source = frame < Upsampler::ocram_output_queue_frames ?
        output_queue.data() + frame * packed_bytes_per_frame :
        output_queue_extension.data() +
            (frame - Upsampler::ocram_output_queue_frames) * packed_bytes_per_frame;
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
    active_slice_ = slices_per_block;
    output_read_ = 0U;
    output_write_ = 0U;
    output_available_ = 0U;
    block_queue_ok_ = false;
    // A stream restart must never change loudness: keep the target and snap
    // the applied gain to it (the restart begins with startup zeros).
    gain_ = gain_target_;
    gain_step_ = output_rate_hz != 0U ?
        1.0F / (gain_ramp_seconds * static_cast<float>(output_rate_hz)) : 1.0F;
    dsp_.reset_state();
    native_bit_exact_ = false;
    dither_state_[0] = dither_seed_left;
    dither_state_[1] = dither_seed_right;
    fft_history.fill(0.0F);
    stage2_buffer.fill(0.0F);
    output_queue.fill(0U);
    output_queue_extension.fill(0U);
    input_block.fill(0.0F);
    spectral_accumulator.fill(0.0F);
    fft_scratch.fill(0.0F);
    ola_overlap.fill(0.0F);
    for (std::size_t tap = 0U; tap < halfband_branch_taps; ++tap)
        halfband_taps[tap] = detail::halfband_branch[tap];
#if defined(DYKODAC_HOST_TEST)
    debug_output_queue.fill(0.0F);
    last_float_output.fill(0.0F);
    debug_stage1_block.fill(0.0F);
    last_float_frames_ = 0U;
    stage1_blocks_ = 0U;
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

void PolyphaseUpsampler::set_gain_target(float linear) noexcept {
    // The negated comparison also maps NaN to silence.
    if (!(linear > 0.0F)) linear = 0.0F;
    if (linear > 1.0F) linear = 1.0F;
    gain_target_ = linear;
}

std::size_t PolyphaseUpsampler::input_frames_needed(std::size_t output_frames) const noexcept {
    if (input_rate_hz_ == 0U || output_frames == 0U) return 0U;
    if (!exact_4x_) return output_frames;
    return (output_frames + phase_count - 1U) / phase_count;
}

DYKODAC_FFT_HOT bool PolyphaseUpsampler::begin_fft_block() noexcept {
    if (input_fill_ != partition_size || active_slice_ != slices_per_block) return false;
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
    active_slice_ = 0U;
    return true;
}

void PolyphaseUpsampler::append_input(const std::uint8_t* input,
                                      std::size_t input_frames) noexcept {
    if (input == nullptr) return;
    // Neutral settings (the default) run exactly the pre-EQ code: the chain
    // is not entered at all, not even per sample.
    const bool processing = !dsp_.identity();
    for (std::size_t frame = 0U; frame < input_frames; ++frame) {
        if (input_fill_ == partition_size) {
            // With the production 1 ms cadence the four slices finish before
            // another 256-sample block can fill. Do not overwrite a full
            // block if a caller violates that bounded schedule.
            if (!begin_fft_block()) break;
        }
        float left = decode_sample(input + frame * pcm_bytes_per_frame);
        float right = decode_sample(input + frame * pcm_bytes_per_frame + 4U);
        if (processing) dsp_.process(left, right);
        input_block[input_fill_] = left;
        input_block[partition_size + input_fill_] = right;
        ++input_fill_;
    }
    if (input_fill_ == partition_size && active_slice_ == slices_per_block)
        (void)begin_fft_block();
}

DYKODAC_FFT_HOT void PolyphaseUpsampler::stage1_slice(std::size_t phase) noexcept {
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

    for (std::size_t channel = 0U; channel < channels; ++channel) {
        auto* const accumulator = channel == 0U ? left_accumulator : right_accumulator;
        arm_rfft_fast_f32(&fft_instance, accumulator, fft_scratch.data(), 1U);
        auto* const overlap = ola_overlap.data() + overlap_offset(phase, channel);
        // Stage-1 phases interleave in time on the 2x grid.
        auto* const u = stage2_buffer.data() + channel * stage2_stride +
                        halfband_history_frames + phase;
        for (std::size_t sample = 0U; sample < partition_size; ++sample)
            u[sample * stage1_phase_count] = fft_scratch[sample] + overlap[sample];
        std::memcpy(overlap, fft_scratch.data() + partition_size,
                    partition_size * sizeof(float));
    }
#if defined(DYKODAC_HOST_TEST)
    if (phase + 1U == stage1_phase_count) {
        for (std::size_t frame = 0U; frame < stage1_block_frames; ++frame)
            for (std::size_t channel = 0U; channel < channels; ++channel)
                debug_stage1_block[frame * channels + channel] =
                    stage2_buffer[channel * stage2_stride + halfband_history_frames + frame];
        ++stage1_blocks_;
    }
#endif
}

__attribute__((always_inline)) inline float PolyphaseUpsampler::next_gain() noexcept {
    // Per-output-frame linear slew toward the target; both channels share it.
    if (gain_ < gain_target_) {
        gain_ += gain_step_;
        if (gain_ > gain_target_) gain_ = gain_target_;
    } else if (gain_ > gain_target_) {
        gain_ -= gain_step_;
        if (gain_ < gain_target_) gain_ = gain_target_;
    }
    return gain_;
}

DYKODAC_FFT_HOT void PolyphaseUpsampler::stage2_slice(std::size_t half) noexcept {
    // Readers only consume published frames, so the next 1024 queue frames
    // after output_write_ are free whenever this holds. Reading between the
    // two stage-2 slices only increases free space, so the first half decides
    // for the whole block.
    if (half == 0U)
        block_queue_ok_ = output_queue_frames - output_available_ >= block_output_frames;
    const bool store = block_queue_ok_;
    const float* const taps = halfband_taps.data();
    // Index u[m - j] as base[history + m - j]; history >= every j, so the
    // index never goes below the start of the channel buffer.
    const float* const left_base = stage2_buffer.data();
    const float* const right_base = left_base + stage2_stride;
    const std::size_t first = half * stage2_slice_frames;
    for (std::size_t m = first; m < first + stage2_slice_frames; ++m) {
        // y[2m] = sum_j h[j] u[m - j]
        float left_even = 0.0F;
        float right_even = 0.0F;
        for (std::size_t tap = 0U; tap < halfband_branch_taps; ++tap) {
            left_even += taps[tap] * left_base[halfband_history_frames + m - tap];
            right_even += taps[tap] * right_base[halfband_history_frames + m - tap];
        }
        // y[2m + 1] = u[m - delay], exact pass-through.
        const float left_odd = left_base[halfband_history_frames + m - halfband_delay_frames];
        const float right_odd = right_base[halfband_history_frames + m - halfband_delay_frames];

        const std::size_t even_frame = (output_write_ + 2U * m) % output_queue_frames;
        const std::size_t odd_frame = (output_write_ + 2U * m + 1U) % output_queue_frames;
        // value * gain + TPDF dither, then round/saturate to 24 bit. Dither
        // is drawn for every frame (also at gain 1 and on silence) so both
        // channel sequences stay independent of the queue state.
        const float even_gain = next_gain();
        const std::int32_t left_even_out =
            quantise_sample(left_even * even_gain, tpdf_dither(dither_state_[0]));
        const std::int32_t right_even_out =
            quantise_sample(right_even * even_gain, tpdf_dither(dither_state_[1]));
        const float odd_gain = next_gain();
        const std::int32_t left_odd_out =
            quantise_sample(left_odd * odd_gain, tpdf_dither(dither_state_[0]));
        const std::int32_t right_odd_out =
            quantise_sample(right_odd * odd_gain, tpdf_dither(dither_state_[1]));
        if (store) {
            store_packed_queue(even_frame, 0U, left_even_out);
            store_packed_queue(even_frame, 1U, right_even_out);
            store_packed_queue(odd_frame, 0U, left_odd_out);
            store_packed_queue(odd_frame, 1U, right_odd_out);
#if defined(DYKODAC_HOST_TEST)
            debug_output_queue[even_frame * channels] = left_even;
            debug_output_queue[even_frame * channels + 1U] = right_even;
            debug_output_queue[odd_frame * channels] = left_odd;
            debug_output_queue[odd_frame * channels + 1U] = right_odd;
#endif
        }
    }

    if (half == 1U) {
        // Carry the newest u samples into the half-band history.
        for (std::size_t channel = 0U; channel < channels; ++channel) {
            auto* const buffer = stage2_buffer.data() + channel * stage2_stride;
            std::memcpy(buffer, buffer + stage1_block_frames,
                        halfband_history_frames * sizeof(float));
        }
        if (store) {
            output_write_ = (output_write_ + block_output_frames) % output_queue_frames;
            output_available_ += block_output_frames;
        }
    }
}

void PolyphaseUpsampler::process_one_slice() noexcept {
    if (active_slice_ >= slices_per_block) return;
    if (active_slice_ < stage1_phase_count) stage1_slice(active_slice_);
    else stage2_slice(active_slice_ - stage1_phase_count);
    ++active_slice_;
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

std::size_t PolyphaseUpsampler::process_native(const std::uint8_t* input,
                                               std::uint8_t* output,
                                               std::size_t frames) noexcept {
    // Bit-perfect only while nothing would change a sample. The dither
    // sequence is not advanced then, which is harmless: it restarts from an
    // arbitrary state the moment processing becomes non-trivial.
    native_bit_exact_ = dsp_.identity() && gain_ == 1.0F && gain_target_ == 1.0F;
#if defined(DYKODAC_HOST_TEST)
    last_float_frames_ = frames < output_queue_frames ? frames : output_queue_frames;
#endif
    if (native_bit_exact_) {
        std::memmove(output, input, frames * pcm_bytes_per_frame);
#if defined(DYKODAC_HOST_TEST)
        for (std::size_t frame = 0U; frame < last_float_frames_; ++frame) {
            last_float_output[frame * channels] = decode_sample(input + frame * pcm_bytes_per_frame);
            last_float_output[frame * channels + 1U] =
                decode_sample(input + frame * pcm_bytes_per_frame + 4U);
        }
#endif
        return frames;
    }
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        float left = decode_sample(input + frame * pcm_bytes_per_frame);
        float right = decode_sample(input + frame * pcm_bytes_per_frame + 4U);
        dsp_.process(left, right);
#if defined(DYKODAC_HOST_TEST)
        if (frame < last_float_frames_) {
            last_float_output[frame * channels] = left;
            last_float_output[frame * channels + 1U] = right;
        }
#endif
        const float gain = next_gain();
        const std::int32_t l = quantise_sample(left * gain, tpdf_dither(dither_state_[0]));
        const std::int32_t r = quantise_sample(right * gain, tpdf_dither(dither_state_[1]));
        auto* const destination = output + frame * pcm_bytes_per_frame;
        const auto lw = static_cast<std::uint32_t>(l) << 8U;
        const auto rw = static_cast<std::uint32_t>(r) << 8U;
        std::memcpy(destination, &lw, 4U);
        std::memcpy(destination + 4U, &rw, 4U);
    }
    return frames;
}

std::size_t PolyphaseUpsampler::process(const std::uint8_t* input,
                                        std::size_t input_frames,
                                        std::uint8_t* output,
                                        std::size_t output_frames) noexcept {
    if (input_rate_hz_ == 0U || output == nullptr || output_frames == 0U) return 0U;
    if (!exact_4x_) {
        const std::size_t produced = input_frames < output_frames ? input_frames : output_frames;
        if (produced == 0U || input == nullptr) return 0U;
        process_native(input, output, produced);
        position_numerator_ += static_cast<std::uint64_t>(produced) * input_rate_hz_;
        return produced;
    }

    const bool had_active_slice = active_slice_ < slices_per_block;
    append_input(input, input_frames);
    // Exactly one slice is evaluated per normal 1 ms refill: stage-1 phase 0,
    // stage-1 phase 1, then stage 2 for each half of the block. Four slices
    // finish before the next 256-frame source block, bounding both CPU and
    // FlexSPI demand.
    if (had_active_slice) process_one_slice();
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

void PolyphaseUpsampler::copy_last_stage1_block(float* output) const noexcept {
    if (output == nullptr) return;
    std::memcpy(output, debug_stage1_block.data(), debug_stage1_block.size() * sizeof(float));
}
#endif
} // namespace audio
