#pragma once

// Source-rate float processing in front of the interpolator (or in front of
// the single quantisation in upsampler bypass): parametric EQ, crossfeed,
// balance, polarity and the automatic EQ boost compensation. Everything
// stays in float; the only rounding to 24 bit remains the dithered quantiser
// in PolyphaseUpsampler.
//
// Design (coefficients, worst-case boost) is control-rate code in main
// context. process() is called per source frame from the same context.
//
// EQ bands run as trapezoidal (Simper/Cytomic) state-variable filters. They
// have exactly the response of the RBJ cookbook biquads but, unlike a direct
// form biquad, keep low, high-Q bands quiet in single precision: a 20 Hz
// shelf in float32 TDF-II leaves an error near -80 dBFS on loud music, the
// SVF stays near -140 dBFS (the RT1011 FPU is single precision only). The
// Biquad form is kept for analysis (worst-case boost, configurator plots).
//
// Crossfeed follows the EQ: either bs2b, or a mid/side form (Natural,
// virtual speakers) that passes the centre untouched and shapes only the
// side signal through a short delay line (head-shadow filters, interaural
// delay, early reflections).
//
// Two more SVF slots behind the EQ carry the loudness compensation (ISO 226
// low and high shelf, set from the host volume). They are not part of the
// boost compensation: their boost never exceeds the host attenuation below
// the reference volume, so the output never exceeds the reference level.
//
// The loudness leveler meters the input (before EQ) with the BS.1770
// K-weighting, averages 100 ms block energies with asymmetric time
// constants and turns the chain down (never up) towards a target loudness.
// Its gain is a per-sample smoothed multiplier at the end of the chain.
//
// Live changes are click-free: set_design() glides every parameter (band
// tuning and mix, trims, crossfeed amount) linearly to the new design over
// ramp_seconds. Bands that appear or disappear fade from or to unity.

#include "config/user_settings.hpp"

#include <array>
#include <cstdint>

namespace audio {

struct Biquad {
    // y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 (a0 normalised to 1).
    float b0{1.0F};
    float b1{0.0F};
    float b2{0.0F};
    float a1{0.0F};
    float a2{0.0F};
};

// Trapezoidal SVF: g = tan(pi f / fs), k = damping, output
// m0 * input + m1 * band-pass + m2 * low-pass. m = (1, 0, 0) is exact unity
// for any g and k, which is how bands fade in and out.
struct SvfParams {
    float g{0.0F};
    float k{1.0F};
    float m0{1.0F};
    float m1{0.0F};
    float m2{0.0F};
};

// y = b0 x + b1 x[n-1] + d1 y[n-1].
struct FirstOrder {
    float b0{1.0F};
    float b1{0.0F};
    float d1{0.0F};
};

// Side-signal ring of the mid/side crossfeeds: power of two, longer than
// the latest reflection (8.85 ms = 425 frames at 48 kHz).
inline constexpr std::size_t crossfeed_line_frames = 512U;
inline constexpr std::size_t crossfeed_max_reflections = 12U;

struct CrossfeedReflection {
    std::uint16_t delay{0U}; // frames
    float gain{0.0F};        // signed: + near-ear, - far-ear arrival
};

struct CrossfeedCoefficients {
    bool enabled{false};
    // false: bs2b (fields *_lo/*_hi/gain); true: mid/side shuffler.
    bool mid_side{false};
    std::uint8_t preset{0U};
    float a0_lo{0.0F};
    float b1_lo{0.0F};
    float a0_hi{1.0F};
    float a1_hi{0.0F};
    float b1_hi{0.0F};
    float gain{1.0F};
    // Mid/side: L = M + S', R = M - S' with M = (L + R) / 2, S = (L - R) / 2
    // and S' = post(near(S) - far(S[n - far_delay]) + band(sum of
    // reflections of S)). The centre is untouched by construction.
    FirstOrder near{};
    FirstOrder far{};
    std::uint16_t far_delay{0U};
    std::uint8_t reflection_count{0U};
    std::array<CrossfeedReflection, crossfeed_max_reflections> reflections{};
    FirstOrder reflection_highpass{};
    FirstOrder reflection_lowpass{};
    bool post_enabled{false};
    Biquad post{};         // analysis form
    SvfParams post_svf{};  // runtime form
};

// Slots behind the user EQ bands for loudness compensation.
inline constexpr std::size_t loudness_bass_slot = settings::eq_band_count;
inline constexpr std::size_t loudness_treble_slot = settings::eq_band_count + 1U;
inline constexpr std::size_t dsp_band_count = settings::eq_band_count + 2U;

// ISO 226:2003 fit (80 phon reference, 10-40 dB attenuation, within about
// 1 dB from 20 Hz to 12.5 kHz): shelf gain per dB of attenuation.
inline constexpr float loudness_bass_hz = 111.0F;
inline constexpr float loudness_bass_q = 0.38F;
inline constexpr float loudness_bass_per_db = 0.56F;
inline constexpr float loudness_treble_hz = 10'200.0F;
inline constexpr float loudness_treble_q = 1.22F;
inline constexpr float loudness_treble_per_db = 0.17F;

struct LevelerDesign {
    bool enabled{false};
    Biquad k_shelf{};   // BS.1770 stage 1 (high shelf)
    Biquad k_highpass{}; // BS.1770 stage 2 (RLB high-pass)
    std::uint32_t block_frames{4'800U};
    float attack{0.0F};  // per-block energy smoothing while louder
    float release{0.0F}; // per-block energy smoothing while quieter
    float smooth{0.0F};  // per-frame gain smoothing
    float power_scale{0.0F};   // block energy -> mean square re full scale
    float target_power{0.0F};  // mean square of the target loudness
    float gate_power{0.0F};    // blocks below this are silence: hold
    float min_gain{1.0F};      // 10^(-max_cut / 20)
};

struct SourceDspDesign {
    std::uint32_t sample_rate_hz{0U};
    // Slot i of each channel is band i; inactive slots are skipped.
    std::array<Biquad, settings::eq_band_count> left{};
    std::array<Biquad, settings::eq_band_count> right{};
    std::array<SvfParams, dsp_band_count> left_svf{};
    std::array<SvfParams, dsp_band_count> right_svf{};
    std::uint16_t left_mask{0U};
    std::uint16_t right_mask{0U};
    CrossfeedCoefficients crossfeed{};
    // Balance and polarity only (what the user asked for).
    float trim_left{1.0F};
    float trim_right{1.0F};
    // Worst-case magnitude of the user chain over frequency, as a positive
    // boost in dB (0 when the chain never amplifies).
    float boost_db{0.0F};
    // 10^(-boost_db / 20): applied inside the chain, so the compensation is
    // time-aligned with the EQ it compensates (the output gain sits behind
    // the interpolator latency) and glides together with it.
    float makeup{1.0F};
    // Loudness compensation shelf gains in effect (dB >= 0).
    float loudness_bass_db{0.0F};
    float loudness_treble_db{0.0F};
    LevelerDesign leveler{};
    bool identity{true};
};

// RBJ "Audio EQ Cookbook" biquad for one band at the given rate. Shelves use
// the Q form, as Equalizer APO / AutoEQ ParametricEQ.txt files do.
Biquad design_band(const settings::EqBand& band, std::uint32_t sample_rate_hz) noexcept;
// The same band as SVF parameters (identical response to design_band()).
SvfParams design_band_svf(const settings::EqBand& band, std::uint32_t sample_rate_hz) noexcept;
// Off, or a 0 dB peak/shelf: contributes nothing and is never run.
bool band_is_neutral(const settings::EqBand& band) noexcept;
// |H(e^jw)| of one biquad at angular frequency w (radians per sample),
// evaluated in the cancellation-free phi = sin^2(w/2) form.
float biquad_magnitude(const Biquad& biquad, float w) noexcept;
// Everything except the loudness compensation (which only depends on the
// host volume and is added by apply_loudness_compensation()).
SourceDspDesign design_source_dsp(const settings::Settings& s, std::uint32_t sample_rate_hz) noexcept;
// Sets the loudness slots for the host volume (1/256 dB). Cheap: no boost
// analysis, so it can follow every volume step.
void apply_loudness_compensation(SourceDspDesign& d, const settings::Settings& s,
                                 std::int16_t host_volume_db_8_8) noexcept;
// Shelf gains (dB) of the loudness compensation at a host volume.
void loudness_shelf_gains(const settings::Settings& s, std::int16_t host_volume_db_8_8,
                          float& bass_db, float& treble_db) noexcept;

class SourceDsp {
public:
    // Length of the glide between two designs, and how often (in source
    // frames) the gliding parameters are updated.
    static constexpr float ramp_seconds = 0.020F;
    static constexpr std::uint32_t ramp_block_frames = 16U;

    // Glides from the running parameters to the new design. Snaps instead
    // (no glide) after reset_state() or when the sample rate changes.
    void set_design(const SourceDspDesign& design) noexcept;
    const SourceDspDesign& design() const noexcept { return design_; }
    void reset_state() noexcept;
    // True while nothing (not even a glide) changes a sample.
    bool identity() const noexcept { return idle_; }
    bool ramping() const noexcept { return ramp_steps_left_ != 0U; }
    // Leveler gain currently applied (dB <= 0) and its target.
    float leveler_gain_db() const noexcept;
    float leveler_target_gain() const noexcept { return lev_target_; }

    inline void process(float& left, float& right) noexcept {
        if (idle_) return;
        if (ramp_steps_left_ != 0U && ++ramp_phase_ == ramp_block_frames) advance_ramp();
        float l = left;
        float r = right;
        if (lev_.enabled) meter(l, r);
        for (std::size_t i = 0U; i < dsp_band_count; ++i) {
            const auto bit = static_cast<std::uint16_t>(1U << i);
            if ((live_left_mask_ & bit) != 0U) l = run(left_[i], l);
            if ((live_right_mask_ & bit) != 0U) r = run(right_[i], r);
        }
        if (xf_active_ && xf_.mid_side) {
            const float m = 0.5F * (l + r);
            const float s = shuffle(0.5F * (l - r));
            l += (m + s - l) * xf_wet_;
            r += (m - s - r) * xf_wet_;
        } else if (xf_active_) {
            const auto& c = xf_;
            lo_left_ = c.a0_lo * l + c.b1_lo * lo_left_;
            lo_right_ = c.a0_lo * r + c.b1_lo * lo_right_;
            hi_left_ = c.a0_hi * l + c.a1_hi * in_left_ + c.b1_hi * hi_left_;
            hi_right_ = c.a0_hi * r + c.a1_hi * in_right_ + c.b1_hi * hi_right_;
            in_left_ = l;
            in_right_ = r;
            const float xl = (hi_left_ + lo_right_) * c.gain;
            const float xr = (hi_right_ + lo_left_) * c.gain;
            l += (xl - l) * xf_wet_;
            r += (xr - r) * xf_wet_;
        }
        if (lev_gain_ != lev_target_) {
            const float delta = lev_target_ - lev_gain_;
            // Near the target the exponential step drops below one ulp of
            // the gain and would stall; a minimum step finishes linearly.
            constexpr float min_step = 1.0e-7F;
            if (delta <= min_step && delta >= -min_step) {
                lev_gain_ = lev_target_;
                if (!lev_.enabled) update_idle(); // switched off: back to unity
            } else {
                float step = delta * lev_.smooth;
                if (step < min_step && step > -min_step) step = delta > 0.0F ? min_step : -min_step;
                lev_gain_ += step;
            }
        }
        left = l * (trim_left_ * lev_gain_);
        right = r * (trim_right_ * lev_gain_);
    }

private:
    struct Band {
        SvfParams p{};    // running parameters
        SvfParams from{};
        SvfParams to{};
        // d1 = 1 - a1 with a1 = 1 / (1 + g (g + k)): a1 sits next to 1 for
        // low bands and would lose its low bits in float, d1 keeps them.
        float d1{0.0F};
        float a2{0.0F};
        float a3{0.0F};
        float ic1{0.0F};
        float ic2{0.0F};
    };
    static inline float run(Band& b, float v0) noexcept {
        const float v3 = v0 - b.ic2;
        const float v1 = b.ic1 - b.d1 * b.ic1 + b.a2 * v3;
        const float v2 = b.ic2 + b.a2 * b.ic1 + b.a3 * v3;
        b.ic1 = 2.0F * v1 - b.ic1;
        b.ic2 = 2.0F * v2 - b.ic2;
        return b.p.m0 * v0 + b.p.m1 * v1 + b.p.m2 * v2;
    }
    static inline float biquad(const Biquad& c, float x, float& s1, float& s2) noexcept {
        // Transposed direct form II (metering only: its float noise is
        // far below the 1 dB this measurement needs).
        const float y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        return y;
    }
    static inline float first_order(const FirstOrder& c, float x, float& x1, float& y1) noexcept {
        const float y = c.b0 * x + c.b1 * x1 + c.d1 * y1;
        x1 = x;
        y1 = y;
        return y;
    }
    // Side signal of the mid/side crossfeeds (see CrossfeedCoefficients).
    inline float shuffle(float s) noexcept {
        constexpr std::size_t mask = crossfeed_line_frames - 1U;
        const auto& c = xf_;
        line_[line_pos_] = s;
        float out = first_order(c.near, s, near_x1_, near_y1_) -
                    first_order(c.far, line_[(line_pos_ - c.far_delay) & mask], far_x1_, far_y1_);
        if (c.reflection_count != 0U) {
            float sum = 0.0F;
            for (std::size_t i = 0U; i < c.reflection_count; ++i)
                sum += c.reflections[i].gain * line_[(line_pos_ - c.reflections[i].delay) & mask];
            sum = first_order(c.reflection_highpass, sum, refl_hp_x1_, refl_hp_y1_);
            out += first_order(c.reflection_lowpass, sum, refl_lp_x1_, refl_lp_y1_);
        }
        if (c.post_enabled) out = run(xf_post_, out);
        line_pos_ = (line_pos_ + 1U) & mask;
        return out;
    }
    void clear_crossfeed_state() noexcept;
    inline void meter(float l, float r) noexcept {
        const float kl = biquad(lev_.k_highpass, biquad(lev_.k_shelf, l, k_state_[0], k_state_[1]),
                                k_state_[2], k_state_[3]);
        const float kr = biquad(lev_.k_highpass, biquad(lev_.k_shelf, r, k_state_[4], k_state_[5]),
                                k_state_[6], k_state_[7]);
        block_energy_ += kl * kl + kr * kr;
        if (++block_fill_ == lev_.block_frames) end_meter_block();
    }
    void end_meter_block() noexcept;
    void adopt_leveler(const LevelerDesign& design, bool restart_meter) noexcept;
    void update_idle() noexcept;
    static void update_coefficients(Band& b) noexcept;
    void advance_ramp() noexcept;
    void snap() noexcept;

    SourceDspDesign design_{};
    std::array<Band, dsp_band_count> left_{};
    std::array<Band, dsp_band_count> right_{};
    // Bands that run now: during a glide the union of old and new.
    std::uint16_t live_left_mask_{0U};
    std::uint16_t live_right_mask_{0U};
    float trim_left_{1.0F};
    float trim_right_{1.0F};
    float trim_left_from_{1.0F};
    float trim_right_from_{1.0F};
    float trim_left_to_{1.0F};
    float trim_right_to_{1.0F};
    CrossfeedCoefficients xf_{};
    bool xf_active_{false};
    float xf_wet_{0.0F};
    float xf_wet_from_{0.0F};
    float xf_wet_to_{0.0F};
    std::uint32_t ramp_steps_total_{0U};
    std::uint32_t ramp_steps_left_{0U};
    std::uint32_t ramp_phase_{0U};
    bool idle_{true};
    bool fresh_{true};
    float lo_left_{0.0F};
    float lo_right_{0.0F};
    float hi_left_{0.0F};
    float hi_right_{0.0F};
    float in_left_{0.0F};
    float in_right_{0.0F};
    // Mid/side crossfeed state. A change of mid/side preset while on fades
    // the old one out first, then the new one in from cleared state.
    bool xf_pending_{false};
    std::array<float, crossfeed_line_frames> line_{};
    std::size_t line_pos_{0U};
    float near_x1_{0.0F};
    float near_y1_{0.0F};
    float far_x1_{0.0F};
    float far_y1_{0.0F};
    float refl_hp_x1_{0.0F};
    float refl_hp_y1_{0.0F};
    float refl_lp_x1_{0.0F};
    float refl_lp_y1_{0.0F};
    Band xf_post_{};
    // Leveler. The loudness estimate and gain survive stream restarts and
    // redesigns; only the meter filters restart.
    LevelerDesign lev_{};
    float k_state_[8]{};
    float block_energy_{0.0F};
    std::uint32_t block_fill_{0U};
    float lev_power_{0.0F}; // smoothed mean square (sum of both channels)
    float lev_gain_{1.0F};
    float lev_target_{1.0F};
};

} // namespace audio
