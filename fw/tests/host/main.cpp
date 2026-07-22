#include "app/app_controller.hpp"
#include "audio/sample_rate.hpp"
#include "audio/stream_buffer.hpp"
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
    void set_chip_select(bool asserted) noexcept override { chip_select = asserted; }
    bool transfer(const devices::DacCommand& command) noexcept override {
        check(chip_select, "CS asserted during DAC transfer");
        writes.push_back(command);
        return transfer_ok;
    }
    bool initialized{false};
    bool reset{false};
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
    check(sizeof(usb::maintenance::DiagnosticReport) == 64U, "diagnostic report fits HID packet");
    check(sizeof(usb::EventTraceReport) == 64U, "event trace fits HID packet");
    check(sizeof(usb::maintenance::DmaDiagnosticReport) == 64U, "DMA diagnostic report fits HID packet");
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
    devices::Cs4398 dac(transport, {10U, 20U, 2U, 3U});
    dac.begin(100U);
    check(transport.initialized && transport.reset && !transport.chip_select, "DAC begins reset-safe");
    check(dac.deadline_ms() == 110U, "DAC reset assertion deadline");
    dac.poll(109U);
    check(transport.reset && transport.writes.empty(), "DAC deadline does not fire early");
    dac.poll(110U);
    check(!transport.reset && dac.deadline_ms() == 130U, "DAC reset release deadline");
    dac.poll(130U);
    check(transport.chip_select && dac.deadline_ms() == 132U, "DAC CS setup deadline");
    dac.poll(132U);
    check(transport.writes.size() == 1U && !transport.chip_select, "first DAC write and CS release");
    for (std::uint32_t now : {135U, 137U, 140U, 142U, 145U, 147U, 150U, 152U}) dac.poll(now);
    check(dac.ready_muted() && transport.writes.size() == 5U, "DAC bootstrap completes muted");
    check(transport.writes[0].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x08U, 0xC0U}}, "DAC bootstrap write 1");
    check(transport.writes[1].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x02U, 0x10U}}, "DAC bootstrap write 2");
    check(transport.writes[2].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x07U, 0xB0U}}, "DAC bootstrap write 3");
    check(transport.writes[3].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x08U, 0x40U}}, "DAC bootstrap write 4");
    check(transport.writes[4].bytes == std::array<std::uint8_t, 3>{{0x98U, 0x04U, 0xD8U}}, "DAC explicit safety mute");
    check(!transport.reset && !transport.chip_select, "DAC ready state remains reset-released and CS inactive");
    check(dac.configure_format(f441), "DAC format configuration");
    check(dac.set_attenuation(-256), "DAC volume update");
    check(transport.writes[6].bytes[1] == 0x05U && transport.writes[7].bytes[1] == 0x06U &&
          transport.writes[6].bytes[2] == transport.writes[7].bytes[2], "DAC volume updates both channels");
    check(dac.set_mute(false) && !dac.muted(), "DAC unmute command");
    check(transport.writes.back().bytes == std::array<std::uint8_t, 3>{{0x98U, 0x04U, 0xC0U}},
          "DAC unmute clears both channel mute bits");
    check(dac.set_mute(true) && dac.muted(), "DAC mute command");

    MockDacTransport wrap_transport;
    devices::Cs4398 wrap_dac(wrap_transport, {10U, 20U, 2U, 3U});
    wrap_dac.begin(0xFFFF'FFFAU);
    wrap_dac.poll(3U);
    check(wrap_transport.reset, "DAC deadline handles monotonic wrap before expiry");
    wrap_dac.poll(4U);
    check(!wrap_transport.reset, "DAC deadline handles monotonic wrap at expiry");

    MockDacTransport failed_transport;
    failed_transport.transfer_ok = false;
    devices::Cs4398 failed_dac(failed_transport, {0U, 0U, 0U, 0U});
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
    for (auto safety_event : {app::Event::Detach, app::Event::Suspend, app::Event::DmaFailure,
                              app::Event::ClockFailure, app::Event::RepeatedUnderrun}) {
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
