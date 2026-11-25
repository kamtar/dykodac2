#pragma once

// User-adjustable DAC settings: the single source of truth for the RAM copy,
// the flash record and the HID configuration protocol (tools/dykodac_config).
// Every multi-byte field is little-endian; the layout is a wire format and
// must only change together with settings::version.

#include "config/board_config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace settings {

inline constexpr std::uint32_t magic = 0x46434B44U; // "DKCF"
inline constexpr std::uint16_t version = 1U;
inline constexpr std::size_t eq_band_count = 10U;

enum class UpsamplerMode : std::uint8_t {
    Interpolate4x = 0U, // MCU two-stage FFT interpolator to 176.4/192 kHz
    Bypass = 1U,        // 44.1/48 kHz straight to the CS4398 (its own filter)
};
enum class DacFilter : std::uint8_t { FastRollOff = 0U, SlowRollOff = 1U };
enum class FilterType : std::uint8_t {
    Off = 0U, Peaking = 1U, LowShelf = 2U, HighShelf = 3U,
    LowPass = 4U, HighPass = 5U, Notch = 6U,
};
inline constexpr std::uint8_t filter_type_count = 7U;
enum class Crossfeed : std::uint8_t {
    Off = 0U,
    Bs2bDefault = 1U, // 700 Hz, 4.5 dB
    ChuMoy = 2U,      // 700 Hz, 6.0 dB
    JanMeier = 3U,    // 650 Hz, 9.5 dB
};
inline constexpr std::uint8_t crossfeed_count = 4U;
inline constexpr std::uint8_t channel_left = 0x01U;
inline constexpr std::uint8_t channel_right = 0x02U;
inline constexpr std::uint8_t channel_both = 0x03U;

struct EqBand {
    std::uint8_t type{static_cast<std::uint8_t>(FilterType::Off)};
    std::uint8_t channels{channel_both};
    std::uint16_t reserved{0U};
    float frequency_hz{1000.0F};
    float gain_db{0.0F};
    float q{0.7071F};
};
static_assert(sizeof(EqBand) == 16U, "EQ band wire layout changed");

struct Settings {
    std::uint32_t magic{settings::magic};
    std::uint16_t version{settings::version};
    std::uint16_t size{0U}; // filled by defaults(); sizeof(Settings)
    std::uint8_t upsampler_mode{static_cast<std::uint8_t>(UpsamplerMode::Interpolate4x)};
    std::uint8_t dac_filter{static_cast<std::uint8_t>(DacFilter::FastRollOff)};
    std::uint8_t eq_enabled{0U};
    std::uint8_t crossfeed{static_cast<std::uint8_t>(Crossfeed::Off)};
    // Digital headroom in front of the single 24-bit quantisation, <= 0.
    // The automatic EQ/crossfeed boost compensation is subtracted on top.
    std::int16_t headroom_db_8_8{config::board::mcu_headroom_db_8_8};
    // CS4398 attenuation (05h/06h): the analog safety ceiling, 0.5 dB steps.
    std::int16_t dac_ceiling_db_8_8{config::board::dac_ceiling_db_8_8};
    // > 0 attenuates the left channel, < 0 attenuates the right channel.
    std::int16_t balance_db_8_8{0};
    std::uint8_t invert_polarity{0U}; // channel_left / channel_right bits
    std::uint8_t reserved0{0U};
    std::array<std::uint8_t, 12> reserved1{};
    std::array<EqBand, eq_band_count> bands{};
};
static_assert(sizeof(Settings) == 192U, "settings wire layout changed");
static_assert(offsetof(Settings, upsampler_mode) == 8U);
static_assert(offsetof(Settings, headroom_db_8_8) == 12U);
static_assert(offsetof(Settings, balance_db_8_8) == 16U);
static_assert(offsetof(Settings, bands) == 32U);

// Accepted ranges. Values outside are rejected by validate() rather than
// clamped so the host always sees exactly what the DAC runs.
inline constexpr std::int16_t minimum_headroom_db_8_8 = -24 * 256;
inline constexpr std::int16_t minimum_dac_ceiling_db_8_8 = -40 * 256;
inline constexpr std::int16_t maximum_balance_db_8_8 = 12 * 256;
inline constexpr float minimum_frequency_hz = 10.0F;
inline constexpr float maximum_frequency_hz = 20'000.0F;
inline constexpr float maximum_band_gain_db = 20.0F;
inline constexpr float minimum_q = 0.1F;
inline constexpr float maximum_q = 20.0F;

inline Settings defaults() noexcept {
    Settings s{};
    s.size = static_cast<std::uint16_t>(sizeof(Settings));
    return s;
}

enum class Invalid : std::uint8_t {
    None = 0U, Header, UpsamplerMode, DacFilter, Crossfeed, Headroom, DacCeiling,
    Balance, Polarity, Reserved, BandType, BandChannels, BandFrequency, BandGain, BandQ,
};

inline Invalid validate(const Settings& s) noexcept {
    if (s.magic != magic || s.version != version || s.size != sizeof(Settings)) return Invalid::Header;
    if (s.upsampler_mode > 1U) return Invalid::UpsamplerMode;
    if (s.dac_filter > 1U) return Invalid::DacFilter;
    if (s.crossfeed >= crossfeed_count) return Invalid::Crossfeed;
    if (s.eq_enabled > 1U) return Invalid::Reserved;
    if (s.headroom_db_8_8 > 0 || s.headroom_db_8_8 < minimum_headroom_db_8_8) return Invalid::Headroom;
    if (s.dac_ceiling_db_8_8 > 0 || s.dac_ceiling_db_8_8 < minimum_dac_ceiling_db_8_8 ||
        (s.dac_ceiling_db_8_8 % 128) != 0) return Invalid::DacCeiling;
    if (s.balance_db_8_8 > maximum_balance_db_8_8 || s.balance_db_8_8 < -maximum_balance_db_8_8)
        return Invalid::Balance;
    if ((s.invert_polarity & ~channel_both) != 0U) return Invalid::Polarity;
    if (s.reserved0 != 0U) return Invalid::Reserved;
    for (const auto byte : s.reserved1) if (byte != 0U) return Invalid::Reserved;
    for (const auto& band : s.bands) {
        if (band.type >= filter_type_count) return Invalid::BandType;
        if (band.reserved != 0U) return Invalid::Reserved;
        if (band.type == static_cast<std::uint8_t>(FilterType::Off)) continue;
        if (band.channels == 0U || (band.channels & ~channel_both) != 0U) return Invalid::BandChannels;
        // Negated comparisons also reject NaN.
        if (!(band.frequency_hz >= minimum_frequency_hz && band.frequency_hz <= maximum_frequency_hz))
            return Invalid::BandFrequency;
        if (!(band.gain_db >= -maximum_band_gain_db && band.gain_db <= maximum_band_gain_db))
            return Invalid::BandGain;
        if (!(band.q >= minimum_q && band.q <= maximum_q)) return Invalid::BandQ;
    }
    return Invalid::None;
}

// CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF) for the flash record.
inline std::uint32_t crc32(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (std::size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB8'8320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

} // namespace settings
