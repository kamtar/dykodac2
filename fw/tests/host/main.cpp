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

extern "C" {
#include "arm_common_tables.h"
#include "arm_const_structs.h"
#include "dsp/transform_functions.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {
double fft_direct_peak_error = 0.0;
double fft_direct_worst_rms_error = 0.0;

void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void pack_sample(std::uint8_t* destination, std::int32_t sample) {
    const std::uint32_t packed = (static_cast<std::uint32_t>(sample) & 0x00FF'FFFFU) << 8U;
    for (std::size_t byte = 0U; byte < 4U; ++byte)
        destination[byte] = static_cast<std::uint8_t>(packed >> (byte * 8U));
}

std::vector<double> direct_interpolate(const std::vector<std::int32_t>& source) {
    const std::size_t output_source_frames =
        source.size() + audio::PolyphaseUpsampler::taps_per_phase - 1U;
    std::vector<double> output(output_source_frames * 4U, 0.0);
    for (std::size_t n = 0U; n < output_source_frames; ++n) {
        const std::size_t first_tap = n < source.size() ? 0U : n - source.size() + 1U;
        const std::size_t last_tap = n < audio::PolyphaseUpsampler::taps_per_phase ?
            n : audio::PolyphaseUpsampler::taps_per_phase - 1U;
        if (first_tap > last_tap) continue;
        for (std::size_t phase = 0U; phase < 4U; ++phase) {
            double sum = 0.0;
            for (std::size_t tap = first_tap; tap <= last_tap; ++tap) {
                sum += static_cast<double>(source[n - tap]) *
                    static_cast<double>(audio::test_detail::prototype_coefficients[tap * 4U + phase]);
            }
            output[n * 4U + phase] = sum;
        }
    }
    return output;
}

std::vector<float> fft_interpolate(const std::vector<std::int32_t>& source,
                                   std::uint32_t input_rate) {
    const std::uint32_t output_rate = input_rate * 4U;
    const std::size_t dma_output_frames = output_rate / 1'000U;
    const std::size_t dma_input_frames = dma_output_frames / 4U;
    const std::size_t latency_frames = output_rate / 100U;
    const std::size_t reference_frames =
        (source.size() + audio::PolyphaseUpsampler::taps_per_phase - 1U) * 4U;
    const std::size_t wanted_frames = latency_frames + reference_frames + dma_output_frames;
    const std::size_t calls = (wanted_frames + dma_output_frames - 1U) / dma_output_frames;

    audio::PolyphaseUpsampler converter;
    check(converter.configure(input_rate, output_rate), "FFT test converter configures");
    std::vector<float> result;
    result.reserve(calls * dma_output_frames);
    std::vector<std::uint8_t> input(dma_input_frames * 8U, 0U);
    std::vector<std::uint8_t> output(dma_output_frames * 8U, 0U);
    std::vector<float> float_output(dma_output_frames * 2U, 0.0F);
    std::size_t source_offset = 0U;
    for (std::size_t call = 0U; call < calls; ++call) {
        std::fill(input.begin(), input.end(), 0U);
        for (std::size_t frame = 0U; frame < dma_input_frames; ++frame) {
            const std::int32_t value = source_offset < source.size() ? source[source_offset] : 0;
            pack_sample(input.data() + frame * 8U, value);
            pack_sample(input.data() + frame * 8U + 4U, -value);
            ++source_offset;
        }
        check(converter.process(input.data(), dma_input_frames, output.data(), dma_output_frames) ==
              dma_output_frames, "FFT test produces a complete DMA block");
        check(converter.copy_last_float_output(float_output.data(), dma_output_frames) ==
              dma_output_frames, "FFT test exposes pre-quantization output");
        for (std::size_t frame = 0U; frame < dma_output_frames; ++frame)
            result.push_back(float_output[frame * 2U]);
    }
    return result;
}

void compare_fft_reference(const std::vector<std::int32_t>& source,
                           std::uint32_t input_rate, const char* name,
                           double tolerance) {
    const auto reference = direct_interpolate(source);
    const auto actual = fft_interpolate(source, input_rate);
    const std::size_t latency = input_rate * 4U / 100U;
    check(actual.size() >= latency + reference.size(), "FFT result covers direct reference");
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
    const double rms = std::sqrt(static_cast<double>(error_energy / reference.size()));
    fft_direct_peak_error = std::max(fft_direct_peak_error, maximum);
    fft_direct_worst_rms_error = std::max(fft_direct_worst_rms_error, rms);
    if (maximum > tolerance) {
        std::size_t actual_peak = 0U;
        std::size_t actual_first = 0U;
        while (actual_first < actual.size() && std::abs(actual[actual_first]) < 1.0e-12F)
            ++actual_first;
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
        for (std::size_t frame = 1U; frame < reference.size(); ++frame)
            if (std::abs(actual[latency + frame]) > std::abs(actual[latency + actual_peak]))
                actual_peak = frame;
        std::cerr << "FAIL: " << name << " FFT/direct max=" << maximum
                  << " rms=" << rms << " frame=" << maximum_frame
                  << " actual=" << actual[latency + maximum_frame]
                  << " reference=" << reference[maximum_frame]
                  << " actual_peak_frame=" << actual_peak
                  << " actual_first=" << actual_first
                  << " best_shift=" << best_shift
                  << " tolerance=" << tolerance << '\n';
        std::exit(1);
    }
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
    check(audio::PolyphaseUpsampler::prototype_taps == 8192U &&
          audio::PolyphaseUpsampler::phase_count == 4U &&
          audio::PolyphaseUpsampler::taps_per_phase == 2048U &&
          audio::PolyphaseUpsampler::partition_size == 256U &&
          audio::PolyphaseUpsampler::fft_size == 512U &&
          audio::PolyphaseUpsampler::partition_count == 8U,
          "production FFT interpolator geometry is fixed");
    for (std::size_t phase = 0U; phase < 4U; ++phase) {
        double gain = 0.0;
        for (std::size_t tap = 0U; tap < 2048U; ++tap)
            gain += audio::test_detail::prototype_coefficients[tap * 4U + phase];
        check(std::abs(gain - 1.0) < 2.0e-7, "each exact fractional phase has unity DC gain");
    }
    for (std::size_t tap = 0U; tap < 8192U; ++tap)
        check(audio::test_detail::prototype_coefficients[tap] ==
              audio::test_detail::prototype_coefficients[8191U - tap],
              "prototype remains exactly symmetric in float32");
    arm_rfft_fast_instance_f32 test_fft{};
    test_fft.Sint = arm_cfft_sR_f32_len256;
    test_fft.fftLenRFFT = 512U;
    test_fft.pTwiddleRFFT = twiddleCoef_rfft_512;
    std::array<float, 512U> test_time{};
    std::array<float, 512U> test_spectrum{};
    for (std::size_t tap = 0U; tap < 256U; ++tap)
        test_time[tap] = audio::test_detail::prototype_coefficients[tap * 4U];
    arm_rfft_fast_f32(&test_fft, test_time.data(), test_spectrum.data(), 0U);
    float spectrum_error = 0.0F;
    for (std::size_t bin = 0U; bin < 512U; ++bin)
        spectrum_error = std::max(spectrum_error,
            std::abs(test_spectrum[bin] - audio::detail::fft_filter_spectra[bin]));
    check(spectrum_error < 2.0e-6F, "offline spectra match CMSIS packed RFFT convention");

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
    check(native.configure(176'400U, 176'400U), "native 176.4 kHz configuration");
    std::array<std::uint8_t, 32U> native_input{};
    for (std::size_t i = 0U; i < native_input.size(); ++i)
        native_input[i] = static_cast<std::uint8_t>(i * 13U);
    std::array<std::uint8_t, 32U> native_output{};
    check(native.input_frames_needed(4U) == 4U &&
          native.process(native_input.data(), 4U, native_output.data(), 4U) == 4U &&
          native_output == native_input,
          "native-rate path bypasses the FIR without changing PCM");

    // Direct binary64 convolution uses the exact checked-in float32 prototype.
    // These vectors exercise every 256-frame partition and FFT boundary.
    std::vector<std::int32_t> impulse(1'025U, 0);
    impulse[0] = 1'000;
    compare_fft_reference(impulse, 48'000U, "impulse", 2.0e-3);
    std::vector<std::int32_t> dc(1'025U, 1'000);
    compare_fft_reference(dc, 44'100U, "DC", 8.0e-3);
    for (const double frequency : {1'000.0, 10'000.0, 19'000.0, 20'000.0}) {
        std::vector<std::int32_t> tone(1'025U, 0);
        const double rate = frequency == 1'000.0 ? 48'000.0 : 44'100.0;
        for (std::size_t sample = 0U; sample < tone.size(); ++sample)
            tone[sample] = static_cast<std::int32_t>(std::lround(
                1'000.0 * std::sin(2.0 * 3.14159265358979323846 * frequency * sample / rate)));
        compare_fft_reference(tone, static_cast<std::uint32_t>(rate), "sine", 1.2e-2);
    }
    std::vector<std::int32_t> multitone(1'025U, 0);
    for (std::size_t sample = 0U; sample < multitone.size(); ++sample) {
        const double time = static_cast<double>(sample) / 48'000.0;
        multitone[sample] = static_cast<std::int32_t>(std::lround(
            250.0 * (std::sin(2.0 * 3.14159265358979323846 * 997.0 * time) +
                     std::sin(2.0 * 3.14159265358979323846 * 7'001.0 * time) +
                     std::sin(2.0 * 3.14159265358979323846 * 13'003.0 * time))));
    }
    compare_fft_reference(multitone, 48'000U, "multitone", 1.2e-2);
    std::vector<std::int32_t> sweep(1'025U, 0);
    for (std::size_t sample = 0U; sample < sweep.size(); ++sample) {
        const double fraction = static_cast<double>(sample) / (sweep.size() - 1U);
        const double phase = 2.0 * 3.14159265358979323846 *
            (100.0 * sample / 44'100.0 + 0.5 * (20'000.0 - 100.0) *
             sample * sample / (44'100.0 * sweep.size()));
        sweep[sample] = static_cast<std::int32_t>(std::lround(1'000.0 * std::sin(phase + fraction * 0.0)));
    }
    compare_fft_reference(sweep, 44'100U, "swept sine", 1.2e-2);
    std::mt19937 random_generator(0x8192U);
    std::uniform_int_distribution<std::int32_t> random_sample(-1'000, 1'000);
    std::vector<std::int32_t> random(1'537U, 0);
    for (auto& sample : random) sample = random_sample(random_generator);
    compare_fft_reference(random, 48'000U, "random", 1.5e-2);

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
    check(dac.set_attenuation(-256), "DAC volume update");
    check(transport.writes[7].bytes[1] == 0x05U && transport.writes[8].bytes[1] == 0x06U &&
          transport.writes[7].bytes[2] == transport.writes[8].bytes[2], "DAC volume updates both channels");
    check(dac.set_mute(false) && !dac.muted(), "DAC unmute command");
    check(transport.writes.back().bytes == std::array<std::uint8_t, 3>{{0x98U, 0x04U, 0x00U}},
          "DAC unmute disables automatic and explicit mute");
    check(dac.set_mute(true) && dac.muted(), "DAC mute command");
    check(transport.writes.back().bytes == std::array<std::uint8_t, 3>{{0x98U, 0x04U, 0x18U}},
          "DAC explicit mute does not re-enable automatic silence mute");

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
    std::cout << "All DykoDAC2 host tests passed\n";
    return 0;
}
