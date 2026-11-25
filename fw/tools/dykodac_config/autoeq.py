"""Parse and write Equalizer APO / AutoEQ "ParametricEQ.txt" text.

Python 3.10+, standard library only. The only I/O is in load_parametric_eq(),
which reads a file given by path.
"""

import math
import re
from dataclasses import dataclass

FILTER_OFF, FILTER_PEAKING, FILTER_LOW_SHELF, FILTER_HIGH_SHELF, FILTER_LOW_PASS, FILTER_HIGH_PASS, FILTER_NOTCH = range(7)
CH_LEFT, CH_RIGHT, CH_BOTH = 1, 2, 3


@dataclass
class Band:
    type: int = FILTER_OFF
    channels: int = CH_BOTH
    frequency_hz: float = 1000.0
    gain_db: float = 0.0
    q: float = 0.7071


@dataclass
class ParsedEq:
    preamp_db: float
    bands: list[Band]
    warnings: list[str]


DEFAULT_Q = 0.7071
FREQ_RANGE = (10.0, 20000.0)
GAIN_RANGE = (-20.0, 20.0)
Q_RANGE = (0.1, 20.0)

_TYPE_MAP = {
    "PK": FILTER_PEAKING, "PEQ": FILTER_PEAKING,
    "LSC": FILTER_LOW_SHELF, "LS": FILTER_LOW_SHELF, "LSQ": FILTER_LOW_SHELF,
    "HSC": FILTER_HIGH_SHELF, "HS": FILTER_HIGH_SHELF, "HSQ": FILTER_HIGH_SHELF,
    "LP": FILTER_LOW_PASS, "LPQ": FILTER_LOW_PASS,
    "HP": FILTER_HIGH_PASS, "HPQ": FILTER_HIGH_PASS,
    "NO": FILTER_NOTCH,
}
_TYPE_NAME = {
    FILTER_PEAKING: "PK",
    FILTER_LOW_SHELF: "LSC",
    FILTER_HIGH_SHELF: "HSC",
    FILTER_LOW_PASS: "LP",
    FILTER_HIGH_PASS: "HP",
    FILTER_NOTCH: "NO",
}
_SHELF_TYPES = (FILTER_LOW_SHELF, FILTER_HIGH_SHELF)
_NUM_RE = re.compile(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")
_FILTER_KEY_RE = re.compile(r"filter(?: \d+)?")


def _float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"not a finite number: {text!r}")
    return value


def _parse_freq(token: str) -> float:
    s = token.lower().replace(",", "")
    if s.endswith("hz"):
        s = s[:-2]
    if s.endswith("k"):
        return _float(s[:-1]) * 1000.0
    return _float(s)


def _clamp(value: float, bounds: tuple[float, float], name: str, unit: str,
           lineno: int, warnings: list[str]) -> float:
    lo, hi = bounds
    if value < lo or value > hi:
        clamped = lo if value < lo else hi
        warnings.append(f"line {lineno}: {name} {value:g} {unit} clamped to {clamped:g} {unit}")
        return clamped
    return value


def _parse_filter(value: str, channels: int, lineno: int, warnings: list[str]) -> Band | None:
    toks = value.split()
    if not toks:
        warnings.append(f"line {lineno}: filter has no ON/OFF state, skipped")
        return None
    state = toks[0].upper()
    if state == "OFF":
        return None
    if state != "ON":
        warnings.append(f"line {lineno}: unknown filter state {toks[0]!r}, skipped")
        return None
    if len(toks) < 2:
        warnings.append(f"line {lineno}: filter has no type, skipped")
        return None

    ftype = _TYPE_MAP.get(toks[1].upper())
    if ftype is None:
        warnings.append(f"line {lineno}: unknown filter type {toks[1]!r}, skipped")
        return None

    freq, gain, q = 1000.0, 0.0, DEFAULT_Q
    has_q = has_bw = False
    n = len(toks)
    i = 2
    try:
        while i < n:
            key = toks[i].lower()
            if key == "fc":
                freq = _parse_freq(toks[i + 1])
                i += 2
            elif key == "gain":
                gain = _float(toks[i + 1])
                i += 2
            elif key == "q":
                q = _float(toks[i + 1])
                has_q = True
                i += 2
            elif key == "bw":
                has_bw = True
                i += 1
                if i < n and toks[i].lower() == "oct":
                    i += 1
                i += 1  # bandwidth value
            elif key == "s":
                has_bw = True
                i += 2  # shelf slope value
            else:
                i += 1  # "Hz", "dB", or unrecognized word
    except (ValueError, IndexError):
        warnings.append(f"line {lineno}: could not parse filter, skipped")
        return None

    if not has_q and has_bw and ftype in _SHELF_TYPES:
        warnings.append(f"line {lineno}: shelf has BW/S but no Q, using Q {DEFAULT_Q}")

    return Band(type=ftype, channels=channels, frequency_hz=freq, gain_db=gain, q=q)


def parse_parametric_eq(text: str, max_bands: int = 10) -> ParsedEq:
    warnings: list[str] = []
    preamp_db = 0.0
    entries: list[tuple[int, Band]] = []
    channels = CH_BOTH

    for lineno, raw in enumerate(text.lstrip("﻿").splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line or ":" not in line:
            continue
        key, _, value = line.partition(":")
        key = " ".join(key.split()).lower()
        value = value.strip()

        if key == "preamp":
            m = _NUM_RE.search(value)
            if m:
                preamp_db = _float(m.group())
            else:
                warnings.append(f"line {lineno}: could not parse preamp {value!r}, ignored")
        elif _FILTER_KEY_RE.fullmatch(key):
            band = _parse_filter(value, channels, lineno, warnings)
            if band is not None:
                entries.append((lineno, band))
        elif key == "channel":
            mask = 0
            for tok in value.lower().split():
                if tok == "l":
                    mask |= CH_LEFT
                elif tok == "r":
                    mask |= CH_RIGHT
                elif tok in ("all", "both"):
                    mask |= CH_BOTH
                else:
                    warnings.append(f"line {lineno}: unknown channel {tok!r}, ignored")
            if mask:
                channels = mask
            else:
                warnings.append(f"line {lineno}: no usable channel in {value!r}, ignored")
        elif key in ("include", "graphiceq"):
            warnings.append(f"line {lineno}: {key} is not supported and was ignored")
        # Any other directive is ignored silently.

    if len(entries) > max_bands:
        dropped = len(entries) - max_bands
        warnings.append(
            f"{len(entries)} filters found, only the first {max_bands} kept ({dropped} dropped)")
        entries = entries[:max_bands]

    bands: list[Band] = []
    for lineno, band in entries:
        band.frequency_hz = _clamp(band.frequency_hz, FREQ_RANGE, "frequency", "Hz", lineno, warnings)
        band.gain_db = _clamp(band.gain_db, GAIN_RANGE, "gain", "dB", lineno, warnings)
        band.q = _clamp(band.q, Q_RANGE, "Q", "", lineno, warnings)
        bands.append(band)

    return ParsedEq(preamp_db=preamp_db, bands=bands, warnings=warnings)


def load_parametric_eq(path, max_bands: int = 10) -> ParsedEq:
    with open(path, encoding="utf-8", errors="replace") as f:
        return parse_parametric_eq(f.read(), max_bands=max_bands)


def _num(x: float) -> str:
    return repr(float(x))


def format_parametric_eq(bands: list[Band], preamp_db: float | None = None) -> str:
    out: list[str] = []
    if preamp_db is not None:
        out.append(f"Preamp: {_num(preamp_db)} dB")
    n = 0
    for band in bands:
        if band.type == FILTER_OFF:
            continue
        name = _TYPE_NAME.get(band.type)
        if name is None:
            raise ValueError(f"unknown filter type: {band.type}")
        n += 1
        single = band.channels in (CH_LEFT, CH_RIGHT)
        if band.channels == CH_LEFT:
            out.append("Channel: L")
        elif band.channels == CH_RIGHT:
            out.append("Channel: R")
        out.append(
            f"Filter {n}: ON {name} Fc {_num(band.frequency_hz)} Hz "
            f"Gain {_num(band.gain_db)} dB Q {_num(band.q)}")
        if single:
            out.append("Channel: all")
    return "\n".join(out) + "\n"
