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
    d.identity = d.left_mask == 0U && d.right_mask == 0U && !d.crossfeed.enabled &&
                 d.trim_left == 1.0F && d.trim_right == 1.0F;
    if (d.identity) return d;

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
    d.makeup = std::pow(10.0F, -d.boost_db / 20.0F);
    return d;
}

void SourceDsp::update_coefficients(Band& b) noexcept {
    const float h = b.p.g * (b.p.g + b.p.k);
    const float a1 = 1.0F / (1.0F + h);
    b.d1 = h * a1;
    b.a2 = b.p.g * a1;
    b.a3 = b.p.g * b.a2;
}

void SourceDsp::snap() noexcept {
    for (std::size_t i = 0U; i < settings::eq_band_count; ++i) {
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
    idle_ = design_.identity;
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
    for (std::size_t i = 0U; i < settings::eq_band_count; ++i) {
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
    for (std::size_t i = 0U; i < settings::eq_band_count; ++i) {
        const auto bit = static_cast<std::uint16_t>(1U << i);
        if ((live_left_mask_ & bit) != 0U) glide(left_[i]);
        if ((live_right_mask_ & bit) != 0U) glide(right_[i]);
    }
    trim_left_ = lerp(trim_left_from_, trim_left_to_);
    trim_right_ = lerp(trim_right_from_, trim_right_to_);
    xf_wet_ = lerp(xf_wet_from_, xf_wet_to_);
    if (ramp_steps_left_ != 0U) return;

    // Glide finished: land exactly on the design and drop faded-out parts.
    for (std::size_t i = 0U; i < settings::eq_band_count; ++i) {
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
    idle_ = design_.identity;
}

void SourceDsp::reset_state() noexcept {
    // Clears every filter state and makes the next set_design() snap.
    fresh_ = true;
    snap();
}

} // namespace audio
