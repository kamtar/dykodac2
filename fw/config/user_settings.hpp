#pragma once

// User-adjustable DAC settings: the single source of truth for the RAM copy,
// the flash record and the HID configuration protocol (tools/dykodac_config).
// Every multi-byte field is little-endian; the layout is a wire format and
// must only change together with settings::version.

#include "config/board_config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
    // Mid/side forms: the centre (L = R) passes untouched, only the side
    // signal is shaped (audio/source_dsp.cpp).
    Natural = 4U,     // side bass cut, ~0.45 ms interaural delay
    Speakers30 = 5U,  // spherical-head virtual speakers at +-30 deg
    Speakers45 = 6U,  // the same at +-45 deg
    Spacious = 7U,    // full-range delayed crossfeed, side widened to keep reverb
};
inline constexpr std::uint8_t crossfeed_count = 8U;
// Automatic loudness leveler: off or its speed (attack / release).
enum class Leveler : std::uint8_t { Off = 0U, Slow = 1U, Medium = 2U, Fast = 3U };
inline constexpr std::uint8_t leveler_mode_count = 4U;
inline constexpr std::uint8_t channel_left = 0x01U;
inline constexpr std::uint8_t channel_right = 0x02U;
inline constexpr std::uint8_t channel_both = 0x03U;
inline constexpr std::uint8_t detail_proximity = 0x01U;
inline constexpr std::uint8_t detail_transient = 0x02U;
inline constexpr std::uint8_t detail_width = 0x04U;
inline constexpr std::uint8_t detail_front = 0x08U;
inline constexpr std::uint8_t detail_all = 0x0FU;

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
    // Detail stage (audio/source_dsp.hpp): detail_proximity / _transient /
    // _width / _front bits. Its other fields are only checked while the
    // part is on.
    std::uint8_t detail_flags{0U};
    // Loudness leveler (cut-only AGC on BS.1770 K-weighted loudness of the
    // input). Its other fields are only checked while it is on.
    std::uint8_t leveler_mode{static_cast<std::uint8_t>(Leveler::Off)};
    std::uint8_t leveler_max_cut_db{12U};
    std::int16_t leveler_target_db_8_8{-18 * 256}; // LUFS (dB re full scale)
    // Loudness compensation (ISO 226 bass/treble shelves that follow host
    // volume below the reference volume).
    std::uint8_t loudness_enabled{0U};
    std::uint8_t loudness_amount_pct{70U};
    std::int16_t loudness_reference_db_8_8{-10 * 256}; // host volume, flat there and above
    std::uint8_t loudness_max_boost_db{12U};
    // Proximity: interaural level difference expansion, 100 % doubles it.
    std::uint8_t proximity_amount_pct{50U};
    // Transient shaper: attack boost of the band above the crossover,
    // 100 % = maximum_transient_boost_db.
    std::uint8_t transient_amount_pct{50U};
    std::uint8_t transient_crossover_100hz{25U}; // 2.5 kHz
    std::array<EqBand, eq_band_count> bands{};
    // ---- appended in the 200-byte layout (legacy records are 192 bytes) ----
    // Width expander: side boost above 300 Hz that grows with the side/mid
    // ratio, 100 % = maximum_width_boost_db for fully wide sound.
    std::uint8_t width_amount_pct{50U};
    // Front cue: mid/side spectral cue that pulls the centre image forward,
    // 100 % = maximum_front_mid_cut_db / maximum_front_side_boost_db.
    std::uint8_t front_amount_pct{50U};
    std::array<std::uint8_t, 6> reserved{};
};
static_assert(sizeof(Settings) == 200U, "settings wire layout changed");
static_assert(offsetof(Settings, upsampler_mode) == 8U);
static_assert(offsetof(Settings, headroom_db_8_8) == 12U);
static_assert(offsetof(Settings, balance_db_8_8) == 16U);
static_assert(offsetof(Settings, leveler_mode) == 20U);
static_assert(offsetof(Settings, leveler_target_db_8_8) == 22U);
static_assert(offsetof(Settings, loudness_reference_db_8_8) == 26U);
static_assert(offsetof(Settings, detail_flags) == 19U);
static_assert(offsetof(Settings, proximity_amount_pct) == 29U);
static_assert(offsetof(Settings, bands) == 32U);
static_assert(offsetof(Settings, width_amount_pct) == 192U);
// Size of the original layout (up to and including the EQ bands).
inline constexpr std::size_t legacy_size = 192U;

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
inline constexpr std::int16_t minimum_leveler_target_db_8_8 = -36 * 256;
inline constexpr std::int16_t maximum_leveler_target_db_8_8 = -6 * 256;
inline constexpr std::uint8_t minimum_leveler_cut_db = 1U;
inline constexpr std::uint8_t maximum_leveler_cut_db = 30U;
inline constexpr std::uint8_t minimum_loudness_amount_pct = 10U;
inline constexpr std::uint8_t maximum_loudness_amount_pct = 100U;
inline constexpr std::int16_t minimum_loudness_reference_db_8_8 = -60 * 256;
inline constexpr std::uint8_t minimum_loudness_boost_db = 1U;
inline constexpr std::uint8_t maximum_loudness_boost_db = 20U;
inline constexpr std::uint8_t minimum_detail_amount_pct = 10U;
inline constexpr std::uint8_t maximum_detail_amount_pct = 100U;
inline constexpr std::uint8_t minimum_transient_crossover_100hz = 10U; // 1 kHz
inline constexpr std::uint8_t maximum_transient_crossover_100hz = 80U; // 8 kHz
inline constexpr float maximum_transient_boost_db = 10.0F;
inline constexpr float maximum_width_boost_db = 6.0F;
inline constexpr float maximum_front_mid_cut_db = 6.0F;    // at 8 kHz
inline constexpr float maximum_front_side_boost_db = 3.0F; // at 3 kHz

inline Settings defaults() noexcept {
    Settings s{};
    s.size = static_cast<std::uint16_t>(sizeof(Settings));
    return s;
}

enum class Invalid : std::uint8_t {
    None = 0U, Header, UpsamplerMode, DacFilter, Crossfeed, Headroom, DacCeiling,
    Balance, Polarity, Reserved, BandType, BandChannels, BandFrequency, BandGain, BandQ,
    Leveler, Loudness, Proximity, Transient, Width, Front,
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
    if ((s.detail_flags & ~detail_all) != 0U) return Invalid::Reserved;
    // Records written before these features existed hold zeros here: off.
    if (s.leveler_mode >= leveler_mode_count) return Invalid::Leveler;
    if (s.leveler_mode != 0U &&
        (s.leveler_target_db_8_8 < minimum_leveler_target_db_8_8 ||
         s.leveler_target_db_8_8 > maximum_leveler_target_db_8_8 ||
         s.leveler_max_cut_db < minimum_leveler_cut_db || s.leveler_max_cut_db > maximum_leveler_cut_db))
        return Invalid::Leveler;
    if (s.loudness_enabled > 1U) return Invalid::Loudness;
    if (s.loudness_enabled != 0U &&
        (s.loudness_amount_pct < minimum_loudness_amount_pct ||
         s.loudness_amount_pct > maximum_loudness_amount_pct ||
         s.loudness_reference_db_8_8 > 0 ||
         s.loudness_reference_db_8_8 < minimum_loudness_reference_db_8_8 ||
         s.loudness_max_boost_db < minimum_loudness_boost_db ||
         s.loudness_max_boost_db > maximum_loudness_boost_db))
        return Invalid::Loudness;
    auto amount_ok = [](std::uint8_t pct) {
        return pct >= minimum_detail_amount_pct && pct <= maximum_detail_amount_pct;
    };
    if ((s.detail_flags & detail_proximity) != 0U && !amount_ok(s.proximity_amount_pct))
        return Invalid::Proximity;
    if ((s.detail_flags & detail_transient) != 0U &&
        (!amount_ok(s.transient_amount_pct) ||
         s.transient_crossover_100hz < minimum_transient_crossover_100hz ||
         s.transient_crossover_100hz > maximum_transient_crossover_100hz))
        return Invalid::Transient;
    if ((s.detail_flags & detail_width) != 0U && !amount_ok(s.width_amount_pct)) return Invalid::Width;
    if ((s.detail_flags & detail_front) != 0U && !amount_ok(s.front_amount_pct)) return Invalid::Front;
    for (const auto byte : s.reserved)
        if (byte != 0U) return Invalid::Reserved;
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

// Records saved before the leveler, loudness compensation or detail stage existed hold
// zeros in those fields (both off). Give them the default parameters, so the
// running copy matches what a host writes back and switching on starts from
// sensible values. Settings that are switched on are left alone.
inline void upgrade(Settings& s) noexcept {
    const Settings d{};
    if (s.leveler_mode == 0U && s.leveler_max_cut_db == 0U && s.leveler_target_db_8_8 == 0) {
        s.leveler_max_cut_db = d.leveler_max_cut_db;
        s.leveler_target_db_8_8 = d.leveler_target_db_8_8;
    }
    if (s.loudness_enabled == 0U && s.loudness_amount_pct == 0U && s.loudness_reference_db_8_8 == 0 &&
        s.loudness_max_boost_db == 0U) {
        s.loudness_amount_pct = d.loudness_amount_pct;
        s.loudness_reference_db_8_8 = d.loudness_reference_db_8_8;
        s.loudness_max_boost_db = d.loudness_max_boost_db;
    }
    if (s.detail_flags == 0U && s.proximity_amount_pct == 0U && s.transient_amount_pct == 0U &&
        s.transient_crossover_100hz == 0U) {
        s.proximity_amount_pct = d.proximity_amount_pct;
        s.transient_amount_pct = d.transient_amount_pct;
        s.transient_crossover_100hz = d.transient_crossover_100hz;
    }
    if ((s.detail_flags & (detail_width | detail_front)) == 0U && s.width_amount_pct == 0U &&
        s.front_amount_pct == 0U) {
        s.width_amount_pct = d.width_amount_pct;
        s.front_amount_pct = d.front_amount_pct;
    }
}

// Reads a stored record of the current or the legacy (192-byte) layout.
// The fields a legacy record lacks become zero (off) and upgrade() then
// gives them their defaults. False for any other length.
inline bool from_record(Settings& out, const void* data, std::size_t length) noexcept {
    if (length != sizeof(Settings) && length != legacy_size) return false;
    std::memset(static_cast<void*>(&out), 0, sizeof(out));
    std::memcpy(&out, data, length);
    if (length == legacy_size && out.size == legacy_size) out.size = static_cast<std::uint16_t>(sizeof(Settings));
    return true;
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
