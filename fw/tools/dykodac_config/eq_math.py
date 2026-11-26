"""Host mirror of audio/source_dsp.cpp: biquad design, response and the
worst-case boost the firmware subtracts from the headroom."""

from __future__ import annotations

import cmath
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


def _fo(b0, b1, d1, zi):
    """First-order section (b0 + b1 z^-1) / (1 - d1 z^-1) at z^-1 = zi."""
    return (b0 + b1 * zi) / (1 - d1 * zi)


def _frames(seconds, fs):
    return int(seconds * fs + 0.5)


def _natural(fs):
    x = math.exp(-2 * math.pi * 500.0 / fs)
    post = design_band(p.EqBand(type=p.FILTER_PEAKING, channels=p.CH_BOTH, frequency_hz=1200.0, gain_db=-2.0, q=0.5), fs)
    return dict(near=(1.0, 0.0, 0.0), far=(0.5 * (1 - x), 0.0, x), far_delay=_frames(0.10e-3, fs),
                refl=[], post=post)


_REFLECTIONS = [(1.30, 0.20, 1.55, 0.15), (2.90, 0.16, 3.20, 0.10), (3.70, 0.09, 3.95, 0.13),
                (4.80, 0.12, 5.00, 0.11), (6.90, 0.08, 7.30, 0.07), (8.60, 0.07, 8.85, 0.07)]


def _speakers(az_deg, fs):
    radius, sound = 0.0875, 343.0
    az = math.radians(az_deg)
    kt = 2 * fs * radius / (2 * sound)

    def shadow(from_ear):
        alpha = 1.05 + 0.95 * math.cos(from_ear / 150 * math.pi)
        return (0.5 * (1 + alpha * kt) / (1 + kt), 0.5 * (1 - alpha * kt) / (1 + kt), -(1 - kt) / (1 + kt))

    level = 0.35 * 0.5
    refl = []
    for nm, ng, fm, fg in _REFLECTIONS:
        refl += [(_frames(nm * 1e-3, fs), level * ng), (_frames(fm * 1e-3, fs), -level * fg)]
    xh, xl = math.exp(-2 * math.pi * 500.0 / fs), math.exp(-2 * math.pi * 6000.0 / fs)
    return dict(near=shadow(90 - az_deg), far=shadow(90 + az_deg),
                far_delay=_frames(radius / sound * (az + math.sin(az)), fs), refl=refl,
                hp=(0.5 * (1 + xh), -0.5 * (1 + xh), xh), lp=(1 - xl, 0.0, xl), post=None)


def _spacious(fs):
    low, high, k = 0.45, 0.18, math.tan(math.pi * 1500.0 / fs)
    widen = design_band(p.EqBand(type=p.FILTER_LOW_SHELF, channels=p.CH_BOTH, frequency_hz=300.0, gain_db=2.5, q=0.6), fs)
    return dict(near=(1.0, 0.0, 0.0), far=((low * k + high) / (k + 1), (low * k - high) / (k + 1), -(k - 1) / (k + 1)),
                far_delay=_frames(0.30e-3, fs), refl=[], post=widen, amounts=(0.5, 1.4))


def _crossfeed(preset: int, fs: float):
    """Mirror of design_crossfeed(); returns a function w -> [(direct, cross), ...] complex,
    one pair per end of the adaptive amount range (one pair for fixed presets)."""
    if preset == 7:
        c = _spacious(fs)
    elif preset == 4:
        c = _natural(fs)
    elif preset in (5, 6):
        c = _speakers(30.0 if preset == 5 else 45.0, fs)
    elif 0 < preset < len(_XF_PRESETS):
        fc_lo, feed = _XF_PRESETS[preset]
        gb_lo, gb_hi = feed * -5 / 6 - 3, feed / 6 - 3
        g_lo, g_hi = 10 ** (gb_lo / 20), 1 - 10 ** (gb_hi / 20)
        fc_hi = fc_lo * 2 ** ((gb_lo - 20 * math.log10(g_hi)) / 12)
        x_lo, x_hi = math.exp(-2 * math.pi * fc_lo / fs), math.exp(-2 * math.pi * fc_hi / fs)
        gain = 1 / (1 - g_hi + g_lo)

        def bs2b(w):
            zi = cmath.exp(-1j * w)
            return [(gain * _fo(1 - g_hi * (1 - x_hi), -x_hi, x_hi, zi), gain * _fo(g_lo * (1 - x_lo), 0.0, x_lo, zi))]
        return bs2b
    else:
        return None

    def mid_side(w):
        return [_mid_side(w, amount) for amount in c.get("amounts", (1.0,))]

    def _mid_side(w, amount):
        zi = cmath.exp(-1j * w)
        side = _fo(*c["near"], zi) - amount * _fo(*c["far"], zi) * cmath.exp(-1j * w * c["far_delay"])
        if c["refl"]:
            s = sum(g * cmath.exp(-1j * w * n) for n, g in c["refl"])
            side += s * _fo(*c["hp"], zi) * _fo(*c["lp"], zi)
        if c["post"]:
            b0, b1, b2, a1, a2 = c["post"]
            side *= (b0 + b1 * zi + b2 * zi * zi) / (1 + a1 * zi + a2 * zi * zi)
        return 0.5 * (1 + side), 0.5 * (1 - side)
    return mid_side


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
        # Transient shaper: x + b G HP(x), b in [0, 1] (proximity only cuts).
        self.transient = None
        if s.transient_enabled:
            hp = design_band(p.EqBand(type=p.FILTER_HIGH_PASS, channels=p.CH_BOTH,
                                      frequency_hz=float(s.transient_crossover_hz), q=0.7071), fs)
            boost = 10 ** (p.MAX_TRANSIENT_BOOST_DB * s.transient_amount_pct / 100 / 20) - 1
            self.transient = (hp, boost)
            self.centres.append(float(s.transient_crossover_hz))
        # Space stage (mid/side): width extra side gain e in [0, E] on a
        # first-order 300 Hz high-pass, front cue peaks on mid and side.
        self.width = None
        if s.width_enabled:
            k = math.tan(math.pi * p.WIDTH_SPLIT_HZ / fs)
            b0, d1 = 1 / (1 + k), (1 - k) / (1 + k)
            extra = 10 ** (p.MAX_WIDTH_BOOST_DB * s.width_amount_pct / 100 / 20) - 1
            self.width = (b0, d1, extra)
        self.front = None
        if s.front_enabled:
            a = s.front_amount_pct / 100
            mid = design_band(p.EqBand(type=p.FILTER_PEAKING, channels=p.CH_BOTH, frequency_hz=p.FRONT_MID_HZ,
                                       gain_db=-p.MAX_FRONT_MID_CUT_DB * a, q=p.FRONT_MID_Q), fs)
            side = design_band(p.EqBand(type=p.FILTER_PEAKING, channels=p.CH_BOTH, frequency_hz=p.FRONT_SIDE_HZ,
                                        gain_db=p.MAX_FRONT_SIDE_BOOST_DB * a, q=p.FRONT_SIDE_Q), fs)
            self.front = (mid, side)
            self.centres += [p.FRONT_SIDE_HZ, p.FRONT_MID_HZ]

    def _space_bound(self, w: float) -> float:
        """Mirror of space_bound(): (|Hm + Hs A| + |Hm - Hs A|) / 2, A = 1 + e H_hp."""
        if not self.width and not self.front:
            return 1.0
        zi = cmath.exp(-1j * w)

        def resp(c):
            b0, b1, b2, a1, a2 = c
            return (b0 + b1 * zi + b2 * zi * zi) / (1 + a1 * zi + a2 * zi * zi)
        hm, hs = (resp(self.front[0]), resp(self.front[1])) if self.front else (1.0, 1.0)
        hp, extra = 0.0, 0.0
        if self.width:
            b0, d1, extra = self.width
            hp = b0 * (1 - zi) / (1 - d1 * zi)
        bound = 0.0
        for e in (0.0, extra):
            side = hs * (1 + e * hp)
            bound = max(bound, 0.5 * (abs(hm + side) + abs(hm - side)))
        return bound

    def _transient_bound(self, w: float) -> float:
        if not self.transient:
            return 1.0
        (b0, b1, b2, a1, a2), boost = self.transient
        zi = cmath.exp(-1j * w)
        h = (b0 + b1 * zi + b2 * zi * zi) / (1 + a1 * zi + a2 * zi * zi)
        return max(1.0, abs(1 + boost * h))

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
        if self.width or self.front:  # the space stage mixes the channels
            el = er = max(el, er)
        ol, orr = el, er
        if self.xf:
            pairs = [(abs(d), abs(c)) for d, c in self.xf(w)]
            ol = max(hi * el + lo * er for hi, lo in pairs)
            orr = max(hi * er + lo * el for hi, lo in pairs)
        t = self._transient_bound(w) * self._space_bound(w)
        return max(ol * self.trim_l * t, orr * self.trim_r * t)

    def boost_db(self) -> float:
        """Same grid, centres and 0.1 dB margin as the firmware."""
        if not self.left and not self.right and not self.xf and not self.transient \
                and not self.width and not self.front:
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
