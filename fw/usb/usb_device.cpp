#include "usb/usb_device.hpp"

#include "audio/sample_rate.hpp"
#include "board/monotonic_timer.hpp"
#include "tusb.h"
#include "usb/usb_descriptors.h"
#include "usb/maintenance_protocol.hpp"

#include <cstring>

extern "C" {
#include "fsl_clock.h"
#include "MIMXRT1011.h"
}

namespace {
usb::UsbDevice* instance = nullptr;
audio::StreamBuffer* stream_buffer = nullptr;
audio::FeedbackController* feedback_controller = nullptr;
alignas(32) std::uint8_t packet[DYKODAC_USB_HS_MAX_PACKET];
}

namespace usb {
bool UsbDevice::initialize(audio::StreamBuffer& stream, audio::FeedbackController& feedback) noexcept {
    instance = this; stream_buffer = &stream; feedback_controller = &feedback;
    requested_rate_ = audio::supported_format(0U).format.sample_rate_hz;
    CLOCK_EnableUsbhs0PhyPllClock(kCLOCK_Usbphy480M, 24'000'000U);
    CLOCK_EnableUsbhs0Clock(kCLOCK_Usb480M, 24'000'000U);
    USBPHY->CTRL |= USBPHY_CTRL_SET_ENUTMILEVEL2_MASK | USBPHY_CTRL_SET_ENUTMILEVEL3_MASK;
    USBPHY->PWD = 0U;
    tusb_rhport_init_t init{TUSB_ROLE_DEVICE, TUSB_SPEED_HIGH};
    return tusb_init(0U, &init);
}
void UsbDevice::task() noexcept {
    tud_task();
    if (!tud_hid_ready()) return;
    if (pending_report_ == PendingReport::Diagnostics &&
        tud_hid_report(0U, &diagnostics_, sizeof(diagnostics_))) pending_report_ = PendingReport::None;
    else if (pending_report_ == PendingReport::EventTrace &&
             tud_hid_report(0U, &event_trace_, sizeof(event_trace_))) pending_report_ = PendingReport::None;
}
EventRecord UsbDevice::take_event() noexcept {
    if (event_count_ == 0U) return {};
    const EventRecord result = events_[event_read_];
    event_read_ = static_cast<std::uint8_t>((event_read_ + 1U) % event_capacity);
    --event_count_;
    return result;
}
bool UsbDevice::post_event(EventRecord event) noexcept {
    event.reserved = static_cast<std::uint16_t>(board::MonotonicTimer::now_ms());
    if (event.event != Event::Diagnostics && event.event != Event::EventTrace) {
        event_history_[event_history_write_] = event;
        event_history_write_ = static_cast<std::uint8_t>((event_history_write_ + 1U) % event_history_.size());
        if (event_history_count_ < event_history_.size()) ++event_history_count_;
    }
    if (event_count_ == event_capacity) { ++dropped_events_; return false; }
    events_[event_write_] = event;
    event_write_ = static_cast<std::uint8_t>((event_write_ + 1U) % event_capacity);
    ++event_count_;
    return true;
}
void UsbDevice::set_active_format(const audio::FormatConfig& format) noexcept {
    feedback_controller->reset(format, audio::StreamBuffer::capacity_bytes / 2U);
}
void UsbDevice::start_feedback() noexcept {
    if (feedback_controller) tud_audio_fb_set(feedback_controller->value());
}
void UsbDevice::reset_stream() noexcept { stream_buffer->clear(); clock_valid_ = false; }
void UsbDevice::update_diagnostics(const maintenance::DiagnosticReport& report) noexcept {
    const std::uint16_t next_sequence = static_cast<std::uint16_t>(diagnostics_.sequence + 1U);
    diagnostics_ = report;
    diagnostics_.magic = maintenance::diagnostic_magic;
    diagnostics_.version = maintenance::diagnostic_version;
    diagnostics_.size = static_cast<std::uint8_t>(sizeof(diagnostics_));
    diagnostics_.sequence = next_sequence;
}
bool UsbDevice::send_diagnostics() noexcept {
    if (tud_hid_ready() && tud_hid_report(0U, &diagnostics_, sizeof(diagnostics_))) {
        pending_report_ = PendingReport::None;
        return true;
    }
    pending_report_ = PendingReport::Diagnostics;
    return false;
}
bool UsbDevice::send_event_trace() noexcept {
    event_trace_ = {};
    event_trace_.event_count = event_history_count_;
    const std::size_t oldest = (event_history_write_ + event_history_.size() - event_history_count_) % event_history_.size();
    for (std::size_t i = 0U; i < event_history_count_; ++i)
        event_trace_.events[i] = event_history_[(oldest + i) % event_history_.size()];
    if (tud_hid_ready() && tud_hid_report(0U, &event_trace_, sizeof(event_trace_))) {
        pending_report_ = PendingReport::None;
        return true;
    }
    pending_report_ = PendingReport::EventTrace;
    return false;
}
} // namespace usb

extern "C" void USB_OTG1_IRQHandler() { tusb_int_handler(0U, true); }
extern "C" void tud_mount_cb() { if (instance) instance->post_event(usb::Event::Mounted); }
extern "C" void tud_umount_cb() { if (instance) instance->post_event(usb::Event::Detached); }
extern "C" void tud_suspend_cb(bool) { if (instance) instance->post_event(usb::Event::Suspended); }
extern "C" void tud_resume_cb() { if (instance) instance->post_event(usb::Event::Resumed); }

extern "C" bool tud_audio_rx_done_post_read_cb(std::uint8_t, std::uint16_t received, std::uint8_t, std::uint8_t, std::uint8_t) {
    if (!instance || !stream_buffer) return false;
    if (received > sizeof(packet)) { ++instance->dropped_packets_; return false; }
    const std::uint16_t count = tud_audio_read(packet, received);
    ++instance->packet_count_;
    instance->last_packet_size_ = static_cast<std::uint8_t>(received);
    if (received < instance->minimum_packet_size_) instance->minimum_packet_size_ = static_cast<std::uint8_t>(received);
    if (received > instance->maximum_packet_size_) instance->maximum_packet_size_ = static_cast<std::uint8_t>(received);
    // UAC2 PCM samples are left-justified. For little-endian 24-in-32 PCM,
    // byte zero of each subslot is trailing padding and must be zero.
    for (std::uint16_t i = 0U; i + 3U < count; i = static_cast<std::uint16_t>(i + 4U))
        if (packet[i] != 0U) ++instance->padding_errors_;
    if (count != received || !stream_buffer->push(packet, count)) { ++instance->dropped_packets_; }
    return true;
}
extern "C" void tud_audio_feedback_params_cb(std::uint8_t, std::uint8_t, audio_feedback_params_t* params) {
    params->method = AUDIO_FEEDBACK_METHOD_DISABLED;
    params->sample_freq = instance ? instance->requested_rate() : 0U;
}
extern "C" void tud_audio_fb_done_cb(std::uint8_t) {
    if (instance && feedback_controller && stream_buffer) {
        tud_audio_fb_set(feedback_controller->update(stream_buffer->size()));
    }
}
extern "C" bool tud_audio_set_itf_cb(std::uint8_t, tusb_control_request_t const* request) {
    if (!instance) return false;
    const std::uint8_t alt = tu_u16_low(tu_le16toh(request->wValue));
    instance->last_interface_alt_ = alt;
    instance->post_event({alt == 0U ? usb::Event::Stop : usb::Event::Start,
                          usb::Control::InterfaceAlternate, 0U, alt});
    return alt <= 1U;
}
extern "C" bool tud_audio_set_itf_close_EP_cb(std::uint8_t, tusb_control_request_t const*) {
    if (instance) instance->post_event({usb::Event::Stop, usb::Control::InterfaceAlternate, 0U, 0U});
    return true;
}

extern "C" bool tud_audio_get_req_entity_cb(std::uint8_t rhport, tusb_control_request_t const* raw) {
    if (!instance) return false;
    auto const* request = reinterpret_cast<audio_control_request_t const*>(raw);
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT && request->bChannelNumber > 2U) return false;
    if (request->bEntityID == UAC2_ENTITY_CLOCK && request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t value{static_cast<std::int32_t>(instance->requested_rate())};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, raw, &value, sizeof(value));
        }
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            const auto rate0 = static_cast<std::int32_t>(audio::supported_format(0U).format.sample_rate_hz);
            const auto rate1 = static_cast<std::int32_t>(audio::supported_format(1U).format.sample_rate_hz);
            audio_control_range_4_n_t(2) ranges{2U, {{rate0, rate0, 0}, {rate1, rate1, 0}}};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, raw, &ranges, sizeof(ranges));
        }
    }
    if (request->bEntityID == UAC2_ENTITY_CLOCK && request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t valid{static_cast<std::int8_t>(instance->clock_valid_ ? 1 : 0)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, raw, &valid, sizeof(valid));
    }
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT && request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute{static_cast<std::int8_t>(instance->controls_.muted ? 1 : 0)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, raw, &mute, sizeof(mute));
    }
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT && request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t volume{instance->controls_.volume_db_8_8};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, raw, &volume, sizeof(volume));
        }
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range{1U, {{-0x7F00, 0, 0x0080}}};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, raw, &range, sizeof(range));
        }
    }
    return false;
}

extern "C" bool tud_audio_set_req_entity_cb(std::uint8_t, tusb_control_request_t const* raw, std::uint8_t* data) {
    if (!instance) return false;
    auto const* request = reinterpret_cast<audio_control_request_t const*>(raw);
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT && request->bChannelNumber > 2U) return false;
    if (request->bEntityID == UAC2_ENTITY_CLOCK && request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ && request->wLength == 4U) {
        const std::uint32_t rate = static_cast<std::uint32_t>(reinterpret_cast<audio_control_cur_4_t*>(data)->bCur);
        if (!audio::find_format(rate)) return false;
        instance->requested_rate_ = rate;
        instance->last_set_cur_rate_ = rate;
        instance->post_event({usb::Event::Rate, usb::Control::SampleRate, 0U, rate});
        return true;
    }
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT && request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->wLength == 1U) {
        instance->controls_.muted = reinterpret_cast<audio_control_cur_1_t*>(data)->bCur != 0;
        instance->post_event({usb::Event::Controls, usb::Control::Mute, 0U,
                              instance->controls_.muted ? 1U : 0U}); return true;
    }
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT && request->bControlSelector == AUDIO_FU_CTRL_VOLUME && request->wLength == 2U) {
        std::int16_t value = reinterpret_cast<audio_control_cur_2_t*>(data)->bCur;
        if (value > 0) value = 0;
        if (value < -0x7F00) value = -0x7F00;
        instance->controls_.volume_db_8_8 = value;
        instance->post_event({usb::Event::Controls, usb::Control::Volume, 0U,
                              static_cast<std::uint32_t>(static_cast<std::int32_t>(value))}); return true;
    }
    return false;
}

extern "C" std::uint16_t tud_hid_get_report_cb(std::uint8_t, std::uint8_t,
    hid_report_type_t, std::uint8_t* buffer, std::uint16_t requested) {
    if (!instance || requested == 0U) return 0U;
    const std::uint16_t count = requested < sizeof(instance->diagnostics_) ?
        requested : static_cast<std::uint16_t>(sizeof(instance->diagnostics_));
    std::memcpy(buffer, &instance->diagnostics_, count);
    return count;
}

extern "C" void tud_hid_set_report_cb(std::uint8_t, std::uint8_t,
    hid_report_type_t type, std::uint8_t const* buffer, std::uint16_t size) {
    if (!instance || type != HID_REPORT_TYPE_OUTPUT) return;
    if (usb::maintenance::is_enter_rom(buffer, size)) instance->post_event(usb::Event::BootloaderArm);
    else if (usb::maintenance::is_get_diagnostics(buffer, size)) instance->post_event(usb::Event::Diagnostics);
    else if (usb::maintenance::is_get_events(buffer, size)) instance->post_event(usb::Event::EventTrace);
}
