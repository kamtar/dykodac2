#!/usr/bin/env python3
"""Measure response and known-reference reconstruction for the production FIR."""
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
    indices = np.arange(h.size, dtype=np.float64)
    return np.sum(h * np.exp(-1j * omega * indices)) / 4.0


def response_report(h: np.ndarray, source_rate: float) -> dict[str, object]:
    points = [0.0, 1_000.0, 10_000.0, 19_000.0, 20_000.0]
    delay = (h.size - 1.0) / 2.0
    results: dict[str, object] = {}
    for frequency in points:
        response = complex_response(h, frequency, source_rate)
        omega = 2.0 * np.pi * frequency / (4.0 * source_rate)
        corrected = response * np.exp(1j * omega * delay)
        error = corrected - 1.0
        results[f"{frequency:g}_hz"] = {
            "gain_db": float(20.0 * np.log10(max(abs(response), 1e-300))),
            "rms_sine_error": float(abs(error) / np.sqrt(2.0)),
            "peak_sine_error": float(abs(error)),
        }

    transition = {}
    for frequency in [20_000.0, 20_500.0, 21_000.0, 21_025.675,
                      21_500.0, 22_000.0, 22_050.0]:
        transition[f"{frequency:g}_hz"] = float(20.0 * np.log10(
            max(abs(complex_response(h, frequency, 44_100.0)), 1e-300)))

    image_frequency = source_rate - 20_000.0
    image = abs(complex_response(h, image_frequency, source_rate))
    wanted = abs(complex_response(h, 20_000.0, source_rate))
    results["20khz_first_image"] = {
        "frequency_hz": image_frequency,
        "relative_db": float(20.0 * np.log10(max(image / wanted, 1e-300))),
    }
    results["transition_44k1_db"] = transition
    return results


def reconstruction_report(h: np.ndarray, source_rate: float) -> dict[str, object]:
    sample_count = 8192
    source_index = np.arange(sample_count, dtype=np.float64)
    frequencies = [997.0, 7_001.0, 13_003.0, 19_000.0]
    amplitudes = [0.35, 0.25, 0.20, 0.15]
    source = sum(amplitude * np.sin(2.0 * np.pi * frequency * source_index / source_rate)
                 for frequency, amplitude in zip(frequencies, amplitudes))
    reconstructed = upfirdn(h, source, up=4)
    delay = (h.size - 1.0) / 2.0
    first = 4 * (generator.PHASE_TAPS - 1)
    last = 4 * sample_count
    output_index = np.arange(first, last, dtype=np.float64)
    reference_time = (output_index - delay) / (4.0 * source_rate)
    reference = sum(amplitude * np.sin(2.0 * np.pi * frequency * reference_time)
                    for frequency, amplitude in zip(frequencies, amplitudes))
    error = reconstructed[first:last] - reference
    per_phase = {}
    for phase in range(4):
        phase_error = error[phase::4]
        per_phase[str(phase)] = {
            "rms": float(np.sqrt(np.mean(phase_error * phase_error))),
            "peak": float(np.max(np.abs(phase_error))),
        }
    return {
        "signal": "997 + 7001 + 13003 + 19000 Hz multitone",
        "rms": float(np.sqrt(np.mean(error * error))),
        "peak": float(np.max(np.abs(error))),
        "fractional_positions": per_phase,
    }


def main() -> None:
    h = generator.load_generated_prototype(
        ROOT / "tests" / "host" / "fft_prototype_coefficients.hpp"
    ).astype(np.float64)
    report = {
        "method": "exact checked-in float32 prototype; binary64 analysis",
        "response": {
            "44100_to_176400": response_report(h, 44_100.0),
            "48000_to_192000": response_report(h, 48_000.0),
        },
        "known_high_rate_reconstruction": {
            "44100_to_176400": reconstruction_report(h, 44_100.0),
            "48000_to_192000": reconstruction_report(h, 48_000.0),
        },
    }
    destination = ROOT / "docs" / "fft-interpolator-verification.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
