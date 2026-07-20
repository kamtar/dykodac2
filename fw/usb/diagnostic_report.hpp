#pragma once

#include <cstddef>
#include <cstdint>

namespace usb::maintenance {

inline constexpr std::uint32_t diagnostic_magic = 0x32444B44U; // "DKD2"
inline constexpr std::uint8_t diagnostic_version = 1U;

// One full-speed/ high-speed HID interrupt packet. Multi-byte fields are
// little-endian, matching the RT1011 and all supported host tools.
struct DiagnosticReport {
    std::uint32_t magic{diagnostic_magic};
    std::uint8_t version{diagnostic_version};
    std::uint8_t size{64U};
    std::uint16_t sequence{0U};
    std::uint32_t uptime_ms{0U};
    std::uint32_t last_set_cur_rate_hz{0U};
    std::uint32_t desired_rate_hz{0U};
    std::uint32_t active_rate_hz{0U};
    std::uint32_t feedback_16_16{0U};
    std::uint32_t usb_packet_count{0U};
    std::uint32_t dropped_packets{0U};
    std::uint32_t dropped_events{0U};
    std::uint32_t dma_completions{0U};
    std::uint16_t dma_errors{0U};
    std::uint16_t underruns{0U};
    std::uint16_t overflows{0U};
    std::uint16_t padding_errors{0U};
    std::uint16_t ring_fill_bytes{0U};
    std::uint8_t last_packet_bytes{0U};
    std::uint8_t minimum_packet_bytes{0U};
    std::uint8_t maximum_packet_bytes{0U};
    std::uint8_t start_step{0U};
    std::uint8_t last_stop_step{0U};
    std::uint8_t stop_reason{0U};
    std::uint8_t event_queue_depth{0U};
    std::uint8_t last_interface_alt{0U};
    // bits 0..3 app state; bit 4 selected 48k family; bit 5 alt 1;
    // bit 6 stream requested; bit 7 host mute control.
    std::uint8_t state{0U};
    // bit 0 mounted; bit 1 suspended; bit 2 clock stable; bit 3 raw
    // oscillator-select level; bit 4 DAC muted; bit 5 relay connected;
    // bit 6 engine running; bit 7 UAC2 clock valid.
    std::uint8_t flags{0U};
};

static_assert(sizeof(DiagnosticReport) == 64U, "maintenance report must fit one HID packet");
static_assert(offsetof(DiagnosticReport, uptime_ms) == 8U, "diagnostic ABI changed");

} // namespace usb::maintenance
