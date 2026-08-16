#!/usr/bin/env python3
"""Generate the production 8192-tap exact-4x FIR and FFT partitions.

The prototype is evaluated on the 4x output grid in binary64, normalized so
each of its four polyphase branches has unity DC gain, and then converted to
binary32.  Runtime tables use the packed format consumed by CMSIS-DSP's
arm_rfft_fast_f32: DC, Nyquist, then interleaved real/imaginary bins.
"""
from __future__ import annotations

import json
import argparse
import re
from pathlib import Path

import numpy as np


PROTOTYPE_TAPS = 8192
PHASES = 4
PHASE_TAPS = PROTOTYPE_TAPS // PHASES
PARTITION_SIZE = 256
FFT_SIZE = 2 * PARTITION_SIZE
PARTITIONS = PHASE_TAPS // PARTITION_SIZE
KAISER_BETA = 18.0
# Cycles per source sample. On the 4x grid this is 0.1191875 cycles/sample.
# At 44.1 kHz the -6 dB midpoint is 21.025675 kHz, leaving a symmetric
# transition between the 20 kHz audio-band edge and source Nyquist.
CUTOFF_SOURCE = 0.47677267573696145
CUTOFF_OUTPUT = CUTOFF_SOURCE / PHASES


def design_prototype() -> np.ndarray:
    n = np.arange(PROTOTYPE_TAPS, dtype=np.float64)
    centre = (PROTOTYPE_TAPS - 1) / 2.0
    prototype = (2.0 * CUTOFF_OUTPUT *
                 np.sinc(2.0 * CUTOFF_OUTPUT * (n - centre)) *
                 np.kaiser(PROTOTYPE_TAPS, KAISER_BETA))

    # A 4x interpolation prototype must have total DC gain four. Normalizing
    # each phase separately makes constant input exactly constant at all four
    # fractional positions while preserving symmetry (phase 0 mirrors 3 and
    # phase 1 mirrors 2).
    for phase in range(PHASES):
        prototype[phase::PHASES] /= np.sum(prototype[phase::PHASES])
    # The checked-in binary32 prototype is the authoritative FIR. Spectra are
    # generated from these exact values (promoted back to binary64 for the
    # offline FFT), so direct and FFT tests never compare different filters.
    return prototype.astype(np.float32)


def load_header_prototype(path: Path) -> tuple[np.ndarray, int]:
    """Load a sequential float32 FIR header and its documented gain factor."""
    text = path.read_text(encoding="utf-8")
    count_match = re.search(r"#define\s+FIR_COEFFICIENT_COUNT\s+(\d+)u", text)
    phase_match = re.search(r"#define\s+FIR_INTERPOLATION_PHASES\s+(\d+)u", text)
    array_marker = "static const float fir_coefficients_fp32"
    if count_match is None or phase_match is None or array_marker not in text:
        raise ValueError(f"{path} is not a recognized FIR coefficient header")
    array_start = text.index("{", text.index(array_marker)) + 1
    array_end = text.index("};", array_start)
    values = np.array([
        float(value.rstrip("fF"))
        for value in re.findall(
            r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?[fF]?",
            text[array_start:array_end],
        )
    ], dtype=np.float32)
    declared_count = int(count_match.group(1))
    if values.size != declared_count or declared_count != PROTOTYPE_TAPS:
        raise ValueError(
            f"{path} contains {values.size} taps, expected {declared_count} and "
            f"project size {PROTOTYPE_TAPS}"
        )
    return values, int(phase_match.group(1))


def load_generated_prototype(path: Path) -> np.ndarray:
    """Load the effective float32 prototype emitted for host verification."""
    text = path.read_text(encoding="utf-8")
    marker = "prototype_coefficients{"
    if marker not in text:
        raise ValueError(f"{path} is not a generated prototype header")
    array_start = text.index("{", text.index(marker)) + 1
    array_end = text.index("};", array_start)
    values = np.array([
        float(value.rstrip("fF"))
        for value in re.findall(
            r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?[fF]?",
            text[array_start:array_end],
        )
    ], dtype=np.float32)
    if values.size != PROTOTYPE_TAPS:
        raise ValueError(
            f"{path} contains {values.size} taps, expected {PROTOTYPE_TAPS}"
        )
    return values


def cmsis_packed_rfft(values: np.ndarray) -> np.ndarray:
    spectrum = np.fft.rfft(values)
    packed = np.empty(FFT_SIZE, dtype=np.float32)
    packed[0] = np.float32(spectrum[0].real)
    packed[1] = np.float32(spectrum[-1].real)
    packed[2::2] = spectrum[1:-1].real.astype(np.float32)
    packed[3::2] = spectrum[1:-1].imag.astype(np.float32)
    return packed


def build_spectra(prototype: np.ndarray) -> np.ndarray:
    spectra = np.empty((PHASES, PARTITIONS, FFT_SIZE), dtype=np.float32)
    for phase in range(PHASES):
        branch = prototype[phase::PHASES]
        for partition in range(PARTITIONS):
            block = np.zeros(FFT_SIZE, dtype=np.float64)
            first = partition * PARTITION_SIZE
            block[:PARTITION_SIZE] = branch[first:first + PARTITION_SIZE].astype(np.float64)
            spectra[phase, partition] = cmsis_packed_rfft(block)
    return spectra


def float_literal(value: np.float32) -> str:
    text = format(float(value), ".9g")
    if "." not in text and "e" not in text:
        text += ".0"
    return text + "F"


def render_spectra(spectra: np.ndarray) -> str:
    lines = [
        "#pragma once", "", "#include <array>", "#include <cstddef>", "",
        "namespace audio::detail {",
        "// Layout: [phase][partition][CMSIS packed-real-FFT float].",
        "// Kept in XIP QSPI; the runtime consumes each phase sequentially and",
        "// shares every coefficient fetch between the left and right channels.",
        "__attribute__((section(\".fft_coefficients\"), aligned(32)))",
        f"inline const std::array<float, {PHASES}U * {PARTITIONS}U * {FFT_SIZE}U> "
        "fft_filter_spectra{",
    ]
    for phase in range(PHASES):
        lines.append(f"    // phase {phase}")
        for partition in range(PARTITIONS):
            lines.append(f"    // partition {partition}")
            row = spectra[phase, partition]
            for offset in range(0, FFT_SIZE, 8):
                lines.append("    " + ", ".join(float_literal(v) for v in row[offset:offset + 8]) + ",")
    lines.extend(["};", "} // namespace audio::detail", ""])
    return "\n".join(lines)


def render_prototype(prototype: np.ndarray) -> str:
    values = prototype
    lines = [
        "#pragma once", "", "#include <array>", "",
        "namespace audio::test_detail {",
        f"inline const std::array<float, {PROTOTYPE_TAPS}U> prototype_coefficients{{",
    ]
    for offset in range(0, PROTOTYPE_TAPS, 8):
        lines.append("    " + ", ".join(float_literal(v) for v in values[offset:offset + 8]) + ",")
    lines.extend(["};", "} // namespace audio::test_detail", ""])
    return "\n".join(lines)


def response_metrics(prototype: np.ndarray) -> dict[str, object]:
    fft_points = 1 << 22
    response = np.fft.rfft(prototype.astype(np.float64), fft_points)
    magnitude = np.abs(response) / PHASES
    frequency_output = np.fft.rfftfreq(fft_points, 1.0)

    def db_at_source_fraction(fraction: float) -> float:
        index = int(round((fraction / PHASES) * fft_points))
        return float(20.0 * np.log10(max(magnitude[index], 1e-300)))

    passband = frequency_output <= (20_000.0 / 176_400.0)
    stopband = frequency_output >= (22_050.0 / 176_400.0)
    passband_db = 20.0 * np.log10(np.maximum(magnitude[passband], 1e-300))
    stopband_db = 20.0 * np.log10(np.maximum(magnitude[stopband], 1e-300))
    return {
        "prototype_taps": PROTOTYPE_TAPS,
        "phase_taps": PHASE_TAPS,
        "phases": PHASES,
        "partition_size": PARTITION_SIZE,
        "fft_size": FFT_SIZE,
        "partitions_per_phase": PARTITIONS,
        "kaiser_beta": KAISER_BETA,
        "cutoff_cycles_per_source_sample": CUTOFF_SOURCE,
        "cutoff_cycles_per_output_sample": CUTOFF_OUTPUT,
        "dc_gain": float(np.sum(prototype) / PHASES),
        "phase_dc_gains": [float(np.sum(prototype[p::PHASES])) for p in range(PHASES)],
        "symmetry_peak_error": float(np.max(np.abs(prototype - prototype[::-1]))),
        "passband_ripple_db_0_to_20khz_at_44k1": float(np.max(passband_db) - np.min(passband_db)),
        "response_db_at_source_fraction": {
            "10khz_44k1": db_at_source_fraction(10_000.0 / 44_100.0),
            "19khz_44k1": db_at_source_fraction(19_000.0 / 44_100.0),
            "20khz_44k1": db_at_source_fraction(20_000.0 / 44_100.0),
            "nyquist_44k1": db_at_source_fraction(0.5),
        },
        "worst_stopband_db_from_22k05": float(np.max(stopband_db)),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input",
        type=Path,
        help="sequential FIR header; its FIR_INTERPOLATION_PHASES gain is applied",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.input is None:
        prototype = design_prototype()
    else:
        raw_prototype, interpolation_phases = load_header_prototype(args.input)
        if interpolation_phases != PHASES:
            raise ValueError(
                f"input declares {interpolation_phases} interpolation phases, "
                f"but this firmware is configured for {PHASES}x"
            )
        # The supplied header is the unscaled sequential FIR.  The current
        # polyphase path is equivalent to zero-stuff interpolation, so apply
        # the documented interpolation gain before partitioning its branches.
        prototype = (raw_prototype * np.float32(interpolation_phases)).astype(np.float32)
    spectra = build_spectra(prototype)
    (root / "audio" / "fft_filter_spectra.hpp").write_text(
        render_spectra(spectra), encoding="utf-8", newline="\n")
    (root / "tests" / "host" / "fft_prototype_coefficients.hpp").write_text(
        render_prototype(prototype), encoding="utf-8", newline="\n")
    report = response_metrics(prototype)
    (root / "tests" / "host" / "fft_filter_metrics.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
