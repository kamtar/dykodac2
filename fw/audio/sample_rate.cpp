#include "audio/sample_rate.hpp"
#include "audio/format_values.h"

#include <array>

namespace audio {
namespace {
constexpr std::uint16_t output_dma_frames(std::uint32_t rate_hz) {
    // Keep each eDMA transfer close to one millisecond in its own output
    // clock domain. The fractional remainder is naturally carried by the
    // oscillator itself, not by this block-size choice.
    return static_cast<std::uint16_t>(rate_hz / 1'000U);
}

constexpr std::uint16_t max_hs_packet(std::uint32_t rate) {
    // HS UAC2 interval is 125 us. An asynchronous sink must accept the
    // rounded-up nominal payload plus one correction frame.
    return static_cast<std::uint16_t>((((rate + 7'999U) / 8'000U) + 1U) * 8U);
}

constexpr std::array<FormatConfig, 2> formats{{
    {{DYKODAC_RATE_44K1, DYKODAC_CHANNELS, DYKODAC_SUBSLOT_BYTES * 8U, DYKODAC_VALID_BITS}, ClockFamily::Family44k1, DYKODAC_OSC_44K1, 512U,
     max_hs_packet(DYKODAC_RATE_44K1), 1U, output_dma_frames(DYKODAC_OUTPUT_RATE_44K1),
     (DYKODAC_RATE_44K1 << 16U) / 8'000U, 0U,
     DYKODAC_OUTPUT_RATE_44K1, DYKODAC_OSC_44K1, ClockFamily::Family44k1},
    {{DYKODAC_RATE_48K, DYKODAC_CHANNELS, DYKODAC_SUBSLOT_BYTES * 8U, DYKODAC_VALID_BITS}, ClockFamily::Family48k, DYKODAC_OSC_48K, 512U,
     max_hs_packet(DYKODAC_RATE_48K), 1U, output_dma_frames(DYKODAC_OUTPUT_RATE_48K),
     (DYKODAC_RATE_48K << 16U) / 8'000U, 0U,
     DYKODAC_OUTPUT_RATE_48K, DYKODAC_OSC_48K, ClockFamily::Family48k},
}};
static_assert(DYKODAC_OUTPUT_RATE_44K1 * 128U == DYKODAC_OSC_44K1);
static_assert(DYKODAC_OUTPUT_RATE_48K * 128U == DYKODAC_OSC_48K);
static_assert(max_hs_packet(DYKODAC_RATE_44K1) == DYKODAC_USB_HS_MAX_PACKET);
static_assert(max_hs_packet(DYKODAC_RATE_48K) == DYKODAC_USB_HS_MAX_PACKET);
} // namespace

const FormatConfig* find_format(std::uint32_t sample_rate_hz) noexcept {
    for (const auto& config : formats) {
        if (config.format.sample_rate_hz == sample_rate_hz) return &config;
    }
    return nullptr;
}

std::size_t supported_format_count() noexcept { return formats.size(); }
const FormatConfig& supported_format(std::size_t index) noexcept { return formats[index]; }

std::uint16_t usb_hs_packet_bytes(const FormatConfig& config, std::uint32_t microframe) noexcept {
    const std::uint32_t frames_before = (microframe * config.format.sample_rate_hz) / 8'000U;
    const std::uint32_t frames_after = ((microframe + 1U) * config.format.sample_rate_hz) / 8'000U;
    return static_cast<std::uint16_t>((frames_after - frames_before) * bytes_per_frame(config.format));
}
} // namespace audio
