#!/usr/bin/env python3
"""Build docs/report/filter-report.pdf (figures + explanatory text).

Needs numpy, scipy, matplotlib, reportlab.  The "previous" filter is read from
git history (commit 6e02ee9, the last one before the two-stage redesign).
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import FancyBboxPatch
from scipy.signal import upfirdn

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tools"))
import generate_polyphase_coefficients as generator  # noqa: E402

OUT = ROOT / "docs" / "report"
PREVIOUS_COMMIT = "6e02ee9"
FS = 44_100.0

SURFACE, INK, INK2, MUTED = "#fcfcfb", "#0b0b0b", "#52514e", "#898781"
GRID, AXIS = "#e1e0d9", "#c3c2b7"
BLUE, ORANGE, AQUA = "#2a78d6", "#eb6834", "#1baf7a"   # validated categorical slots 1-3
OLD, NEW, DAC = ORANGE, BLUE, AQUA

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 9, "axes.edgecolor": AXIS,
    "axes.labelcolor": INK2, "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.facecolor": SURFACE, "figure.facecolor": SURFACE, "axes.grid": True,
    "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False,
    "axes.spines.right": False, "axes.titlesize": 10.5, "axes.titleweight": "bold",
    "axes.titlecolor": INK, "axes.titlelocation": "left", "legend.frameon": False,
    "lines.linewidth": 1.6, "savefig.dpi": 200,
})


def previous_prototype() -> np.ndarray:
    text = subprocess.run(
        ["git", "show", f"{PREVIOUS_COMMIT}:fw/tests/host/fft_prototype_coefficients.hpp"],
        cwd=ROOT, capture_output=True, text=True, check=True).stdout
    body = text[text.index("{", text.index("prototype_coefficients{")) + 1:text.index("};")]
    return np.array([float(v.rstrip("fF")) for v in re.findall(
        r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?[fF]?", body)], dtype=np.float64)


def magnitude_db(h: np.ndarray, points: int = 1 << 21):
    freq = np.fft.rfftfreq(points, 1.0) * 4.0 * FS
    return freq, 20.0 * np.log10(np.maximum(np.abs(np.fft.rfft(h, points)) / 4.0, 1e-12))


def save(fig, name: str) -> Path:
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / name
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    return path


def fig_chain() -> Path:
    fig, ax = plt.subplots(figsize=(7.6, 1.75))
    ax.set_axis_off()
    ax.set_xlim(0, 104)
    ax.set_ylim(0, 24)
    boxes = [("USB audio\n44.1 / 48 kHz", MUTED), ("Stage 1: x2\nsharp filter\n1024 taps", NEW),
             ("Stage 2: x2\nhalf-band\n63 taps", NEW), ("Volume,\nheadroom,\ndither", NEW),
             ("CS4398 DAC\ngentle filter,\nfixed ceiling", DAC), ("Headphone\namplifier", MUTED)]
    width, gap = 14.0, 3.0
    for index, (label, colour) in enumerate(boxes):
        x = 1.0 + index * (width + gap)
        ax.add_patch(FancyBboxPatch((x, 6), width, 14, boxstyle="round,pad=0.2,rounding_size=1.2",
                                    linewidth=1.4, edgecolor=colour, facecolor="white"))
        ax.text(x + width / 2, 13, label, ha="center", va="center", fontsize=7.0, color=INK)
        if index:
            ax.annotate("", xy=(x - 0.3, 13), xytext=(x - gap + 0.3, 13),
                        arrowprops=dict(arrowstyle="-|>", color=INK2, lw=1.0))
    ax.text(1.0 + 1 * (width + gap), 2.2, "new firmware: 176.4 / 192 kHz, 24-bit out", fontsize=7.4,
            color=NEW)
    return save(fig, "fig_chain.png")


def fig_response(old, new) -> Path:
    fig, ax = plt.subplots(figsize=(7.2, 3.3))
    for h, colour, label in ((old, OLD, "previous"), (new, NEW, "new")):
        freq, mag = magnitude_db(h, 1 << 19)
        # peak-hold so stop-band lobes read as a floor rather than a comb
        step = 64
        n = (mag.size // step) * step
        ax.plot(freq[:n:step] / 1e3, mag[:n].reshape(-1, step).max(axis=1), color=colour, label=label)
    ax.axhline(-130, color=INK2, lw=0.8, ls=(0, (4, 3)))
    ax.text(87.5, -127, "requirement: -130 dB", ha="right", fontsize=7.5, color=INK2)
    ax.axvspan(0, 22.05, color=GRID, alpha=0.45, lw=0)
    ax.text(11, -12, "music\nband", ha="center", va="top", fontsize=7.5, color=INK2)
    ax.text(25, -112, "previous: -121 dB at worst", color=INK2, fontsize=8)
    ax.text(50.5, -160, "new: -141 dB at worst", color=INK2, fontsize=8)
    ax.set(xlim=(0, 88.2), ylim=(-185, 8), xlabel="frequency (kHz), 44.1 kHz source",
           ylabel="level (dB)", title="Everything above the music band must be removed")
    ax.legend(loc="upper right")
    return save(fig, "fig_response.png")


def fig_transition(old, new) -> Path:
    fig, ax = plt.subplots(figsize=(7.2, 3.1))
    for h, colour, label in ((old, OLD, "previous"), (new, NEW, "new")):
        freq, mag = magnitude_db(h)
        mask = (freq > 20_500) & (freq < 22_700)
        ax.plot(freq[mask] / 1e3, mag[mask], color=colour, label=label)
    ax.axvline(22.05, color=INK2, lw=0.8, ls=(0, (4, 3)))
    ax.text(22.06, -8, " fs/2 = 22.05 kHz", fontsize=7.5, color=INK2)
    ax.axhline(-130, color=INK2, lw=0.8, ls=(0, (4, 3)))
    ax.text(20.52, -127, "-130 dB", fontsize=7.5, color=INK2)
    ax.set(xlim=(20.5, 22.7), ylim=(-175, 8), xlabel="frequency (kHz), 44.1 kHz source",
           ylabel="level (dB)", title="The edge of the band, zoomed in")
    ax.legend(loc="lower left")
    return save(fig, "fig_transition.png")


def fig_impulse(old, new) -> Path:
    fig, ax = plt.subplots(figsize=(7.2, 3.1))
    for h, colour, label in ((old, OLD, "previous"), (new, NEW, "new")):
        envelope = np.abs(h) / np.max(np.abs(h))
        peak = int(np.argmax(envelope))
        window = 24
        n = (envelope.size // window) * window
        held = envelope[:n].reshape(-1, window).max(axis=1)
        time_ms = (np.arange(held.size) * window + window / 2 - peak) / (4.0 * FS) * 1e3
        ax.plot(time_ms, 20 * np.log10(np.maximum(held, 1e-9)), color=colour, label=label)
    ax.axvline(0, color=AXIS, lw=0.8)
    ax.text(-22.5, -52, "ringing before\nthe click", fontsize=7.5, color=INK2, va="top")
    ax.text(22.5, -52, "ringing after\nthe click", fontsize=7.5, color=INK2, va="top", ha="right")
    ax.set(xlim=(-24, 24), ylim=(-150, 5), xlabel="time relative to the click (ms)",
           ylabel="level relative to the click (dB)",
           title="How long one click rings in the filter")
    ax.legend(loc="upper right")
    return save(fig, "fig_impulse.png")


def fig_flatness(new) -> Path:
    fig, ax = plt.subplots(figsize=(7.2, 3.0))
    freq, mag = magnitude_db(new)
    mask = freq <= 20_000
    f = freq[mask]
    dac = generator.dac_response_db(f / (4.0 * FS))
    for values, colour, label in ((dac * 1e3, DAC, "DAC alone"),
                                  (mag[mask] * 1e3, ORANGE, "new filter alone"),
                                  ((mag[mask] + dac) * 1e3, BLUE, "filter + DAC")):
        ax.plot(f / 1e3, values, color=colour, label=label)
    ax.text(11.6, 11.0, "DAC alone", color=INK2, fontsize=7.5, ha="center")
    ax.text(11.6, -12.2, "new filter alone (mirror image)", color=INK2, fontsize=7.5, ha="center")
    ax.text(13.0, 1.0, "together: flat", color=INK2, fontsize=7.5)
    ax.set(xlim=(0, 20), ylim=(-15, 15), xlabel="frequency (kHz), 44.1 kHz source",
           ylabel="deviation (thousandths of a dB)",
           title="The filter cancels the DAC's own small frequency error")
    ax.legend(loc="upper left", fontsize=7.5)
    return save(fig, "fig_flatness.png")


def fig_intersample(new) -> Path:
    fig, ax = plt.subplots(figsize=(7.2, 3.4))
    n = np.arange(400)
    source = np.sin(2 * np.pi * n / 4 + np.pi / 4)
    source /= np.max(np.abs(source))
    wave = upfirdn(new, source, up=4)
    delay = (new.size - 1) // 2
    first = 200
    t_out = (np.arange(4 * 6 + 1) / 4.0)
    segment = wave[delay + 4 * first: delay + 4 * first + t_out.size]
    fine = np.linspace(0, 6, 600)
    ax.plot(fine, np.interp(fine, t_out, segment) * 0 + np.sqrt(2) * np.sin(2 * np.pi * (fine + first) / 4 + np.pi / 4),
            color=OLD, label="true waveform between the samples")
    ax.plot(fine, 10 ** (-3.25 / 20) * np.sqrt(2) * np.sin(2 * np.pi * (fine + first) / 4 + np.pi / 4),
            color=NEW, label="new: lowered 3.25 dB before conversion")
    ax.plot(np.arange(7), source[first:first + 7], "o", color=INK, ms=5, zorder=5,
            label="samples on the disc (all at or below full scale)")
    for level in (1.0, -1.0):
        ax.axhline(level, color=INK2, lw=0.8, ls=(0, (4, 3)))
    ax.text(6.02, 1.0, " full scale", fontsize=7.5, color=INK2, va="center")
    ax.axhspan(1.0, 1.6, color=OLD, alpha=0.10, lw=0)
    ax.axhspan(-1.6, -1.0, color=OLD, alpha=0.10, lw=0)
    ax.text(0.05, 1.47, "previously cut off here (clipping)", fontsize=7.5, color=INK2)
    ax.set(xlim=(0, 6), ylim=(-1.6, 1.6), xlabel="time (source samples)", ylabel="level",
           title="A waveform can be louder between its samples than at them")
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=1, fontsize=7.5)
    return save(fig, "fig_intersample.png")


def figures() -> dict[str, Path]:
    old = previous_prototype()
    _, _, new, _ = generator.design()
    return {"chain": fig_chain(), "response": fig_response(old, new),
            "transition": fig_transition(old, new), "impulse": fig_impulse(old, new),
            "flatness": fig_flatness(new), "intersample": fig_intersample(new)}


if __name__ == "__main__":
    paths = figures()
    if "--figures-only" not in sys.argv:
        import filter_report_text
        filter_report_text.build(paths, OUT / "filter-report.pdf")
    print("\n".join(str(p) for p in paths.values()))
