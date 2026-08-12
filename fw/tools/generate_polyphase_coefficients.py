#!/usr/bin/env python3
"""Generate the checked-in binary32 FIR bank used by PolyphaseUpsampler.

The bank stores phases 0 through 0.5. Phases above 0.5 are exact reversed
kernels from the complementary stored phase. It intentionally has no
third-party dependencies.
"""
import math
from pathlib import Path

PHASES = 64
TAPS = 128
CUTOFF = 0.47675             # cycles/input-sample
KAISER_BETA = 10.0


def i0(value: float) -> float:
    total = 1.0
    term = 1.0
    for order in range(1, 40):
        term *= (value * value) / (4.0 * order * order)
        total += term
        if abs(term) < abs(total) * 1e-15:
            break
    return total


def phase_row(phase: int):
    delay = phase / PHASES
    denominator = i0(KAISER_BETA)
    values = []
    for tap in range(TAPS):
        x = tap - (TAPS / 2 - 1) - delay
        argument = 2.0 * CUTOFF * x
        sinc = 1.0 if abs(argument) < 1e-14 else math.sin(math.pi * argument) / (math.pi * argument)
        radius = 2.0 * tap / (TAPS - 1) - 1.0
        window = i0(KAISER_BETA * math.sqrt(max(0.0, 1.0 - radius * radius))) / denominator
        values.append(2.0 * CUTOFF * sinc * window)
    scale = sum(values)
    return [value / scale for value in values]


def float_literal(value: float) -> str:
    text = format(value, ".9g")
    if "." not in text and "e" not in text:
        text += ".0"
    return text + "F"


def render() -> str:
    stored_phases = PHASES // 2 + 1
    rows = [phase_row(phase) for phase in range(stored_phases)]
    lines = ["#pragma once", "", "#include <array>", "#include <cstdint>", "",
             "namespace audio::detail {",
             "__attribute__((section(\".dtcm_coefficients\"), aligned(32)))", 
             f"inline const std::array<float, {stored_phases}U * {TAPS}U> polyphase_coefficients{{{{"]
    for phase, row in enumerate(rows):
        lines.append(f"    // phase {phase} ({phase / PHASES:.8f} input samples)")
        lines.append("    " + ", ".join(float_literal(value) for value in row) + ",")
    lines.extend(["}};", "} // namespace audio::detail", ""])
    return "\n".join(lines)


if __name__ == "__main__":
    destination = Path(__file__).resolve().parents[1] / "audio" / "polyphase_coefficients.hpp"
    destination.write_text(render(), encoding="utf-8", newline="\n")
    print(f"wrote {destination} ({(PHASES // 2 + 1) * TAPS} coefficients)")
