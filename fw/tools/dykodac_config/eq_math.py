"""Host mirror of audio/source_dsp.cpp: biquad design, response and the
worst-case boost the firmware subtracts from the headroom."""

from __future__ import annotations

import math

from . import protocol as p

_XF_PRESETS = [(0.0, 0.0), (700.0, 4.5), (700.0, 6.0), (650.0, 9.5)]


def design_band(band: p.EqBand, fs: float):
    """RBJ cookbook biquad (b0, b1, b2, a1, a2), a0 normalised; shelves use Q."""
    if band.type == p.FILTER_OFF:
        return (1.0, 0.0, 0.0, 0.0, 0.0)
    f = min(band.frequency_hz, 0.49 * fs)
    w0 = 2 * math.pi * f / fs
    cw, alpha = math.cos(w0), math.sin(w0) / (2 * band.q)
    a = 10 ** (band.gain_db / 40)
    sa = 2 * math.sqrt(a) * alpha
    t = band.type
    if t == p.FILTER_PEAKING:
        b0, b1, b2, a0, a1, a2 = 1 + alpha * a, -2 * cw, 1 - alpha * a, 1 + alpha / a, -2 * cw, 1 - alpha / a
    elif t == p.FILTER_LOW_SHELF:
        b0 = a * ((a + 1) - (a - 1) * cw + sa); b1 = 2 * a * ((a - 1) - (a + 1) * cw)
        b2 = a * ((a + 1) - (a - 1) * cw - sa); a0 = (a + 1) + (a - 1) * cw + sa
        a1 = -2 * ((a - 1) + (a + 1) * cw); a2 = (a + 1) + (a - 1) * cw - sa
    elif t == p.FILTER_HIGH_SHELF:
        b0 = a * ((a + 1) + (a - 1) * cw + sa); b1 = -2 * a * ((a - 1) + (a + 1) * cw)
        b2 = a * ((a + 1) + (a - 1) * cw - sa); a0 = (a + 1) - (a - 1) * cw + sa
        a1 = 2 * ((a - 1) - (a + 1) * cw); a2 = (a + 1) - (a - 1) * cw - sa
    elif t == p.FILTER_LOW_PASS:
        b0, b1, b2, a0, a1, a2 = (1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha
    elif t == p.FILTER_HIGH_PASS:
        b0, b1, b2, a0, a1, a2 = (1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha
    elif t == p.FILTER_NOTCH:
        b0, b1, b2, a0, a1, a2 = 1.0, -2 * cw, 1.0, 1 + alpha, -2 * cw, 1 - alpha
    else:
        return (1.0, 0.0, 0.0, 0.0, 0.0)
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def magnitude(c, w: float) -> float:
    b0, b1, b2, a1, a2 = c
    phi = math.sin(w / 2) ** 2
    num = (b0 + b1 + b2) ** 2 - 4 * (b0 * b1 + 4 * b0 * b2 + b1 * b2) * phi + 16 * b0 * b2 * phi * phi
    den = (1 + a1 + a2) ** 2 - 4 * (a1 + 4 * a2 + a1 * a2) * phi + 16 * a2 * phi * phi
    return math.sqrt(max(num, 0.0) / den) if den > 0 else 0.0


def _first_order(n0, n1, d1, w):
    c = math.cos(w)
    return math.sqrt((n0 * n0 + n1 * n1 + 2 * n0 * n1 * c) / (1 + d1 * d1 - 2 * d1 * c))


def _crossfeed(preset: int, fs: float):
    if not 0 < preset < len(_XF_PRESETS):
        return None
    fc_lo, feed = _XF_PRESETS[preset]
    gb_lo, gb_hi = feed * -5 / 6 - 3, feed / 6 - 3
    g_lo, g_hi = 10 ** (gb_lo / 20), 1 - 10 ** (gb_hi / 20)
    fc_hi = fc_lo * 2 ** ((gb_lo - 20 * math.log10(g_hi)) / 12)
    x_lo, x_hi = math.exp(-2 * math.pi * fc_lo / fs), math.exp(-2 * math.pi * fc_hi / fs)
    return dict(a0_lo=g_lo * (1 - x_lo), b1_lo=x_lo, a0_hi=1 - g_hi * (1 - x_hi), a1_hi=-x_hi, b1_hi=x_hi,
                gain=1 / (1 - g_hi + g_lo))


class Chain:
    """Designed processing chain for one settings snapshot at one rate."""

    def __init__(self, s: p.Settings, fs: float = 48000.0):
        self.fs = fs
        self.left, self.right = [], []
        if s.eq_enabled:
            for b in s.bands:
                if b.type == p.FILTER_OFF:
                    continue
                c = design_band(b, fs)
                if b.channels & p.CH_LEFT:
                    self.left.append(c)
                if b.channels & p.CH_RIGHT:
                    self.right.append(c)
        self.xf = _crossfeed(s.crossfeed, fs)
        self.trim_l = 10 ** (-s.balance_db / 20) if s.balance_db > 0 else 1.0
        self.trim_r = 10 ** (s.balance_db / 20) if s.balance_db < 0 else 1.0
        self.centres = [b.frequency_hz for b in s.bands if s.eq_enabled and b.type != p.FILTER_OFF]

    def eq_db(self, f: float, left: bool = True) -> float:
        w = 2 * math.pi * f / self.fs
        m = 1.0
        for c in (self.left if left else self.right):
            m *= magnitude(c, w)
        return 20 * math.log10(max(m, 1e-9))

    def band_db(self, band: p.EqBand, f: float) -> float:
        return 20 * math.log10(max(magnitude(design_band(band, self.fs), 2 * math.pi * f / self.fs), 1e-9))

    def _worst(self, f: float) -> float:
        w = 2 * math.pi * f / self.fs
        el = 10 ** (self.eq_db(f, True) / 20)
        er = 10 ** (self.eq_db(f, False) / 20)
        ol, orr = el, er
        if self.xf:
            x = self.xf
            hi = _first_order(x["a0_hi"], x["a1_hi"], x["b1_hi"], w) * x["gain"]
            lo = _first_order(x["a0_lo"], 0.0, x["b1_lo"], w) * x["gain"]
            ol, orr = hi * el + lo * er, hi * er + lo * el
        return max(ol * self.trim_l, orr * self.trim_r)

    def boost_db(self) -> float:
        """Same grid, centres and 0.1 dB margin as the firmware."""
        if not self.left and not self.right and not self.xf:
            return 0.0
        top, n = 0.499 * self.fs, 192
        ratio = (top / 10.0) ** (1 / (n - 1))
        peak, f = 0.0, 10.0
        for _ in range(n):
            peak = max(peak, self._worst(f))
            f *= ratio
        for fc in self.centres:
            if fc < top:
                peak = max(peak, self._worst(fc))
        return 20 * math.log10(peak) + 0.1 if peak > 1.0 else 0.0
