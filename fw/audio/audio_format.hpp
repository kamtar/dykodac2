#pragma once

#include <cstdint>

namespace audio {
enum class ClockFamily : std::uint8_t { Family44k1, Family48k };

struct AudioFormat {
    std::uint32_t sample_rate_hz;
    std::uint8_t channels;
    std::uint8_t container_bits;
    std::uint8_t valid_bits;
};

struct FormatConfig {
    AudioFormat format;
    ClockFamily family;
    std::uint32_t oscillator_hz;
    std::uint16_t mclk_ratio;
    std::uint16_t usb_hs_max_packet_bytes;
    std::uint16_t usb_hs_interval_microframes;
    std::uint16_t dma_frames_per_transfer;
    std::uint32_t feedback_nominal_16_16;
    std::uint8_t dac_mode;
};

inline constexpr std::uint32_t bytes_per_frame(const AudioFormat& f) noexcept {
    return static_cast<std::uint32_t>(f.channels) * (f.container_bits / 8U);
}
} // namespace audio
