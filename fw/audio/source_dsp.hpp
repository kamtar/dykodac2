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

struct CrossfeedCoefficients {
    bool enabled{false};
    float a0_lo{0.0F};
    float b1_lo{0.0F};
    float a0_hi{1.0F};
    float a1_hi{0.0F};
    float b1_hi{0.0F};
    float gain{1.0F};
};

struct SourceDspDesign {
    std::uint32_t sample_rate_hz{0U};
    // Slot i of each channel is band i; inactive slots are skipped.
    std::array<Biquad, settings::eq_band_count> left{};
    std::array<Biquad, settings::eq_band_count> right{};
    std::array<SvfParams, settings::eq_band_count> left_svf{};
    std::array<SvfParams, settings::eq_band_count> right_svf{};
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
SourceDspDesign design_source_dsp(const settings::Settings& s, std::uint32_t sample_rate_hz) noexcept;

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

    inline void process(float& left, float& right) noexcept {
        if (idle_) return;
        if (ramp_steps_left_ != 0U && ++ramp_phase_ == ramp_block_frames) advance_ramp();
        float l = left;
        float r = right;
        for (std::size_t i = 0U; i < settings::eq_band_count; ++i) {
            const auto bit = static_cast<std::uint16_t>(1U << i);
            if ((live_left_mask_ & bit) != 0U) l = run(left_[i], l);
            if ((live_right_mask_ & bit) != 0U) r = run(right_[i], r);
        }
        if (xf_active_) {
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
        left = l * trim_left_;
        right = r * trim_right_;
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
    static void update_coefficients(Band& b) noexcept;
    void advance_ramp() noexcept;
    void snap() noexcept;

    SourceDspDesign design_{};
    std::array<Band, settings::eq_band_count> left_{};
    std::array<Band, settings::eq_band_count> right_{};
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
};

} // namespace audio
