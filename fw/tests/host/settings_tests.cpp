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
