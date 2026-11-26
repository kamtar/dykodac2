// Host tests for user settings, the source-rate DSP chain, the bypass
// formats/DAC modes and the HID configuration service.

#include "app/config_service.hpp"
#include "audio/polyphase_upsampler.hpp"
#include "audio/sample_rate.hpp"
#include "audio/source_dsp.hpp"
#include "audio/volume.hpp"
#include "config/user_settings.hpp"
#include "devices/dac.hpp"
#include "usb/config_protocol.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

constexpr double pi = 3.14159265358979323846;

float db_at(const audio::Biquad& b, double f, double fs) {
    return 20.0F * std::log10(audio::biquad_magnitude(b, static_cast<float>(2.0 * pi * f / fs)));
}

// Measured steady-state gain of a sine through SourceDsp (left channel).
double measured_db(audio::SourceDsp& dsp, double f, double fs) {
    dsp.reset_state();
    // True amplitude by quadrature projection over the second half: the
    // sample peak undershoots when f/fs has few distinct phases.
    double si = 0.0, co = 0.0;
    const int n = static_cast<int>(fs); // one second
    for (int i = 0; i < n; ++i) {
        const double phase = 2.0 * pi * f * i / fs;
        float l = static_cast<float>(1.0e6 * std::sin(phase));
        float r = l;
        dsp.process(l, r);
        if (i >= n / 2) { si += l * std::sin(phase); co += l * std::cos(phase); }
    }
    const double amplitude = 2.0 * std::sqrt(si * si + co * co) / (n - n / 2);
    return 20.0 * std::log10(amplitude / 1.0e6);
}

struct MockBackend final : app::ConfigBackend {
    settings::Settings applied{};
    int apply_count{0};
    bool flash_ok{true};
    bool has_record{false};
    settings::Settings stored{};
    std::uint32_t stored_sequence{0U};
    void apply(const settings::Settings& s) noexcept override { applied = s; ++apply_count; }
    bool flash_available() const noexcept override { return flash_ok; }
    app::FlashResult flash_load(settings::Settings& out, std::uint32_t& sequence) noexcept override {
        if (!has_record) return app::FlashResult::NoRecord;
        out = stored; sequence = stored_sequence; return app::FlashResult::Ok;
    }
    app::FlashResult flash_save(const settings::Settings& s, std::uint32_t& sequence) noexcept override {
        stored = s; stored_sequence = sequence + 1U; sequence = stored_sequence; has_record = true;
        return app::FlashResult::Ok;
    }
    void fill_runtime_info(usb::config::Info& info) noexcept override { info.source_rate_hz = 48'000U; }
};

usb::config::Response call(app::ConfigService& service, MockBackend& backend, usb::config::Opcode op,
                           std::uint16_t offset = 0U, std::uint8_t length = 0U, const void* data = nullptr) {
    usb::config::Request request{};
    request.prefix = usb::config::request_prefix;
    request.opcode = static_cast<std::uint8_t>(op);
    request.tag = 0x5AU;
    request.offset = offset;
    request.length = length;
    if (data) std::memcpy(request.data.data(), data, length);
    std::uint8_t packet[64];
    std::memcpy(packet, &request, sizeof(packet));
    return service.handle(packet, sizeof(packet), backend);
}

void upload(app::ConfigService& service, MockBackend& backend, const settings::Settings& s) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&s);
    for (std::size_t offset = 0U; offset < sizeof(s); offset += usb::config::chunk_bytes) {
        const std::size_t n = sizeof(s) - offset < usb::config::chunk_bytes ? sizeof(s) - offset : usb::config::chunk_bytes;
        const auto r = call(service, backend, usb::config::Opcode::Write, static_cast<std::uint16_t>(offset),
                            static_cast<std::uint8_t>(n), bytes + offset);
        expect(r.status == 0U, "staging write accepted");
    }
}
} // namespace

void run_settings_tests() {
    // ---- settings model -------------------------------------------------
    const settings::Settings d = settings::defaults();
    expect(settings::validate(d) == settings::Invalid::None, "defaults are valid");
    expect(d.headroom_db_8_8 == config::board::mcu_headroom_db_8_8 &&
           d.dac_ceiling_db_8_8 == config::board::dac_ceiling_db_8_8 &&
           d.upsampler_mode == 0U && d.eq_enabled == 0U, "defaults reproduce the fixed board budget");
    {
        settings::Settings s = d;
        s.headroom_db_8_8 = 1; expect(settings::validate(s) == settings::Invalid::Headroom, "positive headroom rejected");
        s = d; s.dac_ceiling_db_8_8 = -100; expect(settings::validate(s) == settings::Invalid::DacCeiling, "ceiling off 0.5 dB grid rejected");
        s = d; s.bands[0].type = 1U; s.bands[0].frequency_hz = NAN;
        expect(settings::validate(s) == settings::Invalid::BandFrequency, "NaN frequency rejected");
        s = d; s.bands[3].type = 1U; s.bands[3].gain_db = 25.0F;
        expect(settings::validate(s) == settings::Invalid::BandGain, "excessive band gain rejected");
        s = d; s.bands[3].type = 9U; expect(settings::validate(s) == settings::Invalid::BandType, "unknown filter type rejected");
        s = d; s.bands[2].type = 0U; s.bands[2].gain_db = 99.0F;
        expect(settings::validate(s) == settings::Invalid::None, "an Off band's parameters are ignored");
    }
    expect(settings::crc32("123456789", 9U) == 0xCBF4'3926U, "CRC-32 check value");

    // ---- biquad design --------------------------------------------------
    for (const double fs : {44'100.0, 48'000.0}) {
        settings::EqBand peak{1U, 3U, 0U, 1'000.0F, 6.0F, 1.41F};
        const auto b = audio::design_band(peak, static_cast<std::uint32_t>(fs));
        expect(std::fabs(db_at(b, 1'000.0, fs) - 6.0F) < 0.01F, "peaking gain at centre");
        expect(std::fabs(db_at(b, 20.0, fs)) < 0.05F, "peaking unity far below");
        settings::EqBand low{2U, 3U, 0U, 100.0F, -4.0F, 0.7071F};
        const auto ls = audio::design_band(low, static_cast<std::uint32_t>(fs));
        expect(std::fabs(db_at(ls, 10.0, fs) + 4.0F) < 0.05F && std::fabs(db_at(ls, 10'000.0, fs)) < 0.05F,
               "low shelf plateau and unity top");
        settings::EqBand high{3U, 3U, 0U, 8'000.0F, 3.0F, 0.7071F};
        const auto hs = audio::design_band(high, static_cast<std::uint32_t>(fs));
        expect(std::fabs(db_at(hs, 20.0, fs)) < 0.05F && std::fabs(db_at(hs, 0.49 * fs, fs) - 3.0F) < 0.3F,
               "high shelf unity bottom and plateau top");
        settings::EqBand hp{5U, 3U, 0U, 30.0F, 0.0F, 0.7071F};
        expect(std::fabs(db_at(audio::design_band(hp, static_cast<std::uint32_t>(fs)), 30.0, fs) + 3.01F) < 0.05F,
               "Butterworth high-pass -3 dB at cutoff");
    }

    // ---- source DSP chain and automatic boost compensation ---------------
    {
        settings::Settings s = d;
        auto design = audio::design_source_dsp(s, 48'000U);
        expect(design.identity && design.boost_db == 0.0F, "default chain is identity");
        settings::Settings flat = d;
        flat.eq_enabled = 1U;
        flat.bands[0] = {1U, 3U, 0U, 1'000.0F, 0.0F, 1.0F};
        flat.bands[1] = {2U, 3U, 0U, 100.0F, 0.0F, 0.7F};
        flat.bands[2] = {3U, 1U, 0U, 9'000.0F, 0.0F, 0.7F};
        const auto flat_design = audio::design_source_dsp(flat, 44'100U);
        expect(flat_design.identity && flat_design.left_mask == 0U && flat_design.boost_db == 0.0F,
               "EQ enabled with only 0 dB peaks/shelves is an exact identity");
        s.eq_enabled = 1U;
        s.bands[0] = {1U, 3U, 0U, 3'000.0F, 6.0F, 2.0F};
        s.bands[1] = {2U, 3U, 0U, 105.0F, 4.0F, 0.7F};
        s.bands[2] = {1U, 3U, 0U, 6'000.0F, -5.0F, 3.0F};
        design = audio::design_source_dsp(s, 48'000U);
        expect(!design.identity, "EQ makes the chain non-identity");
        double true_peak_db = -100.0;
        for (double f = 10.0; f < 24'000.0; f *= 1.0005) {
            double db = 0.0;
            for (std::size_t i = 0U; i < 3U; ++i) db += db_at(design.left[i], f, 48'000.0);
            if (db > true_peak_db) true_peak_db = db;
        }
        expect(design.boost_db >= true_peak_db && design.boost_db < true_peak_db + 0.2,
               "boost covers the dense-sweep peak with a small margin");
        audio::SourceDsp dsp;
        dsp.set_design(design);
        expect(std::fabs(design.makeup - std::pow(10.0F, -design.boost_db / 20.0F)) < 1.0e-6F, "makeup matches boost");
        expect(std::fabs(measured_db(dsp, 3'000.0, 48'000.0) - (6.0 - design.boost_db)) < 0.3,
               "running chain matches design at 3 kHz, makeup included");
        // The rule: the in-chain makeup keeps a full-scale sine at the EQ
        // peak below full scale with 0 dB headroom and 0 dB host volume.
        const float gain = audio::mcu_gain_from_uac2_db(0, 0);
        const double peak = std::pow(10.0, measured_db(dsp, 3'000.0, 48'000.0) / 20.0) * gain;
        expect(peak <= 1.0, "boost compensation prevents EQ clipping at 0 dB headroom");

        // A band on one channel only touches that channel.
        settings::Settings one = d;
        one.eq_enabled = 1U;
        one.bands[4] = {1U, settings::channel_right, 0U, 1'000.0F, -12.0F, 1.0F};
        const auto od = audio::design_source_dsp(one, 44'100U);
        expect(od.left_mask == 0U && od.right_mask == (1U << 4U) && od.boost_db == 0.0F,
               "channel mask and cut-only chain needs no compensation");

        settings::Settings bal = d;
        bal.balance_db_8_8 = 6 * 256;
        bal.invert_polarity = settings::channel_right;
        const auto bd = audio::design_source_dsp(bal, 44'100U);
        expect(std::fabs(bd.trim_left - 0.501187F) < 1.0e-4F && bd.trim_right == -1.0F && bd.boost_db == 0.0F,
               "balance attenuates one side, polarity inverts");

        settings::Settings xf = d;
        xf.crossfeed = static_cast<std::uint8_t>(settings::Crossfeed::Bs2bDefault);
        const auto xd = audio::design_source_dsp(xf, 44'100U);
        audio::SourceDsp xdsp;
        xdsp.set_design(xd);
        // Mono at LF passes at unity (bs2b normalisation).
        double peak_mono = 0.0;
        for (int i = 0; i < 44'100; ++i) {
            float l = static_cast<float>(std::sin(2.0 * pi * 50.0 * i / 44'100.0));
            float r = l;
            xdsp.process(l, r);
            if (i > 22'050 && std::fabs(l) > peak_mono) peak_mono = std::fabs(l);
        }
        expect(std::fabs(20.0 * std::log10(peak_mono) + xd.boost_db) < 0.2,
               "crossfeed keeps mono bass at unity (before makeup)");
        expect(xd.boost_db < 1.0F, "crossfeed worst-case bound stays small");
    }

    // ---- mid/side crossfeeds: flat centre, head-like cues for panned -----
    {
        using settings::Crossfeed;
        settings::Settings bad = d;
        bad.crossfeed = settings::crossfeed_count;
        expect(settings::validate(bad) == settings::Invalid::Crossfeed, "unknown crossfeed rejected");
        struct Case { Crossfeed mode; double itd_min_ms; double itd_max_ms; double ild_lo_db; double ild_hi_db; float max_boost_db; };
        // 100 Hz far/near ear for a hard-left source. Speakers: Brown-Duda
        // low-frequency ITD (Woodworth delay + head-shadow group delay).
        const Case cases[] = {
            {Crossfeed::Natural, 0.40, 0.50, -11.0, -9.0, 0.8F},
            {Crossfeed::Speakers30, 0.37, 0.44, -1.5, 0.0, 2.0F},
            {Crossfeed::Speakers45, 0.51, 0.60, -1.5, 0.0, 2.2F},
            {Crossfeed::Spacious, 0.40, 0.60, -19.0, -16.0, 3.8F}, // nominal amount; boost covers 0.5-1.4
        };
        for (const std::uint32_t fs : {44'100U, 48'000U}) {
            for (const auto& c : cases) {
                settings::Settings s = d;
                s.crossfeed = static_cast<std::uint8_t>(c.mode);
                expect(settings::validate(s) == settings::Invalid::None, "mid/side crossfeed is valid");
                const auto design = audio::design_source_dsp(s, fs);
                expect(design.crossfeed.enabled && design.crossfeed.mid_side && !design.identity,
                       "mid/side crossfeed designs as mid/side");
                expect(design.boost_db > 0.0F && design.boost_db < c.max_boost_db, "mid/side crossfeed headroom cost as designed");
                audio::SourceDsp dsp;
                dsp.set_design(design);
                // The centre passes bit-exactly (only the makeup gain).
                bool exact = true;
                for (int i = 0; i < 20'000; ++i) {
                    const float x = static_cast<float>(0.5 * std::sin(2.0 * pi * 997.0 * i / fs) +
                                                       0.3 * std::sin(2.0 * pi * 61.0 * i / fs));
                    float l = x, r = x;
                    dsp.process(l, r);
                    if (l != x * design.makeup || r != x * design.makeup) exact = false;
                }
                expect(exact, "mid/side crossfeed leaves the centre untouched");
                // Hard-left 100 Hz: interaural level and delay.
                dsp.reset_state();
                dsp.set_design(design);
                double li = 0.0, lq = 0.0, ri = 0.0, rq = 0.0;
                const int n = static_cast<int>(fs);
                for (int i = 0; i < n; ++i) {
                    const double phase = 2.0 * pi * 100.0 * i / fs;
                    float l = static_cast<float>(0.5 * std::sin(phase)), r = 0.0F;
                    dsp.process(l, r);
                    if (i < n / 2) continue;
                    li += l * std::sin(phase); lq += l * std::cos(phase);
                    ri += r * std::sin(phase); rq += r * std::cos(phase);
                }
                const double ild = 10.0 * std::log10((ri * ri + rq * rq) / (li * li + lq * lq));
                double lag = std::atan2(lq, li) - std::atan2(rq, ri); // far ear behind
                while (lag < 0.0) lag += 2.0 * pi;
                while (lag >= 2.0 * pi) lag -= 2.0 * pi;
                const double itd_ms = lag / (2.0 * pi * 100.0) * 1'000.0;
                std::cout << "  crossfeed " << static_cast<int>(c.mode) << " @" << fs << ": boost " << design.boost_db
                          << " dB, 100 Hz hard-left ILD " << ild << " dB, ITD " << itd_ms << " ms\n";
                expect(ild > c.ild_lo_db && ild < c.ild_hi_db, "mid/side crossfeed low-frequency level cue");
                expect(itd_ms > c.itd_min_ms && itd_ms < c.itd_max_ms, "mid/side crossfeed low-frequency delay cue");
            }
        }
    }

    // ---- Spacious: the amount follows the programme's correlation ---------
    {
        constexpr std::uint32_t fs = 48'000U;
        settings::Settings s = d;
        s.crossfeed = static_cast<std::uint8_t>(settings::Crossfeed::Spacious);
        const auto design = audio::design_source_dsp(s, fs);
        expect(design.crossfeed.adaptive, "Spacious adapts");
        std::uint32_t seed = 12345U;
        auto noise = [&seed]() { // uniform -1..1
            seed = seed * 1'664'525U + 1'013'904'223U;
            return static_cast<double>(seed >> 8U) / 8'388'608.0 - 1.0;
        };
        // Decaying bursts: a burst every period, 150 ms decay.
        auto burst = [](int i, double period, double phase) {
            const double t = std::fmod(static_cast<double>(i) / fs + phase, period);
            return std::exp(-t / 0.150);
        };
        constexpr double amp = 2.0e6; // the chain runs in 24-bit integer scale
        auto settle = [&](auto&& source) {
            audio::SourceDsp dsp;
            dsp.set_design(design);
            for (int i = 0; i < 10 * static_cast<int>(fs); ++i) {
                float l, r;
                source(i, l, r);
                dsp.process(l, r);
            }
            return dsp;
        };
        // Reverb-like: independent noise in L and R, one shared envelope.
        const auto diffuse = settle([&](int i, float& l, float& r) {
            const double e = amp * burst(i, 0.5, 0.0);
            l = static_cast<float>(e * noise());
            r = static_cast<float>(e * noise());
        });
        // Hard-panned independent sources: their own noise and envelope.
        const auto panned = settle([&](int i, float& l, float& r) {
            l = static_cast<float>(amp * burst(i, 0.37, 0.0) * noise());
            r = static_cast<float>(amp * burst(i, 0.53, 0.2) * noise());
        });
        // Mostly correlated: one source panned a little left plus a trace.
        const auto correlated = settle([&](int i, float& l, float& r) {
            const double x = amp * burst(i, 0.45, 0.0) * noise();
            l = static_cast<float>(x + 0.1 * amp * noise());
            r = static_cast<float>(0.6 * x);
        });
        std::cout << "  Spacious amount: diffuse " << diffuse.crossfeed_amount() << " (rho wave "
                  << diffuse.waveform_correlation() << ", env " << diffuse.envelope_correlation() << "), panned "
                  << panned.crossfeed_amount() << " (" << panned.waveform_correlation() << ", "
                  << panned.envelope_correlation() << "), correlated " << correlated.crossfeed_amount() << " ("
                  << correlated.waveform_correlation() << ", " << correlated.envelope_correlation() << ")\n";
        expect(diffuse.crossfeed_amount() < 0.7F, "diffuse sound gets less crossfeed (reverb kept)");
        expect(panned.crossfeed_amount() > 1.2F, "independent hard-panned sources get more crossfeed");
        expect(correlated.crossfeed_amount() > 0.9F && correlated.crossfeed_amount() < 1.1F,
               "correlated material gets the nominal amount");
            }

    // ---- switching between crossfeed presets glides without clicks --------
    {
        constexpr double fs = 48'000.0;
        using settings::Crossfeed;
        auto input = [](int i, float& l, float& r) {
            l = static_cast<float>(0.3 * std::sin(2.0 * pi * 40.0 * i / fs) + 0.3 * std::sin(2.0 * pi * 1'000.0 * i / fs));
            r = static_cast<float>(0.3 * std::sin(2.0 * pi * 40.0 * i / fs) - 0.2 * std::sin(2.0 * pi * 3'100.0 * i / fs));
        };
        auto run = [&](audio::SourceDsp& dsp, int from, int to, float& last) {
            float worst = 0.0F;
            for (int i = from; i < to; ++i) {
                float l, r;
                input(i, l, r);
                dsp.process(l, r);
                if (i > from && std::fabs(l - last) > worst) worst = std::fabs(l - last);
                last = l;
            }
            return worst;
        };
        auto design_for = [&](Crossfeed mode) {
            settings::Settings s = d;
            s.crossfeed = static_cast<std::uint8_t>(mode);
            return audio::design_source_dsp(s, 48'000U);
        };
        const Crossfeed sequence[] = {Crossfeed::Natural, Crossfeed::Speakers30, Crossfeed::Speakers45,
                                      Crossfeed::Spacious, Crossfeed::Bs2bDefault, Crossfeed::Natural,
                                      Crossfeed::Off};
        // Steady step size of every preset and of the dry signal.
        float steady = 0.0F;
        for (const auto mode : {Crossfeed::Off, Crossfeed::Natural, Crossfeed::Speakers30, Crossfeed::Speakers45,
                                Crossfeed::Spacious, Crossfeed::Bs2bDefault}) {
            audio::SourceDsp ref;
            ref.set_design(design_for(mode));
            float ref_last = 0.0F;
            run(ref, 0, 24'000, ref_last);
            steady = std::fmax(steady, run(ref, 24'000, 48'000, ref_last));
        }
        audio::SourceDsp dsp;
        dsp.set_design(design_for(Crossfeed::Off));
        float last = 0.0F;
        int t = 0;
        run(dsp, t, t + 24'000, last);
        t += 24'000;
        for (const auto mode : sequence) {
            dsp.set_design(design_for(mode));
            const float transition = run(dsp, t, t + 4'800, last); // two 20 ms phases fit in 100 ms
            t += 4'800;
            expect(!dsp.ramping(), "crossfeed preset change finishes within two glides");
            expect(transition <= 1.05F * steady, "no click when switching crossfeed presets");
            run(dsp, t, t + 9'600, last);
            t += 9'600;
        }
        expect(dsp.identity(), "crossfeed off again ends in identity");

        // A preset change lands on the same output as a chain started on it.
        audio::SourceDsp a, fresh;
        a.set_design(design_for(Crossfeed::Natural));
        fresh.set_design(design_for(Crossfeed::Speakers30));
        float la = 0.0F, lf = 0.0F;
        run(a, 0, 24'000, la);
        a.set_design(design_for(Crossfeed::Speakers30));
        run(a, 24'000, 48'000, la);
        run(fresh, 0, 48'000, lf);
        float max_diff = 0.0F;
        for (int i = 48'000; i < 52'000; ++i) {
            float l, r;
            input(i, l, r);
            float fl = l, fr = r;
            a.process(l, r);
            fresh.process(fl, fr);
            max_diff = std::fmax(max_diff, std::fmax(std::fabs(l - fl), std::fabs(r - fr)));
        }
        expect(max_diff < 1.0e-5F, "crossfeed preset change ends on the new design");
    }

    // ---- SVF runtime: same response as the RBJ biquads ------------------
    for (const std::uint32_t fs : {44'100U, 48'000U}) {
        const settings::EqBand bands[] = {
            {1U, 3U, 0U, 1'000.0F, 6.0F, 1.41F}, {1U, 3U, 0U, 25.0F, -8.0F, 4.0F},
            {2U, 3U, 0U, 105.0F, 6.5F, 0.7F},    {3U, 3U, 0U, 9'000.0F, -4.0F, 0.7F},
            {4U, 3U, 0U, 6'000.0F, 0.0F, 0.9F},  {5U, 3U, 0U, 30.0F, 0.0F, 0.7071F},
            {6U, 3U, 0U, 2'000.0F, 0.0F, 3.0F},
        };
        for (const auto& band : bands) {
            settings::Settings s = d;
            s.eq_enabled = 1U;
            s.bands[0] = band;
            const auto sd = audio::design_source_dsp(s, fs);
            audio::SourceDsp dsp;
            dsp.set_design(sd);
            for (const double f : {40.0, 300.0, 1'000.0, 2'100.0, 7'000.0, 15'000.0}) {
                const double expected = db_at(sd.left[0], f, fs) + 20.0 * std::log10(sd.makeup);
                if (expected < -40.0) continue; // deep in a stop band / notch
                const double got = measured_db(dsp, f, fs);
                if (std::fabs(got - expected) >= 0.05) std::cerr << "type " << int(band.type) << " fs " << fs << " f " << f << " got " << got << " exp " << expected << '\n';
                expect(std::fabs(got - expected) < 0.05, "SVF band matches the RBJ response");
            }
        }
    }

    // ---- SVF precision: low, high-Q bands stay at the dither floor -------
    {
        // Independent double-precision RBJ reference (TDF-II in double).
        struct Ref { double b0, b1, b2, a1, a2, s1{0.0}, s2{0.0}; };
        auto ref_design = [](int type, double f, double gain_db, double q, double fs) {
            const double w0 = 2.0 * pi * f / fs, cw = std::cos(w0), al = std::sin(w0) / (2.0 * q);
            const double a = std::pow(10.0, gain_db / 40.0), sa = 2.0 * std::sqrt(a) * al;
            double b0, b1, b2, a0, a1, a2;
            if (type == 1) { b0 = 1 + al * a; b1 = -2 * cw; b2 = 1 - al * a; a0 = 1 + al / a; a1 = -2 * cw; a2 = 1 - al / a; }
            else if (type == 2) {
                b0 = a * ((a + 1) - (a - 1) * cw + sa); b1 = 2 * a * ((a - 1) - (a + 1) * cw);
                b2 = a * ((a + 1) - (a - 1) * cw - sa); a0 = (a + 1) + (a - 1) * cw + sa;
                a1 = -2 * ((a - 1) + (a + 1) * cw); a2 = (a + 1) + (a - 1) * cw - sa;
            } else { b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; }
            return Ref{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
        };
        auto ref_run = [](Ref& r, double x) {
            const double y = r.b0 * x + r.s1;
            r.s1 = r.b1 * x - r.a1 * y + r.s2;
            r.s2 = r.b2 * x - r.a2 * y;
            return y;
        };
        struct Case { int type; double f, gain, q; };
        const Case cases[] = {{2, 20.0, 10.0, 0.7}, {1, 25.0, 8.0, 4.0}, {5, 15.0, 0.0, 0.7071}, {2, 105.0, 6.0, 0.7}};
        for (const double fs : {44'100.0, 48'000.0}) {
            // Loud-music proxy: pink noise (Kellet), 20 Hz high-passed,
            // -15 dBFS RMS. Deterministic LCG for reproducibility.
            const int n = static_cast<int>(fs);
            std::vector<float> x(static_cast<std::size_t>(n));
            std::uint32_t seed = 12345U;
            double p0 = 0, p1 = 0, p2 = 0, energy = 0;
            Ref band_limit = ref_design(5, 20.0, 0.0, 0.7071, fs);
            std::vector<double> raw(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                seed = seed * 1664525U + 1013904223U;
                const double w = static_cast<double>(seed) / 4294967296.0 - 0.5;
                p0 = 0.99765 * p0 + w * 0.0990460; p1 = 0.96300 * p1 + w * 0.2965164;
                p2 = 0.57000 * p2 + w * 1.0526913;
                raw[static_cast<std::size_t>(i)] = ref_run(band_limit, p0 + p1 + p2 + w * 0.1848);
                energy += raw[static_cast<std::size_t>(i)] * raw[static_cast<std::size_t>(i)];
            }
            const double scale = std::pow(10.0, -15.0 / 20.0) / std::sqrt(energy / n);
            for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] = static_cast<float>(raw[static_cast<std::size_t>(i)] * scale);
            for (const auto& c : cases) {
                settings::Settings s = d;
                s.eq_enabled = 1U;
                s.bands[0] = {static_cast<std::uint8_t>(c.type), 3U, 0U, static_cast<float>(c.f),
                              static_cast<float>(c.gain), static_cast<float>(c.q)};
                const auto sd = audio::design_source_dsp(s, static_cast<std::uint32_t>(fs));
                audio::SourceDsp dsp;
                dsp.set_design(sd);
                Ref ref = ref_design(c.type, c.f, c.gain, c.q, fs);
                double err = 0.0;
                int counted = 0;
                for (int i = 0; i < n; ++i) {
                    float l = x[static_cast<std::size_t>(i)], r = l;
                    dsp.process(l, r);
                    const double y = ref_run(ref, x[static_cast<std::size_t>(i)]) * sd.makeup;
                    if (i >= n / 8) { err += (l - y) * (l - y); ++counted; }
                }
                const double err_db = 10.0 * std::log10(err / counted);
                std::cout << "  SVF precision @" << fs << " type " << c.type << " " << c.f << " Hz: error " << err_db << " dBFS RMS" << '\n';
                if (err_db > -125.0) std::cerr << "SVF error " << err_db << " dBFS (type " << c.type << ", " << c.f << " Hz)\n";
                expect(err_db < -125.0, "float32 SVF error stays below -125 dBFS RMS on loud music");
            }
        }
    }

    // ---- live changes glide without clicks ------------------------------
    {
        constexpr double fs = 48'000.0;
        settings::Settings a = d;
        a.eq_enabled = 1U;
        a.bands[0] = {1U, 3U, 0U, 40.0F, 6.0F, 1.0F};
        a.bands[3] = {3U, 3U, 0U, 8'000.0F, 4.0F, 0.7F};
        settings::Settings b = d;
        b.eq_enabled = 1U;
        b.bands[0] = {2U, 3U, 0U, 30.0F, 10.0F, 0.7F};  // retuned and retyped
        b.bands[1] = {1U, 3U, 0U, 1'000.0F, -9.0F, 2.0F}; // new band
        b.balance_db_8_8 = 3 * 256;
        b.crossfeed = static_cast<std::uint8_t>(settings::Crossfeed::Bs2bDefault);
        const auto da = audio::design_source_dsp(a, 48'000U);
        const auto db = audio::design_source_dsp(b, 48'000U);
        expect(std::fabs(da.boost_db - db.boost_db) > 1.0F, "test designs differ in makeup");
        auto input = [](int i) {
            return static_cast<float>(0.3 * std::sin(2.0 * pi * 40.0 * i / fs) + 0.3 * std::sin(2.0 * pi * 1'000.0 * i / fs));
        };
        // Largest sample-to-sample step: a click shows up as a jump well above
        // what the two steady states ever produce.
        auto run = [&](audio::SourceDsp& dsp, int from, int to, float& last) {
            float worst = 0.0F;
            for (int i = from; i < to; ++i) {
                float l = input(i), r = l;
                dsp.process(l, r);
                if (i > from && std::fabs(l - last) > worst) worst = std::fabs(l - last);
                last = l;
            }
            return worst;
        };
        audio::SourceDsp dsp;
        dsp.reset_state();
        dsp.set_design(da);
        expect(!dsp.ramping(), "first design after reset snaps");
        float last = 0.0F;
        run(dsp, 0, 24'000, last);
        const float steady_a = run(dsp, 24'000, 48'000, last);
        dsp.set_design(db);
        expect(dsp.ramping() && !dsp.identity(), "live change glides");
        const float transition = run(dsp, 48'000, 48'000 + 4'800, last);
        expect(!dsp.ramping(), "glide ends within 20 ms");
        run(dsp, 52'800, 72'000, last);
        const float steady_b = run(dsp, 72'000, 96'000, last);
        const float bound = 1.05F * (steady_a > steady_b ? steady_a : steady_b);
        expect(transition <= bound, "no click while gliding between designs");

        // The glide lands exactly on the target: once settled, the same
        // output sample for sample as a chain that started on it.
        audio::SourceDsp fresh;
        fresh.set_design(db);
        float fresh_last = 0.0F;
        run(fresh, 0, 96'000, fresh_last);
        float max_diff = 0.0F;
        for (int i = 96'000; i < 100'000; ++i) {
            float l = input(i), r = l, fl = l, fr = l;
            dsp.process(l, r);
            fresh.process(fl, fr);
            max_diff = std::fmax(max_diff, std::fmax(std::fabs(l - fl), std::fabs(r - fr)));
        }
        expect(max_diff < 1.0e-5F, "glide ends on the new design");

        // Gliding back to neutral ends in true identity (bit-perfect path).
        dsp.set_design(audio::design_source_dsp(d, 48'000U));
        expect(!dsp.identity(), "fade to neutral is processed");
        run(dsp, 0, 1'000, last);
        expect(dsp.identity(), "neutral design is identity once the fade ends");
    }

    // ---- detail stage: settings ------------------------------------------
    {
        settings::Settings s = d;
        s.proximity_amount_pct = 0U; s.transient_amount_pct = 0U; s.transient_crossover_100hz = 0U;
        expect(settings::validate(s) == settings::Invalid::None, "records from before the detail stage are valid (off)");
        settings::upgrade(s);
        expect(std::memcmp(&s, &d, sizeof(s)) == 0, "old records get the default detail parameters");
        s = d; s.detail_flags = settings::detail_all;
        expect(settings::validate(s) == settings::Invalid::None, "default detail values are valid");
        s.proximity_amount_pct = 5U;
        expect(settings::validate(s) == settings::Invalid::Proximity, "proximity amount below 10 % rejected");
        s = d; s.detail_flags = settings::detail_transient; s.transient_amount_pct = 101U;
        expect(settings::validate(s) == settings::Invalid::Transient, "transient amount over 100 % rejected");
        s = d; s.detail_flags = settings::detail_transient; s.transient_crossover_100hz = 81U;
        expect(settings::validate(s) == settings::Invalid::Transient, "transient crossover above 8 kHz rejected");
        s = d; s.transient_crossover_100hz = 200U;
        expect(settings::validate(s) == settings::Invalid::None, "detail fields are only checked while on");
    }

    // ---- detail stage: proximity expands the level difference ------------
    {
        constexpr double fs = 48'000.0;
        settings::Settings s = d;
        s.detail_flags = settings::detail_proximity;
        s.proximity_amount_pct = 100U;
        const auto design = audio::design_source_dsp(s, 48'000U);
        expect(!design.identity && design.boost_db == 0.0F, "proximity is processed and needs no headroom");
        // Amplitude of one frequency by quadrature projection.
        struct Probe { double f; double si{0.0}; double co{0.0}; };
        auto amplitude = [](const Probe& p, int n) { return 2.0 * std::sqrt(p.si * p.si + p.co * p.co) / n; };
        for (const double ratio : {1.0, 0.5, 0.25}) {
            audio::SourceDsp dsp;
            dsp.set_design(design);
            Probe pl[2]{{300.0}, {4'000.0}}, pr[2]{{300.0}, {4'000.0}};
            const int n = static_cast<int>(fs);
            for (int i = 0; i < 2 * n; ++i) {
                const double lo = std::sin(2.0 * pi * 300.0 * i / fs), hi = std::sin(2.0 * pi * 4'000.0 * i / fs);
                float l = static_cast<float>(1.0e6 * (lo + hi));
                float r = static_cast<float>(1.0e6 * ratio * (lo + hi));
                dsp.process(l, r);
                if (i < n) continue;
                for (int b = 0; b < 2; ++b) {
                    const double ph = 2.0 * pi * pl[b].f * i / fs;
                    pl[b].si += l * std::sin(ph); pl[b].co += l * std::cos(ph);
                    pr[b].si += r * std::sin(ph); pr[b].co += r * std::cos(ph);
                }
            }
            const double ild_in = -20.0 * std::log10(ratio);
            for (int b = 0; b < 2; ++b) {
                const double al = amplitude(pl[b], n), ar = amplitude(pr[b], n);
                const double ild = 20.0 * std::log10(al / ar);
                std::cout << "  proximity 100 % @" << pl[b].f << " Hz: ILD " << ild_in << " -> " << ild
                          << " dB, louder side " << 20.0 * std::log10(al / 1.0e6) << " dB\n";
                expect(std::fabs(20.0 * std::log10(al / 1.0e6)) < 0.05, "proximity leaves the louder side alone");
                expect(std::fabs(ild - 2.0 * ild_in) < 0.3, "proximity 100 % doubles the level difference");
            }
        }
        // Never louder than the input, whatever the (independent) channels do.
        audio::SourceDsp dsp;
        dsp.set_design(design);
        std::uint32_t seed = 777U;
        auto noise = [&seed]() {
            seed = seed * 1'664'525U + 1'013'904'223U;
            return static_cast<double>(seed >> 8U) / 8'388'608.0 - 1.0;
        };
        double in_l = 0.0, in_r = 0.0, out_l = 0.0, out_r = 0.0;
        for (int i = 0; i < 5 * static_cast<int>(fs); ++i) {
            const double env_l = 0.5 + 0.5 * std::sin(2.0 * pi * 0.7 * i / fs);
            const double env_r = 0.5 + 0.5 * std::sin(2.0 * pi * 1.1 * i / fs);
            float l = static_cast<float>(2.0e6 * env_l * noise());
            float r = static_cast<float>(2.0e6 * env_r * noise());
            in_l += static_cast<double>(l) * l; in_r += static_cast<double>(r) * r;
            dsp.process(l, r);
            out_l += static_cast<double>(l) * l; out_r += static_cast<double>(r) * r;
        }
        expect(out_l <= in_l && out_r <= in_r, "proximity only cuts");
        // Switching off glides back to unity and ends in identity.
        dsp.set_design(audio::design_source_dsp(d, 48'000U));
        for (int i = 0; i < 4'800; ++i) { float l = 1.0F, r = 0.5F; dsp.process(l, r); }
        expect(dsp.identity(), "proximity switched off ends in identity");
    }

    // ---- detail stage: transient shaper ---------------------------------
    {
        constexpr double fs = 48'000.0;
        settings::Settings s = d;
        s.detail_flags = settings::detail_transient;
        s.transient_amount_pct = 100U;
        s.transient_crossover_100hz = 25U;
        const auto design = audio::design_source_dsp(s, 48'000U);
        std::cout << "  transient 100 %: headroom " << design.boost_db << " dB\n";
        expect(design.boost_db > 9.5F && design.boost_db < 11.0F, "transient boost is covered by the headroom");
        // Tone bursts: 60 ms on, 140 ms off, at 6 kHz (boosted) and 200 Hz (not).
        for (const double f : {6'000.0, 200.0}) {
            audio::SourceDsp dsp;
            dsp.set_design(design);
            double onset_peak = 0.0, steady_peak = 0.0, in_peak = 0.0;
            for (int i = 0; i < 2 * static_cast<int>(fs); ++i) {
                const int t = i % 9'600;
                const double x = t < 2'880 ? 3.0e6 * std::sin(2.0 * pi * f * t / fs) : 0.0;
                float l = static_cast<float>(x), r = l;
                dsp.process(l, r);
                in_peak = std::fmax(in_peak, std::fabs(x));
                if (i < static_cast<int>(fs)) continue;
                if (t < 480) onset_peak = std::fmax(onset_peak, std::fabs(l));
                else if (t > 2'000 && t < 2'880) steady_peak = std::fmax(steady_peak, std::fabs(l));
            }
            const double lift = 20.0 * std::log10(onset_peak / steady_peak);
            std::cout << "  transient @" << f << " Hz: onset " << lift << " dB above steady, peak "
                      << 20.0 * std::log10(onset_peak / in_peak) << " dB re input\n";
            expect(onset_peak <= in_peak * 1.0001, "transient shaper never exceeds the input peak");
            if (f > 1'000.0) expect(lift > 6.0, "onsets of the high band are lifted");
            else expect(lift < 0.5, "low frequencies are left alone");
        }
        // Switching off fades out and ends in identity.
        audio::SourceDsp dsp;
        dsp.set_design(design);
        dsp.set_design(audio::design_source_dsp(d, 48'000U));
        for (int i = 0; i < 4'800; ++i) { float l = 1.0F, r = 0.5F; dsp.process(l, r); }
        expect(dsp.identity(), "transient shaper switched off ends in identity");
    }

    // ---- space stage: settings and the legacy record --------------------
    {
        settings::Settings s = d;
        s.detail_flags = settings::detail_width | settings::detail_front;
        expect(settings::validate(s) == settings::Invalid::None, "default width/front values are valid");
        s.width_amount_pct = 5U;
        expect(settings::validate(s) == settings::Invalid::Width, "width amount below 10 % rejected");
        s = d; s.detail_flags = settings::detail_front; s.front_amount_pct = 101U;
        expect(settings::validate(s) == settings::Invalid::Front, "front amount over 100 % rejected");
        s = d; s.reserved[5] = 1U;
        expect(settings::validate(s) == settings::Invalid::Reserved, "trailing reserved bytes checked");
        // A 192-byte record from before the space stage.
        std::array<std::uint8_t, settings::legacy_size> legacy{};
        settings::Settings old = d;
        old.size = static_cast<std::uint16_t>(settings::legacy_size);
        old.crossfeed = static_cast<std::uint8_t>(settings::Crossfeed::Spacious);
        std::memcpy(legacy.data(), &old, legacy.size());
        settings::Settings loaded{};
        expect(settings::from_record(loaded, legacy.data(), legacy.size()), "legacy record accepted");
        expect(settings::validate(loaded) == settings::Invalid::None, "legacy record is valid (space stage off)");
        settings::upgrade(loaded);
        settings::Settings want = d;
        want.crossfeed = old.crossfeed;
        expect(std::memcmp(&loaded, &want, sizeof(want)) == 0, "legacy record gets the default width/front values");
        expect(!settings::from_record(loaded, legacy.data(), 100U), "other record lengths rejected");
        expect(settings::from_record(loaded, &want, sizeof(want)) && std::memcmp(&loaded, &want, sizeof(want)) == 0,
               "current record read as is");
    }

    // ---- space stage: width expander and front cue ----------------------
    {
        constexpr double fs = 48'000.0;
        // Steady-state peak of each output channel for a stereo tone pair.
        // Steady-state peaks of both outputs, and the side amplitude
        // |L - R| / 2 at f by quadrature projection (all re 1.0e6).
        double side_amp = 0.0;
        double left_amp = 0.0;
        auto run_tone = [&](audio::SourceDsp& dsp, double f, double gl, double gr, double& out_l, double& out_r) {
            out_l = out_r = 0.0;
            double si = 0.0, co = 0.0, lsi = 0.0, lco = 0.0;
            const int n = static_cast<int>(fs);
            for (int i = 0; i < n; ++i) {
                const double x = std::sin(2.0 * pi * f * i / fs);
                float l = static_cast<float>(1.0e6 * gl * x);
                float r = static_cast<float>(1.0e6 * gr * x);
                dsp.process(l, r);
                if (i < n - 9'600) continue;
                out_l = std::fmax(out_l, std::fabs(l) / 1.0e6);
                out_r = std::fmax(out_r, std::fabs(r) / 1.0e6);
                const double side = 0.5 * (static_cast<double>(l) - r) / 1.0e6;
                si += side * std::sin(2.0 * pi * f * i / fs);
                co += side * std::cos(2.0 * pi * f * i / fs);
                lsi += l / 1.0e6 * std::sin(2.0 * pi * f * i / fs);
                lco += l / 1.0e6 * std::cos(2.0 * pi * f * i / fs);
            }
            side_amp = 2.0 * std::sqrt(si * si + co * co) / 9'600.0;
            left_amp = 2.0 * std::sqrt(lsi * lsi + lco * lco) / 9'600.0;
        };
        settings::Settings s = d;
        s.detail_flags = settings::detail_width;
        s.width_amount_pct = 100U;
        const auto wide = audio::design_source_dsp(s, 48'000U);
        std::cout << "  width 100 %: headroom " << wide.boost_db << " dB\n";
        expect(wide.boost_db > 5.5F && wide.boost_db < 6.5F, "width boost is covered by the headroom");
        const double mk = wide.makeup;
        double ol = 0.0, orr = 0.0;
        {
            audio::SourceDsp dsp;
            dsp.set_design(wide);
            run_tone(dsp, 1'000.0, 1.0, 1.0, ol, orr);
            expect(std::fabs(ol / mk - 1.0) < 1.0e-3 && std::fabs(orr / mk - 1.0) < 1.0e-3,
                   "width leaves a mono signal alone");
        }
        {
            // Hard-panned 1 kHz: S = M, the side grows by up to E p_s / (p_s + 0.1 p_m).
            audio::SourceDsp dsp;
            dsp.set_design(wide);
            run_tone(dsp, 1'000.0, 1.0, 0.0, ol, orr);
            const double side_db = 20.0 * std::log10(side_amp / mk / 0.5);
            std::cout << "  width 100 % hard-panned 1 kHz: side " << side_db << " dB, current boost "
                      << dsp.width_boost_db() << " dB\n";
            expect(side_db > 4.0, "hard-panned sound is widened");
            expect(ol <= 1.0 + 1.0e-4 && orr <= 1.0 + 1.0e-4, "width output stays under the input peak");
            // Switching off glides back and ends in identity.
            dsp.set_design(audio::design_source_dsp(d, 48'000U));
            for (int i = 0; i < 48'000; ++i) { float l = 1.0F, r = 0.5F; dsp.process(l, r); }
            expect(dsp.identity(), "width switched off ends in identity");
        }
        {
            // Centre-heavy: a small side tone (-26 dB) gets almost nothing.
            audio::SourceDsp dsp;
            dsp.set_design(wide);
            const int n = static_cast<int>(fs);
            double si = 0.0, co = 0.0;
            for (int i = 0; i < n; ++i) {
                const double x = std::sin(2.0 * pi * 1'000.0 * i / fs);
                const double y = 0.05 * std::sin(2.0 * pi * 2'500.0 * i / fs);
                float l = static_cast<float>(1.0e6 * (x + y));
                float r = static_cast<float>(1.0e6 * (x - y));
                dsp.process(l, r);
                if (i < n / 2) continue;
                const double side = 0.5 * (l - r) / 1.0e6 / mk;
                si += side * std::sin(2.0 * pi * 2'500.0 * i / fs);
                co += side * std::cos(2.0 * pi * 2'500.0 * i / fs);
            }
            const double amp = 2.0 * std::sqrt(si * si + co * co) / (n / 2);
            const double db = 20.0 * std::log10(amp / 0.05);
            std::cout << "  width 100 % centre-heavy: side " << db << " dB\n";
            expect(db < 0.5, "a near-mono centre is barely widened");
        }
        {
            // Hard-panned bass stays put (the side boost starts at 300 Hz).
            audio::SourceDsp dsp;
            dsp.set_design(wide);
            run_tone(dsp, 60.0, 1.0, 0.0, ol, orr);
            const double side_db = 20.0 * std::log10(side_amp / mk / 0.5);
            std::cout << "  width 100 % hard-panned 60 Hz: side " << side_db << " dB\n";
            expect(side_db < 1.0, "bass is not widened");
        }

        s = d;
        s.detail_flags = settings::detail_front;
        s.front_amount_pct = 100U;
        const auto front = audio::design_source_dsp(s, 48'000U);
        std::cout << "  front 100 %: headroom " << front.boost_db << " dB\n";
        expect(front.boost_db > 2.5F && front.boost_db < 3.6F, "front cue boost is covered by the headroom");
        const double fm = front.makeup;
        struct Case { double f; double gl; double gr; double want_db; const char* what; };
        for (const Case c : {Case{8'000.0, 1.0, 1.0, -6.0, "front cue dips the centre at 8 kHz"},
                             Case{1'000.0, 1.0, 1.0, 0.0, "front cue leaves the centre at 1 kHz"},
                             Case{3'000.0, 1.0, -1.0, 3.0, "front cue lifts the side at 3 kHz"},
                             Case{300.0, 1.0, -1.0, 0.0, "front cue leaves the side at 300 Hz"}}) {
            audio::SourceDsp dsp;
            dsp.set_design(front);
            run_tone(dsp, c.f, c.gl, c.gr, ol, orr);
            const double db = 20.0 * std::log10(left_amp / fm);
            std::cout << "  front 100 % @" << c.f << " Hz (" << (c.gr > 0 ? "mid" : "side") << "): " << db << " dB\n";
            expect(std::fabs(db - c.want_db) < 0.3, c.what);
            expect(ol <= 1.0 + 1.0e-4, "front cue output stays under the input peak");
        }
        {
            audio::SourceDsp dsp;
            dsp.set_design(front);
            dsp.set_design(audio::design_source_dsp(d, 48'000U));
            for (int i = 0; i < 4'800; ++i) { float l = 1.0F, r = 0.5F; dsp.process(l, r); }
            expect(dsp.identity(), "front cue switched off ends in identity");
        }
        {
            // Both at once (with Spacious): still never over the input peak.
            s = d;
            s.detail_flags = settings::detail_width | settings::detail_front;
            s.width_amount_pct = 100U;
            s.front_amount_pct = 100U;
            s.crossfeed = static_cast<std::uint8_t>(settings::Crossfeed::Spacious);
            const auto both = audio::design_source_dsp(s, 48'000U);
            std::cout << "  width + front + Spacious 100 %: headroom " << both.boost_db << " dB\n";
            for (const double f : {200.0, 1'000.0, 3'000.0, 6'000.0}) {
                audio::SourceDsp dsp;
                dsp.set_design(both);
                run_tone(dsp, f, 1.0, -0.3, ol, orr);
                expect(ol <= 1.0 + 1.0e-4 && orr <= 1.0 + 1.0e-4, "width + front + crossfeed stay under the input peak");
            }
        }
    }

    // ---- loudness leveler and loudness compensation ---------------------
    {
        settings::Settings s = d;
        s.leveler_mode = 0U; s.leveler_max_cut_db = 0U; s.leveler_target_db_8_8 = 0;
        s.loudness_enabled = 0U; s.loudness_amount_pct = 0U; s.loudness_reference_db_8_8 = 0;
        s.loudness_max_boost_db = 0U;
        expect(settings::validate(s) == settings::Invalid::None, "records from before leveler/loudness are valid (off)");
        settings::upgrade(s);
        expect(std::memcmp(&s, &d, sizeof(s)) == 0, "old records upgrade to the default parameters");
        MockBackend legacy;
        legacy.has_record = true;
        legacy.stored = s;
        legacy.stored.leveler_max_cut_db = 0U; legacy.stored.leveler_target_db_8_8 = 0;
        legacy.stored.loudness_amount_pct = 0U; legacy.stored.loudness_reference_db_8_8 = 0;
        legacy.stored.loudness_max_boost_db = 0U;
        app::ConfigService booted;
        booted.boot(legacy);
        upload(booted, legacy, d);
        expect(call(booted, legacy, usb::config::Opcode::Apply).status == 0U, "defaults apply over an old record");
        const auto info_reply = call(booted, legacy, usb::config::Opcode::Info);
        usb::config::Info legacy_info{};
        std::memcpy(&legacy_info, info_reply.data.data(), sizeof(legacy_info));
        expect((legacy_info.flags & usb::config::flag_unsaved) == 0U,
               "writing back an upgraded old record is not reported as unsaved");
        s = d; s.leveler_mode = 4U;
        expect(settings::validate(s) == settings::Invalid::Leveler, "unknown leveler mode rejected");
        s = d; s.leveler_mode = 2U; s.leveler_target_db_8_8 = -2 * 256;
        expect(settings::validate(s) == settings::Invalid::Leveler, "leveler target too high rejected");
        s = d; s.leveler_mode = 2U; s.leveler_max_cut_db = 0U;
        expect(settings::validate(s) == settings::Invalid::Leveler, "zero leveler cut rejected");
        s = d; s.loudness_enabled = 1U; s.loudness_reference_db_8_8 = 256;
        expect(settings::validate(s) == settings::Invalid::Loudness, "positive loudness reference rejected");
        s = d; s.loudness_enabled = 1U; s.loudness_amount_pct = 101U;
        expect(settings::validate(s) == settings::Invalid::Loudness, "loudness amount over 100 % rejected");
        s = d; s.leveler_mode = 3U; s.loudness_enabled = 1U;
        expect(settings::validate(s) == settings::Invalid::None, "default leveler/loudness values are valid");
        s = d; s.detail_flags = 0x10U;
        expect(settings::validate(s) == settings::Invalid::Reserved, "unknown detail bits rejected");
    }
    {
        // Shelf gains follow the attenuation below the reference and cap.
        settings::Settings s = d;
        s.loudness_enabled = 1U; s.loudness_amount_pct = 100U;
        s.loudness_reference_db_8_8 = -10 * 256; s.loudness_max_boost_db = 12U;
        float bass = 0.0F, treble = 0.0F;
        audio::loudness_shelf_gains(s, -5 * 256, bass, treble);
        expect(bass == 0.0F && treble == 0.0F, "no loudness compensation above the reference");
        audio::loudness_shelf_gains(s, -20 * 256, bass, treble);
        expect(std::fabs(bass - 5.6F) < 1.0e-4F && std::fabs(treble - 1.7F) < 1.0e-4F,
               "loudness compensation follows ISO 226 fit");
        audio::loudness_shelf_gains(s, -127 * 256, bass, treble);
        expect(bass == 12.0F && std::fabs(treble - 12.0F * 0.17F / 0.56F) < 1.0e-4F,
               "loudness compensation caps and keeps its shape");
        s.loudness_enabled = 0U;
        audio::loudness_shelf_gains(s, -60 * 256, bass, treble);
        expect(bass == 0.0F && treble == 0.0F, "loudness compensation off");

        // At any frequency the compensated level stays below the reference
        // level (volume + boost <= reference), so it cannot clip.
        s.loudness_enabled = 1U; s.loudness_max_boost_db = 20U;
        for (const double fs : {44'100.0, 48'000.0}) {
            for (int vol = -10; vol >= -60; vol -= 5) {
                auto dd = audio::design_source_dsp(s, static_cast<std::uint32_t>(fs));
                audio::apply_loudness_compensation(dd, s, static_cast<std::int16_t>(vol * 256));
                expect(!dd.identity || vol == -10, "loudness slots make the chain non-identity");
                expect(dd.boost_db == 0.0F, "loudness compensation is not part of the boost analysis");
                const settings::EqBand bass_band{2U, 3U, 0U, audio::loudness_bass_hz, dd.loudness_bass_db,
                                                 audio::loudness_bass_q};
                const settings::EqBand treble_band{3U, 3U, 0U, audio::loudness_treble_hz, dd.loudness_treble_db,
                                                   audio::loudness_treble_q};
                const auto bb = audio::design_band(bass_band, static_cast<std::uint32_t>(fs));
                const auto tb = audio::design_band(treble_band, static_cast<std::uint32_t>(fs));
                float worst = -1000.0F;
                for (double f = 10.0; f < 0.499 * fs; f *= 1.01) {
                    const float total = (dd.loudness_bass_db > 0.0F ? db_at(bb, f, fs) : 0.0F) +
                                        (dd.loudness_treble_db > 0.0F ? db_at(tb, f, fs) : 0.0F);
                    if (total > worst) worst = total;
                }
                expect(static_cast<float>(vol) + worst <= -10.0F + 1.0e-3F,
                       "loudness boost never exceeds the attenuation below the reference");
            }
        }

        // Measured: at -30 dB host volume the chain lifts 50 Hz relative to 1 kHz.
        auto dd = audio::design_source_dsp(s, 48'000U);
        audio::apply_loudness_compensation(dd, s, static_cast<std::int16_t>(-30 * 256));
        audio::SourceDsp dsp;
        dsp.set_design(dd);
        const double lift = measured_db(dsp, 50.0, 48'000.0) - measured_db(dsp, 1'000.0, 48'000.0);
        std::cout << "  loudness @-30 dB: 50 Hz vs 1 kHz +" << lift << " dB (bass shelf "
                  << dd.loudness_bass_db << " dB)\n";
        // ISO 226 asks for +8.2 dB at 50 Hz for 20 dB below an 80 phon reference.
        expect(std::fabs(lift - 8.2) < 0.6, "loudness compensation lifts the bass at low volume");
    }
    {
        // Leveler: a loud input converges to the target, a quiet one is left
        // alone, silence holds the gain.
        settings::Settings s = d;
        s.leveler_mode = 3U; s.leveler_target_db_8_8 = -20 * 256; s.leveler_max_cut_db = 12U;
        const double fs = 48'000.0;
        auto run_tone = [&](audio::SourceDsp& dsp, double dbfs, double seconds) {
            const double amplitude = 8'388'608.0 * std::pow(10.0, dbfs / 20.0);
            const int n = static_cast<int>(seconds * fs);
            float peak = 0.0F;
            for (int i = 0; i < n; ++i) {
                float l = static_cast<float>(amplitude * std::sin(2.0 * pi * 1'000.0 * i / fs));
                float r = l;
                dsp.process(l, r);
                if (i > n - 4800 && std::fabs(l) > peak) peak = std::fabs(l);
            }
            return 20.0 * std::log10(peak / amplitude);
        };
        auto design = audio::design_source_dsp(s, 48'000U);
        expect(!design.identity && design.boost_db == 0.0F, "leveler alone runs the chain without boost");
        audio::SourceDsp dsp;
        dsp.set_design(design);
        // A stereo 1 kHz sine at -6 dBFS: K-weighted about -6 + 0.7 - 3 + 3 = -5.3 LUFS
        // (two channels add 3 dB); expect a cut near 20 - 5.3 = 14.7, capped at 12.
        double gain = run_tone(dsp, -6.0, 20.0);
        std::cout << "  leveler: -6 dBFS stereo sine -> " << gain << " dB (cap 12)\n";
        expect(std::fabs(gain + 12.0) < 0.2, "leveler cut is limited to the maximum cut");
        expect(std::fabs(dsp.leveler_gain_db() + 12.0F) < 0.2F, "leveler gain is reported");
        s.leveler_max_cut_db = 30U;
        dsp.set_design(audio::design_source_dsp(s, 48'000U));
        gain = run_tone(dsp, -6.0, 20.0);
        // BS.1770 of a stereo sine: L = -0.691 + 10 log10(2 * K(1k)^2 * A^2 / 2).
        const double k1k = 20.0 * std::log10(audio::biquad_magnitude(design.leveler.k_shelf,
                                                 static_cast<float>(2.0 * pi * 1'000.0 / fs)) *
                                             audio::biquad_magnitude(design.leveler.k_highpass,
                                                 static_cast<float>(2.0 * pi * 1'000.0 / fs)));
        const double lufs = -0.691 + k1k - 6.0;
        std::cout << "  leveler: input " << lufs << " LUFS -> gain " << gain << " dB\n";
        expect(std::fabs((lufs + gain) - (-20.0)) < 0.3, "leveler reaches the target loudness");
        // Silence holds the cut (track gaps do not release it).
        const double held = dsp.leveler_gain_db();
        for (int i = 0; i < 48'000 * 5; ++i) { float l = 0.0F, r = 0.0F; dsp.process(l, r); }
        // (one last partial tone block may still release by ~0.15 dB in Fast mode)
        expect(std::fabs(dsp.leveler_gain_db() - held) < 0.3, "silence holds the leveler gain");
        // Quiet material (below the target) releases back to unity, never above.
        gain = run_tone(dsp, -40.0, 30.0);
        expect(std::fabs(gain) < 0.05 && dsp.leveler_gain_db() <= 0.0F, "quiet material is not boosted");
        // Switching off glides back to unity and lets the chain go idle.
        run_tone(dsp, -6.0, 10.0);
        s.leveler_mode = 0U;
        dsp.set_design(audio::design_source_dsp(s, 48'000U));
        expect(!dsp.identity(), "leveler switched off still glides");
        run_tone(dsp, -60.0, 2.0);
        expect(dsp.identity() && dsp.leveler_gain_db() == 0.0F, "leveler off returns to unity and idle");
    }

    // ---- headroom-aware gain --------------------------------------------
    expect(audio::mcu_gain_from_uac2_db(0, 0, 0.0F) == 1.0F, "0 dB volume and headroom is exactly unity");
    expect(std::fabs(audio::mcu_gain_from_uac2_db(0, -6 * 256, 0.0F) - 0.501187F) < 1.0e-5F, "6 dB headroom");
    expect(std::fabs(audio::mcu_gain_from_uac2_db(0, -3 * 256 - 64, 2.75F) - 0.501187F) < 1.0e-5F,
           "boost is subtracted on top of headroom");

    // ---- bypass formats and DAC modes ------------------------------------
    for (const std::uint32_t rate : {44'100U, 48'000U}) {
        const auto* f = audio::find_format(rate, true);
        expect(f && f->output_sample_rate_hz == rate && f->dac_mode == 1U &&
               f->output_oscillator_hz == rate * 512U && f->dma_frames_per_transfer == rate / 1000U,
               "bypass format runs the DAC at the host rate, 512 fs");
        expect(audio::find_format(rate, false) == audio::find_format(rate), "non-bypass lookup unchanged");
        audio::PolyphaseUpsampler up;
        expect(up.configure(f->format.sample_rate_hz, f->output_sample_rate_hz) &&
               up.input_frames_needed(f->dma_frames_per_transfer) == f->dma_frames_per_transfer,
               "bypass configures the native upsampler path");
    }
    {
        struct Transport final : devices::DacTransport {
            std::vector<std::array<std::uint8_t, 3>> writes;
            void initialize() noexcept override {}
            void set_reset(bool) noexcept override {}
            void wait_after_reset_release() noexcept override {}
            void set_chip_select(bool) noexcept override {}
            bool transfer(const devices::DacCommand& c) noexcept override { writes.push_back(c.bytes); return true; }
        } transport;
        devices::Cs4398 dac(transport, {0U, 0U, 0U});
        dac.begin(0U);
        for (int i = 0; i < 16 && !dac.ready_muted(); ++i) dac.poll(0U);
        transport.writes.clear();
        expect(dac.configure_format(*audio::find_format(48'000U, true), true), "single-speed DAC configuration");
        expect(transport.writes.size() == 2U && transport.writes[0][1] == 0x02U && transport.writes[0][2] == 0x10U &&
               transport.writes[1][1] == 0x07U && transport.writes[1][2] == 0xB4U,
               "bypass selects single-speed I2S and slow roll-off");
        transport.writes.clear();
        expect(dac.configure_format(*audio::find_format(44'100U, false), false) &&
               transport.writes[0][2] == 0x12U && transport.writes[1][2] == 0xB0U,
               "4x path keeps quad-speed and fast roll-off");
        audio::FormatConfig wrong = *audio::find_format(44'100U, true);
        wrong.dac_mode = 0U;
        expect(!dac.configure_format(wrong), "mode/rate mismatch is rejected");
    }

    // ---- configuration service -------------------------------------------
    {
        MockBackend backend;
        app::ConfigService service;
        service.boot(backend);
        expect(backend.apply_count == 1 && std::memcmp(&backend.applied, &d, sizeof(d)) == 0,
               "boot without a flash record applies defaults");

        auto r = call(service, backend, usb::config::Opcode::Info);
        usb::config::Info info{};
        std::memcpy(&info, r.data.data(), sizeof(info));
        expect(r.magic == usb::config::response_magic && r.tag == 0x5AU && r.status == 0U &&
               info.settings_size == sizeof(settings::Settings) && info.eq_bands == settings::eq_band_count &&
               (info.flags & usb::config::flag_flash_available) && !(info.flags & usb::config::flag_flash_record) &&
               (info.flags & usb::config::flag_unsaved) && info.source_rate_hz == 48'000U,
               "info reports layout and flash state");

        settings::Settings custom = d;
        custom.upsampler_mode = 1U;
        custom.headroom_db_8_8 = -6 * 256;
        custom.eq_enabled = 1U;
        custom.bands[0] = {1U, 3U, 0U, 2'500.0F, 3.0F, 1.0F};
        upload(service, backend, custom);
        expect(backend.apply_count == 1, "staging writes do not apply");
        r = call(service, backend, usb::config::Opcode::Info);
        std::memcpy(&info, r.data.data(), sizeof(info));
        expect(info.flags & usb::config::flag_staging_dirty, "staging differs from active");
        r = call(service, backend, usb::config::Opcode::Apply);
        expect(r.status == 0U && backend.apply_count == 2 &&
               std::memcmp(&backend.applied, &custom, sizeof(custom)) == 0, "apply makes staging active");

        settings::Settings read_back{};
        auto* bytes = reinterpret_cast<std::uint8_t*>(&read_back);
        for (std::size_t off = 0U; off < sizeof(read_back); off += 48U) {
            const auto n = static_cast<std::uint8_t>(sizeof(read_back) - off < 48U ? sizeof(read_back) - off : 48U);
            r = call(service, backend, usb::config::Opcode::Read, static_cast<std::uint16_t>(off), n);
            expect(r.status == 0U && r.length == n, "read chunk");
            std::memcpy(bytes + off, r.data.data(), n);
        }
        expect(std::memcmp(&read_back, &custom, sizeof(custom)) == 0, "read returns the active settings");

        expect(call(service, backend, usb::config::Opcode::Read, 180U, 48U).status ==
               static_cast<std::uint8_t>(usb::config::Status::BadRange), "read past the end rejected");

        settings::Settings bad = custom;
        bad.bands[1] = {1U, 3U, 0U, 50'000.0F, 1.0F, 1.0F};
        upload(service, backend, bad);
        r = call(service, backend, usb::config::Opcode::Apply);
        expect(r.status == static_cast<std::uint8_t>(usb::config::Status::InvalidSettings) &&
               r.aux == static_cast<std::uint32_t>(settings::Invalid::BandFrequency) && backend.apply_count == 2,
               "invalid upload rejected with a reason and not applied");

        r = call(service, backend, usb::config::Opcode::Save);
        expect(r.status == 0U && backend.has_record && r.aux == 1U &&
               std::memcmp(&backend.stored, &custom, sizeof(custom)) == 0, "save writes active settings");
        r = call(service, backend, usb::config::Opcode::Info);
        std::memcpy(&info, r.data.data(), sizeof(info));
        expect((info.flags & usb::config::flag_flash_record) && !(info.flags & usb::config::flag_unsaved) &&
               info.flash_sequence == 1U, "saved state is clean");

        r = call(service, backend, usb::config::Opcode::Defaults);
        expect(r.status == 0U && std::memcmp(&backend.applied, &d, sizeof(d)) == 0, "defaults applied to RAM");
        expect(backend.stored.upsampler_mode == 1U, "defaults leave flash untouched");
        r = call(service, backend, usb::config::Opcode::Load);
        expect(r.status == 0U && std::memcmp(&backend.applied, &custom, sizeof(custom)) == 0, "load restores flash");

        MockBackend rebooted;
        rebooted.has_record = true;
        rebooted.stored = custom;
        rebooted.stored_sequence = 7U;
        app::ConfigService fresh;
        fresh.boot(rebooted);
        expect(std::memcmp(&rebooted.applied, &custom, sizeof(custom)) == 0 && fresh.flash_record_valid(),
               "boot applies the saved record");

        MockBackend no_flash;
        no_flash.flash_ok = false;
        app::ConfigService service2;
        service2.boot(no_flash);
        expect(call(service2, no_flash, usb::config::Opcode::Save).status ==
               static_cast<std::uint8_t>(usb::config::Status::FlashUnavailable), "save without ROM driver refused");

        std::uint8_t junk[64]{};
        expect(service.handle(junk, sizeof(junk), backend).status ==
               static_cast<std::uint8_t>(usb::config::Status::BadRequest), "non-config packet rejected");
    }
    std::cout << "Settings/DSP/config tests passed\n";
}
