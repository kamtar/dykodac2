#include "app/app_controller.hpp"
#include "app/config_service.hpp"
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
#include "platform/nxp/settings_flash.hpp"
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
    bool resume_pending{false};
    StartStep last_stop_step{StartStep::None};
    StopReason stop_reason{StopReason::None};
    // User settings that shape the start sequence (format table, DAC
    // registers); the float chain itself lives in AudioEngine.
    app::ConfigService config;
    bool upsampler_bypass{false};
    bool dac_slow_rolloff{false};
    std::int16_t dac_ceiling_db_8_8{config::board::dac_ceiling_db_8_8};
};

const audio::FormatConfig* format_for(const Runtime& r, std::uint32_t rate) {
    return audio::find_format(rate, r.upsampler_bypass);
}

// Host volume is an MCU gain; the DAC attenuation registers only ever hold
// this fixed safety ceiling (see config/board_config.hpp).
bool apply_dac_ceiling(Runtime& r) {
    return r.dac.set_attenuation(r.dac_ceiling_db_8_8);
}

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
    r.resume_pending = false;
    r.usb.set_clock_valid(false);
    if (fault) {
        r.app.dispatch(app::Event::Fault);
    } else {
        // A host alt-0, suspend, or detach is also the recovery boundary for
        // a latched transport/hardware fault. This lets a later complete start
        // retry safely without requiring a cable replug.
        if (r.app.state() == app::State::FaultMuted) r.app.dispatch(app::Event::ClearFault);
        r.app.dispatch(app::Event::StopRequested);
        r.app.dispatch(app::Event::Stopped);
    }
}

void enter_connected_idle(Runtime& r) {
    // A normal host alt-0 transition is only a pause while USB remains
    // mounted. Keep the already-settled analog path connected and feed the
    // DAC digital zero so relay switching cannot expose channel DC offset.
    r.engine.detach_stream();
    r.stream.clear();
    r.usb.reset_stream();
    r.usb.set_clock_valid(true);
    r.underrun_baseline = r.engine.underruns();
    r.resume_pending = false;
    r.stop_reason = StopReason::HostStop;
}

void begin_start(Runtime& r, std::uint32_t now) {
    r.relay.disconnect();
    r.dac.set_mute(true);
    r.engine.stop();
    r.stream.clear();
    r.usb.set_clock_valid(false);
    r.clocks.invalidate();
    r.resume_pending = false;
    if (r.app.state() == app::State::FaultMuted) {
        r.app.dispatch(app::Event::ClearFault);
        r.app.dispatch(app::Event::StreamRequested);
    } else if (r.app.state() == app::State::Playing) r.app.dispatch(app::Event::RateRequested);
    else if (r.app.state() == app::State::IdleMuted) r.app.dispatch(app::Event::StreamRequested);
    r.step = StartStep::RelayRelease;
    r.deadline = now + config::board::relay_release_us / 1'000U;
}

void recover_from_underrun(Runtime& r, std::uint32_t now) {
    // An underrun is a loss of host audio data, not a hardware failure. Record
    // it, open the relay, mute, and run the normal prefill/stability sequence
    // again while alt-1 remains selected.
    r.last_stop_step = r.step;
    r.stop_reason = StopReason::Underrun;
    begin_start(r, now);
}

void run_start(Runtime& r, std::uint32_t now) {
    if (r.step == StartStep::None || !r.desired) return;
    switch (r.step) {
    case StartStep::RelayRelease:
        if (!reached(now, r.deadline)) break;
        // The DAC output family follows the requested USB family. GPIO1_IO05
        // selects the external 22.5792-MHz or 24.576-MHz oscillator; the SAI
        // input MCLK path remains unchanged while the stopped stream settles.
        r.clocks.select(r.desired->output_family);
        r.deadline = now + config::board::oscillator_settle_us / 1'000U;
        r.step = StartStep::ClockSettle;
        break;
    case StartStep::ClockSettle:
        if (!reached(now, r.deadline)) break;
        r.clocks.mark_stable();
        if (!r.dac.configure_format(*r.desired, r.dac_slow_rolloff)) { safe_stop(r, true, StopReason::DacConfigure); break; }
        // Re-assert the fixed ceiling with every DAC format (re)configuration,
        // always before the unmute in the Stable step.
        if (!apply_dac_ceiling(r)) { safe_stop(r, true, StopReason::VolumeControl); break; }
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
        } else if (reached(now, r.deadline)) {
            r.engine.capture_dma_diagnostics(now);
            safe_stop(r, true, StopReason::DmaNoProgress);
        }
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
        if (r.engine.underruns() != r.underrun_baseline) {
            recover_from_underrun(r, now);
            break;
        }
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
    const bool playing = r.app.state() == app::State::Playing;
    if (fault) {
        const bool on = ((now / 125U) & 1U) != 0U;
        board::target::set_red_led(on);
        board::target::set_green_led(on);
        return;
    }

    // Front-panel meaning follows the format requested by the PC: green is
    // the 44.1-kHz family and red is the 48-kHz family.
    const bool family_48k = r.desired && r.desired->family == audio::ClockFamily::Family48k;
    bool on = true;
    if (!r.mounted) {
        // With no PC connected, give a short heartbeat once every five seconds.
        on = (now % 5'000U) < 100U;
    } else if (playing && !r.stream_requested) {
        // The PC is connected and the analog path remains settled, but no
        // stream is active. Blink more slowly than the cold/muted idle state.
        on = ((now / 1'500U) & 1U) != 0U;
    } else if (!playing) {
        // Enumerated but idle: slowly blink the PC-requested family.
        on = ((now / 500U) & 1U) != 0U;
    }
    board::target::set_red_led(on && family_48k);
    board::target::set_green_led(on && !family_48k);
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
class ConfigBackend final : public app::ConfigBackend {
public:
    explicit ConfigBackend(Runtime& r) noexcept : r_(r) {}
    void set_now(std::uint32_t now) noexcept { now_ = now; }

    void apply(const settings::Settings& s) noexcept override {
        r_.engine.set_settings(s);
        if (s.dac_ceiling_db_8_8 != r_.dac_ceiling_db_8_8) {
            r_.dac_ceiling_db_8_8 = s.dac_ceiling_db_8_8;
            // The DAC soft-ramps attenuation changes; before bootstrap the
            // USB-init path writes the new ceiling instead.
            if (r_.dac.ready_muted() && !apply_dac_ceiling(r_)) safe_stop(r_, true, StopReason::VolumeControl);
        }
        const bool bypass = s.upsampler_mode == static_cast<std::uint8_t>(settings::UpsamplerMode::Bypass);
        const bool slow = s.dac_filter == static_cast<std::uint8_t>(settings::DacFilter::SlowRollOff);
        if (bypass == r_.upsampler_bypass && slow == r_.dac_slow_rolloff) return;
        r_.upsampler_bypass = bypass;
        r_.dac_slow_rolloff = slow;
        if (!r_.usb_ready) return; // the first start already uses the new mode
        // Output rate and DAC speed mode change: go through the complete
        // pop-safe sequence (relay open, mute, reclock, settle, unmute).
        const audio::FormatConfig* format = format_for(r_, r_.usb.requested_rate());
        if (!format) return;
        r_.desired = format;
        if (r_.stream_requested) begin_start(r_, now_);
        else if (r_.engine.running()) safe_stop(r_, false, StopReason::HostStop);
    }
    // The firmware never writes flash; the host saves via the flashloader.
    bool flash_available() const noexcept override { return false; }
    app::FlashResult flash_load(settings::Settings& out, std::uint32_t& sequence) noexcept override {
        return platform::nxp::settings_flash::load(out, sequence);
    }
    app::FlashResult flash_save(const settings::Settings&, std::uint32_t&) noexcept override {
        return app::FlashResult::Unavailable;
    }
    void fill_runtime_info(usb::config::Info& info) noexcept override {
        if (r_.engine.running()) {
            info.source_rate_hz = r_.engine.source_rate_hz();
            info.output_rate_hz = r_.engine.output_rate_hz();
        }
        info.host_volume_db_8_8 = r_.engine.volume_db();
        info.dsp_boost_db = r_.engine.dsp_boost_db();
        info.effective_headroom_db = r_.engine.effective_headroom_db();
        info.leveler_gain_db = r_.engine.leveler_gain_db();
        info.loudness_bass_db = r_.engine.loudness_bass_db();
        info.loudness_treble_db = r_.engine.loudness_treble_db();
        info.app_state = static_cast<std::uint8_t>(r_.app.state());
        info.upsampler_bypass_active = r_.active && r_.active->dac_mode == 1U ? 1U : 0U;
        if (r_.stream_requested) info.flags |= usb::config::flag_streaming;
        if (r_.engine.running() && info.upsampler_bypass_active != 0U && r_.engine.native_bit_exact())
            info.flags |= usb::config::flag_bit_exact;
    }
    bool peek_rom(std::uint32_t address, std::uint8_t* out, std::size_t length) noexcept override {
        return platform::nxp::settings_flash::peek_rom(address, out, length);
    }
private:
    Runtime& r_;
    std::uint32_t now_{0U};
};
} // namespace

int main() {
    // Flashing request from the previous run: hand over to the boot ROM USB
    // serial downloader before anything else is configured.
    platform::nxp::enter_rom_bootloader_if_requested();
    platform::nxp::configure_mpu();
    board::target::initialize_diagnostic_led_early();
    board::target::set_red_led(true);
    BOARD_InitBootClocks();
    board::target::initialize_early_safe();
    board::target::initialize_digital();
    board::target::initialize_dac_bus();
    // The working prototype enables the external analog DC/DC through its
    // package-pin-60 bodge (GPIO_AD_00/GPIO1_IO14), not the schematic pin.
    board::target::set_dcdc_enabled(true);

    // Runtime contains the 8 KiB audio ring. Static storage keeps it out of
    // the configured 8 KiB main/interrupt stack; construction still occurs
    // here, after the hardware-verified clock and safe-GPIO startup order.
    alignas(Runtime) static std::uint8_t runtime_storage[sizeof(Runtime)];
    Runtime& runtime = *::new (static_cast<void*>(runtime_storage)) Runtime;
    runtime.relay.initialize_safe();
    // Start from the device's current host volume, never unity by accident.
    runtime.engine.set_volume_db(runtime.usb.controls().volume_db_8_8);
    // Saved user settings (or defaults) before anything starts: they choose
    // the output format, DAC filter and ceiling of the very first start.
    static ConfigBackend config_backend{runtime};
    runtime.config.boot(config_backend);
    runtime.clocks.initialize_safe();
    if (!board::MonotonicTimer::initialize(SystemCoreClock)) for (;;) __asm volatile("wfi");
    runtime.app.dispatch(app::Event::Initialize);
    runtime.dac.begin(board::MonotonicTimer::now_ms());

    for (;;) {
        const std::uint32_t now = board::MonotonicTimer::now_ms();
        runtime.dac.poll(now);
        // A DMA completion only publishes a refill request. Regenerate retired
        // blocks here while the circular DMA ring keeps transmitting.
        runtime.engine.service();
        if (!runtime.usb_ready && runtime.dac.ready_muted()) {
            runtime.desired = format_for(runtime, audio::supported_format(0U).format.sample_rate_hz);
            runtime.active = runtime.desired;
            runtime.feedback.reset(*runtime.active, audio::StreamBuffer::capacity_bytes / 2U);
            if (!runtime.usb.initialize(runtime.stream, runtime.feedback)) { safe_stop(runtime, true, StopReason::UsbInit); }
            else {
                runtime.usb_ready = true;
                runtime.usb.set_active_format(*runtime.active);
                runtime.app.dispatch(app::Event::Initialized);
                // First DAC attenuation write after bootstrap: the fixed
                // ceiling, long before the first unmute.
                if (!apply_dac_ceiling(runtime)) safe_stop(runtime, true, StopReason::VolumeControl);
            }
        }
        if (runtime.usb_ready) runtime.usb.task();
        for (usb::EventRecord usb_event = runtime.usb.take_event(); usb_event.event != usb::Event::None;
             usb_event = runtime.usb.take_event()) {
            switch (usb_event.event) {
        case usb::Event::Mounted: runtime.mounted = true; runtime.suspended = false; break;
        case usb::Event::Start:
            runtime.stream_requested = true;
            // Re-resolve the stream format from the clock rate most recently
            // selected by the host. This also handles hosts that send SET_CUR
            // before changing the streaming alternate setting.
            runtime.desired = format_for(runtime, runtime.usb.requested_rate());
            if (!runtime.desired) { safe_stop(runtime, true, StopReason::UnsupportedRate); break; }
            runtime.usb.start_feedback();
            if (runtime.app.state() == app::State::Playing && runtime.engine.running() &&
                runtime.relay.connected() && !runtime.dac.muted() && runtime.clocks.stable() &&
                runtime.active == runtime.desired) {
                // Resume the existing settled path. Continue sending zeroes
                // until USB has refilled half the ring, then join the stream.
                runtime.usb.set_clock_valid(true);
                runtime.resume_pending = true;
            } else {
                begin_start(runtime, now);
            }
            break;
        case usb::Event::Rate:
            runtime.desired = format_for(runtime, usb_event.value);
            if (!runtime.desired) safe_stop(runtime, true, StopReason::UnsupportedRate);
            else {
                runtime.feedback.reset(*runtime.desired, audio::StreamBuffer::capacity_bytes / 2U);
                // Some hosts repeat SET_CUR while resuming the same format.
                // Do not turn that harmless repeat into a relay cycle.
                if (runtime.stream_requested && runtime.desired != runtime.active)
                    begin_start(runtime, now);
            }
            break;
        case usb::Event::Controls:
            if (usb_event.control == usb::Control::Volume) {
                // Host volume never touches DAC registers 05h/06h; it only
                // retargets the MCU gain ramp (main context, as is process()).
                const auto volume = static_cast<std::int16_t>(static_cast<std::int32_t>(usb_event.value));
                runtime.engine.set_volume_db(volume);
            } else if (usb_event.control == usb::Control::Mute) {
                if (usb_event.value != 0U) { runtime.relay.disconnect(); runtime.dac.set_mute(true); }
                else if (runtime.stream_requested) begin_start(runtime, now);
            }
            break;
        case usb::Event::Stop:
            runtime.stream_requested = false;
            if (runtime.mounted && runtime.app.state() == app::State::Playing &&
                runtime.engine.running() && runtime.relay.connected() && !runtime.dac.muted())
                enter_connected_idle(runtime);
            else
                safe_stop(runtime, false, StopReason::HostStop);
            break;
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
        case usb::Event::DmaDiagnostics: {
            if (runtime.engine.running()) runtime.engine.capture_dma_diagnostics(now);
            const auto& source = runtime.engine.dma_diagnostics();
            usb::maintenance::DmaDiagnosticReport report{};
            report.captured_ms = source.captured_ms;
            report.dmamux_chcfg = source.dmamux_chcfg;
            report.dma_es = source.dma_es;
            report.tcd_saddr = source.tcd_saddr;
            report.tcd_daddr = source.tcd_daddr;
            report.tcd_nbytes = source.tcd_nbytes;
            report.sai_tcsr = source.sai_tcsr;
            report.sai_tcr1 = source.sai_tcr1;
            report.sai_tcr2 = source.sai_tcr2;
            report.sai_tcr3 = source.sai_tcr3;
            report.sai_tfr0 = source.sai_tfr0;
            report.dma_erq = source.dma_erq;
            report.dma_int = source.dma_int;
            report.dma_hrs = source.dma_hrs;
            report.tcd_citer = source.tcd_citer;
            report.tcd_biter = source.tcd_biter;
            report.tcd_csr = source.tcd_csr;
            runtime.usb.update_dma_diagnostics(report);
            runtime.usb.send_dma_diagnostics();
            break;
        }
        case usb::Event::RefillTiming: {
            const auto& source = runtime.engine.refill_timing();
            usb::maintenance::RefillTimingReport report{};
            report.captured_ms = now;
            report.refill_count = source.count;
            report.refill_total_cycles = source.total_cycles;
            report.refill_maximum_cycles = source.maximum_cycles;
            report.refill_last_cycles = source.last_cycles;
            report.core_clock_hz = SystemCoreClock;
            report.maximum_pending_refills = source.maximum_pending;
            report.dma_buffer_count = audio::AudioEngine::dma_buffer_count;
            report.coalesced_callbacks = source.coalesced_callbacks;
            report.queue_drains = source.queue_drains;
            runtime.usb.update_refill_timing(report);
            runtime.usb.send_refill_timing();
            break;
        }
        case usb::Event::EnterRomBootloader:
            // Leave the analog path safe; the reset then returns every pin to
            // its reset default (relay open) before the ROM takes over.
            safe_stop(runtime, false, StopReason::HostStop);
            runtime.dac.set_mute(true);
            platform::nxp::reboot_to_rom_bootloader(usb_event.value,
                static_cast<platform::nxp::RomEntryMethod>(usb_event.reserved));
        case usb::Event::BootInfo: {
            const auto info = platform::nxp::boot_info();
            runtime.usb.send_boot_info(&info);
            break;
        }
        case usb::Event::Config: {
            config_backend.set_now(now);
            const auto response = runtime.config.handle(runtime.usb.config_request(), 64U, config_backend);
            runtime.usb.release_config_request();
            runtime.usb.send_config_response(&response);
            break;
        }
        default: break;
            }
            // Refill between control/maintenance events. Windows can enqueue
            // many volume updates while dragging its slider; processing the
            // whole burst before servicing audio advances DMA ownership.
            runtime.engine.service();
        }
        if (runtime.resume_pending && runtime.stream_requested &&
            runtime.stream.size() >= audio::StreamBuffer::capacity_bytes / 2U) {
            runtime.engine.attach_stream(runtime.stream);
            runtime.underrun_baseline = runtime.engine.underruns();
            runtime.resume_pending = false;
        }
        run_start(runtime, now);
        // Only live, host-requested playback may promote transport counters to
        // a fault. A completion IRQ can race with alt-0/abort; its counters
        // must not re-latch FaultMuted after the Stop event just recovered it.
        if (runtime.stream_requested && runtime.engine.running() &&
            runtime.app.state() != app::State::FaultMuted && runtime.engine.dma_errors() != 0U)
            safe_stop(runtime, true, StopReason::DmaError);
        // A live stream gap is concealed in AudioEngine as a short fade to
        // digital zero and a fade-in once a full DMA block is available.  Do
        // not cycle the DAC mute or output relay here: those transitions are
        // more audible than the missing samples and turn a recoverable USB
        // scheduling hiccup into several pops.
        update_leds(runtime, now);
        refresh_diagnostics(runtime, now);
        __asm volatile("wfi");
    }
}
