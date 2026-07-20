#include "app/app_controller.hpp"
#include "audio/audio_engine.hpp"
#include "audio/feedback_controller.hpp"
#include "audio/sample_rate.hpp"
#include "audio/stream_buffer.hpp"
#include "board/clock_manager.hpp"
#include "board/monotonic_timer.hpp"
#include "board/output_relay.hpp"
#include "board/target_board.hpp"
#include "config/board_config.hpp"
#include "devices/dac.hpp"
#include "platform/nxp/flexio_dac_transport.hpp"
#include "platform/nxp/mpu_config.hpp"
#include "platform/nxp/rom_bootloader.hpp"
#include "usb/usb_device.hpp"

#include <new>

extern "C" {
#include "clock_config.h"
}

namespace {
enum class StartStep : std::uint8_t { None, RelayRelease, ClockSettle, DmaProgress, Prefill, Stable, DacSettle, Playing };
enum class StopReason : std::uint8_t {
    None, HostStop, Detached, Suspended, HostMuted, UnsupportedRate, UsbInit,
    DacConfigure, EngineStart, DmaNoProgress, Underrun, DmaError, DacUnmute,
    FinalInvariant, VolumeControl
};

struct Runtime {
    board::OutputRelay relay;
    board::ClockManager clocks;
    platform::nxp::FlexioDacTransport transport;
    devices::Cs4398 dac{transport};
    audio::StreamBuffer stream;
    audio::FeedbackController feedback;
    audio::AudioEngine engine;
    usb::UsbDevice usb;
    app::AppController app;
    const audio::FormatConfig* active{nullptr};
    const audio::FormatConfig* desired{nullptr};
    StartStep step{StartStep::None};
    std::uint32_t deadline{0U};
    std::uint32_t dma_baseline{0U};
    std::uint32_t underrun_baseline{0U};
    bool usb_ready{false};
    bool mounted{false};
    bool suspended{false};
    bool stream_requested{false};
    bool bootloader_armed{false};
    std::uint32_t bootloader_deadline{0U};
    StartStep last_stop_step{StartStep::None};
    StopReason stop_reason{StopReason::None};
};

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

void safe_stop(Runtime& r, bool fault, StopReason reason = StopReason::None) {
    // Preserve the first fault stage/reason. Windows commonly sends one or
    // more alt-0 Stop requests after a failed start; those are consequences,
    // not the root cause and must not overwrite the evidence.
    if (reason != StopReason::None && (fault || r.app.state() != app::State::FaultMuted)) {
        r.last_stop_step = r.step;
        r.stop_reason = reason;
    }
    r.relay.disconnect();
    if (r.dac.ready_muted()) r.dac.set_mute(true);
    r.engine.stop();
    r.stream.clear();
    if (r.active) r.feedback.reset(*r.active, audio::StreamBuffer::capacity_bytes / 2U);
    r.usb.reset_stream();
    r.clocks.invalidate();
    r.step = StartStep::None;
    r.usb.set_clock_valid(false);
    r.app.dispatch(fault ? app::Event::Fault : app::Event::StopRequested);
    if (!fault) r.app.dispatch(app::Event::Stopped);
}

void begin_start(Runtime& r, std::uint32_t now) {
    r.relay.disconnect();
    r.dac.set_mute(true);
    r.engine.stop();
    r.stream.clear();
    r.usb.set_clock_valid(false);
    r.clocks.invalidate();
    if (r.app.state() == app::State::FaultMuted) {
        r.app.dispatch(app::Event::ClearFault);
        r.app.dispatch(app::Event::StreamRequested);
    } else if (r.app.state() == app::State::Playing) r.app.dispatch(app::Event::RateRequested);
    else if (r.app.state() == app::State::IdleMuted) r.app.dispatch(app::Event::StreamRequested);
    r.step = StartStep::RelayRelease;
    r.deadline = now + config::board::relay_release_us / 1'000U;
}

void run_start(Runtime& r, std::uint32_t now) {
    if (r.step == StartStep::None || !r.desired) return;
    switch (r.step) {
    case StartStep::RelayRelease:
        if (!reached(now, r.deadline)) break;
        r.clocks.select(r.desired->family);
        r.deadline = now + config::board::oscillator_settle_us / 1'000U;
        r.step = StartStep::ClockSettle;
        break;
    case StartStep::ClockSettle:
        if (!reached(now, r.deadline)) break;
        r.clocks.mark_stable();
        if (!r.dac.configure_format(*r.desired)) { safe_stop(r, true, StopReason::DacConfigure); break; }
        if (!r.engine.start_silence(*r.desired)) { safe_stop(r, true, StopReason::EngineStart); break; }
        r.active = r.desired;
        if (r.app.state() == app::State::SwitchingRate) r.app.dispatch(app::Event::StreamRequested);
        r.usb.set_active_format(*r.active);
        r.dma_baseline = r.engine.dma_completions();
        r.deadline = now + 20U;
        r.step = StartStep::DmaProgress;
        break;
    case StartStep::DmaProgress:
        if (r.engine.dma_completions() != r.dma_baseline) {
            r.usb.set_clock_valid(true);
            r.step = StartStep::Prefill;
        } else if (reached(now, r.deadline)) safe_stop(r, true, StopReason::DmaNoProgress);
        break;
    case StartStep::Prefill:
        if (r.desired != r.active) { begin_start(r, now); break; }
        if (r.stream.size() >= audio::StreamBuffer::capacity_bytes / 2U) {
            r.engine.attach_stream(r.stream);
            r.underrun_baseline = r.engine.underruns();
            r.deadline = now + config::board::stable_audio_us / 1'000U;
            r.step = StartStep::Stable;
        }
        break;
    case StartStep::Stable:
        if (r.engine.underruns() != r.underrun_baseline) { safe_stop(r, true, StopReason::Underrun); break; }
        if (reached(now, r.deadline)) {
            if (r.usb.controls().muted) { safe_stop(r, false, StopReason::HostMuted); break; }
            if (!r.dac.set_mute(false)) { safe_stop(r, true, StopReason::DacUnmute); break; }
            r.deadline = now + config::board::dac_settle_us / 1'000U;
            r.step = StartStep::DacSettle;
        }
        break;
    case StartStep::DacSettle:
        if (!reached(now, r.deadline)) break;
        if (!r.clocks.stable() || !r.engine.running() || r.dac.muted() ||
            r.stream.size() < audio::StreamBuffer::capacity_bytes / 4U) { safe_stop(r, true, StopReason::FinalInvariant); break; }
        r.app.dispatch(app::Event::AudioStable);
        r.relay.connect();
        r.step = StartStep::Playing;
        break;
    case StartStep::Playing: break;
    default: break;
    }
}

void update_leds(Runtime& r, std::uint32_t now) {
    const bool fault = r.app.state() == app::State::FaultMuted;
    if (fault) board::target::set_red_led(((now / 125U) & 1U) != 0U);
    else if (r.clocks.selected() == audio::ClockFamily::Family48k)
        board::target::set_red_led(((now / 500U) & 1U) != 0U);
    else board::target::set_red_led(false);
    const bool playing = r.app.state() == app::State::Playing;
    const std::uint32_t green_period = r.suspended ? 2'000U : (r.mounted ? 500U : 125U);
    board::target::set_green_led(playing ? true : (((now / green_period) & 1U) != 0U));
}

std::uint16_t narrow_counter(std::uint32_t value) {
    return static_cast<std::uint16_t>(value > 0xFFFFU ? 0xFFFFU : value);
}

void refresh_diagnostics(Runtime& r, std::uint32_t now) {
    usb::maintenance::DiagnosticReport report{};
    report.uptime_ms = now;
    report.last_set_cur_rate_hz = r.usb.last_set_cur_rate();
    report.desired_rate_hz = r.desired ? r.desired->format.sample_rate_hz : 0U;
    report.active_rate_hz = r.active ? r.active->format.sample_rate_hz : 0U;
    report.feedback_16_16 = r.feedback.value();
    report.usb_packet_count = r.usb.packet_count();
    report.dropped_packets = r.usb.dropped_packets();
    report.dropped_events = r.usb.dropped_events();
    report.dma_completions = r.engine.dma_completions();
    report.dma_errors = narrow_counter(r.engine.dma_errors());
    report.underruns = narrow_counter(r.engine.underruns());
    report.overflows = narrow_counter(r.stream.overflows());
    report.padding_errors = narrow_counter(r.usb.padding_errors());
    report.ring_fill_bytes = narrow_counter(static_cast<std::uint32_t>(r.stream.size()));
    report.last_packet_bytes = r.usb.last_packet_size();
    report.minimum_packet_bytes = r.usb.minimum_packet_size();
    report.maximum_packet_bytes = r.usb.maximum_packet_size();
    report.start_step = static_cast<std::uint8_t>(r.step);
    report.last_stop_step = static_cast<std::uint8_t>(r.last_stop_step);
    report.stop_reason = static_cast<std::uint8_t>(r.stop_reason);
    report.event_queue_depth = r.usb.event_queue_depth();
    report.last_interface_alt = r.usb.last_interface_alt();
    report.state = static_cast<std::uint8_t>(r.app.state()) & 0x0FU;
    if (r.clocks.selected() == audio::ClockFamily::Family48k) report.state |= 0x10U;
    if (r.usb.last_interface_alt() == 1U) report.state |= 0x20U;
    if (r.stream_requested) report.state |= 0x40U;
    if (r.usb.controls().muted) report.state |= 0x80U;
    if (r.mounted) report.flags |= 0x01U;
    if (r.suspended) report.flags |= 0x02U;
    if (r.clocks.stable()) report.flags |= 0x04U;
    if (board::target::clock_select_level()) report.flags |= 0x08U;
    if (r.dac.muted()) report.flags |= 0x10U;
    if (r.relay.connected()) report.flags |= 0x20U;
    if (r.engine.running()) report.flags |= 0x40U;
    if (r.usb.clock_valid()) report.flags |= 0x80U;
    r.usb.update_diagnostics(report);
}
} // namespace

int main() {
    platform::nxp::configure_mpu();
    board::target::initialize_diagnostic_led_early();
    board::target::set_red_led(true);
    BOARD_InitBootClocks();
    board::target::initialize_early_safe();
    board::target::initialize_digital();
    board::target::initialize_dac_bus();

    // Runtime contains the 8 KiB audio ring. Static storage keeps it out of
    // the configured 8 KiB main/interrupt stack; construction still occurs
    // here, after the hardware-verified clock and safe-GPIO startup order.
    alignas(Runtime) static std::uint8_t runtime_storage[sizeof(Runtime)];
    Runtime& runtime = *::new (static_cast<void*>(runtime_storage)) Runtime;
    runtime.relay.initialize_safe();
    runtime.clocks.initialize_safe();
    if (!board::MonotonicTimer::initialize(SystemCoreClock)) for (;;) __asm volatile("wfi");
    runtime.app.dispatch(app::Event::Initialize);
    runtime.dac.begin(board::MonotonicTimer::now_ms());

    for (;;) {
        const std::uint32_t now = board::MonotonicTimer::now_ms();
        runtime.dac.poll(now);
        if (!runtime.usb_ready && runtime.dac.ready_muted()) {
            runtime.desired = &audio::supported_format(0U);
            runtime.active = runtime.desired;
            runtime.feedback.reset(*runtime.active, audio::StreamBuffer::capacity_bytes / 2U);
            if (!runtime.usb.initialize(runtime.stream, runtime.feedback)) { safe_stop(runtime, true, StopReason::UsbInit); }
            else { runtime.usb_ready = true; runtime.usb.set_active_format(*runtime.active); runtime.app.dispatch(app::Event::Initialized); }
        }
        if (runtime.usb_ready) runtime.usb.task();
        for (usb::EventRecord usb_event = runtime.usb.take_event(); usb_event.event != usb::Event::None;
             usb_event = runtime.usb.take_event()) switch (usb_event.event) {
        case usb::Event::Mounted: runtime.mounted = true; runtime.suspended = false; break;
        case usb::Event::Start:
            runtime.stream_requested = true;
            runtime.usb.start_feedback();
            begin_start(runtime, now);
            break;
        case usb::Event::Rate:
            runtime.desired = audio::find_format(usb_event.value);
            if (!runtime.desired) safe_stop(runtime, true, StopReason::UnsupportedRate);
            else {
                runtime.feedback.reset(*runtime.desired, audio::StreamBuffer::capacity_bytes / 2U);
                if (runtime.stream_requested) begin_start(runtime, now);
            }
            break;
        case usb::Event::Controls:
            if (usb_event.control == usb::Control::Volume) {
                const auto volume = static_cast<std::int16_t>(static_cast<std::int32_t>(usb_event.value));
                if (!runtime.dac.set_attenuation(volume)) safe_stop(runtime, true, StopReason::VolumeControl);
            } else if (usb_event.control == usb::Control::Mute) {
                if (usb_event.value != 0U) { runtime.relay.disconnect(); runtime.dac.set_mute(true); }
                else if (runtime.stream_requested) begin_start(runtime, now);
            }
            break;
        case usb::Event::Stop: runtime.stream_requested = false; safe_stop(runtime, false, StopReason::HostStop); break;
        case usb::Event::Detached: runtime.mounted = false; runtime.suspended = false; runtime.stream_requested = false; safe_stop(runtime, false, StopReason::Detached); break;
        case usb::Event::Suspended: runtime.suspended = true; runtime.stream_requested = false; safe_stop(runtime, false, StopReason::Suspended); break;
        case usb::Event::Resumed: runtime.suspended = false; break;
        case usb::Event::Diagnostics:
            refresh_diagnostics(runtime, now);
            runtime.usb.send_diagnostics();
            break;
        case usb::Event::EventTrace:
            runtime.usb.send_event_trace();
            break;
        case usb::Event::BootloaderArm:
            if (runtime.bootloader_armed && !reached(now, runtime.bootloader_deadline)) {
                safe_stop(runtime, false);
                runtime.relay.disconnect();
                board::target::set_dac_reset(true);
                platform::nxp::enter_rom_serial_downloader();
            }
            runtime.bootloader_armed = true;
            runtime.bootloader_deadline = now + 2'000U;
            break;
        default: break;
        }
        run_start(runtime, now);
        if (runtime.bootloader_armed && reached(now, runtime.bootloader_deadline)) runtime.bootloader_armed = false;
        if (runtime.app.state() != app::State::FaultMuted && runtime.engine.dma_errors() != 0U)
            safe_stop(runtime, true, StopReason::DmaError);
        else if (runtime.app.state() != app::State::FaultMuted &&
                 runtime.engine.underruns() > runtime.underrun_baseline + 2U)
            safe_stop(runtime, true, StopReason::Underrun);
        update_leds(runtime, now);
        refresh_diagnostics(runtime, now);
        __asm volatile("wfi");
    }
}
