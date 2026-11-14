#!/usr/bin/env python3
"""Measure response, time-domain behaviour, and known-reference reconstruction
of the production two-stage interpolator (binary32 coefficients, binary64
analysis), with and without the modelled CS4398 pass-band response."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
from scipy.signal import upfirdn

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools"))
import generate_polyphase_coefficients as generator  # noqa: E402


def complex_response(h: np.ndarray, frequency_hz: float, source_rate: float) -> complex:
    omega = 2.0 * np.pi * frequency_hz / (4.0 * source_rate)
    return np.sum(h * np.exp(-1j * omega * np.arange(h.size))) / 4.0


def db(value: float) -> float:
    return float(20.0 * np.log10(max(abs(value), 1e-300)))


def response_report(h: np.ndarray, source_rate: float) -> dict[str, object]:
    results: dict[str, object] = {}
    for frequency in [1_000.0, 5_000.0, 10_000.0, 15_000.0, 19_000.0, 20_000.0]:
        fir = db(complex_response(h, frequency, source_rate))
        dac = float(generator.dac_response_db(np.array([frequency / (4.0 * source_rate)]))[0])
        results[f"{frequency:g}_hz"] = {
            "fir_db": fir, "dac_model_db": dac, "fir_plus_dac_db": fir + dac}
    nyquist = source_rate / 2.0
    results["transition_db"] = {
        f"{frequency:g}_hz": db(complex_response(h, frequency, source_rate))
        for frequency in [nyquist - 2_000.0, nyquist - 1_000.0, nyquist - 500.0,
                          nyquist - 250.0, nyquist - 100.0, nyquist]}
    wanted = abs(complex_response(h, 20_000.0, source_rate))
    results["images_of_20khz_relative_db"] = {
        f"{image:g}_hz": db(abs(complex_response(h, image, source_rate)) / wanted)
        for image in [source_rate - 20_000.0, source_rate + 20_000.0,
                      2.0 * source_rate - 20_000.0]}
    return results


def time_domain_report(h: np.ndarray, source_rate: float) -> dict[str, object]:
    envelope = np.abs(h) / np.max(np.abs(h))
    peak = int(np.argmax(envelope))
    out_rate = 4.0 * source_rate

    def span_ms(level_db: float) -> float:
        above = np.where(envelope > 10.0 ** (level_db / 20.0))[0]
        return float((peak - above[0]) / out_rate * 1e3)

    worst_gain = max(np.sum(np.abs(h[p::4])) for p in range(4))
    return {
        "group_delay_ms": (h.size - 1) / 2.0 / out_rate * 1e3,
        "pre_ringing_ms_to_minus_60_db": span_ms(-60.0),
        "pre_ringing_ms_to_minus_100_db": span_ms(-100.0),
        "symmetric": bool(np.max(np.abs(h - h[::-1])) < 1e-12),
        "worst_case_peak_gain_db": db(worst_gain),
    }


def intersample_report(h: np.ndarray) -> dict[str, float]:
    n = np.arange(20_000)
    cases = {
        "fs_over_4_sine_45deg": np.sin(2.0 * np.pi * n / 4.0 + np.pi / 4.0),
        "clipped_two_tone": np.clip(4.0 * np.sin(2.0 * np.pi * 3000.0 * n / 44100.0) +
                                    2.0 * np.sin(2.0 * np.pi * 7919.0 * n / 44100.0), -1.0, 1.0),
        "square_1khz": np.sign(np.sin(2.0 * np.pi * 1000.0 * n / 44100.0 + 0.1)),
    }
    report = {}
    for name, source in cases.items():
        source = source / np.max(np.abs(source))
        out = upfirdn(h, source, up=4)[h.size:-h.size]
        report[name + "_true_peak_dbfs"] = db(np.max(np.abs(out)))
    return report


def reconstruction_report(h: np.ndarray, source_rate: float) -> dict[str, object]:
    sample_count = 8192
    index = np.arange(sample_count, dtype=np.float64)
    tones = [(997.0, 0.35), (7_001.0, 0.25), (13_003.0, 0.20), (19_000.0, 0.15)]
    source = sum(a * np.sin(2.0 * np.pi * f * index / source_rate) for f, a in tones)
    reconstructed = upfirdn(h, source, up=4)
    delay = (h.size - 1.0) / 2.0
    first, last = h.size, 4 * sample_count
    time = (np.arange(first, last, dtype=np.float64) - delay) / (4.0 * source_rate)
    # The FIR deliberately carries the inverse DAC response, so the reference
    # for "FIR alone" applies that same gain per tone; "FIR + DAC" is the
    # plain original waveform.
    def gain(frequency):
        return 10.0 ** (-float(generator.dac_response_db(
            np.array([frequency / (4.0 * source_rate)]))[0]) / 20.0)
    reference = sum(a * gain(f) * np.sin(2.0 * np.pi * f * time) for f, a in tones)
    error = reconstructed[first:last] - reference
    return {
        "signal": "997 + 7001 + 13003 + 19000 Hz multitone",
        "rms_error": float(np.sqrt(np.mean(error * error))),
        "peak_error": float(np.max(np.abs(error))),
    }


def main() -> None:
    stage1, stage2, h, cutoff = generator.design()
    report = {
        "method": "binary32 stage coefficients; composite and analysis in binary64",
        "design": generator.response_metrics(stage1, stage2, h, cutoff),
        "intersample_peaks": intersample_report(h),
    }
    for rate in (44_100.0, 48_000.0):
        report[f"{int(rate)}_to_{int(4 * rate)}"] = {
            "response": response_report(h, rate),
            "time_domain": time_domain_report(h, rate),
            "known_reference_reconstruction": reconstruction_report(h, rate),
        }
    destination = ROOT / "docs" / "fft-interpolator-verification.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
