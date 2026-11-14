#!/usr/bin/env python3
"""Build docs/report/interpolator-summary.pdf: a one-page technical
summary of the interpolator redesign for review by a DSP specialist."""
from __future__ import annotations

import sys
from pathlib import Path

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.platypus import Image, SimpleDocTemplate, Spacer, Table

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import filter_report_text as t  # noqa: E402
import make_filter_report as figures  # noqa: E402

H2 = t.ParagraphStyle("h2", parent=t.BODY, fontName="Helvetica-Bold", fontSize=10.5,
                      leading=13, spaceBefore=5, spaceAfter=3, keepWithNext=1)
BODY = t.ParagraphStyle("b", parent=t.BODY, fontSize=9, leading=12, spaceAfter=3)
TITLE = t.ParagraphStyle("t", parent=t.TITLE, fontSize=16, leading=20, spaceAfter=2)


def bullets(items):
    return t.ListFlowable([t.ListItem(t.p(i, BODY), leftIndent=10) for i in items],
                          bulletType="bullet", bulletFontSize=6, leftIndent=12)


def main() -> None:
    paths = figures.figures()
    half = t.WIDTH / 2.0 - 2 * mm

    def image(path):
        img = Image(str(path))
        img.drawHeight = img.imageHeight * half / img.imageWidth
        img.drawWidth = half
        return img

    s = [t.p("DykoDAC2: 4x interpolator redesign", TITLE),
         t.p("44.1 to 176.4 kHz and 48 to 192 kHz. Figures are measured on the float32 "
             "coefficients. Not yet run on hardware.", t.SMALL),
         t.p("Structure", H2),
         t.p("Two stages, both linear phase, Kaiser-windowed, float32.", BODY),
         bullets(["<b>Stage 1 (x2):</b> 1024 taps on the 2x grid, beta = 15, partitioned FFT "
                  "convolution.",
                  "<b>Stage 2 (x2):</b> 63-tap true half-band, beta = 15, time domain; stage-1 "
                  "samples pass through unchanged."]),
         t.p("Measured, 44.1 kHz source", H2),
         t.table([["", "Previous (8192-tap, single stage)", "New"],
                  ["Stop band from fs/2 to 88 kHz", "-121 dB", "-141 dB"],
                  ["Pass-band edge (-0.03 dB)", "21.34 kHz", "21.38 kHz"],
                  ["-6 dB point", "21.59 kHz", "21.64 kHz"],
                  ["Group delay", "23.2 ms", "5.98 ms"],
                  ["Pre-ringing to -60 / -100 dB", "not measured / 22.2 ms", "2.25 / 4.4 ms"]],
                 [0.40, 0.34, 0.26]),
         Table([[image(paths["transition"]), image(paths["impulse"])]],
               colWidths=[t.WIDTH / 2.0] * 2, hAlign="LEFT"),
         Spacer(1, 4),
         t.p("Design choices", H2),
         bullets(["<b>Stop band starts at fs/2</b>, so no images of the top of the band survive.",
                  "<b>Linear phase</b> to preserve transient shape; minimum phase was rejected.",
                  "<b>Kaiser, not equiripple</b>, for near-optimal time concentration and a "
                  "decaying stop band.",
                  "<b>Length:</b> 1024 taps passes to 21.4 kHz. A 512-tap option gives a 20 kHz "
                  "pass band, about -137 dB, and half the delay and ringing; it is a one-line "
                  "switch."]),
         t.p("DAC (CS4398, quad-speed, fast roll-off, no filter bypass)", H2),
         bullets(["Its filter is -3 dB at 0.49 Fs with the stop band from 0.635 Fs at 90 dB, so "
                  "the FIR does all the work below 88 kHz. The DAC only removes the images at "
                  "154 kHz and above.",
                  "Its pass-band deviation (+0.010 dB at 0.065 Fs, datasheet figure 39) is "
                  "pre-compensated in stage 1. This is a 0.01 dB correction from a typical "
                  "curve and can be disabled."]),
         t.p("Output stage", H2),
         bullets(["Volume is applied in float before the single 24-bit quantisation, with TPDF "
                  "dither (+/-1 LSB).",
                  "A fixed -3.25 dB of headroom means intersample peaks up to +3.25 dBFS are not "
                  "clipped; previously they were.",
                  "The DAC attenuator is fixed as a protective ceiling."]),
         t.p("Open", H2),
         t.p("Not yet run on hardware; CPU load and analog output are unmeasured.", BODY),
         t.p("Question", H2),
         t.p("Is a pass band to 21.4 kHz with +/-2.25 ms ringing (to -60 dB) preferred, or the "
             "shorter 512-tap variant with a 20 kHz pass band?", BODY)]

    destination = figures.OUT / "interpolator-summary.pdf"
    SimpleDocTemplate(str(destination), pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm,
                      topMargin=14 * mm, bottomMargin=14 * mm,
                      title="DykoDAC2 4x interpolator redesign - summary").build(s)
    print(destination)


if __name__ == "__main__":
    main()
