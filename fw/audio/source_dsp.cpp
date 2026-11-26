#include "audio/source_dsp.hpp"

#include <cmath>
#include <complex>

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

std::uint16_t frames(double seconds, std::uint32_t rate) noexcept {
    return static_cast<std::uint16_t>(seconds * rate + 0.5);
}

FirstOrder one_pole_lowpass(double fc, double gain, std::uint32_t rate) noexcept {
    const double x = std::exp(-2.0 * pi * fc / rate);
    return {static_cast<float>(gain * (1.0 - x)), 0.0F, static_cast<float>(x)};
}

FirstOrder one_pole_highpass(double fc, std::uint32_t rate) noexcept {
    const double x = std::exp(-2.0 * pi * fc / rate);
    return {static_cast<float>(0.5 * (1.0 + x)), static_cast<float>(-0.5 * (1.0 + x)), static_cast<float>(x)};
}

// "Natural": the centre passes untouched; the side signal loses its bass
// through a 500 Hz low-passed copy of itself, 0.1 ms late, so a panned
// source reaches the far ear 0.45 ms later (delay plus low-pass lag, about
// a +-30 deg speaker) and 10 dB lower at low frequencies, as from a head.
// S' = (S - 2 g LP(S[n-d])) * peak: the 1.2 kHz cut removes the +2 dB
// bump where the delayed copy turns in phase (worst-case gain 2 -> 0.4 dB),
// and leaves the side within 0.5 dB above 2.5 kHz.
void design_natural(CrossfeedCoefficients& c, std::uint32_t rate) noexcept {
    constexpr double feed = 0.25; // g
    c.near = {};
    c.far = one_pole_lowpass(500.0, 2.0 * feed, rate);
    c.far_delay = frames(0.10e-3, rate);
    c.reflection_count = 0U;
    const settings::EqBand post{static_cast<std::uint8_t>(settings::FilterType::Peaking), settings::channel_both,
                                0U, 1'200.0F, -2.0F, 0.5F};
    c.post_enabled = true;
    c.post = design_band(post, rate);
    c.post_svf = design_band_svf(post, rate);
}

// Virtual speakers at +-azimuth: Brown & Duda spherical head (radius
// 8.75 cm) with its one-pole/one-zero head shadow per ear and the Woodworth
// interaural delay, plus six early reflections per ear. Run in mid/side
// form: with the physical near-ear response D and far-ear response C, the
// side signal gets (D - C) / 2 and the mid signal passes flat instead of
// (D + C) / 2. A hard-panned source keeps the model's interaural delay at
// low frequencies and its level difference at high ones, while the
// phantom centre avoids the ~10 dB crosstalk comb (1-1.4 kHz) that no EQ
// can fill without colouring every panned source and costing ~11 dB of
// headroom. Reflections are band-limited (500 Hz - 6 kHz) so they add
// space without a low-mid comb.
void design_speakers(CrossfeedCoefficients& c, double azimuth_deg, std::uint32_t rate) noexcept {
    constexpr double radius = 0.0875;
    constexpr double sound = 343.0;
    const double az = azimuth_deg * pi / 180.0;
    // Head shadow (1 + alpha s / 2w0) / (1 + s / 2w0), w0 = c / a, with
    // alpha from the angle between source and ear axis (Brown-Duda).
    auto shadow = [&](double from_ear_deg) {
        const double alpha = 1.05 + 0.95 * std::cos(from_ear_deg / 150.0 * pi);
        const double kt = 2.0 * rate * radius / (2.0 * sound);
        const double den = 1.0 + kt;
        // Folded in: the 1/2 that normalises D + C to unity at DC.
        return FirstOrder{static_cast<float>(0.5 * (1.0 + alpha * kt) / den),
                          static_cast<float>(0.5 * (1.0 - alpha * kt) / den),
                          static_cast<float>(-(1.0 - kt) / den)};
    };
    c.near = shadow(90.0 - azimuth_deg);
    c.far = shadow(90.0 + azimuth_deg);
    c.far_delay = frames(radius / sound * (az + std::sin(az)), rate);
    // (near-ear ms, gain, far-ear ms, gain): desk/floor, near and far side
    // walls, ceiling, two late ones.
    struct Reflection { double near_ms; double near_gain; double far_ms; double far_gain; };
    constexpr Reflection pattern[] = {
        {1.30, 0.20, 1.55, 0.15}, {2.90, 0.16, 3.20, 0.10}, {3.70, 0.09, 3.95, 0.13},
        {4.80, 0.12, 5.00, 0.11}, {6.90, 0.08, 7.30, 0.07}, {8.60, 0.07, 8.85, 0.07},
    };
    constexpr double level = 0.35 * 0.5; // reflection level and the DC normalisation
    std::size_t n = 0U;
    for (const auto& r : pattern) {
        c.reflections[n++] = {frames(r.near_ms * 1.0e-3, rate), static_cast<float>(level * r.near_gain)};
        c.reflections[n++] = {frames(r.far_ms * 1.0e-3, rate), static_cast<float>(-level * r.far_gain)};
    }
    c.reflection_count = static_cast<std::uint8_t>(n);
    c.reflection_highpass = one_pole_highpass(500.0, rate);
    c.reflection_lowpass = one_pole_lowpass(6'000.0, 1.0, rate);
    c.post_enabled = false;
}

// "Spacious": crossfeed keeps the recording's own reverberation instead of
// trading it for localisation. Reverb lives mostly in the side signal, and
// an ordinary crossfeed shrinks it (Natural: -6 dB in the bass), so the
// space collapses. Here the cross path is full range (a generic average
// head shadow: 0.45 below ~1.5 kHz falling to 0.18), 0.30 ms late at all
// frequencies, and the side is widened by a +2.5 dB shelf below 300 Hz:
// S' = widen(S - shadow(S[n - d])). The side stays within +-2.7 dB
// (1/3 octave) everywhere; the comb of the delayed copy is left in, as on
// loudspeakers. No reflections are added. Hard-panned far ear at the
// nominal amount: -17.6 dB at 100 Hz, -15 dB at 1 kHz, -20 dB at 8 kHz,
// 0.4-0.5 ms late.
//
// The amount follows the L/R correlation of the programme above 300 Hz
// (see SourceDsp::end_correlation_block()): correlated material gets the
// nominal amount, diffuse sound (decorrelated waveforms whose envelopes
// still move together: reverb, spaced microphones) down to 0.5 so its
// space is kept, independent hard-panned sources (decorrelated waveforms
// and envelopes) up to 1.4.
void design_spacious(CrossfeedCoefficients& c, std::uint32_t rate) noexcept {
    c.adaptive = true;
    c.amount_min = 0.5F;
    c.amount_max = 1.4F;
    c.meter_highpass = one_pole_highpass(300.0, rate);
    constexpr double block_seconds = 0.010;
    c.meter_block = frames(block_seconds, rate);
    c.meter_smooth = static_cast<float>(1.0 - std::exp(-block_seconds / 1.5));
    c.amount_smooth = static_cast<float>(1.0 - std::exp(-block_seconds / 1.0));
    // -70 dBFS per channel in the chain's 24-bit integer scale.
    const double floor = 8'388'608.0 * std::pow(10.0, -70.0 / 20.0);
    c.meter_gate = static_cast<float>(c.meter_block * floor * floor);
    constexpr double low = 0.45, high = 0.18, corner_hz = 1'500.0;
    const double k = std::tan(pi * corner_hz / rate);
    c.near = {};
    c.far = {static_cast<float>((low * k + high) / (k + 1.0)), static_cast<float>((low * k - high) / (k + 1.0)),
             static_cast<float>(-(k - 1.0) / (k + 1.0))};
    c.far_delay = frames(0.30e-3, rate);
    c.reflection_count = 0U;
    const settings::EqBand widen{static_cast<std::uint8_t>(settings::FilterType::LowShelf), settings::channel_both,
                                 0U, 300.0F, 2.5F, 0.6F};
    c.post_enabled = true;
    c.post = design_band(widen, rate);
    c.post_svf = design_band_svf(widen, rate);
}

std::complex<double> first_order_response(const FirstOrder& f, std::complex<double> zinv) noexcept {
    return (static_cast<double>(f.b0) + static_cast<double>(f.b1) * zinv) / (1.0 - static_cast<double>(f.d1) * zinv);
}

// Complex near-ear (direct) and far-ear (cross) responses of a crossfeed at
// w radians per sample: L' = D L + C R.
void crossfeed_response(const CrossfeedCoefficients& c, double w, double amount, std::complex<double>& direct,
                        std::complex<double>& cross) noexcept {
    const std::complex<double> zinv = std::polar(1.0, -w);
    if (!c.mid_side) {
        const double g = c.gain;
        direct = g * first_order_response({c.a0_hi, c.a1_hi, c.b1_hi}, zinv);
        cross = g * first_order_response({c.a0_lo, 0.0F, c.b1_lo}, zinv);
        return;
    }
    std::complex<double> side = first_order_response(c.near, zinv) -
                                amount * first_order_response(c.far, zinv) * std::polar(1.0, -w * c.far_delay);
    if (c.reflection_count != 0U) {
        std::complex<double> sum = 0.0;
        for (std::size_t i = 0U; i < c.reflection_count; ++i)
            sum += static_cast<double>(c.reflections[i].gain) * std::polar(1.0, -w * c.reflections[i].delay);
        side += sum * first_order_response(c.reflection_highpass, zinv) *
                first_order_response(c.reflection_lowpass, zinv);
    }
    if (c.post_enabled) {
        const auto& b = c.post;
        side *= (static_cast<double>(b.b0) + static_cast<double>(b.b1) * zinv + static_cast<double>(b.b2) * zinv * zinv) /
                (1.0 + static_cast<double>(b.a1) * zinv + static_cast<double>(b.a2) * zinv * zinv);
    }
    direct = 0.5 * (1.0 + side);
    cross = 0.5 * (1.0 - side);
}

CrossfeedCoefficients design_crossfeed(std::uint8_t preset, std::uint32_t rate) noexcept {
    CrossfeedCoefficients c{};
    if (preset == 0U || preset >= settings::crossfeed_count || rate == 0U) return c;
    c.preset = preset;
    using settings::Crossfeed;
    switch (static_cast<Crossfeed>(preset)) {
    case Crossfeed::Natural: c.enabled = c.mid_side = true; design_natural(c, rate); return c;
    case Crossfeed::Speakers30: c.enabled = c.mid_side = true; design_speakers(c, 30.0, rate); return c;
    case Crossfeed::Speakers45: c.enabled = c.mid_side = true; design_speakers(c, 45.0, rate); return c;
    case Crossfeed::Spacious: c.enabled = c.mid_side = true; design_spacious(c, rate); return c;
    default: break;
    }
    // Bauer stereophonic-to-binaural (bs2b) first-order crossfeed.
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

// Detail stage (see source_dsp.hpp). The chain runs in 24-bit integer scale.
constexpr double full_scale = 8'388'608.0;
constexpr double proximity_split_hz = 1200.0;
constexpr double proximity_envelope_s = 0.020;
constexpr double proximity_max_cut_db = 18.0;
constexpr double proximity_floor_dbfs = -65.0;
constexpr double transient_fast_s = 0.001;
constexpr double transient_slow_attack_s = 0.015;
constexpr double transient_slow_release_s = 0.020;
constexpr double transient_floor_dbfs = -70.0;

float smoothing(double seconds, std::uint32_t rate) noexcept {
    return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * rate)));
}

ProximityDesign design_proximity(const settings::Settings& s, std::uint32_t rate) noexcept {
    ProximityDesign p{};
    p.enabled = (s.detail_flags & settings::detail_proximity) != 0U;
    // Bilinear (not impulse-invariant) one-pole, so LP and 1 - LP are in
    // quadrature and |gl LP + gh (1 - LP)| <= max(gl, gh) at every frequency.
    const double k = std::tan(pi * proximity_split_hz / rate);
    p.split = {static_cast<float>(k / (1.0 + k)), static_cast<float>(k / (1.0 + k)),
               static_cast<float>((1.0 - k) / (1.0 + k))};
    p.envelope = smoothing(proximity_envelope_s, rate);
    // Cut the quieter side by amount x the level difference (dB): its gain
    // is (p_quiet / p_loud)^(amount / 2); 100 % doubles the difference.
    p.exponent = p.enabled ? 0.5F * static_cast<float>(s.proximity_amount_pct) / 100.0F : 0.0F;
    p.min_gain = static_cast<float>(std::pow(10.0, -proximity_max_cut_db / 20.0));
    const double floor = full_scale * std::pow(10.0, proximity_floor_dbfs / 20.0);
    p.floor_power = static_cast<float>(floor * floor);
    return p;
}

TransientDesign design_transient(const settings::Settings& s, std::uint32_t rate) noexcept {
    TransientDesign t{};
    t.enabled = (s.detail_flags & settings::detail_transient) != 0U;
    if (!t.enabled) return t;
    const settings::EqBand hp{static_cast<std::uint8_t>(settings::FilterType::HighPass), settings::channel_both,
                              0U, 100.0F * static_cast<float>(s.transient_crossover_100hz), 0.0F, 0.7071F};
    t.highpass = design_band(hp, rate);
    t.highpass_svf = design_band_svf(hp, rate);
    t.fast = smoothing(transient_fast_s, rate);
    t.slow_attack = smoothing(transient_slow_attack_s, rate);
    t.slow_release = smoothing(transient_slow_release_s, rate);
    const double db = settings::maximum_transient_boost_db * s.transient_amount_pct / 100.0;
    t.boost = static_cast<float>(std::pow(10.0, db / 20.0) - 1.0);
    const double floor = full_scale * std::pow(10.0, transient_floor_dbfs / 20.0);
    t.floor = static_cast<float>(floor * floor);
    return t;
}

// Largest gain of the transient shaper at w: x + b G H_hp(x) for b in
// [0, 1]; |1 + b G H| is convex in b, so the ends bound it.
float transient_bound(const TransientDesign& t, float w) noexcept {
    if (!t.enabled) return 1.0F;
    const std::complex<double> zi = std::polar(1.0, -static_cast<double>(w));
    const auto& c = t.highpass;
    const std::complex<double> h = (static_cast<double>(c.b0) + static_cast<double>(c.b1) * zi +
                                    static_cast<double>(c.b2) * zi * zi) /
                                   (1.0 + static_cast<double>(c.a1) * zi + static_cast<double>(c.a2) * zi * zi);
    const auto m = static_cast<float>(std::abs(1.0 + static_cast<double>(t.boost) * h));
    return m > 1.0F ? m : 1.0F;
}

void refresh_identity(SourceDspDesign& d) noexcept {
    d.identity = d.left_mask == 0U && d.right_mask == 0U && !d.crossfeed.enabled &&
                 d.trim_left == 1.0F && d.trim_right == 1.0F && d.makeup == 1.0F && !d.leveler.enabled &&
                 !d.proximity.enabled && !d.transient.enabled;
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
    const float eq_left = chain_magnitude(d, true, w);
    const float eq_right = chain_magnitude(d, false, w);
    float out_left = eq_left;
    float out_right = eq_right;
    if (d.crossfeed.enabled) {
        // Upper bound: both paths in phase with fully correlated channels.
        // An adaptive amount moves the side response along a line, and
        // |D| + |C| is convex along it: the ends of the range bound it.
        const auto& c = d.crossfeed;
        out_left = out_right = 0.0F;
        for (const float amount : {c.amount_min, c.amount_max}) {
            std::complex<double> direct, cross;
            crossfeed_response(c, static_cast<double>(w), amount, direct, cross);
            const auto hi = static_cast<float>(std::abs(direct));
            const auto lo = static_cast<float>(std::abs(cross));
            out_left = std::fmax(out_left, hi * eq_left + lo * eq_right);
            out_right = std::fmax(out_right, hi * eq_right + lo * eq_left);
        }
    }
    // The detail stage runs per channel in front of the crossfeed; the
    // proximity part never amplifies, and the transient bound is the same
    // for both channels, so it factors out.
    const float detail = transient_bound(d.transient, w);
    out_left *= std::fabs(d.trim_left) * detail;
    out_right *= std::fabs(d.trim_right) * detail;
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
    d.proximity = design_proximity(s, sample_rate_hz);
    d.transient = design_transient(s, sample_rate_hz);
    // The boost analysis covers the user chain only (EQ, crossfeed, trims);
    // the leveler only cuts and the loudness slots are bounded by the host
    // attenuation (see apply_loudness_compensation()).
    if (d.left_mask == 0U && d.right_mask == 0U && !d.crossfeed.enabled && !d.transient.enabled &&
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
    if (d.transient.enabled) {
        // The peak of |1 + G H_hp| sits near the crossover.
        const float m = worst_case_magnitude(d, 100.0F * static_cast<float>(s.transient_crossover_100hz));
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
    xf_pending_ = false;
    xf_wet_ = xf_wet_from_ = xf_wet_to_ = xf_.enabled ? 1.0F : 0.0F;
    clear_crossfeed_state();
    px_ = design_.proximity;
    px_active_ = px_.enabled;
    tr_ = design_.transient;
    tr_active_ = tr_.enabled;
    tr_wet_ = tr_wet_from_ = tr_wet_to_ = tr_.enabled ? 1.0F : 0.0F;
    clear_detail_state();
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

    // Crossfeed glides by its wet amount. New bs2b coefficients take over at
    // once when it stays on (its one-pole states carry over smoothly); when
    // it turns off, the old ones keep running until the fade-out ends. A
    // change to or from a mid/side preset (other delays, other structure)
    // fades the old preset out, then advance_ramp() fades the new one in.
    const auto& xf = design.crossfeed;
    xf_pending_ = false;
    if (xf.enabled) {
        if (!xf_active_) {
            xf_ = xf;
            xf_active_ = true;
            xf_wet_ = 0.0F;
            clear_crossfeed_state();
        } else if ((xf_.mid_side || xf.mid_side) && xf_.preset != xf.preset) {
            xf_pending_ = true;
        } else {
            xf_ = xf;
        }
    }
    xf_wet_from_ = xf_wet_;
    xf_wet_to_ = xf.enabled && !xf_pending_ ? 1.0F : 0.0F;

    // Proximity: a new exponent takes over at once and the gains glide
    // there block by block; switched off, they glide back to unity and
    // end_proximity_block() retires it.
    if (design.proximity.enabled) {
        if (!px_active_) {
            px_active_ = true;
            px_ = design.proximity;
            clear_detail_state_proximity();
        } else {
            px_ = design.proximity;
        }
    } else if (px_active_) {
        px_.enabled = false;
        px_.exponent = 0.0F;
    }
    // Transient shaper: fades by its wet amount; a new crossover takes
    // over at once (the SVF stays well-behaved under modulation).
    const auto& tr = design.transient;
    if (tr.enabled) {
        if (!tr_active_) {
            tr_active_ = true;
            tr_ = tr;
            tr_wet_ = 0.0F;
            clear_detail_state_transient();
        } else {
            tr_ = tr;
            for (auto& b : tr_highpass_) {
                b.p = b.from = b.to = tr_.highpass_svf;
                update_coefficients(b);
            }
        }
    }
    tr_wet_from_ = tr_wet_;
    tr_wet_to_ = tr.enabled ? 1.0F : 0.0F;
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
    tr_wet_ = lerp(tr_wet_from_, tr_wet_to_);
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
    tr_wet_ = tr_wet_from_ = tr_wet_to_;
    if (!design_.transient.enabled && tr_active_) {
        tr_active_ = false;
        tr_ = {};
        clear_detail_state_transient();
    }
    if (!design_.crossfeed.enabled) {
        xf_active_ = false;
        xf_ = {};
        clear_crossfeed_state();
    } else if (xf_pending_) {
        // The old preset has faded out: start the new one from rest and
        // fade it in. Everything else has landed and holds still.
        xf_pending_ = false;
        xf_ = design_.crossfeed;
        clear_crossfeed_state();
        for (std::size_t i = 0U; i < dsp_band_count; ++i) {
            left_[i].from = left_[i].to;
            right_[i].from = right_[i].to;
        }
        trim_left_from_ = trim_left_to_;
        trim_right_from_ = trim_right_to_;
        xf_wet_ = xf_wet_from_ = 0.0F;
        xf_wet_to_ = 1.0F;
        ramp_steps_left_ = ramp_steps_total_;
        return;
    }
    xf_wet_ = xf_wet_to_;
    update_idle();
}

void SourceDsp::clear_crossfeed_state() noexcept {
    lo_left_ = lo_right_ = hi_left_ = hi_right_ = in_left_ = in_right_ = 0.0F;
    line_.fill(0.0F);
    line_pos_ = 0U;
    near_x1_ = near_y1_ = far_x1_ = far_y1_ = 0.0F;
    refl_hp_x1_ = refl_hp_y1_ = refl_lp_x1_ = refl_lp_y1_ = 0.0F;
    xf_post_ = {};
    if (xf_.post_enabled) {
        xf_post_.p = xf_post_.from = xf_post_.to = xf_.post_svf;
        update_coefficients(xf_post_);
    }
    xf_amount_ = 1.0F;
    xf_amount_step_ = 0.0F;
    cm_x1_[0] = cm_x1_[1] = cm_y1_[0] = cm_y1_[1] = 0.0F;
    cm_ll_ = cm_rr_ = cm_lr_ = 0.0F;
    cm_fill_ = 0U;
    for (auto& v : cm_wave_) v = 0.0F;
    for (auto& v : cm_mean_) v = 0.0F;
    for (auto& v : cm_env_) v = 0.0F;
    cm_primed_ = false;
    waveform_correlation_ = 1.0F;
    envelope_correlation_ = 0.5F;
}

// Spacious crossfeed detector, every 10 ms. Waveform correlation (from
// the block cross products) separates correlated from decorrelated
// material; for the decorrelated part, the correlation of the two 10 ms
// envelopes separates diffuse sound (reverb decays in both channels at
// once) from independent sources panned hard left and right.
void SourceDsp::end_correlation_block() noexcept {
    const float ll = cm_ll_, rr = cm_rr_, lr = cm_lr_;
    cm_ll_ = cm_rr_ = cm_lr_ = 0.0F;
    cm_fill_ = 0U;
    if (ll + rr < xf_.meter_gate) {
        xf_amount_step_ = 0.0F; // silence and gaps: hold
        return;
    }
    const float a = xf_.meter_smooth;
    const float env_l = std::sqrt(ll);
    const float env_r = std::sqrt(rr);
    if (!cm_primed_) {
        cm_wave_[0] = ll; cm_wave_[1] = rr; cm_wave_[2] = lr;
        cm_mean_[0] = env_l; cm_mean_[1] = env_r;
        cm_primed_ = true;
    } else {
        cm_wave_[0] += (ll - cm_wave_[0]) * a;
        cm_wave_[1] += (rr - cm_wave_[1]) * a;
        cm_wave_[2] += (lr - cm_wave_[2]) * a;
        const float dl = env_l - cm_mean_[0];
        const float dr = env_r - cm_mean_[1];
        cm_mean_[0] += dl * a;
        cm_mean_[1] += dr * a;
        cm_env_[0] += (dl * dl - cm_env_[0]) * a;
        cm_env_[1] += (dr * dr - cm_env_[1]) * a;
        cm_env_[2] += (dl * dr - cm_env_[2]) * a;
    }
    const float wave_norm = std::sqrt(cm_wave_[0] * cm_wave_[1]);
    const float rho_wave = wave_norm > 0.0F ? cm_wave_[2] / wave_norm : 1.0F;
    // Steady envelopes carry no evidence either way: the regularisation
    // pulls them to the neutral 0.5.
    const float eps = 0.01F * cm_mean_[0] * cm_mean_[1];
    float rho_env = (cm_env_[2] + 0.5F * eps) / (std::sqrt(cm_env_[0] * cm_env_[1]) + eps);
    rho_env = rho_env < 0.0F ? 0.0F : (rho_env > 1.0F ? 1.0F : rho_env);
    waveform_correlation_ = rho_wave;
    envelope_correlation_ = rho_env;
    const float decorrelated = 1.0F - (rho_wave > 0.0F ? rho_wave : 0.0F);
    float target = 1.0F + decorrelated * (0.4F * (1.0F - rho_env) - 0.5F * rho_env);
    target = target < xf_.amount_min ? xf_.amount_min : (target > xf_.amount_max ? xf_.amount_max : target);
    // Glide there over the next block, one small step per frame.
    const float next = xf_amount_ + (target - xf_amount_) * xf_.amount_smooth;
    xf_amount_step_ = (next - xf_amount_) / static_cast<float>(xf_.meter_block);
}

void SourceDsp::clear_detail_state_proximity() noexcept {
    px_x1_[0] = px_x1_[1] = px_y1_[0] = px_y1_[1] = 0.0F;
    for (std::size_t i = 0U; i < 4U; ++i) {
        px_power_[i] = 0.0F;
        px_gain_[i] = 1.0F;
        px_step_[i] = 0.0F;
    }
    px_fill_ = 0U;
}

void SourceDsp::clear_detail_state_transient() noexcept {
    for (std::size_t ch = 0U; ch < 2U; ++ch) {
        tr_highpass_[ch] = {};
        if (tr_.enabled) {
            tr_highpass_[ch].p = tr_highpass_[ch].from = tr_highpass_[ch].to = tr_.highpass_svf;
            update_coefficients(tr_highpass_[ch]);
        }
        tr_fast_[ch] = tr_slow_[ch] = tr_lead_[ch] = 0.0F;
    }
}

void SourceDsp::clear_detail_state() noexcept {
    clear_detail_state_proximity();
    clear_detail_state_transient();
}

// Proximity, every 16 frames: the quieter side of each band is cut by
// exponent x its level difference; the gains glide there over the block.
void SourceDsp::end_proximity_block() noexcept {
    px_fill_ = 0U;
    const auto& c = px_;
    constexpr float inv = 1.0F / static_cast<float>(ramp_block_frames);
    constexpr float tolerance = 1.0e-6F;
    bool unity = true;
    for (std::size_t band = 0U; band < 4U; band += 2U) {
        const float pl = px_power_[band] + c.floor_power;
        const float pr = px_power_[band + 1U] + c.floor_power;
        float gl = 1.0F;
        float gr = 1.0F;
        if (c.exponent > 0.0F) {
            if (pl > pr) gr = std::pow(pr / pl, c.exponent);
            else gl = std::pow(pl / pr, c.exponent);
            gl = gl < c.min_gain ? c.min_gain : gl;
            gr = gr < c.min_gain ? c.min_gain : gr;
        }
        px_step_[band] = (gl - px_gain_[band]) * inv;
        px_step_[band + 1U] = (gr - px_gain_[band + 1U]) * inv;
        for (std::size_t i = band; i < band + 2U; ++i)
            if (px_gain_[i] < 1.0F - tolerance) unity = false;
    }
    if (!c.enabled && unity) {
        // Switched off and back at unity: retire.
        px_active_ = false;
        px_ = {};
        clear_detail_state_proximity();
        update_idle();
    }
}

float SourceDsp::transient_boost_db(std::size_t channel) const noexcept {
    if (!tr_active_ || channel > 1U) return 0.0F;
    // Gain of the high band at the current lead.
    return 20.0F * std::log10(1.0F + tr_wet_ * tr_.boost * tr_lead_[channel]);
}

void SourceDsp::update_idle() noexcept {
    idle_ = design_.identity && !lev_.enabled && lev_gain_ == 1.0F && lev_target_ == 1.0F &&
            ramp_steps_left_ == 0U && !px_active_ && !tr_active_;
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
