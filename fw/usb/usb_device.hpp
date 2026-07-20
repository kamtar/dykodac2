#pragma once

#include "audio/feedback_controller.hpp"
#include "audio/stream_buffer.hpp"
#include "usb/diagnostic_report.hpp"
#include <cstdint>
#include <array>

namespace usb {
enum class Event : std::uint8_t { None, Mounted, Detached, Suspended, Resumed, Start, Stop, Rate, Controls, Diagnostics, EventTrace, BootloaderArm };
enum class Control : std::uint8_t { None, SampleRate, Mute, Volume, InterfaceAlternate };
struct EventRecord {
    Event event{Event::None};
    Control control{Control::None};
    std::uint16_t reserved{0U};
    std::uint32_t value{0U};
};
static_assert(sizeof(EventRecord) == 8U, "event record ABI changed");

struct EventTraceReport {
    std::uint32_t magic{0x56454B44U}; // "DKEV"
    std::uint8_t version{1U};
    std::uint8_t size{64U};
    std::uint8_t event_count{0U};
    std::uint8_t reserved{0U};
    std::array<EventRecord, 7U> events{};
};
static_assert(sizeof(EventTraceReport) == 64U, "event trace must fit one HID packet");
struct ControlState { bool muted; std::int16_t volume_db_8_8; };

class UsbDevice {
public:
    bool initialize(audio::StreamBuffer& stream, audio::FeedbackController& feedback) noexcept;
    void task() noexcept;
    EventRecord take_event() noexcept;
    bool post_event(EventRecord event) noexcept;
    bool post_event(Event event) noexcept { return post_event({event, Control::None, 0U, 0U}); }
    std::uint32_t requested_rate() const noexcept { return requested_rate_; }
    std::uint32_t last_set_cur_rate() const noexcept { return last_set_cur_rate_; }
    ControlState controls() const noexcept { return controls_; }
    void set_clock_valid(bool valid) noexcept { clock_valid_ = valid; }
    bool clock_valid() const noexcept { return clock_valid_; }
    void set_active_format(const audio::FormatConfig& format) noexcept;
    void start_feedback() noexcept;
    void reset_stream() noexcept;
    void update_diagnostics(const maintenance::DiagnosticReport& report) noexcept;
    bool send_diagnostics() noexcept;
    bool send_event_trace() noexcept;
    std::uint32_t dropped_packets() const noexcept { return dropped_packets_; }
    std::uint32_t dropped_events() const noexcept { return dropped_events_; }
    std::uint32_t packet_count() const noexcept { return packet_count_; }
    std::uint32_t padding_errors() const noexcept { return padding_errors_; }
    std::uint8_t last_packet_size() const noexcept { return last_packet_size_; }
    std::uint8_t minimum_packet_size() const noexcept { return minimum_packet_size_ == 0xFFU ? 0U : minimum_packet_size_; }
    std::uint8_t maximum_packet_size() const noexcept { return maximum_packet_size_; }
    std::uint8_t event_queue_depth() const noexcept { return event_count_; }
    std::uint8_t last_interface_alt() const noexcept { return last_interface_alt_; }
public: // callback-facing state; only TinyUSB callbacks in usb_device.cpp write it
    static constexpr std::size_t event_capacity = 16U;
    std::array<EventRecord, event_capacity> events_{};
    std::uint8_t event_read_{0U};
    std::uint8_t event_write_{0U};
    std::uint8_t event_count_{0U};
    std::uint32_t requested_rate_{0U};
    std::uint32_t last_set_cur_rate_{0U};
    ControlState controls_{true, 0};
    bool clock_valid_{false};
    std::uint8_t last_interface_alt_{0U};
    std::uint8_t last_packet_size_{0U};
    std::uint8_t minimum_packet_size_{0xFFU};
    std::uint8_t maximum_packet_size_{0U};
    std::uint32_t packet_count_{0U};
    std::uint32_t padding_errors_{0U};
    std::uint32_t dropped_packets_{0U};
    std::uint32_t dropped_events_{0U};
    enum class PendingReport : std::uint8_t { None, Diagnostics, EventTrace };
    PendingReport pending_report_{PendingReport::None};
    std::array<EventRecord, 7U> event_history_{};
    std::uint8_t event_history_write_{0U};
    std::uint8_t event_history_count_{0U};
    maintenance::DiagnosticReport diagnostics_{};
    EventTraceReport event_trace_{};
};
} // namespace usb
