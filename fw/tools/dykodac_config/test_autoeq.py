import os
import tempfile
import unittest

from autoeq import (
    CH_BOTH, CH_LEFT, CH_RIGHT,
    FILTER_HIGH_PASS, FILTER_HIGH_SHELF, FILTER_LOW_PASS, FILTER_LOW_SHELF,
    FILTER_NOTCH, FILTER_OFF, FILTER_PEAKING,
    Band, format_parametric_eq, load_parametric_eq, parse_parametric_eq,
)

TYPICAL = """\
# AutoEQ test profile
Preamp: -6.2 dB
Filter 1: ON LSC Fc 105 Hz Gain 6.5 dB Q 0.70
Filter 2: ON PK Fc 250 Hz Gain -2.1 dB Q 1.20
Filter 3: ON PK Fc 630 Hz Gain 1.8 dB Q 0.90
Filter 4: ON PK Fc 1000 Hz Gain -3.4 dB Q 2.10
Filter 5: ON PK Fc 2500 Hz Gain 2.2 dB Q 1.40
Filter 6: ON PK Fc 4000 Hz Gain -1.0 dB Q 3.00
Filter 7: ON PK Fc 6300 Hz Gain 0.7 dB Q 1.10
Filter 8: ON PK Fc 8000 Hz Gain -4.6 dB Q 2.50
Filter 9: ON PK Fc 12000 Hz Gain 1.5 dB Q 0.80
Filter 10: ON HSC Fc 10000 Hz Gain 3.0 dB Q 0.70
"""


class ParseTests(unittest.TestCase):
    def test_typical_autoeq_file(self):
        eq = parse_parametric_eq(TYPICAL)
        self.assertEqual(eq.warnings, [])
        self.assertAlmostEqual(eq.preamp_db, -6.2)
        self.assertEqual(len(eq.bands), 10)
        self.assertEqual(eq.bands[0].type, FILTER_LOW_SHELF)
        self.assertEqual(eq.bands[0].frequency_hz, 105)
        self.assertAlmostEqual(eq.bands[0].gain_db, 6.5)
        self.assertAlmostEqual(eq.bands[0].q, 0.70)
        self.assertEqual(eq.bands[1].type, FILTER_PEAKING)
        self.assertAlmostEqual(eq.bands[1].gain_db, -2.1)
        self.assertEqual(eq.bands[9].type, FILTER_HIGH_SHELF)
        self.assertEqual(eq.bands[9].frequency_hz, 10000)
        for b in eq.bands:
            self.assertEqual(b.channels, CH_BOTH)

    def test_off_lines_skipped(self):
        eq = parse_parametric_eq(
            "Filter 1: OFF PK Fc 500 Hz Gain 1 dB Q 1\n"
            "Filter 2: ON PK Fc 800 Hz Gain 2 dB Q 1\n"
            "Filter: OFF PK Fc 900 Hz Gain 1 dB Q 1\n")
        self.assertEqual(eq.warnings, [])
        self.assertEqual(len(eq.bands), 1)
        self.assertEqual(eq.bands[0].frequency_hz, 800)

    def test_channel_switching(self):
        eq = parse_parametric_eq(
            "Channel: L\n"
            "Filter 1: ON PK Fc 100 Hz Gain 1 dB Q 1\n"
            "Channel: R\n"
            "Filter 2: ON PK Fc 200 Hz Gain 1 dB Q 1\n"
            "Channel: all\n"
            "Filter 3: ON PK Fc 300 Hz Gain 1 dB Q 1\n"
            "Channel: L R\n"
            "Filter 4: ON PK Fc 400 Hz Gain 1 dB Q 1\n")
        self.assertEqual(eq.warnings, [])
        self.assertEqual([b.channels for b in eq.bands],
                         [CH_LEFT, CH_RIGHT, CH_BOTH, CH_BOTH])

    def test_truncation_warning(self):
        lines = [f"Filter {i}: ON PK Fc {100 + i} Hz Gain 1 dB Q 1" for i in range(1, 13)]
        eq = parse_parametric_eq("\n".join(lines))
        self.assertEqual(len(eq.bands), 10)
        self.assertEqual(eq.bands[-1].frequency_hz, 110)
        self.assertEqual(len(eq.warnings), 1)
        self.assertIn("12 filters", eq.warnings[0])
        self.assertIn("2 dropped", eq.warnings[0])

    def test_max_bands_argument(self):
        lines = [f"Filter {i}: ON PK Fc 100 Hz Gain 1 dB Q 1" for i in range(1, 4)]
        eq = parse_parametric_eq("\n".join(lines), max_bands=2)
        self.assertEqual(len(eq.bands), 2)
        self.assertIn("1 dropped", eq.warnings[0])

    def test_clamping_warnings(self):
        eq = parse_parametric_eq(
            "Filter 1: ON PK Fc 5 Hz Gain -25 dB Q 30\n"
            "Filter 2: ON PK Fc 30000 Hz Gain 25 dB Q 0.05\n")
        b1, b2 = eq.bands
        self.assertEqual(b1.frequency_hz, 10)
        self.assertEqual(b1.gain_db, -20)
        self.assertEqual(b1.q, 20)
        self.assertEqual(b2.frequency_hz, 20000)
        self.assertEqual(b2.gain_db, 20)
        self.assertAlmostEqual(b2.q, 0.1)
        self.assertEqual(len(eq.warnings), 6)
        self.assertIn("line 1", eq.warnings[0])
        self.assertIn("line 2", eq.warnings[-1])

    def test_no_clamp_warning_within_range(self):
        eq = parse_parametric_eq("Filter 1: ON PK Fc 10 Hz Gain -20 dB Q 20\n")
        self.assertEqual(eq.warnings, [])
        self.assertEqual(eq.bands[0].frequency_hz, 10)

    def test_k_frequency_suffix(self):
        eq = parse_parametric_eq(
            "Filter 1: ON PK Fc 1k Gain 1 dB Q 1\n"
            "Filter 2: ON PK Fc 1.5k Gain 1 dB Q 1\n"
            "Filter 3: ON PK Fc 1,000 Hz Gain 1 dB Q 1\n")
        self.assertEqual(eq.bands[0].frequency_hz, 1000)
        self.assertEqual(eq.bands[1].frequency_hz, 1500)
        self.assertEqual(eq.bands[2].frequency_hz, 1000)
        self.assertEqual(eq.warnings, [])

    def test_missing_q_uses_default(self):
        eq = parse_parametric_eq("Filter 1: ON PK Fc 1000 Hz Gain 3 dB\n")
        self.assertAlmostEqual(eq.bands[0].q, 0.7071)
        self.assertEqual(eq.warnings, [])

    def test_shelf_with_bw_but_no_q_warns(self):
        eq = parse_parametric_eq("Filter 1: ON LS Fc 100 Hz Gain 3 dB BW Oct 1.5\n")
        self.assertEqual(eq.bands[0].type, FILTER_LOW_SHELF)
        self.assertAlmostEqual(eq.bands[0].q, 0.7071)
        self.assertEqual(len(eq.warnings), 1)
        self.assertIn("line 1", eq.warnings[0])

    def test_unknown_type_skipped_with_line_number(self):
        eq = parse_parametric_eq(
            "Preamp: -1 dB\n"
            "Filter 1: ON PK Fc 100 Hz Gain 1 dB Q 1\n"
            "Filter 2: ON XYZ Fc 200 Hz Gain 1 dB Q 1\n")
        self.assertEqual(len(eq.bands), 1)
        self.assertEqual(len(eq.warnings), 1)
        self.assertIn("line 3", eq.warnings[0])

    def test_all_type_tokens(self):
        text = "\n".join([
            "Filter 1: ON PEQ Fc 100 Hz Gain 1 dB Q 1",
            "Filter 2: ON LS Fc 100 Hz Gain 1 dB Q 1",
            "Filter 3: ON LSQ Fc 100 Hz Gain 1 dB Q 1",
            "Filter 4: ON HS Fc 100 Hz Gain 1 dB Q 1",
            "Filter 5: ON HSQ Fc 100 Hz Gain 1 dB Q 1",
            "Filter 6: ON LP Fc 100 Hz Q 1",
            "Filter 7: ON LPQ Fc 100 Hz Q 1",
            "Filter 8: ON HP Fc 100 Hz Q 1",
            "Filter 9: ON HPQ Fc 100 Hz Q 1",
            "Filter 10: ON NO Fc 100 Hz Gain 1 dB Q 1",
        ])
        eq = parse_parametric_eq(text)
        self.assertEqual([b.type for b in eq.bands], [
            FILTER_PEAKING, FILTER_LOW_SHELF, FILTER_LOW_SHELF, FILTER_HIGH_SHELF,
            FILTER_HIGH_SHELF, FILTER_LOW_PASS, FILTER_LOW_PASS, FILTER_HIGH_PASS,
            FILTER_HIGH_PASS, FILTER_NOTCH])

    def test_keywords_case_insensitive(self):
        eq = parse_parametric_eq("preamp:   -2 DB\nfilter 1: on pk fc 100 hz gain 1 db q 1\n")
        self.assertEqual(eq.preamp_db, -2)
        self.assertEqual(eq.bands[0].type, FILTER_PEAKING)

    def test_graphiceq_and_include_warn(self):
        eq = parse_parametric_eq(
            "Include: other.txt\n"
            "GraphicEQ: 20 -3; 1000 2\n"
            "Device: something\n"
            "Filter 1: ON PK Fc 100 Hz Gain 1 dB Q 1\n")
        self.assertEqual(len(eq.bands), 1)
        self.assertEqual(len(eq.warnings), 2)
        self.assertIn("line 1", eq.warnings[0])
        self.assertIn("line 2", eq.warnings[1])

    def test_default_preamp_and_empty_input(self):
        eq = parse_parametric_eq("")
        self.assertEqual(eq.preamp_db, 0.0)
        self.assertEqual(eq.bands, [])
        self.assertEqual(eq.warnings, [])

    def test_load_file_replaces_bad_bytes(self):
        fd, path = tempfile.mkstemp(suffix=".txt")
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(b"Preamp: -1.5 dB\n# caf\xff\nFilter 1: ON PK Fc 100 Hz Gain 1 dB Q 1\n")
            eq = load_parametric_eq(path)
        finally:
            os.remove(path)
        self.assertEqual(eq.preamp_db, -1.5)
        self.assertEqual(len(eq.bands), 1)


class FormatTests(unittest.TestCase):
    def test_round_trip(self):
        bands = [
            Band(FILTER_LOW_SHELF, CH_BOTH, 105.0, 6.5, 0.7),
            Band(FILTER_PEAKING, CH_LEFT, 1234.5678, -3.25, 0.123456789),
            Band(FILTER_OFF, CH_BOTH, 500.0, 1.0, 1.0),
            Band(FILTER_PEAKING, CH_RIGHT, 9876.54321, 19.999999, 19.5),
            Band(FILTER_HIGH_SHELF, CH_BOTH, 10000.0, -20.0, 0.7071),
            Band(FILTER_LOW_PASS, CH_BOTH, 15000.0, 0.0, 0.5),
            Band(FILTER_HIGH_PASS, CH_BOTH, 30.25, 0.0, 1.2345678),
            Band(FILTER_NOTCH, CH_BOTH, 60.0, 0.0, 8.0),
        ]
        text = format_parametric_eq(bands, preamp_db=-3.14159265)
        eq = parse_parametric_eq(text, max_bands=10)
        self.assertEqual(eq.warnings, [])
        self.assertAlmostEqual(eq.preamp_db, -3.14159265, places=6)

        expected = [b for b in bands if b.type != FILTER_OFF]
        self.assertEqual(len(eq.bands), len(expected))
        for got, want in zip(eq.bands, expected):
            self.assertEqual(got.type, want.type)
            self.assertEqual(got.channels, want.channels)
            self.assertAlmostEqual(got.frequency_hz, want.frequency_hz, delta=1e-6)
            self.assertAlmostEqual(got.gain_db, want.gain_db, delta=1e-6)
            self.assertAlmostEqual(got.q, want.q, delta=1e-6)

    def test_format_skips_off_and_writes_channel_lines(self):
        text = format_parametric_eq([
            Band(FILTER_OFF),
            Band(FILTER_PEAKING, CH_LEFT, 100.0, 1.0, 1.0),
            Band(FILTER_PEAKING, CH_BOTH, 200.0, 1.0, 1.0),
        ])
        lines = [ln for ln in text.splitlines() if ln.strip()]
        self.assertEqual(lines[0], "Channel: L")
        self.assertEqual(lines[1].split(":")[0], "Filter 1")
        self.assertEqual(lines[2], "Channel: all")
        self.assertEqual(lines[3].split(":")[0], "Filter 2")
        self.assertEqual(len(lines), 4)
        self.assertNotIn("Preamp", text)

    def test_format_without_preamp(self):
        text = format_parametric_eq([Band(FILTER_PEAKING, CH_BOTH, 100.0, 1.0, 1.0)])
        self.assertNotIn("Preamp", text)
        self.assertEqual(parse_parametric_eq(text).preamp_db, 0.0)


if __name__ == "__main__":
    unittest.main()
