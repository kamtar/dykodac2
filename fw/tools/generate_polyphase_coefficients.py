#!/usr/bin/env python3
"""Design and emit the two-stage exact-4x interpolator coefficients.

Stage 1 (source rate -> 2x): long linear-phase Kaiser-windowed low-pass that
does all the sharp work.  Its pass-band also carries the inverse of the CS4398
quad-speed pass-band deviation, so the FIR + DAC cascade is flat.  It is
evaluated at run time by partitioned FFT convolution, so it is emitted as
CMSIS-DSP packed real-FFT partitions.

Stage 2 (2x -> 4x): short true half-band Kaiser FIR evaluated in the time
domain.  One of its two phases is a pure delay, so stage-1 samples pass
through unchanged and only the in-between samples are computed.

Everything is designed in binary64, rounded to binary32, and all reported
metrics are measured on the rounded binary32 coefficients.  See
docs/filter-and-volume-design.md for the reasoning behind every constant.
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------- stage 1 ---
STAGE1_PHASES = 2
# Taps on the 2x grid. Must be a multiple of 2 * PARTITION_SIZE. 1024 keeps
# the pass-band flat to ~21.2 kHz (44.1 kHz source); 512 is the short-ringing
# alternative (flat to 20 kHz, about -137 dB) - change it here and rebuild.
STAGE1_TAPS = 1024
STAGE1_BETA = 15.0
# Width (cycles per source sample) of the Kaiser transition for the chosen
# length/beta, measured from the -6 dB point to the first point that stays
# below the stop-band target. The cutoff is placed so the stop band starts at
# source Nyquist exactly; nothing above fs/2 survives to be imaged.
STOPBAND_TARGET_DB = -130.0
STOPBAND_EDGE_SOURCE = 0.5
PARTITION_SIZE = 256
FFT_SIZE = 2 * PARTITION_SIZE
STAGE1_PHASE_TAPS = STAGE1_TAPS // STAGE1_PHASES
STAGE1_PARTITIONS = STAGE1_PHASE_TAPS // PARTITION_SIZE

# ---------------------------------------------------------------- stage 2 ---
STAGE2_TAPS = 63          # 4k+3 so the half-band has non-zero end taps
STAGE2_BETA = 15.0

# ------------------------------------------------------- DAC compensation ---
# CS4398 quad-speed, fast roll-off, combined digital + on-chip analog
# pass-band response, read from datasheet DS568F3 figure 39 ("Quad-Speed
# (fast) Passband Ripple").  Frequency is normalised to the DAC sample rate,
# so the same table is valid for 176.4 and 192 kHz.  Reading accuracy is about
# +/-0.003 dB; the correction itself is only ~0.01 dB.  Set to None to
# disable.  Replace with a measurement of the real board when one exists
# (that would also fold in the OPA1612/TPA6120 analog stage).
DAC_RESPONSE_DB = [
    (0.000, 0.000),
    (0.025, 0.003),
    (0.050, 0.009),
    (0.065, 0.010),
    (0.080, 0.008),
    (0.100, 0.001),
    (0.105, 0.000),
    (0.115, -0.008),
    (0.125, -0.020),
    (0.150, -0.065),
]


def dac_response_db(f_dac: np.ndarray) -> np.ndarray:
    """DAC pass-band deviation in dB at frequencies normalised to the DAC rate."""
    if DAC_RESPONSE_DB is None:
        return np.zeros_like(f_dac)
    from scipy.interpolate import PchipInterpolator
    points = np.array(DAC_RESPONSE_DB)
    # Mirror about DC: a magnitude response is even, so the model must have
    # zero slope at 0 Hz (a kink there would smear through the window).
    x = np.concatenate((-points[:0:-1, 0], points[:, 0]))
    y = np.concatenate((points[:0:-1, 1], points[:, 1]))
    return PchipInterpolator(x, y)(np.clip(np.abs(f_dac), 0.0, points[-1, 0]))


def kaiser_lowpass(taps: int, beta: float, cutoff: float, phases: int,
                   compensation=None) -> np.ndarray:
    """Windowed low-pass on its own output grid (cutoff in cycles/sample).

    With `compensation(f)` (linear gain vs cycles/sample) the ideal response
    inside the pass band is that curve instead of 1, obtained by numerically
    inverting the desired zero-phase response.
    """
    n = np.arange(taps, dtype=np.float64) - (taps - 1) / 2.0
    if compensation is None:
        ideal = 2.0 * cutoff * np.sinc(2.0 * cutoff * n)
    else:
        grid = (np.arange(1 << 15, dtype=np.float64) + 0.5) / (1 << 15) * cutoff
        weight = compensation(grid) * (cutoff / (1 << 15))
        ideal = 2.0 * (np.cos(2.0 * np.pi * np.outer(n, grid)) @ weight)
    prototype = ideal * np.kaiser(taps, beta)
    # Interpolation gain equals the phase count; normalising each branch makes
    # DC exactly constant at every fractional position.
    for phase in range(phases):
        prototype[phase::phases] /= np.sum(prototype[phase::phases])
    return prototype


def response_db(h: np.ndarray, gain: float, points: int = 1 << 21) -> np.ndarray:
    return 20.0 * np.log10(np.maximum(np.abs(np.fft.rfft(h, points)) / gain, 1e-300))


def stage1_compensation(f_stage1: np.ndarray) -> np.ndarray:
    # Stage 1 runs at half the DAC rate.
    return 10.0 ** (-dac_response_db(f_stage1 / 2.0) / 20.0)


def design_stage1() -> tuple[np.ndarray, float]:
    """Place the cutoff as high as possible with the stop band met at fs/2."""
    points = 1 << 21
    freq_source = np.fft.rfftfreq(points, 1.0) * STAGE1_PHASES
    low, high = 0.40, 0.50
    best = None
    for _ in range(40):
        mid = 0.5 * (low + high)
        candidate = kaiser_lowpass(STAGE1_TAPS, STAGE1_BETA, mid / STAGE1_PHASES,
                                   STAGE1_PHASES, stage1_compensation).astype(np.float32)
        response = response_db(candidate.astype(np.float64), STAGE1_PHASES, points)
        # 10 dB design margin over the requirement.
        if np.max(response[freq_source >= STOPBAND_EDGE_SOURCE]) <= STOPBAND_TARGET_DB - 10.0:
            best, low = (candidate, mid), mid
        else:
            high = mid
    if best is None:
        raise RuntimeError("stage 1 cannot meet the stop-band target")
    return best


def design_stage2() -> np.ndarray:
    prototype = kaiser_lowpass(STAGE2_TAPS, STAGE2_BETA, 0.25, 2)
    centre = (STAGE2_TAPS - 1) // 2
    # True half-band: every second tap away from the centre is exactly zero
    # and the centre tap is exactly one (pure-delay phase).
    for index in range(STAGE2_TAPS):
        if index != centre and (index - centre) % 2 == 0:
            prototype[index] = 0.0
    prototype[centre] = 1.0
    prototype[(centre + 1) % 2::2] /= np.sum(prototype[(centre + 1) % 2::2])
    return prototype.astype(np.float32)


def composite(stage1: np.ndarray, stage2: np.ndarray) -> np.ndarray:
    """Equivalent single 4x prototype (binary64) of the two binary32 stages."""
    stuffed = np.zeros(2 * stage1.size - 1, dtype=np.float64)
    stuffed[::2] = stage1.astype(np.float64)
    return np.convolve(stuffed, stage2.astype(np.float64))


def cmsis_packed_rfft(values: np.ndarray) -> np.ndarray:
    spectrum = np.fft.rfft(values)
    packed = np.empty(FFT_SIZE, dtype=np.float32)
    packed[0] = np.float32(spectrum[0].real)
    packed[1] = np.float32(spectrum[-1].real)
    packed[2::2] = spectrum[1:-1].real.astype(np.float32)
    packed[3::2] = spectrum[1:-1].imag.astype(np.float32)
    return packed


def build_spectra(stage1: np.ndarray) -> np.ndarray:
    spectra = np.empty((STAGE1_PHASES, STAGE1_PARTITIONS, FFT_SIZE), dtype=np.float32)
    for phase in range(STAGE1_PHASES):
        branch = stage1[phase::STAGE1_PHASES]
        for partition in range(STAGE1_PARTITIONS):
            block = np.zeros(FFT_SIZE, dtype=np.float64)
            first = partition * PARTITION_SIZE
            block[:PARTITION_SIZE] = branch[first:first + PARTITION_SIZE].astype(np.float64)
            spectra[phase, partition] = cmsis_packed_rfft(block)
    return spectra


def float_literal(value) -> str:
    text = format(float(value), ".9g")
    if "." not in text and "e" not in text and "inf" not in text:
        text += ".0"
    return text + "F"


def rows(values, literal, per_row=8):
    return ["    " + ", ".join(literal(v) for v in values[i:i + per_row]) + ","
            for i in range(0, len(values), per_row)]


def render_spectra(spectra: np.ndarray) -> str:
    lines = [
        "#pragma once", "", "// Generated by tools/generate_polyphase_coefficients.py - do not edit.",
        "", "#include <array>", "#include <cstddef>", "",
        "namespace audio::detail {",
        f"inline constexpr std::size_t stage1_phases = {STAGE1_PHASES}U;",
        f"inline constexpr std::size_t stage1_prototype_taps = {STAGE1_TAPS}U;",
        f"inline constexpr std::size_t stage1_partition_size = {PARTITION_SIZE}U;",
        f"inline constexpr std::size_t stage1_fft_size = {FFT_SIZE}U;",
        f"inline constexpr std::size_t stage1_partitions = {STAGE1_PARTITIONS}U;",
        "// Layout: [phase][partition][CMSIS packed-real-FFT float].",
        "// Kept in XIP QSPI; the runtime consumes each phase sequentially and",
        "// shares every coefficient fetch between the left and right channels.",
        "__attribute__((section(\".fft_coefficients\"), aligned(32)))",
        f"inline const std::array<float, {STAGE1_PHASES}U * {STAGE1_PARTITIONS}U * {FFT_SIZE}U> "
        "fft_filter_spectra{",
    ]
    for phase in range(STAGE1_PHASES):
        lines.append(f"    // phase {phase}")
        for partition in range(STAGE1_PARTITIONS):
            lines.append(f"    // partition {partition}")
            lines.extend(rows(spectra[phase, partition], float_literal))
    lines.extend(["};", "} // namespace audio::detail", ""])
    return "\n".join(lines)


def render_halfband(stage2: np.ndarray) -> str:
    centre = (STAGE2_TAPS - 1) // 2
    branch = stage2[(centre + 1) % 2::2]
    lines = [
        "#pragma once", "", "// Generated by tools/generate_polyphase_coefficients.py - do not edit.",
        "", "#include <array>", "#include <cstddef>", "",
        "namespace audio::detail {",
        "// Stage 2: 2x half-band interpolator on the stage-1 stream u[m].",
        "//   y[2m]   = sum_j halfband_branch[j] * u[m - j]",
        "//   y[2m+1] = u[m - halfband_delay_frames]          (exact pass-through)",
        f"inline constexpr std::size_t halfband_prototype_taps = {STAGE2_TAPS}U;",
        f"inline constexpr std::size_t halfband_branch_taps = {branch.size}U;",
        f"inline constexpr std::size_t halfband_delay_frames = {centre // 2}U;",
        f"inline constexpr std::array<float, {branch.size}U> halfband_branch{{",
    ]
    lines.extend(rows(branch, float_literal))
    lines.extend(["};", "} // namespace audio::detail", ""])
    return "\n".join(lines)


def render_test_prototypes(stage1, stage2, comp) -> str:
    lines = [
        "#pragma once", "", "// Generated by tools/generate_polyphase_coefficients.py - do not edit.",
        "", "#include <array>", "",
        "namespace audio::test_detail {",
        "// Stage 1 on the 2x grid (two phases, each with unity DC gain).",
        f"inline const std::array<float, {stage1.size}U> stage1_coefficients{{",
    ]
    lines.extend(rows(stage1, float_literal))
    lines.extend(["};", "// Stage 2 half-band on the 4x grid (gain 2).",
                  f"inline const std::array<float, {stage2.size}U> stage2_coefficients{{"])
    lines.extend(rows(stage2, float_literal))
    lines.extend(["};",
                  "// Equivalent single 4x prototype: stage 1 zero-stuffed by two, convolved",
                  "// with stage 2, computed in binary64 from the binary32 stage values.",
                  "// output[n] = sum_k composite[k] * zero_stuffed_by_4(input)[n - k].",
                  f"inline const std::array<double, {comp.size}U> composite_coefficients{{"])
    lines.extend(rows(comp, lambda v: format(float(v), ".17g"), 4))
    lines.extend(["};", "} // namespace audio::test_detail", ""])
    return "\n".join(lines)


def response_metrics(stage1, stage2, comp, cutoff_source) -> dict[str, object]:
    points = 1 << 22
    magnitude_db = response_db(comp, 4.0, points)
    freq_source = np.fft.rfftfreq(points, 1.0) * 4.0
    dac_db = dac_response_db(freq_source / 4.0)

    def first_below(level):
        return float(freq_source[np.where(magnitude_db < level)[0][0]])

    def band(lo, hi):
        mask = (freq_source >= lo) & (freq_source < hi)
        return float(np.max(magnitude_db[mask]))

    report: dict[str, object] = {
        "stage1": {"taps_on_2x_grid": STAGE1_TAPS, "kaiser_beta": STAGE1_BETA,
                   "cutoff_cycles_per_source_sample": cutoff_source,
                   "partitions_per_phase": STAGE1_PARTITIONS, "fft_size": FFT_SIZE,
                   "phase_dc_gains": [float(np.sum(stage1[p::2].astype(np.float64))) for p in range(2)],
                   "symmetry_peak_error": float(np.max(np.abs(stage1 - stage1[::-1])))},
        "stage2": {"taps_on_4x_grid": STAGE2_TAPS, "kaiser_beta": STAGE2_BETA,
                   "nonzero_taps": int(np.count_nonzero(stage2))},
        "dac_compensation": "CS4398 quad-speed fast roll-off, DS568F3 figure 39"
                            if DAC_RESPONSE_DB is not None else "disabled",
        "composite_taps_on_4x_grid": int(comp.size),
        "group_delay_output_samples": (comp.size - 1) / 2.0,
        "edges_cycles_per_source_sample": {
            "passband_edge_minus_0p03_db": first_below(-0.03), "minus_6_db": first_below(-6.0),
            "minus_100_db": first_below(-100.0), "minus_130_db": first_below(-130.0)},
        "worst_stopband_db": {
            "from_source_nyquist_to_output_nyquist": band(0.5, 2.0001),
            "first_image_0p5_to_1p5": band(0.5, 1.5),
            "stage2_image_1p5_to_2p0": band(1.5, 2.0001)},
    }
    for rate in (44_100.0, 48_000.0):
        passband = freq_source * rate <= 20_000.0
        fir = magnitude_db[passband]
        cascade = fir + dac_db[passband]
        report[f"source_{int(rate)}"] = {
            "fir_deviation_db_0_to_20khz": [float(fir.min()), float(fir.max())],
            "fir_plus_dac_deviation_db_0_to_20khz": [float(cascade.min()), float(cascade.max())],
            "dac_alone_deviation_db_0_to_20khz": [float(dac_db[passband].min()),
                                                  float(dac_db[passband].max())],
            "passband_edge_minus_0p03_db_hz": first_below(-0.03) * rate,
            "minus_130_db_edge_hz": first_below(-130.0) * rate,
            "group_delay_ms": (comp.size - 1) / 2.0 / (4.0 * rate) * 1e3,
        }
    return report


def design():
    stage1, cutoff_source = design_stage1()
    stage2 = design_stage2()
    return stage1, stage2, composite(stage1, stage2), cutoff_source


def main() -> None:
    assert STAGE1_TAPS % (STAGE1_PHASES * PARTITION_SIZE) == 0
    assert STAGE2_TAPS % 4 == 3
    root = Path(__file__).resolve().parents[1]
    stage1, stage2, comp, cutoff_source = design()
    report = response_metrics(stage1, stage2, comp, cutoff_source)
    worst = report["worst_stopband_db"]["from_source_nyquist_to_output_nyquist"]
    if worst > STOPBAND_TARGET_DB:
        raise RuntimeError(f"stop band {worst:.1f} dB misses {STOPBAND_TARGET_DB} dB")

    def write(relative, text):
        (root / relative).write_text(text, encoding="utf-8", newline="\n")

    write("audio/fft_filter_spectra.hpp", render_spectra(build_spectra(stage1)))
    write("audio/halfband_coefficients.hpp", render_halfband(stage2))
    write("tests/host/fft_prototype_coefficients.hpp",
          render_test_prototypes(stage1, stage2, comp))
    write("tests/host/fft_filter_metrics.json", json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
