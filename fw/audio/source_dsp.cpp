#include "audio/source_dsp.hpp"

#include <cmath>

namespace audio {
namespace {
constexpr double pi = 3.14159265358979323846;
// Frequencies at which the worst-case chain magnitude is evaluated, in
// addition to every band centre: log-spaced from 10 Hz to just below fs/2.
constexpr std::size_t boost_grid_points = 192U;

struct CrossfeedPreset { double cutoff_hz; double feed_db; };
constexpr CrossfeedPreset crossfeed_presets[settings::crossfeed_count] = {
    {0.0, 0.0}, {700.0, 4.5}, {700.0, 6.0}, {650.0, 9.5},
};

// Bauer stereophonic-to-binaural (bs2b) first-order crossfeed.
CrossfeedCoefficients design_crossfeed(std::uint8_t preset, std::uint32_t rate) noexcept {
    CrossfeedCoefficients c{};
    if (preset == 0U || preset >= settings::crossfeed_count) return c;
    const double fc_lo = crossfeed_presets[preset].cutoff_hz;
    const double feed = crossfeed_presets[preset].feed_db;
    const double gb_lo = feed * -5.0 / 6.0 - 3.0;
    const double gb_hi = feed / 6.0 - 3.0;
    const double g_lo = std::pow(10.0, gb_lo / 20.0);
    const double g_hi = 1.0 - std::pow(10.0, gb_hi / 20.0);
    const double fc_hi = fc_lo * std::pow(2.0, (gb_lo - 20.0 * std::log10(g_hi)) / 12.0);
    const double x_lo = std::exp(-2.0 * pi * fc_lo / rate);
    const double x_hi = std::exp(-2.0 * pi * fc_hi / rate);
    c.enabled = true;
    c.a0_lo = static_cast<float>(g_lo * (1.0 - x_lo));
    c.b1_lo = static_cast<float>(x_lo);
    c.a0_hi = static_cast<float>(1.0 - g_hi * (1.0 - x_hi));
    c.a1_hi = static_cast<float>(-x_hi);
    c.b1_hi = static_cast<float>(x_hi);
    c.gain = static_cast<float>(1.0 / (1.0 - g_hi + g_lo));
    return c;
}

// |(n0 + n1 z^-1) / (1 - d1 z^-1)| on the unit circle.
float first_order_magnitude(float n0, float n1, float d1, float cos_w) noexcept {
    const float num = n0 * n0 + n1 * n1 + 2.0F * n0 * n1 * cos_w;
    const float den = 1.0F + d1 * d1 - 2.0F * d1 * cos_w;
    return std::sqrt(num / den);
}

// BS.1770-4 K-weighting for any sample rate (the two analog prototypes,
// bilinear transformed with prewarping), as libebur128 derives them.
void design_k_weighting(std::uint32_t rate, Biquad& shelf, Biquad& highpass) noexcept {
    {
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(pi * f0 / rate);
        const double vh = std::pow(10.0, g / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf = {static_cast<float>((vh + vb * k / q + k * k) / a0), static_cast<float>(2.0 * (k * k - vh) / a0),
                 static_cast<float>((vh - vb * k / q + k * k) / a0), static_cast<float>(2.0 * (k * k - 1.0) / a0),
                 static_cast<float>((1.0 - k / q + k * k) / a0)};
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(pi * f0 / rate);
        const double a0 = 1.0 + k / q + k * k;
        highpass = {1.0F, -2.0F, 1.0F, static_cast<float>(2.0 * (k * k - 1.0) / a0),
                    static_cast<float>((1.0 - k / q + k * k) / a0)};
    }
}

struct LevelerSpeed { double attack_s; double release_s; };
constexpr LevelerSpeed leveler_speeds[settings::leveler_mode_count] = {
    {1.5, 10.0}, {4.0, 30.0}, {1.5, 10.0}, {0.5, 3.0},
};
constexpr double leveler_block_seconds = 0.1;
// Blocks quieter than this (silence, fade tails, track gaps) hold the gain.
constexpr double leveler_gate_lufs = -50.0;

double lufs_to_power(double lufs) noexcept { return std::pow(10.0, (lufs + 0.691) / 10.0); }

LevelerDesign design_leveler(const settings::Settings& s, std::uint32_t rate) noexcept {
    LevelerDesign l{};
    const std::uint8_t mode = s.leveler_mode < settings::leveler_mode_count ? s.leveler_mode : 0U;
    l.enabled = mode != 0U;
    design_k_weighting(rate, l.k_shelf, l.k_highpass);
    l.block_frames = static_cast<std::uint32_t>(rate * leveler_block_seconds + 0.5);
    const double full_scale = 8'388'608.0; // the chain runs in 24-bit integer scale
    l.power_scale = static_cast<float>(1.0 / (static_cast<double>(l.block_frames) * full_scale * full_scale));
    l.attack = static_cast<float>(1.0 - std::exp(-leveler_block_seconds / leveler_speeds[mode].attack_s));
    l.release = static_cast<float>(1.0 - std::exp(-leveler_block_seconds / leveler_speeds[mode].release_s));
    // The gain glides with the block length as time constant, so the
    // 100 ms target steps become a smooth curve.
    l.smooth = static_cast<float>(1.0 - std::exp(-1.0 / (leveler_block_seconds * rate)));
    const double target = l.enabled ? s.leveler_target_db_8_8 / 256.0 : -18.0;
    l.target_power = static_cast<float>(lufs_to_power(target));
    l.gate_power = static_cast<float>(lufs_to_power(leveler_gate_lufs));
    l.min_gain = l.enabled ? static_cast<float>(std::pow(10.0, -s.leveler_max_cut_db / 20.0)) : 1.0F;
    return l;
}

void refresh_identity(SourceDspDesign& d) noexcept {
    d.identity = d.left_mask == 0U && d.right_mask == 0U && !d.crossfeed.enabled &&
                 d.trim_left == 1.0F && d.trim_right == 1.0F && d.makeup == 1.0F && !d.leveler.enabled;
}

float chain_magnitude(const SourceDspDesign& d, bool left, float w) noexcept {
    float m = 1.0F;
    const auto& bands = left ? d.left : d.right;
    const std::uint16_t mask = left ? d.left_mask : d.right_mask;
    for (std::size_t i = 0U; i < settings::eq_band_count; ++i)
        if ((mask & (1U << i)) != 0U) m *= biquad_magnitude(bands[i], w);
    return m;
}

float worst_case_magnitude(const SourceDspDesign& d, float frequency_hz) noexcept {
    const float w = 2.0F * static_cast<float>(pi) * frequency_hz / static_cast<float>(d.sample_rate_hz);
    const float cos_w = std::cos(w);
    const float eq_left = chain_magnitude(d, true, w);
    const float eq_right = chain_magnitude(d, false, w);
    float out_left = eq_left;
    float out_right = eq_right;
    if (d.crossfeed.enabled) {
        const auto& c = d.crossfeed;
        const float hi = first_order_magnitude(c.a0_hi, c.a1_hi, c.b1_hi, cos_w) * c.gain;
        const float lo = first_order_magnitude(c.a0_lo, 0.0F, c.b1_lo, cos_w) * c.gain;
        // Upper bound: both paths in phase with fully correlated channels.
        out_left = hi * eq_left + lo * eq_right;
        out_right = hi * eq_right + lo * eq_left;
    }
    out_left *= std::fabs(d.trim_left);
    out_right *= std::fabs(d.trim_right);
    return out_left > out_right ? out_left : out_right;
}
} // namespace

bool band_is_neutral(const settings::EqBand& band) noexcept {
    using settings::FilterType;
    const auto type = static_cast<FilterType>(band.type);
    if (type == FilterType::Off) return true;
    // A 0 dB peak or shelf is exactly unity: leave it out of the chain so a
    // flat EQ runs no filter at all.
    return (type == FilterType::Peaking || type == FilterType::LowShelf || type == FilterType::HighShelf) &&
           band.gain_db == 0.0F;
}

Biquad design_band(const settings::EqBand& band, std::uint32_t sample_rate_hz) noexcept {
    using settings::FilterType;
    const auto type = static_cast<FilterType>(band.type);
    if (type == FilterType::Off || sample_rate_hz == 0U) return {};
    // Keep every centre safely below Nyquist (20 kHz at 44.1 kHz is fine).
    double f = band.frequency_hz;
    const double nyquist_limit = 0.49 * sample_rate_hz;
    if (f > nyquist_limit) f = nyquist_limit;
    const double w0 = 2.0 * pi * f / sample_rate_hz;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * static_cast<double>(band.q));
    const double a = std::pow(10.0, static_cast<double>(band.gain_db) / 40.0);
    const double sa = 2.0 * std::sqrt(a) * alpha;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (type) {
    case FilterType::Peaking:
        b0 = 1.0 + alpha * a; b1 = -2.0 * cw; b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a; a1 = -2.0 * cw; a2 = 1.0 - alpha / a;
        break;
    case FilterType::LowShelf:
        b0 = a * ((a + 1.0) - (a - 1.0) * cw + sa);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
        b2 = a * ((a + 1.0) - (a - 1.0) * cw - sa);
        a0 = (a + 1.0) + (a - 1.0) * cw + sa;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
        a2 = (a + 1.0) + (a - 1.0) * cw - sa;
        break;
    case FilterType::HighShelf:
        b0 = a * ((a + 1.0) + (a - 1.0) * cw + sa);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
        b2 = a * ((a + 1.0) + (a - 1.0) * cw - sa);
        a0 = (a + 1.0) - (a - 1.0) * cw + sa;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cw);
        a2 = (a + 1.0) - (a - 1.0) * cw - sa;
        break;
    case FilterType::LowPass:
        b0 = (1.0 - cw) / 2.0; b1 = 1.0 - cw; b2 = (1.0 - cw) / 2.0;
        a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
        break;
    case FilterType::HighPass:
        b0 = (1.0 + cw) / 2.0; b1 = -(1.0 + cw); b2 = (1.0 + cw) / 2.0;
        a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
        break;
    case FilterType::Notch:
        b0 = 1.0; b1 = -2.0 * cw; b2 = 1.0;
        a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
        break;
    default: return {};
    }
    return {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0), static_cast<float>(b2 / a0),
            static_cast<float>(a1 / a0), static_cast<float>(a2 / a0)};
}

SvfParams design_band_svf(const settings::EqBand& band, std::uint32_t sample_rate_hz) noexcept {
    using settings::FilterType;
    const auto type = static_cast<FilterType>(band.type);
    if (type == FilterType::Off || sample_rate_hz == 0U) return {};
    // Same prewarped bilinear design as design_band(), so the responses are
    // identical (Simper, "Linear Trapezoidal Integrated State Variable Filter").
    double f = band.frequency_hz;
    const double nyquist_limit = 0.49 * sample_rate_hz;
    if (f > nyquist_limit) f = nyquist_limit;
    double g = std::tan(pi * f / sample_rate_hz);
    double k = 1.0 / static_cast<double>(band.q);
    const double a = std::pow(10.0, static_cast<double>(band.gain_db) / 40.0);
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
    switch (type) {
    case FilterType::Peaking: k = 1.0 / (static_cast<double>(band.q) * a); m1 = k * (a * a - 1.0); break;
    case FilterType::LowShelf: g /= std::sqrt(a); m1 = k * (a - 1.0); m2 = a * a - 1.0; break;
    case FilterType::HighShelf: g *= std::sqrt(a); m0 = a * a; m1 = k * (1.0 - a) * a; m2 = 1.0 - a * a; break;
    case FilterType::LowPass: m0 = 0.0; m2 = 1.0; break;
    case FilterType::HighPass: m1 = -k; m2 = -1.0; break;
    case FilterType::Notch: m1 = -k; break;
    default: return {};
    }
    return {static_cast<float>(g), static_cast<float>(k), static_cast<float>(m0), static_cast<float>(m1),
            static_cast<float>(m2)};
}

float biquad_magnitude(const Biquad& b, float w) noexcept {
    const float s = std::sin(0.5F * w);
    const float phi = s * s;
    const float bs = b.b0 + b.b1 + b.b2;
    const float as = 1.0F + b.a1 + b.a2;
    const float num = bs * bs - 4.0F * (b.b0 * b.b1 + 4.0F * b.b0 * b.b2 + b.b1 * b.b2) * phi +
                      16.0F * b.b0 * b.b2 * phi * phi;
    const float den = as * as - 4.0F * (b.a1 + 4.0F * b.a2 + b.a1 * b.a2) * phi +
                      16.0F * b.a2 * phi * phi;
    if (!(den > 0.0F) || !(num >= 0.0F)) return 0.0F;
    return std::sqrt(num / den);
}

SourceDspDesign design_source_dsp(const settings::Settings& s, std::uint32_t sample_rate_hz) noexcept {
    SourceDspDesign d{};
    d.sample_rate_hz = sample_rate_hz;
    if (sample_rate_hz == 0U) return d;
    if (s.eq_enabled != 0U) {
        for (std::size_t i = 0U; i < settings::eq_band_count; ++i) {
            const auto& band = s.bands[i];
            if (band_is_neutral(band)) continue;
            const Biquad biquad = design_band(band, sample_rate_hz);
            const SvfParams svf = design_band_svf(band, sample_rate_hz);
            const auto bit = static_cast<std::uint16_t>(1U << i);
            if ((band.channels & settings::channel_left) != 0U) {
                d.left[i] = biquad; d.left_svf[i] = svf; d.left_mask |= bit;
            }
            if ((band.channels & settings::channel_right) != 0U) {
                d.right[i] = biquad; d.right_svf[i] = svf; d.right_mask |= bit;
            }
        }
    }
    d.crossfeed = design_crossfeed(s.crossfeed, sample_rate_hz);
    const float balance_db = static_cast<float>(s.balance_db_8_8) / 256.0F;
    d.trim_left = balance_db > 0.0F ? std::pow(10.0F, -balance_db / 20.0F) : 1.0F;
    d.trim_right = balance_db < 0.0F ? std::pow(10.0F, balance_db / 20.0F) : 1.0F;
    if ((s.invert_polarity & settings::channel_left) != 0U) d.trim_left = -d.trim_left;
    if ((s.invert_polarity & settings::channel_right) != 0U) d.trim_right = -d.trim_right;
    d.leveler = design_leveler(s, sample_rate_hz);
    // The boost analysis covers the user chain only (EQ, crossfeed, trims);
    // the leveler only cuts and the loudness slots are bounded by the host
    // attenuation (see apply_loudness_compensation()).
    if (d.left_mask == 0U && d.right_mask == 0U && !d.crossfeed.enabled &&
        d.trim_left == 1.0F && d.trim_right == 1.0F) {
        refresh_identity(d);
        return d;
    }

    float peak = 0.0F;
    const float top = 0.499F * static_cast<float>(sample_rate_hz);
    const float ratio = std::pow(top / 10.0F, 1.0F / static_cast<float>(boost_grid_points - 1U));
    float f = 10.0F;
    for (std::size_t i = 0U; i < boost_grid_points; ++i, f *= ratio) {
        const float m = worst_case_magnitude(d, f);
        if (m > peak) peak = m;
    }
    for (const auto& band : s.bands) {
        if (s.eq_enabled == 0U || band_is_neutral(band)) continue;
        if (band.frequency_hz < top) {
            const float m = worst_case_magnitude(d, band.frequency_hz);
            if (m > peak) peak = m;
        }
    }
    // A 0.1 dB margin covers peaks between grid points.
    d.boost_db = peak > 1.0F ? 20.0F * std::log10(peak) + 0.1F : 0.0F;
    d.makeup = d.boost_db > 0.0F ? std::pow(10.0F, -d.boost_db / 20.0F) : 1.0F;
    refresh_identity(d);
    return d;
}

void loudness_shelf_gains(const settings::Settings& s, std::int16_t host_volume_db_8_8,
                          float& bass_db, float& treble_db) noexcept {
    bass_db = 0.0F;
    treble_db = 0.0F;
    if (s.loudness_enabled == 0U) return;
    const float attenuation =
        static_cast<float>(static_cast<std::int32_t>(s.loudness_reference_db_8_8) - host_volume_db_8_8) / 256.0F;
    if (!(attenuation > 0.0F)) return;
    const float amount = static_cast<float>(s.loudness_amount_pct) / 100.0F;
    bass_db = loudness_bass_per_db * amount * attenuation;
    treble_db = loudness_treble_per_db * amount * attenuation;
    const float cap = static_cast<float>(s.loudness_max_boost_db);
    if (bass_db > cap) {
        // Keep the curve's shape: both shelves stop growing together.
        treble_db *= cap / bass_db;
        bass_db = cap;
    }
}

void apply_loudness_compensation(SourceDspDesign& d, const settings::Settings& s,
                                 std::int16_t host_volume_db_8_8) noexcept {
    float bass = 0.0F;
    float treble = 0.0F;
    loudness_shelf_gains(s, host_volume_db_8_8, bass, treble);
    constexpr auto bass_bit = static_cast<std::uint16_t>(1U << loudness_bass_slot);
    constexpr auto treble_bit = static_cast<std::uint16_t>(1U << loudness_treble_slot);
    d.left_mask = static_cast<std::uint16_t>(d.left_mask & ~(bass_bit | treble_bit));
    d.right_mask = static_cast<std::uint16_t>(d.right_mask & ~(bass_bit | treble_bit));
    d.left_svf[loudness_bass_slot] = d.right_svf[loudness_bass_slot] = {};
    d.left_svf[loudness_treble_slot] = d.right_svf[loudness_treble_slot] = {};
    // Below 0.01 dB the slot is left out (and the chain can go idle).
    d.loudness_bass_db = bass >= 0.01F ? bass : 0.0F;
    d.loudness_treble_db = treble >= 0.01F ? treble : 0.0F;
    if (d.sample_rate_hz != 0U && d.loudness_bass_db > 0.0F) {
        const settings::EqBand band{static_cast<std::uint8_t>(settings::FilterType::LowShelf), settings::channel_both,
                                    0U, loudness_bass_hz, d.loudness_bass_db, loudness_bass_q};
        d.left_svf[loudness_bass_slot] = d.right_svf[loudness_bass_slot] = design_band_svf(band, d.sample_rate_hz);
        d.left_mask |= bass_bit;
        d.right_mask |= bass_bit;
    }
    if (d.sample_rate_hz != 0U && d.loudness_treble_db > 0.0F) {
        const settings::EqBand band{static_cast<std::uint8_t>(settings::FilterType::HighShelf), settings::channel_both,
                                    0U, loudness_treble_hz, d.loudness_treble_db, loudness_treble_q};
        d.left_svf[loudness_treble_slot] = d.right_svf[loudness_treble_slot] =
            design_band_svf(band, d.sample_rate_hz);
        d.left_mask |= treble_bit;
        d.right_mask |= treble_bit;
    }
    refresh_identity(d);
}

void SourceDsp::update_coefficients(Band& b) noexcept {
    const float h = b.p.g * (b.p.g + b.p.k);
    const float a1 = 1.0F / (1.0F + h);
    b.d1 = h * a1;
    b.a2 = b.p.g * a1;
    b.a3 = b.p.g * b.a2;
}

void SourceDsp::snap() noexcept {
    for (std::size_t i = 0U; i < dsp_band_count; ++i) {
        left_[i] = {};
        right_[i] = {};
        const auto bit = static_cast<std::uint16_t>(1U << i);
        if ((design_.left_mask & bit) != 0U) left_[i].p = design_.left_svf[i];
        if ((design_.right_mask & bit) != 0U) right_[i].p = design_.right_svf[i];
        left_[i].from = left_[i].to = left_[i].p;
        right_[i].from = right_[i].to = right_[i].p;
        update_coefficients(left_[i]);
        update_coefficients(right_[i]);
    }
    live_left_mask_ = design_.left_mask;
    live_right_mask_ = design_.right_mask;
    trim_left_ = trim_left_from_ = trim_left_to_ = design_.trim_left * design_.makeup;
    trim_right_ = trim_right_from_ = trim_right_to_ = design_.trim_right * design_.makeup;
    xf_ = design_.crossfeed;
    xf_active_ = xf_.enabled;
    xf_wet_ = xf_wet_from_ = xf_wet_to_ = xf_.enabled ? 1.0F : 0.0F;
    lo_left_ = lo_right_ = hi_left_ = hi_right_ = in_left_ = in_right_ = 0.0F;
    ramp_steps_total_ = ramp_steps_left_ = ramp_phase_ = 0U;
    adopt_leveler(design_.leveler, true);
    update_idle();
}

void SourceDsp::set_design(const SourceDspDesign& design) noexcept {
    const bool rate_changed = design.sample_rate_hz != design_.sample_rate_hz;
    design_ = design;
    if (fresh_ || rate_changed) {
        // Stream (re)start: the signal begins from silence, nothing to glide.
        fresh_ = false;
        snap();
        return;
    }
    auto retarget = [](Band& b, bool live, bool target, const SvfParams& p) {
        if (!live && !target) return;
        if (!live) {
            // Fade in from unity with the final tuning, from rest.
            b = {};
            b.p = SvfParams{p.g, p.k, 1.0F, 0.0F, 0.0F};
            update_coefficients(b);
        }
        b.from = b.p;
        // A band that goes away fades to unity with its current tuning.
        b.to = target ? p : SvfParams{b.p.g, b.p.k, 1.0F, 0.0F, 0.0F};
    };
    for (std::size_t i = 0U; i < dsp_band_count; ++i) {
        const auto bit = static_cast<std::uint16_t>(1U << i);
        retarget(left_[i], (live_left_mask_ & bit) != 0U, (design.left_mask & bit) != 0U, design.left_svf[i]);
        retarget(right_[i], (live_right_mask_ & bit) != 0U, (design.right_mask & bit) != 0U, design.right_svf[i]);
    }
    live_left_mask_ |= design.left_mask;
    live_right_mask_ |= design.right_mask;

    trim_left_from_ = trim_left_;
    trim_right_from_ = trim_right_;
    trim_left_to_ = design.trim_left * design.makeup;
    trim_right_to_ = design.trim_right * design.makeup;

    // Crossfeed glides by its wet amount. New coefficients take over at
    // once when it stays on (its one-pole states carry over smoothly); when
    // it turns off, the old ones keep running until the fade-out ends.
    if (design.crossfeed.enabled) {
        if (!xf_active_) {
            lo_left_ = lo_right_ = hi_left_ = hi_right_ = in_left_ = in_right_ = 0.0F;
            xf_wet_ = 0.0F;
        }
        xf_ = design.crossfeed;
        xf_active_ = true;
    }
    xf_wet_from_ = xf_wet_;
    xf_wet_to_ = design.crossfeed.enabled ? 1.0F : 0.0F;
    adopt_leveler(design.leveler, false);

    const float steps = ramp_seconds * static_cast<float>(design.sample_rate_hz) /
                        static_cast<float>(ramp_block_frames);
    ramp_steps_total_ = steps >= 1.0F ? static_cast<std::uint32_t>(steps + 0.5F) : 1U;
    ramp_steps_left_ = ramp_steps_total_;
    ramp_phase_ = 0U;
    idle_ = false;
}

void SourceDsp::advance_ramp() noexcept {
    ramp_phase_ = 0U;
    --ramp_steps_left_;
    const float t = 1.0F - static_cast<float>(ramp_steps_left_) / static_cast<float>(ramp_steps_total_);
    auto lerp = [t](float a, float b) { return a + (b - a) * t; };
    auto glide = [&](Band& b) {
        b.p.g = lerp(b.from.g, b.to.g);
        b.p.k = lerp(b.from.k, b.to.k);
        b.p.m0 = lerp(b.from.m0, b.to.m0);
        b.p.m1 = lerp(b.from.m1, b.to.m1);
        b.p.m2 = lerp(b.from.m2, b.to.m2);
        update_coefficients(b);
    };
    for (std::size_t i = 0U; i < dsp_band_count; ++i) {
        const auto bit = static_cast<std::uint16_t>(1U << i);
        if ((live_left_mask_ & bit) != 0U) glide(left_[i]);
        if ((live_right_mask_ & bit) != 0U) glide(right_[i]);
    }
    trim_left_ = lerp(trim_left_from_, trim_left_to_);
    trim_right_ = lerp(trim_right_from_, trim_right_to_);
    xf_wet_ = lerp(xf_wet_from_, xf_wet_to_);
    if (ramp_steps_left_ != 0U) return;

    // Glide finished: land exactly on the design and drop faded-out parts.
    for (std::size_t i = 0U; i < dsp_band_count; ++i) {
        const auto bit = static_cast<std::uint16_t>(1U << i);
        if ((design_.left_mask & bit) == 0U) left_[i] = {};
        if ((design_.right_mask & bit) == 0U) right_[i] = {};
    }
    live_left_mask_ = design_.left_mask;
    live_right_mask_ = design_.right_mask;
    trim_left_ = trim_left_to_;
    trim_right_ = trim_right_to_;
    if (!design_.crossfeed.enabled) {
        xf_active_ = false;
        xf_ = {};
        lo_left_ = lo_right_ = hi_left_ = hi_right_ = in_left_ = in_right_ = 0.0F;
    }
    xf_wet_ = xf_wet_to_;
    update_idle();
}

void SourceDsp::update_idle() noexcept {
    idle_ = design_.identity && !lev_.enabled && lev_gain_ == 1.0F && lev_target_ == 1.0F &&
            ramp_steps_left_ == 0U;
}

void SourceDsp::adopt_leveler(const LevelerDesign& design, bool restart_meter) noexcept {
    const bool was_enabled = lev_.enabled;
    const bool rate_changed = design.block_frames != lev_.block_frames;
    lev_ = design;
    if (!lev_.enabled) {
        lev_target_ = 1.0F;
        // A restart begins from silence: no need to glide back.
        if (restart_meter) lev_gain_ = 1.0F;
        return;
    }
    if (restart_meter || rate_changed || !was_enabled) {
        for (auto& state : k_state_) state = 0.0F;
        block_energy_ = 0.0F;
        block_fill_ = 0U;
    }
    // Switching on starts at unity, as if the music sat at the target.
    if (!was_enabled) lev_power_ = lev_.target_power;
    // A new target or maximum cut takes effect at once (smoothed).
    float gain = 1.0F;
    if (lev_power_ > lev_.target_power) gain = std::sqrt(lev_.target_power / lev_power_);
    lev_target_ = gain < lev_.min_gain ? lev_.min_gain : gain;
}

void SourceDsp::end_meter_block() noexcept {
    const float z = block_energy_ * lev_.power_scale;
    block_energy_ = 0.0F;
    block_fill_ = 0U;
    if (z >= lev_.gate_power) lev_power_ += (z - lev_power_) * (z > lev_power_ ? lev_.attack : lev_.release);
    float gain = 1.0F;
    if (lev_power_ > lev_.target_power) gain = std::sqrt(lev_.target_power / lev_power_);
    lev_target_ = gain < lev_.min_gain ? lev_.min_gain : gain;
}

float SourceDsp::leveler_gain_db() const noexcept {
    return lev_gain_ > 0.0F ? 20.0F * std::log10(lev_gain_) : -200.0F;
}

void SourceDsp::reset_state() noexcept {
    // Clears every filter state and makes the next set_design() snap.
    fresh_ = true;
    snap();
}

} // namespace audio
