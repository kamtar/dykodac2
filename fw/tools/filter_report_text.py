"""Text and layout of the filter report PDF. Run tools/make_filter_report.py."""
from __future__ import annotations

from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import (CondPageBreak, Image, KeepTogether, ListFlowable, ListItem, PageBreak,
                                Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle)

INK, INK2, MUTED = colors.HexColor("#0b0b0b"), colors.HexColor("#52514e"), colors.HexColor("#898781")
RULE, BLUE, TINT = colors.HexColor("#e1e0d9"), colors.HexColor("#2a78d6"), colors.HexColor("#f4f3ef")

BODY = ParagraphStyle("body", fontName="Helvetica", fontSize=10, leading=14.5, textColor=INK,
                      alignment=TA_LEFT, spaceAfter=7)
SMALL = ParagraphStyle("small", parent=BODY, fontSize=8.5, leading=12, textColor=INK2)
H1 = ParagraphStyle("h1", parent=BODY, fontName="Helvetica-Bold", fontSize=15, leading=19,
                    spaceBefore=12, spaceAfter=8, keepWithNext=1)
TITLE = ParagraphStyle("title", parent=BODY, fontName="Helvetica-Bold", fontSize=24, leading=29,
                       spaceAfter=4)
SUB = ParagraphStyle("sub", parent=BODY, fontSize=11.5, leading=16, textColor=INK2, spaceAfter=14)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=9, leading=12, spaceAfter=0)
CELLB = ParagraphStyle("cellb", parent=CELL, fontName="Helvetica-Bold")
CAPTION = ParagraphStyle("caption", parent=SMALL, spaceBefore=2, spaceAfter=10)
WIDTH = A4[0] - 40 * mm

# Filled in from the host test run and the Release build of this revision.
VERIFICATION = {
    "host_tests": "All 822,894 checks pass.",
    "fft_error": "The fast method used in the firmware agrees with the reference to better than one part in a million.",
    "memory": "It is 43% smaller than before (72 KB instead of 127 KB) and uses 24 KB less memory.",
}


def p(text, style=BODY):
    return Paragraph(text, style)


def bullets(items):
    return ListFlowable([ListItem(p(item), leftIndent=12) for item in items], bulletType="bullet",
                        bulletFontSize=7, leftIndent=14, bulletOffsetY=-1)


def figure(path: Path, caption: str):
    image = Image(str(path))
    scale = 0.9 * WIDTH / image.imageWidth
    image.drawWidth, image.drawHeight = 0.9 * WIDTH, image.imageHeight * scale
    return KeepTogether([image, p(caption, CAPTION)])


def table(rows, widths, header=True):
    data = [[p(cell, CELLB if (header and r == 0) else CELL) for cell in row]
            for r, row in enumerate(rows)]
    style = [("VALIGN", (0, 0), (-1, -1), "TOP"), ("LINEBELOW", (0, 0), (-1, -1), 0.5, RULE),
             ("TOPPADDING", (0, 0), (-1, -1), 4), ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
             ("LEFTPADDING", (0, 0), (-1, -1), 5), ("RIGHTPADDING", (0, 0), (-1, -1), 5)]
    if header:
        style += [("BACKGROUND", (0, 0), (-1, 0), TINT), ("LINEBELOW", (0, 0), (-1, 0), 0.8, MUTED)]
    return KeepTogether(Table(data, colWidths=[w * WIDTH for w in widths], style=TableStyle(style),
                 spaceAfter=10, hAlign="LEFT"))


def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(MUTED)
    canvas.drawString(20 * mm, 11 * mm, "DykoDAC2 firmware - interpolation filter and volume")
    canvas.drawRightString(A4[0] - 20 * mm, 11 * mm, f"{doc.page}")
    canvas.restoreState()


def build(fig: dict[str, Path], destination: Path) -> None:
    s = []
    s += [p("A new reconstruction filter and volume control", TITLE),
          p("DykoDAC2 firmware: what changed, why, and what to expect from it", SUB)]

    s += [p("In short", H1),
          p("When the DAC plays a 44.1 or 48 kHz recording, the firmware first converts it to "
            "four times that rate. The part that does this conversion, the <b>interpolation "
            "filter</b>, decides how accurately the original waveform is rebuilt. This revision "
            "replaces that filter and moves the volume control out of the DAC chip into the "
            "firmware."),
          table([["", "Before", "Now"],
                 ["Unwanted content above the music band", "121 dB down", "141 dB down"],
                 ["Delay through the filter (CD-rate music)", "23.2 ms", "6.0 ms"],
                 ["Ringing around a sharp click (down to -100 dB)", "22.2 ms each side",
                  "4.4 ms each side"],
                 ["Frequency response with the DAC, to 20 kHz", "within about 0.01 dB",
                  "flat within 0.0001 dB (model)"],
                 ["Loud peaks between samples", "cut off (clipped)", "pass through intact"],
                 ["Volume control", "inside the DAC chip, 0.5 dB steps",
                  "in the firmware, dithered, smooth ramps"],
                 ["Protection against a fault at full volume", "none", "fixed limit in the DAC"]],
                [0.44, 0.27, 0.29]),
          p("One of these you may hear: loud modern recordings are no longer clipped. The rest "
            "are improvements you can measure but should not expect to hear, and the report says "
            "so where that is the case. Nothing has been run on the board yet; the hardware was "
            "not available. Everything here comes from calculation and from running the real "
            "signal-processing code on a PC.", BODY)]

    s += [p("1. What the firmware does to the sound", H1),
          figure(fig["chain"], "The playback path. Blue boxes are the parts replaced in this "
                 "revision."),
          p("A digital recording is a list of numbers: the height of the sound wave measured "
            "44,100 times a second. Those measurements are enough to rebuild the complete, "
            "smooth wave, but only if the gaps between them are filled in <i>correctly</i>. "
            "Filling in those gaps is called reconstruction, and there is exactly one "
            "mathematically right answer for it."),
          p("The CS4398 DAC chip has its own gap-filling filter, and it cannot be switched off. "
            "But when it is fed at four times the CD rate, that built-in filter becomes very "
            "gentle and does almost nothing in the audible range. So the firmware does the hard "
            "part itself, with far more precision than the chip could, and hands the DAC a "
            "signal that is already finished."),
          p("The new filter works in two steps. The first doubles the rate and does all the "
            "precise work. The second doubles it again with a much simpler filter, because by "
            "then the remaining job is easy. Two steps give exactly the same result as one big "
            "step, at a fraction of the processor work.")]

    s += [p("2. Removing what should not be there", H1),
          p("Raising the sample rate creates ghost copies of the music at higher frequencies, "
            "called <b>images</b>. They are not part of the recording. The filter's job is to "
            "keep everything in the music band untouched and remove everything above it. How "
            "well it removes them is measured in decibels (dB): every 20 dB is ten times "
            "smaller."),
          figure(fig["response"], "What each filter lets through. Left: the music band, kept at "
                 "full level by both. Right: the region that must be silent. Lower is better."),
          p("The requirement was 130 dB at half the source sample rate. The previous filter "
            "reached 121 dB. The new one reaches 141 dB, which is less than one ten-millionth "
            "of the original level."),
          figure(fig["transition"], "The same comparison at the very top of the band. The new "
                 "filter is fully closed by 22.05 kHz, the highest frequency a CD can hold."),
          p("The dashed vertical line is the highest frequency the recording can contain. A "
            "filter that is still partly open there lets ghost copies fold back just above it. "
            "The new filter is closed to 141 dB at that line. Below it, the music is passed "
            "unchanged up to 21.4 kHz, well above the limit of hearing.")]

    s += [p("3. Timing: how the filter treats a sudden sound", H1),
          p("Any filter that cuts off sharply has a side effect: a sudden click causes a faint, "
            "very high-pitched ringing before and after it. The sharper the cut, the longer the "
            "ringing. This is physics, not a flaw in a particular design, so the aim is to make "
            "it no longer than the job requires."),
          figure(fig["impulse"], "A single click through each filter. The narrow spike is the "
                 "click; the skirts are ringing. Narrower is better."),
          p("The previous filter rang for 22 ms on each side. That was far longer than its "
            "performance needed, a sign it was not designed efficiently. The new one rings for "
            "4.4 ms, five times shorter, while removing images better. The ringing is at about "
            "21.6 kHz, above the range of hearing, and it only appears if the recording itself "
            "has content right at that frequency."),
          p("Two further choices protect the shape of the sound:"),
          bullets(["<b>All frequencies are delayed by exactly the same amount.</b> Low and high "
                   "parts of a drum hit or a plucked string stay lined up, so its shape is "
                   "preserved. The alternative, removing the ringing before the click by "
                   "delaying high frequencies more, changes the shape of every transient and "
                   "was rejected.",
                   "<b>The original samples pass through the second step untouched.</b> Only "
                   "the new in-between points are calculated."])]

    s += [p("4. Designing around the DAC chip", H1),
          p("The filter was designed for this particular DAC, using the response curves in "
            "the Cirrus Logic CS4398 datasheet. Four facts about the chip shaped it:"),
          bullets(["<b>The chip does no filtering below 86 kHz at this rate.</b> So the firmware "
                   "filter must do the entire job up to that point on its own. This is why the "
                   "second step is a full-strength filter and not a token one.",
                   "<b>The chip does remove what lies above 112 kHz.</b> The only ghost copies "
                   "left after the firmware filter sit at 154 kHz and above, exactly where the "
                   "chip's filter is strong (90 dB). The two filters share the work with no gap "
                   "between them.",
                   "<b>The chip is not perfectly flat.</b> It is about 0.01 dB too loud around "
                   "11 kHz and slightly too quiet near 20 kHz. The firmware filter is built with "
                   "the opposite error, so the two cancel.",
                   "<b>The chip offers a second, slower filter.</b> It is weaker exactly where "
                   "the remaining ghost copies are, so the standard one is kept."]),
          figure(fig["flatness"], "The DAC's small frequency error (green), the filter's "
                 "deliberate opposite (orange), and the two together (blue). The vertical scale "
                 "is thousandths of a decibel."),
          p("<b>Is this tuned to one typical chip?</b> Only this last, smallest part, and it "
            "is bounded. The correction is a hundredth of a decibel, far too small to hear. "
            "The curve it cancels comes mostly from the chip's digital filter, which is "
            "identical in every chip (the datasheet states the response is guaranteed by "
            "design), but it was read from a printed graph and the board's own analog output "
            "stage is not included. If a real chip differed completely from the graph, the "
            "result would be off by at most the same hundredth of a decibel as with no "
            "correction at all. Everything else in this report, the image removal, the "
            "ringing, the delay, the headroom, does not depend on that curve or on any "
            "part-to-part variation. The correction can be switched off with one line.")]

    s += [p("5. Volume, headroom and dither", H1),
          p("<b>Peaks between the samples.</b> The true wave can rise higher between two "
            "samples than either sample. Many modern recordings are mastered so loud that the "
            "rebuilt wave exceeds the maximum the DAC can accept, by up to 3 dB. Previously "
            "those peaks were simply cut off, which adds distortion."),
          figure(fig["intersample"], "Every sample (dots) is within the limit, yet the true "
                 "wave (orange) is 3 dB over it. The new firmware lowers the level by 3.25 dB "
                 "inside its calculations (blue), so nothing is cut."),
          p("<b>Volume in the firmware.</b> Volume is now applied inside the firmware's "
            "high-precision calculation, at the last moment before the numbers are sent to the "
            "DAC. Because the level is lowered before the conversion, the peaks above fit. "
            "Volume changes glide over 20 ms, so there are no clicks."),
          p("<b>Dither.</b> Rounding a calculated value to the 24 bits the DAC accepts causes a "
            "tiny error. Left alone, that error follows the music and is a form of distortion. "
            "Adding a minute amount of random noise first, called dither, turns it into a "
            "constant, featureless hiss about 141 dB below full level, far beneath the DAC's "
            "own noise."),
          p("<b>A safety limit in the DAC.</b> The headphone amplifier on this board has much "
            "more gain than needed. The DAC chip's own attenuator is now set once to a fixed "
            "value and never touched again. If the firmware ever misbehaved, the output cannot "
            "exceed that limit. It is set to 9 dB below maximum by default, which makes the top "
            "of the volume control 12.25 dB below the DAC's maximum. This default is a guess and "
            "is one constant in the code: it should be set to the loudest level you would ever "
            "want."),
          p("<b>What this does not fix.</b> The amplifier's excess gain still means the DAC "
            "runs well below its best signal-to-noise ratio. Turning the level down digitally, "
            "in the chip or in the firmware, cannot change that. Only reducing the amplifier "
            "gain in hardware would.")]

    s += [CondPageBreak(75 * mm), p("6. What to expect when listening", H1),
          table([["Change", "Can it be heard?"],
                 ["Peaks between samples no longer clipped",
                  "Possibly, on loud, heavily compressed recordings. It removes a real "
                  "distortion."],
                 ["Images removed to 141 dB instead of 121 dB",
                  "No. Both are far below the DAC's own noise at 120 dB."],
                 ["Ringing five times shorter", "No. It is above the range of hearing."],
                 ["Flat response with the DAC", "No. The error was 0.01 dB."],
                 ["Dither", "No. It replaces an error that was already inaudible."],
                 ["17 ms less delay", "Not in music. Helpful for video sync and games."]],
                [0.42, 0.58]),
          p("The fair summary is that the previous filter was already beyond what this DAC "
            "chip can resolve in most respects. The new design is more correct, meets the "
            "stated requirement, is properly matched to the chip, uses a fraction of the "
            "processor and memory, and can be regenerated from the repository. The one audible "
            "defect it removes is the clipping.")]

    s += [p("7. How it was checked, and what is still open", H1),
          bullets([f"<b>Filter figures</b> are calculated from the exact coefficient values "
                   f"stored in the firmware, not from the ideal design.",
                   f"<b>The signal-processing code itself</b> was run on a PC against a slow, "
                   f"high-precision reference calculation. {VERIFICATION['host_tests']} "
                   f"{VERIFICATION['fft_error']}",
                   f"<b>The firmware builds</b> for the real processor. {VERIFICATION['memory']}"]),
          p("Not yet done, because the board was not available:"),
          bullets(["Processor load and timing on the real hardware. The new filter does much "
                   "less work than the old one, so it is expected to fit comfortably, but this "
                   "has not been measured.",
                   "An audio measurement of the analog output to confirm the calculated "
                   "response.",
                   "Choosing the right value for the DAC safety limit by listening."]),
          p("Glossary", H1),
          table([["dB (decibel)", "A ratio. 20 dB is ten times, 120 dB is a million times."],
                 ["Sample rate", "How many measurements per second the recording holds. CD is "
                  "44,100."],
                 ["fs/2", "Half the sample rate: the highest frequency the recording can hold "
                  "(22.05 kHz for CD)."],
                 ["Image", "A ghost copy of the music at a higher frequency, created when the "
                  "sample rate is raised."],
                 ["Stop band", "The frequency range a filter is meant to remove."],
                 ["Linear phase", "All frequencies are delayed equally, so waveform shapes are "
                  "kept."],
                 ["Dither", "A tiny amount of random noise added before rounding, to turn "
                  "rounding distortion into plain hiss."],
                 ["Headroom", "Spare room below the maximum level, left for peaks."]],
                [0.22, 0.78], header=False),
          p("Technical detail and the reasoning behind each constant are in "
            "docs/filter-and-volume-design.md. Source for the DAC figures: Cirrus Logic CS4398 "
            "datasheet DS568F3.", SMALL)]

    destination.parent.mkdir(parents=True, exist_ok=True)
    SimpleDocTemplate(str(destination), pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm,
                      topMargin=18 * mm, bottomMargin=20 * mm,
                      title="DykoDAC2 interpolation filter and volume report",
                      author="DykoDAC2 firmware").build(s, onFirstPage=footer, onLaterPages=footer)
