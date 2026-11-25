"""EQ preset library for the configurator.

Presets live only on the PC and contain only the EQ (enable + bands). The
DAC holds exactly one settings set (RAM + flash); applying a preset never
touches reconstruction, headroom, ceiling, balance or crossfeed.
"""

from __future__ import annotations

import copy
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

from . import protocol as p


def _pad(bands: list[p.EqBand]) -> list[p.EqBand]:
    return (list(bands) + [p.EqBand() for _ in range(p.EQ_BANDS)])[:p.EQ_BANDS]


@dataclass
class Preset:
    name: str
    bands: list[p.EqBand] = field(default_factory=list)
    eq_enabled: bool = True
    description: str = ""
    builtin: bool = False

    def apply_to(self, current: p.Settings) -> p.Settings:
        out = copy.deepcopy(current)
        out.eq_enabled = self.eq_enabled
        out.bands = copy.deepcopy(_pad(self.bands))
        return out

    def matches(self, s: p.Settings) -> bool:
        """True when `s` runs exactly this EQ (off bands' parameters ignored)."""
        def key(enabled, bands):
            if not enabled:
                return ()
            return tuple((b.type, b.channels, round(b.frequency_hz, 3), round(b.gain_db, 3), round(b.q, 4))
                         for b in _pad(bands) if b.type != p.FILTER_OFF)
        return key(self.eq_enabled, self.bands) == key(s.eq_enabled, s.bands)

    def to_json(self) -> str:
        active = [b for b in self.bands if b.type != p.FILTER_OFF]
        return json.dumps({"dykodac2_eq_preset": 1, "name": self.name, "description": self.description,
                           "eq_enabled": self.eq_enabled,
                           "bands": [b.__dict__ for b in active]}, indent=2)

    @classmethod
    def from_json(cls, text: str, fallback_name: str = "Imported") -> "Preset":
        raw = json.loads(text)
        bands = [p.EqBand(**b) for b in raw.get("bands", [])]
        # Also accepts full settings exports: only their EQ part is used.
        return cls(raw.get("name") or raw.get("preset_name") or fallback_name, _pad(bands),
                   bool(raw.get("eq_enabled", True)),
                   raw.get("description") or raw.get("preset_description") or "")


PK, LS, HS, HP = p.FILTER_PEAKING, p.FILTER_LOW_SHELF, p.FILTER_HIGH_SHELF, p.FILTER_HIGH_PASS
B = p.CH_BOTH


def _eq(name: str, description: str, *bands: tuple, enabled: bool = True) -> Preset:
    return Preset(name, _pad([p.EqBand(*b) for b in bands]), enabled, description, builtin=True)


BUILTIN: list[Preset] = [
    _eq("Flat", "No equalisation: the EQ is off and nothing is computed.", enabled=False),
    _eq("Gentle warmth", "A touch more body and a slightly softer top. Subtle; good for long sessions.",
        (LS, B, 120.0, 2.0, 0.7), (HS, B, 9000.0, -1.5, 0.7)),
    _eq("Tame bright treble", "For bright headphones/speakers or harsh recordings: shelf down the top, "
        "dip the common 8-9 kHz peak.", (HS, B, 6000.0, -3.0, 0.6), (PK, B, 8500.0, -2.0, 2.0)),
    _eq("Reduce sibilance", "Narrow cut where 's' and 't' sounds pile up.", (PK, B, 6500.0, -4.0, 3.0)),
    _eq("Headphone bass shelf (Harman-style)", "The ~+5 dB sub-bass shelf of the Harman headphone target.",
        (LS, B, 105.0, 5.0, 0.7)),
    _eq("Loudness (quiet listening)", "Lifts lows and highs for low volume; switch back at normal levels.",
        (LS, B, 80.0, 6.0, 0.6), (HS, B, 10000.0, 3.0, 0.6)),
    _eq("Vocal presence", "Brings voices forward and clears low-mid mud.",
        (PK, B, 300.0, -1.5, 1.0), (PK, B, 2500.0, 2.5, 1.0)),
    _eq("Treble air", "Gentle lift above 12 kHz for dull or veiled transducers.", (HS, B, 12000.0, 2.5, 0.7)),
    _eq("Fun V-shape", "Punchy bass and sparkle with scooped mids. Coloured on purpose.",
        (LS, B, 90.0, 4.0, 0.7), (PK, B, 1000.0, -2.0, 0.8), (HS, B, 8000.0, 3.0, 0.7)),
    _eq("Speakers: room-boom cut", "Typical room-mode hump; tune the frequency by ear (40-120 Hz).",
        (PK, B, 80.0, -4.0, 2.0)),
    _eq("Speakers: subsonic filter", "Removes inaudible <20 Hz content that wastes woofer excursion.",
        (HP, B, 20.0, 0.0, 0.7071)),
    _eq("Speakers: desk / nearfield", "Cuts the desk-bounce bump, lifts presence slightly, subsonic filter.",
        (PK, B, 160.0, -3.0, 1.4), (PK, B, 3000.0, 1.0, 1.0), (HP, B, 25.0, 0.0, 0.7071)),
]


def user_dir() -> Path:
    base = Path(os.environ.get("APPDATA", Path.home())) / "DykoDAC2" / "eq-presets"
    base.mkdir(parents=True, exist_ok=True)
    return base


def _filename(name: str) -> str:
    return (re.sub(r"[^\w\- .()]+", "_", name).strip() or "preset") + ".json"


def path_for(name: str) -> Path:
    return user_dir() / _filename(name)


def load_user() -> list[Preset]:
    out = []
    for path in sorted(user_dir().glob("*.json"), key=lambda q: q.stem.lower()):
        try:
            out.append(Preset.from_json(path.read_text(encoding="utf-8"), path.stem))
        except Exception:  # noqa: BLE001 - skip unreadable files
            continue
    return out


def save_user(preset: Preset) -> Path:
    path = path_for(preset.name)
    path.write_text(preset.to_json(), encoding="utf-8")
    return path


def delete_user(name: str) -> None:
    path = path_for(name)
    if path.exists():
        path.unlink()


def rename_user(old: str, new: str) -> None:
    src = path_for(old)
    pr = Preset.from_json(src.read_text(encoding="utf-8"), old)
    pr.name = new
    save_user(pr)
    if path_for(new) != src:
        src.unlink()
