#include "app/app_controller.hpp"
#include "audio/sample_rate.hpp"
#include "audio/stream_buffer.hpp"
#include "audio/polyphase_upsampler.hpp"
#include "audio/feedback_controller.hpp"
#include "board/board_pins.hpp"
#include "config/board_config.hpp"
#include "devices/dac.hpp"
#include "usb/descriptor_model.hpp"
#include "usb/usb_device.hpp"
#include "usb/maintenance_protocol.hpp"
#include "usb/diagnostic_report.hpp"
#include "tests/host/fft_prototype_coefficients.hpp"
#include "audio/fft_filter_spectra.hpp"
#include "audio/halfband_coefficients.hpp"
#include "audio/volume.hpp"

extern "C" {
#include "arm_common_tables.h"
#include "arm_const_structs.h"
#include "dsp/transform_functions.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {
double fft_direct_peak_error = 0.0;
double fft_direct_worst_rms_error = 0.0;

unsigned long long checks_passed = 0U;

void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
    ++checks_passed;
}

constexpr double pi = 3.14159265358979323846;
using Upsampler = audio::PolyphaseUpsampler;

void pack_sample(std::uint8_t* destination, std::int32_t sample) {
    const std::uint32_t packed = (static_cast<std::uint32_t>(sample) & 0x00FF'FFFFU) << 8U;
    for (std::size_t byte = 0U; byte < 4U; ++byte)
        destination[byte] = static_cast<std::uint8_t>(packed >> (byte * 8U));
}

std::int32_t unpack_sample(const std::uint8_t* source) {
    const std::uint32_t word = static_cast<std::uint32_t>(source[0]) |
        (static_cast<std::uint32_t>(source[1]) << 8U) |
        (static_cast<std::uint32_t>(source[2]) << 16U) |
        (static_cast<std::uint32_t>(source[3]) << 24U);
    return static_cast<std::int32_t>(word) >> 8;
}

// Binary64 direct convolution with the equivalent single 4x prototype:
// output[n] = sum_k composite[k] * zero_stuffed_by_4(source)[n - k].
std::vector<double> direct_interpolate(const std::vector<std::int32_t>& source) {
    const auto& composite = audio::test_detail::composite_coefficients;
    const std::size_t taps = composite.size();
    std::vector<double> output(4U * (source.size() - 1U) + taps, 0.0);
    for (std::size_t n = 0U; n < output.size(); ++n) {
        const std::size_t first = n < taps ? 0U : (n - taps + 4U) / 4U;
        double sum = 0.0;
        for (std::size_t i = first; i < source.size() && 4U * i <= n; ++i)
            sum += composite[n - 4U * i] * static_cast<double>(source[i]);
        output[n] = sum;
    }
    return output;
}

struct ConverterRun {
    std::size_t latency{0U};
    std::vector<float> left;            // after stage 2, before gain/dither
    std::vector<float> right;
    std::vector<std::int32_t> left_int; // the actual 24-bit output
    std::vector<std::int32_t> right_int;
    std::vector<float> stage1_left;     // stage-1 stream u[] as produced
    std::vector<float> stage1_right;
};

// Streams source through the real converter at the production DMA cadence
// (rate/1000 output frames, rate/4000 input frames per call). When
// gain_change_call is reached the gain target is changed before that call.
ConverterRun run_converter(Upsampler& converter, const std::vector<std::int32_t>& left_source,
                           const std::vector<std::int32_t>& right_source,
                           std::uint32_t input_rate, std::size_t wanted_frames,
                           std::size_t gain_change_call = static_cast<std::size_t>(-1),
                           float gain_change = 1.0F) {
    const std::uint32_t output_rate = input_rate * 4U;
    const std::size_t dma_output_frames = output_rate / 1'000U;
    const std::size_t dma_input_frames = dma_output_frames / 4U;
    ConverterRun run;
    run.latency = output_rate / 100U;
    const std::size_t total = run.latency + wanted_frames;
    const std::size_t calls = (total + dma_output_frames - 1U) / dma_output_frames;
    std::vector<std::uint8_t> input(dma_input_frames * 8U, 0U);
    std::vector<std::uint8_t> output(dma_output_frames * 8U, 0U);
    std::vector<float> float_output(dma_output_frames * 2U, 0.0F);
    std::vector<float> stage1_block(Upsampler::stage1_block_frames * 2U, 0.0F);
    std::uint64_t stage1_blocks = converter.stage1_blocks_completed();
    std::size_t source_offset = 0U;
    for (std::size_t call = 0U; call < calls; ++call) {
        if (call == gain_change_call) converter.set_gain_target(gain_change);
        for (std::size_t frame = 0U; frame < dma_input_frames; ++frame) {
            const bool valid = source_offset < left_source.size();
            pack_sample(input.data() + frame * 8U, valid ? left_source[source_offset] : 0);
            pack_sample(input.data() + frame * 8U + 4U, valid ? right_source[source_offset] : 0);
            ++source_offset;
        }
        check(converter.process(input.data(), dma_input_frames, output.data(), dma_output_frames) ==
              dma_output_frames, "converter produces a complete DMA block");
        check(converter.copy_last_float_output(float_output.data(), dma_output_frames) ==
              dma_output_frames, "converter exposes pre-gain float output");
        for (std::size_t frame = 0U; frame < dma_output_frames; ++frame) {
            run.left.push_back(float_output[frame * 2U]);
            run.right.push_back(float_output[frame * 2U + 1U]);
            run.left_int.push_back(unpack_sample(output.data() + frame * 8U));
            run.right_int.push_back(unpack_sample(output.data() + frame * 8U + 4U));
            check(output[frame * 8U] == 0U && output[frame * 8U + 4U] == 0U,
                  "24-in-32 output keeps the padding byte zero");
        }
        const std::uint64_t blocks = converter.stage1_blocks_completed();
        check(blocks - stage1_blocks <= 1U, "at most one stage-1 block completes per refill");
        if (blocks != stage1_blocks) {
            converter.copy_last_stage1_block(stage1_block.data());
            for (std::size_t frame = 0U; frame < Upsampler::stage1_block_frames; ++frame) {
                run.stage1_left.push_back(stage1_block[frame * 2U]);
                run.stage1_right.push_back(stage1_block[frame * 2U + 1U]);
            }
            stage1_blocks = blocks;
        }
    }
    return run;
}

ConverterRun run_fresh(const std::vector<std::int32_t>& left_source,
                       const std::vector<std::int32_t>& right_source,
                       std::uint32_t input_rate, std::size_t wanted_frames, float gain = 1.0F) {
    Upsampler converter;
    converter.set_gain_target(gain);
    check(converter.configure(input_rate, input_rate * 4U), "test converter configures");
    return run_converter(converter, left_source, right_source, input_rate, wanted_frames);
}

std::vector<std::int32_t> negated(const std::vector<std::int32_t>& source) {
    std::vector<std::int32_t> result(source.size());
    for (std::size_t i = 0U; i < source.size(); ++i) result[i] = -source[i];
    return result;
}

void compare_fft_reference(const std::vector<std::int32_t>& source,
                           std::uint32_t input_rate, const char* name,
                           double tolerance) {
    const auto reference = direct_interpolate(source);
    const auto run = run_fresh(source, negated(source), input_rate, reference.size() + 512U);
    const auto& actual = run.left;
    const std::size_t latency = run.latency;
    check(actual.size() >= latency + reference.size(), "FFT result covers direct reference");
    for (std::size_t frame = 0U; frame < latency; ++frame)
        check(actual[frame] == 0.0F && run.left_int[frame] == 0 && run.right_int[frame] == 0,
              "startup latency is digital zero without dither");
    for (std::size_t frame = 0U; frame < actual.size(); ++frame)
        check(run.right[frame] == -actual[frame], "negated right channel is exactly negated");
    double maximum = 0.0;
    std::size_t maximum_frame = 0U;
    long double error_energy = 0.0L;
    for (std::size_t frame = 0U; frame < reference.size(); ++frame) {
        const double error = static_cast<double>(actual[latency + frame]) - reference[frame];
        if (std::abs(error) > maximum) {
            maximum = std::abs(error);
            maximum_frame = frame;
        }
        error_energy += static_cast<long double>(error) * error;
    }
    // After the composite response has decayed the converter must be silent.
    for (std::size_t frame = latency + reference.size(); frame < actual.size(); ++frame)
        check(std::abs(actual[frame]) < tolerance, "FFT output decays after the reference");
    const double rms = std::sqrt(static_cast<double>(error_energy / reference.size()));
    fft_direct_peak_error = std::max(fft_direct_peak_error, maximum);
    fft_direct_worst_rms_error = std::max(fft_direct_worst_rms_error, rms);
    if (maximum > tolerance) {
        long double best_energy = std::numeric_limits<long double>::max();
        int best_shift = 0;
        for (int shift = -300; shift <= 300; ++shift) {
            long double energy = 0.0L;
            std::size_t count = 0U;
            for (std::size_t frame = 300U; frame + 300U < reference.size(); ++frame) {
                const auto index = static_cast<std::size_t>(static_cast<long long>(latency + frame) + shift);
                const long double error = static_cast<long double>(actual[index]) - reference[frame];
                energy += error * error;
                ++count;
            }
            energy /= count;
            if (energy < best_energy) { best_energy = energy; best_shift = shift; }
        }
        std::cerr << "FAIL: " << name << " FFT/direct max=" << maximum
                  << " rms=" << rms << " frame=" << maximum_frame
                  << " actual=" << actual[latency + maximum_frame]
                  << " reference=" << reference[maximum_frame]
                  << " best_shift=" << best_shift
                  << " tolerance=" << tolerance << '\n';
        std::exit(1);
    }
    std::cout << "  " << name << " @" << input_rate << ": FFT/direct max=" << maximum
              << " rms=" << rms << '\n';
}

// Least-squares gain of the 24-bit output against the pre-gain float output.
double measured_gain(const ConverterRun& run, std::size_t first, std::size_t last) {
    long double cross = 0.0L;
    long double energy = 0.0L;
    for (std::size_t frame = first; frame < last; ++frame) {
        cross += static_cast<long double>(run.left_int[frame]) * run.left[frame];
        cross += static_cast<long double>(run.right_int[frame]) * run.right[frame];
        energy += static_cast<long double>(run.left[frame]) * run.left[frame];
        energy += static_cast<long double>(run.right[frame]) * run.right[frame];
    }
    return static_cast<double>(cross / energy);
}

struct MockDacTransport final : devices::DacTransport {
    void initialize() noexcept override { initialized = true; }
    void set_reset(bool asserted) noexcept override { reset = asserted; }
    void wait_after_reset_release() noexcept override { waited_after_reset_release = true; }
    void set_chip_select(bool asserted) noexcept override { chip_select = asserted; }
    bool transfer(const devices::DacCommand& command) noexcept override {
        check(chip_select, "CS asserted during DAC transfer");
        writes.push_back(command);
        return transfer_ok;
    }
    bool initialized{false};
    bool reset{false};
    bool waited_after_reset_release{false};
    bool chip_select{false};
    bool transfer_ok{true};
    std::vector<devices::DacCommand> writes;
};
}

int main() {
    std::array<std::uint8_t, 64> padded_diagnostic_command{};
    for (std::size_t i = 0; i < usb::maintenance::get_diagnostics.size(); ++i)
        padded_diagnostic_command[i] = usb::maintenance::get_diagnostics[i];
    check(usb::maintenance::is_get_diagnostics(padded_diagnostic_command.data(), padded_diagnostic_command.size()),
          "zero-padded diagnostic command accepted");
    check(usb::maintenance::is_get_events(usb::maintenance::get_events.data(), usb::maintenance::get_events.size()),
          "event-trace command accepted");
    check(usb::maintenance::is_get_dma_diagnostics(usb::maintenance::get_dma_diagnostics.data(),
          usb::maintenance::get_dma_diagnostics.size()), "DMA diagnostic command accepted");
    check(usb::maintenance::is_get_refill_timing(usb::maintenance::get_refill_timing.data(),
          usb::maintenance::get_refill_timing.size()), "refill timing command accepted");
    check(sizeof(usb::maintenance::DiagnosticReport) == 64U, "diagnostic report fits HID packet");
    check(sizeof(usb::EventTraceReport) == 64U, "event trace fits HID packet");
    check(sizeof(usb::maintenance::DmaDiagnosticReport) == 64U, "DMA diagnostic report fits HID packet");
    check(sizeof(usb::maintenance::RefillTimingReport) == 64U, "refill timing report fits HID packet");
    usb::UsbDevice usb_events;
    for (auto event : {usb::Event::Mounted, usb::Event::Controls, usb::Event::Start})
        check(usb_events.post_event(event), "USB burst event enqueued");
    check(usb_events.post_event({usb::Event::Rate, usb::Control::SampleRate, 0U, 44'100U}),
          "first rate payload enqueued");
    check(usb_events.post_event({usb::Event::Rate, usb::Control::SampleRate, 0U, 48'000U}),
          "second rate payload enqueued");
    check(usb_events.take_event().event == usb::Event::Mounted, "USB mount event preserved");
    check(usb_events.take_event().event == usb::Event::Controls, "USB control event preserves order");
    check(usb_events.take_event().event == usb::Event::Start, "USB start event preserved");
    const auto first_rate = usb_events.take_event();
    const auto second_rate = usb_events.take_event();
    check(first_rate.event == usb::Event::Rate && first_rate.control == usb::Control::SampleRate &&
          first_rate.value == 44'100U, "first queued rate keeps its value");
    check(second_rate.event == usb::Event::Rate && second_rate.value == 48'000U,
          "second queued rate keeps its value");
    check(usb_events.take_event().event == usb::Event::None, "USB event queue drains");
    check(board::pins::output_relay.safe_level == config::board::relay_disconnected_level, "relay safe polarity");
    check(board::pins::dac_reset.safe_level == config::board::dac_reset_asserted_level, "DAC reset polarity");
    check(audio::supported_format_count() == 2U, "initial format count");
    check(audio::supported_format(0U).format.sample_rate_hz == 44'100U &&
          audio::supported_format(0U).family == audio::ClockFamily::Family44k1,
          "power-on/default format is 44.1 kHz");
    check(audio::find_format(44'100U)->family == audio::ClockFamily::Family44k1, "44.1 clock family");
    check(audio::find_format(48'000U)->oscillator_hz == 24'576'000U, "48k oscillator");
    check(audio::find_format(44'100U)->output_sample_rate_hz == 176'400U &&
          audio::find_format(44'100U)->output_oscillator_hz == 22'579'200U &&
          audio::find_format(44'100U)->output_family == audio::ClockFamily::Family44k1 &&
          audio::find_format(44'100U)->dma_frames_per_transfer == 176U &&
          audio::find_format(48'000U)->output_sample_rate_hz == 192'000U &&
          audio::find_format(48'000U)->output_oscillator_hz == 24'576'000U &&
          audio::find_format(48'000U)->dma_frames_per_transfer == 192U,
          "each clock family has its own near-1-ms output block");
    check(audio::find_format(96'000U) == nullptr, "unsupported rate rejection");
    for (std::size_t i = 0; i < audio::supported_format_count(); ++i) {
        const auto& f = audio::supported_format(i);
        check(f.format.channels == usb::descriptor_model::channels, "descriptor channel count matches format table");
        check(f.format.container_bits / 8U == usb::descriptor_model::subslot_bytes, "descriptor subslot matches format table");
        check(f.format.valid_bits == usb::descriptor_model::valid_bits, "descriptor resolution matches format table");
        check(f.usb_hs_max_packet_bytes <= usb::descriptor_model::maximum_packet_bytes, "descriptor maximum packet covers format");
    }
    check(audio::usb_hs_packet_bytes(*audio::find_format(44'100U), 0U) == 40U, "44.1 first microframe packet");
    check(audio::usb_hs_packet_bytes(*audio::find_format(44'100U), 9U) == 48U, "44.1 fractional packet");
    check(audio::usb_hs_packet_bytes(*audio::find_format(48'000U), 0U) == 48U, "48k packet size");
    check(audio::find_format(48'000U)->usb_hs_max_packet_bytes == 56U,
          "asynchronous endpoint allows one positive correction frame");
    // ---- Two-stage interpolator geometry, derived from the generated headers.
    check(Upsampler::phase_count == 4U && Upsampler::stage1_phase_count == 2U &&
          Upsampler::stage1_prototype_taps == audio::test_detail::stage1_coefficients.size() &&
          Upsampler::stage1_taps_per_phase ==
              Upsampler::partition_count * Upsampler::partition_size &&
          Upsampler::partition_size == 256U && Upsampler::fft_size == 512U &&
          Upsampler::halfband_prototype_taps == audio::test_detail::stage2_coefficients.size() &&
          Upsampler::composite_taps == audio::test_detail::composite_coefficients.size() &&
          Upsampler::block_output_frames == 1024U && Upsampler::slices_per_block == 4U,
          "production two-stage interpolator geometry follows the generated design");
    check(Upsampler::fir_group_delay_output_samples_numerator() ==
              audio::test_detail::composite_coefficients.size() - 1U,
          "group delay follows the composite prototype");
    for (std::size_t phase = 0U; phase < Upsampler::stage1_phase_count; ++phase) {
        double gain = 0.0;
        for (std::size_t tap = phase; tap < Upsampler::stage1_prototype_taps; tap += 2U)
            gain += audio::test_detail::stage1_coefficients[tap];
        check(std::abs(gain - 1.0) < 2.0e-7, "each stage-1 phase has unity DC gain");
    }
    for (std::size_t tap = 0U; tap < Upsampler::stage1_prototype_taps; ++tap)
        check(audio::test_detail::stage1_coefficients[tap] ==
              audio::test_detail::stage1_coefficients[Upsampler::stage1_prototype_taps - 1U - tap],
              "stage-1 prototype remains exactly symmetric in float32");
    {
        const auto& stage2 = audio::test_detail::stage2_coefficients;
        const std::size_t centre = (stage2.size() - 1U) / 2U;
        double branch_gain = 0.0;
        for (std::size_t j = 0U; j < Upsampler::halfband_branch_taps; ++j) {
            check(audio::detail::halfband_branch[j] == stage2[2U * j],
                  "runtime half-band branch is the even stage-2 phase");
            branch_gain += audio::detail::halfband_branch[j];
        }
        check(std::abs(branch_gain - 1.0) < 2.0e-7, "half-band branch has unity DC gain");
        for (std::size_t tap = 1U; tap < stage2.size(); tap += 2U)
            check(stage2[tap] == (tap == centre ? 1.0F : 0.0F),
                  "odd stage-2 phase is an exact pure delay");
        check(centre == 2U * Upsampler::halfband_delay_frames + 1U,
              "pass-through delay matches the half-band centre");
    }
    {
        arm_rfft_fast_instance_f32 test_fft{};
        test_fft.Sint = arm_cfft_sR_f32_len256;
        test_fft.fftLenRFFT = 512U;
        test_fft.pTwiddleRFFT = twiddleCoef_rfft_512;
        float spectrum_error = 0.0F;
        for (std::size_t phase = 0U; phase < Upsampler::stage1_phase_count; ++phase) {
            for (std::size_t partition = 0U; partition < Upsampler::partition_count; ++partition) {
                std::array<float, 512U> test_time{};
                std::array<float, 512U> test_spectrum{};
                for (std::size_t tap = 0U; tap < 256U; ++tap)
                    test_time[tap] = audio::test_detail::stage1_coefficients[
                        (partition * 256U + tap) * 2U + phase];
                arm_rfft_fast_f32(&test_fft, test_time.data(), test_spectrum.data(), 0U);
                const std::size_t base = (phase * Upsampler::partition_count + partition) * 512U;
                for (std::size_t bin = 0U; bin < 512U; ++bin)
                    spectrum_error = std::max(spectrum_error,
                        std::abs(test_spectrum[bin] - audio::detail::fft_filter_spectra[base + bin]));
            }
        }
        check(spectrum_error < 2.0e-6F, "offline spectra match CMSIS packed RFFT convention");
    }
    {
        // The composite really is stage 1 zero-stuffed by two convolved with stage 2.
        const auto& s1 = audio::test_detail::stage1_coefficients;
        const auto& s2 = audio::test_detail::stage2_coefficients;
        double composite_error = 0.0;
        for (std::size_t n = 0U; n < Upsampler::composite_taps; ++n) {
            double sum = 0.0;
            for (std::size_t k = 0U; k < s2.size(); ++k) {
                if (k > n || (n - k) % 2U != 0U || (n - k) / 2U >= s1.size()) continue;
                sum += static_cast<double>(s2[k]) * static_cast<double>(s1[(n - k) / 2U]);
            }
            composite_error = std::max(composite_error,
                std::abs(sum - audio::test_detail::composite_coefficients[n]));
        }
        check(composite_error < 1.0e-15, "composite prototype equals stage1 (x) stage2");
    }

    audio::PolyphaseUpsampler upsampler;
    check(upsampler.configure(44'100U, 176'400U) &&
          upsampler.input_frames_needed(176U) == 44U,
          "44.1 to 176.4 kHz is exact 4x");
    check(upsampler.configure(48'000U, 192'000U) &&
          upsampler.input_frames_needed(192U) == 48U,
          "48 to 192 kHz is exact 4x");
    check(!upsampler.configure(88'200U, 176'400U) &&
          !upsampler.configure(96'000U, 192'000U),
          "unused general fractional and 2x SRC modes are removed");

    audio::PolyphaseUpsampler native;
    native.set_gain_target(0.25F);
    check(native.configure(176'400U, 176'400U), "native 176.4 kHz configuration");
    std::array<std::uint8_t, 32U> native_input{};
    for (std::size_t i = 0U; i < native_input.size(); ++i)
        native_input[i] = static_cast<std::uint8_t>(i * 13U);
    std::array<std::uint8_t, 32U> native_output{};
    check(native.input_frames_needed(4U) == 4U &&
          native.process(native_input.data(), 4U, native_output.data(), 4U) == 4U &&
          native_output == native_input,
          "native-rate path bypasses the FIR, gain and dither without changing PCM");

    // Direct binary64 convolution with the composite 4x prototype. These
    // vectors exercise every 256-frame partition and FFT boundary, both DMA
    // cadences (176/44 and 192/48) and many output-queue wraps.
    std::vector<std::int32_t> impulse(1'025U, 0);
    impulse[0] = 1'000;
    compare_fft_reference(impulse, 48'000U, "impulse", 2.0e-3);
    compare_fft_reference(impulse, 44'100U, "impulse", 2.0e-3);
    std::vector<std::int32_t> late_impulse(1'025U, 0);
    late_impulse[767] = 1'000; // last sample of a partition
    compare_fft_reference(late_impulse, 44'100U, "late impulse", 2.0e-3);
    std::vector<std::int32_t> dc(1'025U, 1'000);
    compare_fft_reference(dc, 44'100U, "DC", 8.0e-3);
    for (const double frequency : {1'000.0, 10'000.0, 19'000.0, 20'000.0}) {
        std::vector<std::int32_t> tone(1'025U, 0);
        const double rate = frequency == 1'000.0 ? 48'000.0 : 44'100.0;
        for (std::size_t sample = 0U; sample < tone.size(); ++sample)
            tone[sample] = static_cast<std::int32_t>(std::lround(
                1'000.0 * std::sin(2.0 * pi * frequency * static_cast<double>(sample) / rate)));
        compare_fft_reference(tone, static_cast<std::uint32_t>(rate), "sine", 1.2e-2);
    }
    std::vector<std::int32_t> multitone(1'025U, 0);
    for (std::size_t sample = 0U; sample < multitone.size(); ++sample) {
        const double time = static_cast<double>(sample) / 48'000.0;
        multitone[sample] = static_cast<std::int32_t>(std::lround(
            250.0 * (std::sin(2.0 * pi * 997.0 * time) +
                     std::sin(2.0 * pi * 7'001.0 * time) +
                     std::sin(2.0 * pi * 13'003.0 * time))));
    }
    compare_fft_reference(multitone, 48'000U, "multitone", 1.2e-2);
    std::vector<std::int32_t> sweep(1'025U, 0);
    for (std::size_t sample = 0U; sample < sweep.size(); ++sample) {
        const double t = static_cast<double>(sample);
        const double phase = 2.0 * pi *
            (100.0 * t / 44'100.0 + 0.5 * (20'000.0 - 100.0) *
             t * t / (44'100.0 * static_cast<double>(sweep.size())));
        sweep[sample] = static_cast<std::int32_t>(std::lround(1'000.0 * std::sin(phase)));
    }
    compare_fft_reference(sweep, 44'100U, "swept sine", 1.2e-2);
    std::mt19937 random_generator(0x8192U);
    std::uniform_int_distribution<std::int32_t> random_sample(-1'000, 1'000);
    std::vector<std::int32_t> random(1'537U, 0);
    for (auto& sample : random) sample = random_sample(random_generator);
    compare_fft_reference(random, 48'000U, "random", 1.5e-2);
    compare_fft_reference(random, 44'100U, "random", 1.5e-2);

    // ---- Stage-2 pass-through: y[2m+1] is bit-identical to u[m - delay].
    {
        const auto run = run_fresh(random, negated(random), 48'000U, 8'192U);
        const std::size_t delay = Upsampler::halfband_delay_frames;
        const std::size_t count = std::min(run.stage1_left.size(),
                                           (run.left.size() - run.latency) / 2U);
        check(count > 3U * Upsampler::stage1_block_frames, "pass-through test spans several blocks");
        std::size_t nonzero = 0U;
        double even_error = 0.0;
        for (std::size_t m = 0U; m < count; ++m) {
            const float left_odd = run.left[run.latency + 2U * m + 1U];
            const float right_odd = run.right[run.latency + 2U * m + 1U];
            const float left_expected = m < delay ? 0.0F : run.stage1_left[m - delay];
            const float right_expected = m < delay ? 0.0F : run.stage1_right[m - delay];
            check(std::memcmp(&left_odd, &left_expected, sizeof(float)) == 0 &&
                  std::memcmp(&right_odd, &right_expected, sizeof(float)) == 0,
                  "odd output samples are the stage-1 samples exactly");
            if (left_odd != 0.0F) ++nonzero;
            double even = 0.0;
            for (std::size_t j = 0U; j < Upsampler::halfband_branch_taps && j <= m; ++j)
                even += static_cast<double>(audio::detail::halfband_branch[j]) *
                        run.stage1_left[m - j];
            even_error = std::max(even_error,
                std::abs(even - static_cast<double>(run.left[run.latency + 2U * m])));
        }
        check(nonzero > count / 2U, "pass-through test carries signal");
        check(even_error < 1.0e-3, "even output samples are the half-band of stage 1");
    }

    // ---- MCU volume law: 10^((db + headroom) / 20), clamped to [-127, 0] dB.
    for (const std::int16_t db : {std::int16_t{0}, std::int16_t{-256}, std::int16_t{-6 * 256},
                                  std::int16_t{-15'488}, std::int16_t{-0x7F00}}) {
        const double expected = std::pow(10.0, (db / 256.0 +
            config::board::mcu_headroom_db_8_8 / 256.0) / 20.0);
        check(std::abs(audio::mcu_gain_from_uac2_db(db) / expected - 1.0) < 1.0e-6,
              "MCU gain follows the dB law with fixed headroom");
    }
    check(audio::mcu_gain_from_uac2_db(0x0100) == audio::mcu_gain_from_uac2_db(0) &&
          audio::mcu_gain_from_uac2_db(-0x7FFF) == audio::mcu_gain_from_uac2_db(-0x7F00),
          "MCU gain clamps host volume to [-127 dB, 0 dB]");
    check(audio::mcu_gain_from_uac2_db(0) < 0.7071F,
          "host 0 dB keeps at least 3.01 dB MCU headroom");
    check(config::board::dac_ceiling_db_8_8 == -9 * 256 &&
          config::board::dac_ceiling_db_8_8 + config::board::mcu_headroom_db_8_8 == -12 * 256 - 64,
          "host 0 dB is ceiling + headroom = -12.25 dB re DAC full scale");

    // ---- Gain accuracy through the 24-bit output (dither averages out).
    {
        std::vector<std::int32_t> tone_left(9'600U, 0);
        std::vector<std::int32_t> tone_right(9'600U, 0);
        for (std::size_t sample = 0U; sample < tone_left.size(); ++sample) {
            tone_left[sample] = static_cast<std::int32_t>(std::lround(
                2'000'000.0 * std::sin(2.0 * pi * 997.0 * static_cast<double>(sample) / 48'000.0)));
            tone_right[sample] = static_cast<std::int32_t>(std::lround(
                1'500'000.0 * std::sin(2.0 * pi * 3'001.0 * static_cast<double>(sample) / 48'000.0)));
        }
        for (const std::int16_t db : {std::int16_t{0}, std::int16_t{-6 * 256},
                                      std::int16_t{-20 * 256}, std::int16_t{-15'488}}) {
            const float gain = audio::mcu_gain_from_uac2_db(db);
            const auto run = run_fresh(tone_left, tone_right, 48'000U, 4U * tone_left.size(), gain);
            const std::size_t first = run.latency + Upsampler::composite_taps;
            const std::size_t last = run.latency + 4U * tone_left.size();
            const double measured = measured_gain(run, first, last);
            check(std::abs(measured / gain - 1.0) < 2.0e-5, "24-bit output gain matches the volume");
            std::cout << "  gain @" << db / 256.0 << " dB: expected=" << gain
                      << " measured=" << measured << '\n';
        }
    }

    // ---- Headroom: a 0 dBFS fs/4 sine at 45 degrees reconstructs to about
    // +3 dBFS and must not clip at host 0 dB.
    {
        std::vector<std::int32_t> quarter(4'800U, 0);
        for (std::size_t sample = 0U; sample < quarter.size(); ++sample)
            quarter[sample] = (sample & 2U) == 0U ? 8'388'607 : -8'388'607;
        const auto run = run_fresh(quarter, quarter, 48'000U, 4U * quarter.size(),
                                   audio::mcu_gain_from_uac2_db(0));
        float float_peak = 0.0F;
        std::int32_t int_peak = 0;
        bool clipped = false;
        for (std::size_t frame = run.latency; frame < run.left.size(); ++frame) {
            float_peak = std::max(float_peak, std::abs(run.left[frame]));
            int_peak = std::max(int_peak, std::abs(run.left_int[frame]));
            clipped = clipped || run.left_int[frame] >= 0x7F'FFFF ||
                      run.left_int[frame] <= -0x7F'FFFF || run.right_int[frame] >= 0x7F'FFFF ||
                      run.right_int[frame] <= -0x7F'FFFF;
        }
        float steady_peak = 0.0F;
        for (std::size_t frame = run.latency + Upsampler::composite_taps;
             frame < run.latency + 4U * quarter.size(); ++frame)
            steady_peak = std::max(steady_peak, std::abs(run.left[frame]));
        std::cout << "  headroom: steady peak=" << 20.0 * std::log10(steady_peak / 8'388'607.0)
                  << " dBFS (" << steady_peak << ")\n";
        std::cout << "  headroom: reconstructed peak=" << 20.0 * std::log10(float_peak / 8'388'607.0)
                  << " dBFS (" << float_peak << "), output peak="
                  << 20.0 * std::log10(int_peak / 8'388'607.0) << " dBFS (" << int_peak << ")\n";
        check(!clipped, "+3 dBFS intersample peaks do not clip at host 0 dB");
        check(float_peak > 8'388'607.0F * 1.40F, "fs/4 45-degree sine reconstructs to about +3 dBFS");
    }

    // ---- TPDF statistics of the 24-bit output against value * gain.
    {
        std::mt19937 dither_generator(0x7DFU);
        std::uniform_int_distribution<std::int32_t> wide(-1'000'000, 1'000'000);
        std::vector<std::int32_t> noise_left(16'000U, 0);
        std::vector<std::int32_t> noise_right(16'000U, 0);
        for (std::size_t i = 0U; i < noise_left.size(); ++i) {
            noise_left[i] = wide(dither_generator);
            noise_right[i] = wide(dither_generator);
        }
        const float gain = 0.6180339F;
        const auto run = run_fresh(noise_left, noise_right, 44'100U, 4U * noise_left.size(), gain);
        long double sum_l = 0.0L, sum_r = 0.0L, sq_l = 0.0L, sq_r = 0.0L, cross = 0.0L;
        double worst = 0.0;
        std::size_t count = 0U;
        for (std::size_t frame = run.latency; frame < run.latency + 4U * noise_left.size(); ++frame) {
            // The converter multiplies in binary32; replicate that product.
            const float left_scaled = run.left[frame] * gain;
            const float right_scaled = run.right[frame] * gain;
            const double el = run.left_int[frame] - static_cast<double>(left_scaled);
            const double er = run.right_int[frame] - static_cast<double>(right_scaled);
            worst = std::max(worst, std::max(std::abs(el), std::abs(er)));
            sum_l += el; sum_r += er; sq_l += el * el; sq_r += er * er; cross += el * er;
            ++count;
        }
        const double mean_l = static_cast<double>(sum_l / count);
        const double mean_r = static_cast<double>(sum_r / count);
        const double var_l = static_cast<double>(sq_l / count) - mean_l * mean_l;
        const double var_r = static_cast<double>(sq_r / count) - mean_r * mean_r;
        const double correlation = (static_cast<double>(cross / count) - mean_l * mean_r) /
                                   std::sqrt(var_l * var_r);
        std::cout << "  TPDF: n=" << count << " mean=" << mean_l << '/' << mean_r
                  << " var=" << var_l << '/' << var_r << " max|e|=" << worst
                  << " L/R corr=" << correlation << '\n';
        check(std::abs(mean_l) < 0.01 && std::abs(mean_r) < 0.01, "TPDF error mean is zero");
        check(std::abs(var_l - 0.25) < 0.01 && std::abs(var_r - 0.25) < 0.01,
              "TPDF + rounding error variance is 1/4 LSB^2");
        check(worst < 1.5, "TPDF error is bounded by 1.5 LSB");
        check(std::abs(correlation) < 0.02, "left and right dither are uncorrelated");

        // Digital silence through the 4x path is dithered, not truncated.
        const std::vector<std::int32_t> silence(4'000U, 0);
        const auto quiet = run_fresh(silence, silence, 48'000U, 4U * silence.size(), gain);
        long double silence_energy = 0.0L;
        std::size_t silence_count = 0U;
        for (std::size_t frame = quiet.latency; frame < quiet.left_int.size(); ++frame) {
            check(std::abs(quiet.left_int[frame]) <= 1 && std::abs(quiet.right_int[frame]) <= 1,
                  "dithered silence stays within one LSB");
            silence_energy += quiet.left_int[frame] * quiet.left_int[frame];
            ++silence_count;
        }
        const double silence_variance = static_cast<double>(silence_energy / silence_count);
        check(std::abs(silence_variance - 0.25) < 0.02, "silence carries the TPDF dither");
    }

    // ---- Gain ramp: slew-limited, reaches the target, survives configure().
    {
        const std::vector<std::int32_t> level(12'000U, 1'000'000);
        Upsampler ramp;
        ramp.set_gain_target(1.0F);
        check(ramp.configure(44'100U, 176'400U), "ramp converter configures");
        // Change the target well inside the steady DC region.
        const auto run = run_converter(ramp, level, level, 44'100U, 4U * level.size(), 60U, 0.25F);
        const double step = 1.0 / (0.020 * 176'400.0);
        const std::size_t first = run.latency + Upsampler::composite_taps;
        const std::size_t last = run.latency + 4U * level.size() - Upsampler::composite_taps;
        std::size_t ramp_start = 0U;
        std::size_t ramp_end = 0U;
        for (std::size_t frame = first; frame + 1U < last; ++frame) {
            const double delta = std::abs(static_cast<double>(run.left_int[frame + 1U]) -
                                          run.left_int[frame]);
            check(delta <= step * 1'000'000.0 + 3.0, "no gain step exceeds the slew limit");
            const double gain_now = run.left_int[frame] / static_cast<double>(run.left[frame]);
            if (ramp_start == 0U && gain_now < 1.0 - 4.0e-6) ramp_start = frame;
            if (ramp_start != 0U && ramp_end == 0U && gain_now < 0.25 + 4.0e-6) ramp_end = frame;
        }
        check(ramp_start != 0U && ramp_end != 0U, "gain ramp starts and reaches its target");
        const double ramp_frames = static_cast<double>(ramp_end - ramp_start);
        std::cout << "  ramp 1.0 -> 0.25: " << ramp_frames << " frames (ideal "
                  << 0.75 / step << ")\n";
        check(std::abs(ramp_frames - 0.75 / step) < 4.0, "ramp slews 0 -> 1 in 20 ms");
        check(ramp.current_gain() == 0.25F && ramp.gain_target() == 0.25F, "ramp settles at target");
        check(std::abs(measured_gain(run, ramp_end + 16U, last) - 0.25) < 2.0e-5,
              "settled output carries the target gain");

        check(ramp.configure(44'100U, 176'400U) && ramp.current_gain() == 0.25F &&
              ramp.gain_target() == 0.25F, "configure preserves gain");
        ramp.set_gain_target(0.5F);
        check(ramp.current_gain() == 0.25F, "a new target alone does not jump the gain");
        check(ramp.configure(48'000U, 192'000U) && ramp.current_gain() == 0.5F,
              "reset snaps the gain to the target");
        const auto restarted = run_converter(ramp, level, level, 48'000U, 4U * level.size());
        for (std::size_t frame = restarted.latency; frame < restarted.left.size(); ++frame) {
            const double expected = 0.5 * restarted.left[frame];
            check(std::abs(restarted.left_int[frame] - expected) < 1.5,
                  "a stream restart never plays at a different loudness");
        }
    }

    // ---- Saturation still clamps to signed 24 bit.
    {
        std::vector<std::int32_t> square(4'800U, 0);
        for (std::size_t sample = 0U; sample < square.size(); ++sample)
            square[sample] = (sample / 300U) % 2U == 0U ? 8'388'607 : -8'388'608;
        const auto run = run_fresh(square, square, 48'000U, 4U * square.size(), 1.0F);
        bool hit_maximum = false;
        bool hit_minimum = false;
        for (std::size_t frame = run.latency; frame < run.left.size(); ++frame) {
            const double value = run.left[frame];
            const std::int32_t sample = run.left_int[frame];
            check(sample <= 0x7F'FFFF && sample >= -0x80'0000, "output stays in 24-bit range");
            if (value >= 8'388'609.0) check(sample == 0x7F'FFFF, "positive overshoot saturates");
            if (value <= -8'388'610.0) check(sample == -0x80'0000, "negative overshoot saturates");
            if (std::abs(value) < 8'388'000.0)
                check(std::abs(sample - value) < 1.5, "unsaturated samples are dithered value");
            hit_maximum = hit_maximum || sample == 0x7F'FFFF;
            hit_minimum = hit_minimum || sample == -0x80'0000;
        }
        check(hit_maximum && hit_minimum, "Gibbs overshoot reaches both rails");
    }

    // ---- reset() clears every piece of streaming state (FFT history, OLA,
    // half-band history, stage-1 block, queue) and reseeds the dither.
    {
        const auto reference = run_fresh(impulse, negated(impulse), 48'000U, 6'000U, 0.5F);
        Upsampler dirty;
        dirty.set_gain_target(0.5F);
        check(dirty.configure(48'000U, 192'000U), "dirty converter configures");
        (void)run_converter(dirty, random, negated(random), 48'000U, 3'333U);
        check(dirty.configure(48'000U, 192'000U), "dirty converter reconfigures");
        const auto again = run_converter(dirty, impulse, negated(impulse), 48'000U, 6'000U);
        check(again.left == reference.left && again.right == reference.right &&
              again.left_int == reference.left_int && again.right_int == reference.right_int,
              "reset converter is bit-identical to a fresh one");
    }

    audio::StreamBuffer ring;
    std::array<std::uint8_t, audio::StreamBuffer::capacity_bytes> ring_data{};
    for (std::size_t i = 0; i < ring_data.size(); ++i) ring_data[i] = static_cast<std::uint8_t>(i);
    check(ring.push(ring_data.data(), ring_data.size()), "ring accepts exact capacity");
    check(!ring.push(ring_data.data(), 1U) && ring.overflows() == 1U, "ring rejects overflow");
    std::array<std::uint8_t, 64> ring_out{};
    check(ring.pop(ring_out.data(), ring_out.size()) == ring_out.size(), "ring pop boundary");
    check(ring_out[63] == 63U, "ring preserves data");
    ring.clear();
    check(ring.pop(ring_out.data(), ring_out.size()) == 0U && ring.underruns() == 1U, "ring zero-fills underrun");
    for (auto value : ring_out) check(value == 0U, "underrun output is silence");

    audio::FeedbackController feedback;
    const auto& f441 = *audio::find_format(44'100U);
    const auto& f480 = *audio::find_format(48'000U);
    feedback.reset(f441, 4096U);
    check(feedback.nominal() == (44'100U << 16U) / 8'000U, "44.1 nominal feedback");
    const auto low_fill = feedback.update(0U);
    check(low_fill > feedback.nominal(), "low fill increases feedback");
    for (int i = 0; i < 1000; ++i) feedback.update(0U);
    check(feedback.value() <= feedback.maximum(), "feedback upper bound");
    feedback.reset(f480, 4096U);
    check(feedback.nominal() == (48'000U << 16U) / 8'000U, "48k nominal feedback");
    const auto high_fill = feedback.update(8192U);
    check(high_fill < feedback.nominal(), "high fill decreases feedback");
    for (int i = 0; i < 1000; ++i) feedback.update(4096U);
    check(feedback.value() > feedback.minimum() && feedback.value() < feedback.maximum(), "feedback converges inside bounds");

    auto command = devices::write_command(devices::dac_registers::Register::Control8, 0xC0U);
    check(command.bytes[0] == 0x98U && command.bytes[1] == 0x08U && command.bytes[2] == 0xC0U, "DAC command encoding");
    check(devices::attenuation_from_uac2_db(0) == 0U, "unity attenuation");
    check(devices::attenuation_from_uac2_db(-256) == 2U, "minus 1 dB attenuation");

    MockDacTransport transport;
    devices::Cs4398 dac(transport, {10U, 2U, 3U});
    dac.begin(100U);
    check(transport.initialized && transport.reset && !transport.chip_select, "DAC begins reset-safe");
    check(dac.deadline_ms() == 110U, "DAC reset assertion deadline");
    dac.poll(109U);
    check(transport.reset && transport.writes.empty(), "DAC deadline does not fire early");
    dac.poll(110U);
    check(!transport.reset && transport.waited_after_reset_release &&
          transport.writes.size() == 1U && !transport.chip_select,
          "DAC enables control port immediately after reset release");
    for (std::uint32_t now : {113U, 115U, 118U, 120U, 123U, 125U, 128U, 130U}) dac.poll(now);
    check(dac.ready_muted() && transport.writes.size() == 5U, "DAC bootstrap completes muted");
    check(transport.writes[0].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x08U, 0xC0U}}, "DAC bootstrap write 1");
    check(transport.writes[1].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x02U, 0x12U}}, "DAC bootstrap write 2");
    check(transport.writes[2].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x07U, 0xB0U}}, "DAC bootstrap write 3");
    check(transport.writes[3].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x08U, 0x40U}}, "DAC bootstrap write 4");
    check(transport.writes[4].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x04U, 0x18U}},
          "DAC explicit safety mute keeps automatic silence mute disabled");
    check(!transport.reset && !transport.chip_select, "DAC ready state remains reset-released and CS inactive");
    check(dac.configure_format(f441), "DAC format configuration");
    check(transport.writes[5].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x02U, 0x12U}},
          "DAC enters quad-speed I2S mode at 192 kHz");
    check(transport.writes[6].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x07U, 0xB0U}},
          "DAC keeps fast internal interpolation and external analog filter");
    // The firmware writes only the fixed safety ceiling to 05h/06h (after
    // bootstrap and with every format configuration, before any unmute).
    check(devices::attenuation_from_uac2_db(config::board::dac_ceiling_db_8_8) == 18U,
          "-9 dB ceiling is 18 half-dB DAC steps");
    check(dac.set_attenuation(config::board::dac_ceiling_db_8_8), "DAC ceiling write");
    check(transport.writes.size() == 9U &&
          transport.writes[7].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x05U, 0x12U}} &&
          transport.writes[8].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x06U, 0x12U}} &&
          dac.attenuation_db_8_8() == config::board::dac_ceiling_db_8_8,
          "DAC ceiling sets both channel attenuation registers");
    {
        // Host volume is routed to the MCU gain exactly as main() does
        // (AudioEngine::set_volume_db -> PolyphaseUpsampler::set_gain_target);
        // it must not produce any DAC control-port traffic.
        const std::size_t writes_before = transport.writes.size();
        audio::PolyphaseUpsampler volume_sink;
        for (const std::int16_t db : {std::int16_t{0}, std::int16_t{-256}, std::int16_t{-0x3000},
                                      std::int16_t{-0x7F00}, std::int16_t{-128}}) {
            volume_sink.set_gain_target(audio::mcu_gain_from_uac2_db(db));
            check(volume_sink.gain_target() == audio::mcu_gain_from_uac2_db(db),
                  "host volume retargets the MCU gain");
        }
        check(transport.writes.size() == writes_before &&
              dac.attenuation_db_8_8() == config::board::dac_ceiling_db_8_8,
              "host volume writes no DAC attenuation register");
        for (const auto& write : transport.writes)
            check((write.bytes[1] != 0x05U && write.bytes[1] != 0x06U) ||
                  write.bytes[2] == 0x12U, "DAC attenuation registers only ever hold the ceiling");
    }
    check(dac.set_mute(false) && !dac.muted(), "DAC unmute command");
    check(transport.writes.back().bytes == std::array<std::uint8_t, 3>{{0x98U, 0x04U, 0x00U}},
          "DAC unmute disables automatic and explicit mute");
    check(dac.set_mute(true) && dac.muted(), "DAC mute command");
    check(transport.writes.back().bytes == std::array<std::uint8_t, 3>{{0x98U, 0x04U, 0x18U}},
          "DAC explicit mute does not re-enable automatic silence mute");

    {
        // A failed ceiling write takes the DAC to its reset-safe fault state;
        // main() then takes the VolumeControl safe-stop path.
        MockDacTransport ceiling_transport;
        devices::Cs4398 ceiling_dac(ceiling_transport, {0U, 0U, 0U});
        ceiling_dac.begin(0U);
        for (int i = 0; i < 16 && !ceiling_dac.ready_muted(); ++i) ceiling_dac.poll(0U);
        check(ceiling_dac.ready_muted(), "ceiling test DAC boots");
        ceiling_transport.transfer_ok = false;
        check(!ceiling_dac.set_attenuation(config::board::dac_ceiling_db_8_8) &&
              ceiling_dac.state() == devices::Cs4398::State::Fault && ceiling_transport.reset &&
              ceiling_dac.muted() && !ceiling_transport.chip_select,
              "failed ceiling write returns the DAC to reset-safe fault");
    }

    MockDacTransport wrap_transport;
    devices::Cs4398 wrap_dac(wrap_transport, {10U, 2U, 3U});
    wrap_dac.begin(0xFFFF'FFFAU);
    wrap_dac.poll(3U);
    check(wrap_transport.reset, "DAC deadline handles monotonic wrap before expiry");
    wrap_dac.poll(4U);
    check(!wrap_transport.reset, "DAC deadline handles monotonic wrap at expiry");

    MockDacTransport failed_transport;
    failed_transport.transfer_ok = false;
    devices::Cs4398 failed_dac(failed_transport, {0U, 0U, 0U});
    failed_dac.begin(0U);
    failed_dac.poll(0U);
    failed_dac.poll(0U);
    failed_dac.poll(0U);
    check(failed_dac.state() == devices::Cs4398::State::Fault && failed_transport.reset &&
          !failed_transport.chip_select, "DAC transfer failure returns to reset-safe state");

    app::AppController controller;
    check(!controller.outputs().relay_connected && controller.outputs().dac_muted, "boot is safe");
    check(controller.dispatch(app::Event::Initialize), "start initialization");
    check(controller.dispatch(app::Event::Initialized), "finish initialization");
    check(controller.dispatch(app::Event::StreamRequested), "prepare stream");
    check(!controller.outputs().relay_connected && controller.outputs().dac_muted, "prepare remains muted");
    check(controller.dispatch(app::Event::AudioStable), "audio stable");
    check(controller.outputs().relay_connected && !controller.outputs().dac_muted, "playing connects output");
    check(controller.dispatch(app::Event::Fault), "fault accepted");
    check(!controller.outputs().relay_connected && controller.outputs().dac_muted && !controller.outputs().audio_running, "fault is safe");
    check(controller.dispatch(app::Event::ClearFault) &&
          controller.state() == app::State::IdleMuted,
          "host lifecycle can clear a latched stream fault");
    for (auto safety_event : {app::Event::Detach, app::Event::Suspend, app::Event::DmaFailure,
                              app::Event::ClockFailure}) {
        app::AppController safety;
        safety.dispatch(app::Event::Initialize); safety.dispatch(app::Event::Initialized);
        safety.dispatch(app::Event::StreamRequested); safety.dispatch(app::Event::AudioStable);
        safety.dispatch(safety_event);
        check(!safety.outputs().relay_connected && safety.outputs().dac_muted &&
              !safety.outputs().audio_running, "all stop/fault events enforce relay safety");
    }

    std::cout << "FFT/direct peak error: " << fft_direct_peak_error
              << ", worst vector RMS error: " << fft_direct_worst_rms_error << '\n';
    std::cout << "Checks passed: " << checks_passed << '\n';
    std::cout << "All DykoDAC2 host tests passed\n";
    return 0;
}
