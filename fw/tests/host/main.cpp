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
#include "platform/nxp/sha256.hpp"
#include "platform/nxp/update_layout.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
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
    platform::nxp::crypto::Sha256 sha;
    sha.update("abc", 3U);
    std::uint8_t sha_result[32]{};
    sha.finish(sha_result);
    constexpr std::uint8_t sha_abc[32]{
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    check(std::equal(std::begin(sha_result), std::end(sha_result), std::begin(sha_abc)), "SHA-256 known vector");
    check(platform::nxp::update::crc32("123456789", 9U) == 0xCBF43926U, "CRC-32 known vector");
    check(usb::maintenance::is_enter_rom(usb::maintenance::enter_rom.data(),
          usb::maintenance::enter_rom.size()), "ROM command magic accepted");
    auto wrong_rom_command = usb::maintenance::enter_rom;
    wrong_rom_command[0] ^= 1U;
    check(!usb::maintenance::is_enter_rom(wrong_rom_command.data(), wrong_rom_command.size()),
          "near-match ROM command rejected");
    check(usb::maintenance::is_enter_updater(usb::maintenance::enter_updater.data(),
          usb::maintenance::enter_updater.size()), "firmware updater command accepted");
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
    check(audio::find_format(44'100U)->dma_frames_per_transfer == 192U &&
          audio::find_format(48'000U)->dma_frames_per_transfer == 192U,
          "DMA uses 192-frame / 1-ms output blocks");
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

    audio::PolyphaseUpsampler upsampler;
    check(upsampler.configure(44'100U), "44.1 kHz upsampler configuration");
    constexpr std::size_t output_frames = 6'400U;
    const std::size_t input_needed = upsampler.input_frames_needed(output_frames);
    std::vector<std::uint8_t> upsample_input(input_needed * 8U, 0U);
    std::vector<std::uint8_t> upsample_output(output_frames * 8U, 0U);
    check(upsampler.process(upsample_input.data(), input_needed, upsample_output.data(), output_frames) == output_frames,
          "44.1 kHz upsampler produces a complete output block");
    check(upsampler.position_numerator() == static_cast<std::uint64_t>(output_frames) * 44'100U,
          "44.1 kHz rational phase accumulator is exact");
    check(upsampler.configure(48'000U), "48 kHz upsampler configuration");
    check(upsampler.input_frames_needed(192U) == 80U,
          "48 kHz phase scheduler includes the fixed FIR lookahead");
    const std::size_t dc_input_frames = upsampler.input_frames_needed(192U);
    std::vector<std::uint8_t> dc_input(dc_input_frames * 8U, 0U);
    for (std::size_t frame = 0U; frame < dc_input_frames; ++frame) {
        dc_input[frame * 8U + 1U] = 0xFFU;
        dc_input[frame * 8U + 2U] = 0xFFU;
        dc_input[frame * 8U + 3U] = 0x7FU;
        dc_input[frame * 8U + 5U] = 0xFFU;
        dc_input[frame * 8U + 6U] = 0xFFU;
        dc_input[frame * 8U + 7U] = 0x7FU;
    }
    std::fill(upsample_output.begin(), upsample_output.end(), 0U);
    check(upsampler.process(dc_input.data(), dc_input_frames, upsample_output.data(), 192U) == 192U,
          "48 kHz upsampler produces a complete DC block");
    check(upsample_output[160U * 8U + 3U] >= 0x7EU &&
          upsample_output[160U * 8U + 7U] >= 0x7EU,
          "polyphase DC response remains near unity after FIR warm-up");

    // The production path stages one 192-frame block before convolution;
    // large calls use the bounded streaming path. They must remain exactly
    // equivalent across mirrored-history wrap boundaries.
    constexpr std::size_t comparison_frames = 192U * 10U;
    audio::PolyphaseUpsampler full_call;
    audio::PolyphaseUpsampler blocked_call;
    check(full_call.configure(44'100U) && blocked_call.configure(44'100U),
          "comparison upsamplers configure");
    const std::size_t comparison_input_frames = full_call.input_frames_needed(comparison_frames);
    std::vector<std::uint8_t> comparison_input(comparison_input_frames * 8U, 0U);
    for (std::size_t frame = 0U; frame < comparison_input_frames; ++frame) {
        const std::int32_t left = static_cast<std::int32_t>((frame * 104'729U) & 0x00FF'FFFFU) - 0x0080'0000;
        const std::int32_t right = -left;
        const std::uint32_t packed_left = (static_cast<std::uint32_t>(left) & 0x00FF'FFFFU) << 8U;
        const std::uint32_t packed_right = (static_cast<std::uint32_t>(right) & 0x00FF'FFFFU) << 8U;
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            comparison_input[frame * 8U + byte] = static_cast<std::uint8_t>(packed_left >> (byte * 8U));
            comparison_input[frame * 8U + 4U + byte] = static_cast<std::uint8_t>(packed_right >> (byte * 8U));
        }
    }
    std::vector<std::uint8_t> full_output(comparison_frames * 8U, 0U);
    std::vector<std::uint8_t> blocked_output(comparison_frames * 8U, 0U);
    check(full_call.process(comparison_input.data(), comparison_input_frames,
                            full_output.data(), comparison_frames) == comparison_frames,
          "large-call reference produces all frames");
    std::size_t comparison_input_offset = 0U;
    for (std::size_t output_offset = 0U; output_offset < comparison_frames; output_offset += 192U) {
        const std::size_t needed = blocked_call.input_frames_needed(192U);
        check(blocked_call.process(comparison_input.data() + comparison_input_offset * 8U, needed,
                                   blocked_output.data() + output_offset * 8U, 192U) == 192U,
              "blocked production path produces all frames");
        comparison_input_offset += needed;
    }
    check(comparison_input_offset == comparison_input_frames,
          "blocked and large-call paths consume the same input schedule");
    check(full_output == blocked_output,
          "mirrored production path is bit-exact with bounded streaming path");

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

    std::cout << "All DykoDAC2 host tests passed\n";
    return 0;
}
