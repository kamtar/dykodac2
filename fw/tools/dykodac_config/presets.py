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


PK, LS, HS, HP, LP = (p.FILTER_PEAKING, p.FILTER_LOW_SHELF, p.FILTER_HIGH_SHELF, p.FILTER_HIGH_PASS,
                      p.FILTER_LOW_PASS)
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

    # --- Headphone corrections -------------------------------------------------
    # Measured corrections to the Harman over-ear target, taken from oratory1990's
    # measurements as published in the AutoEq project (results/oratory1990/over-ear).
    # The preamp from those files is not needed: the firmware removes the EQ's max
    # boost from the headroom automatically.
    _eq("HIFIMAN HE400se (stock)", "oratory1990 measurement -> Harman target (via AutoEq). Sub-bass shelf, "
        "fills the 2 kHz dip, softens the top octave.",
        (LS, B, 105.0, 6.7, 0.70), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (PK, B, 1619.0, 1.1, 3.53), (HS, B, 10000.0, -2.6, 0.70),
        (PK, B, 138.0, 0.7, 1.98), (PK, B, 64.0, -0.7, 1.95), (PK, B, 251.0, -0.4, 2.33),
        (PK, B, 29.0, 0.5, 2.63)),
    _eq("HIFIMAN HE400se (Supergrills)", "Grille-less / Supergrills HE400se: the stock oratory1990 correction "
        "adjusted by a measured stock-vs-Supergrills difference (same rig): fills the deeper 2.5-3.5 kHz "
        "recess and the 1 kHz dip, a touch more mid-bass, less of the extra 20 Hz rumble.",
        (LS, B, 105.0, 6.7, 0.7), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (PK, B, 1619.0, 1.1, 3.53), (HS, B, 10000.0, -2.6, 0.7),
        (PK, B, 90.0, 1.6, 1.0), (PK, B, 20.0, -3.0, 1.6), (PK, B, 1100.0, 1.5, 2.0),
        (PK, B, 3000.0, 3.2, 2.0)),
    _eq("Beyerdynamic DT 990 Pro", "oratory1990 measurement -> Harman target (via AutoEq). Big sub-bass "
        "lift, mid-bass and 6-8 kHz peaks tamed, strong top-octave cut.",
        (LS, B, 105.0, 10.1, 0.70), (PK, B, 541.0, 3.1, 0.52), (PK, B, 64.0, -7.4, 0.38),
        (PK, B, 7731.0, -4.6, 1.06), (PK, B, 640.0, 0.6, 2.47), (HS, B, 10000.0, -9.8, 0.70),
        (PK, B, 7136.0, 3.1, 2.08), (PK, B, 5897.0, -4.8, 5.30), (PK, B, 4193.0, 1.1, 1.51),
        (PK, B, 9436.0, 2.7, 3.53)),
    _eq("Beyerdynamic DT 990 Pro (gentle)", "Light version for people who like the DT 990 sound: only "
        "tames the 6-8 kHz treble peak and fills in a little sub-bass.",
        (LS, B, 105.0, 4.0, 0.70), (PK, B, 5900.0, -3.0, 4.0), (PK, B, 7700.0, -4.0, 1.4),
        (HS, B, 10000.0, -3.0, 0.70)),

    # --- ASMR ----------------------------------------------------------------
    # Meant for neutral (or already well-behaved) headphones; they replace the
    # headphone correction rather than stacking on top of it.
    _eq("ASMR: soft & sleepy", "Warmer and gentler: takes the edge off sharp mouth sounds, sibilance and "
        "crinkles. For relaxing or falling asleep at low volume.",
        (LS, B, 150.0, 2.0, 0.7), (PK, B, 6500.0, -3.5, 2.0), (HS, B, 10000.0, -4.0, 0.7)),
    _eq("ASMR: tingles / detail", "Brings out texture in tapping, brushing and whispers; cleans up "
        "mic-handling rumble. Can get intense on bright recordings.",
        (HP, B, 35.0, 0.0, 0.7071), (PK, B, 250.0, -1.5, 1.0), (PK, B, 4000.0, 2.0, 1.0),
        (HS, B, 8000.0, 3.0, 0.7)),
    _eq("ASMR: whisper clarity", "Removes the boomy proximity effect of close-mic'd whispering and makes the "
        "words easier to follow, with a mild de-ess.",
        (HP, B, 60.0, 0.0, 0.7071), (PK, B, 200.0, -3.0, 1.0), (PK, B, 3000.0, 2.5, 1.2),
        (PK, B, 7000.0, -2.5, 3.0)),
    _eq("ASMR: hiss reduction", "For noisy recordings (cheap mics, high gain): rolls off the hiss above "
        "~8 kHz. Dulls the sound, so use only when the noise bothers you.",
        (HS, B, 8000.0, -5.0, 0.7), (LP, B, 15000.0, 0.0, 0.7071)),
    _eq("ASMR: natural breathing", "Brings breaths and sighs forward (warm body ~650 Hz, presence ~1.6 kHz) "
        "without boosting plosive 'p' pops (cut below ~150 Hz), lip smacks/mouth clicks (dip at 3.5 kHz) "
        "or gushing air (shelf down above 9 kHz).",
        (HP, B, 80.0, 0.0, 0.7071), (PK, B, 120.0, -2.0, 0.9), (PK, B, 650.0, 2.0, 0.8),
        (PK, B, 1600.0, 1.5, 1.2), (PK, B, 3500.0, -2.0, 1.8), (HS, B, 9000.0, -2.0, 0.7)),

    # --- Headphone correction + ASMR ---------------------------------------------
    # The headphone correction above with an ASMR flavour folded in, squeezed into
    # the 10 bands: shelves are summed, the least important correction peaks are
    # dropped. Each stays within ~1.2 dB of running both EQs in full (20 Hz-20 kHz).
    _eq("HE400se + ASMR soft & sleepy", "HE400se (stock) correction plus 'ASMR: soft & sleepy'.",
        (LS, B, 105.0, 8.7, 0.70), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (PK, B, 1619.0, 1.1, 3.53), (HS, B, 10000.0, -6.6, 0.70),
        (PK, B, 138.0, 0.7, 1.98), (PK, B, 251.0, -0.4, 2.33), (PK, B, 29.0, 0.5, 2.63),
        (PK, B, 6500.0, -3.5, 2.0)),
    _eq("HE400se Supergrills + ASMR soft & sleepy", "HE400se (Supergrills) correction plus 'ASMR: soft & sleepy'.",
        (LS, B, 105.0, 8.7, 0.7), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (HS, B, 10000.0, -6.6, 0.7), (PK, B, 90.0, 1.6, 1.0),
        (PK, B, 20.0, -3.0, 1.6), (PK, B, 1100.0, 1.5, 2.0), (PK, B, 3000.0, 3.2, 2.0),
        (PK, B, 6500.0, -3.5, 2.0)),
    _eq("HE400se + ASMR tingles", "HE400se (stock) correction plus 'ASMR: tingles / detail'.",
        (LS, B, 105.0, 6.7, 0.70), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (PK, B, 1619.0, 1.1, 3.53), (HS, B, 10000.0, 0.4, 0.70),
        (PK, B, 138.0, 0.7, 1.98), (HP, B, 35.0, 0.0, 0.7071), (PK, B, 250.0, -1.5, 1.0),
        (PK, B, 4000.0, 2.0, 1.0)),
    _eq("HE400se Supergrills + ASMR tingles", "HE400se (Supergrills) correction plus 'ASMR: tingles / detail'.",
        (LS, B, 105.0, 6.7, 0.7), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (HS, B, 10000.0, 0.4, 0.7), (PK, B, 20.0, -3.0, 1.6),
        (PK, B, 1100.0, 1.5, 2.0), (PK, B, 3000.0, 3.2, 2.0), (HP, B, 35.0, 0.0, 0.7071),
        (PK, B, 4000.0, 2.0, 1.0)),
    _eq("HE400se + ASMR whisper clarity", "HE400se (stock) correction plus 'ASMR: whisper clarity'.",
        (LS, B, 105.0, 6.7, 0.70), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (PK, B, 1619.0, 1.1, 3.53), (HS, B, 10000.0, -2.6, 0.70),
        (HP, B, 60.0, 0.0, 0.7071), (PK, B, 200.0, -3.0, 1.0), (PK, B, 3000.0, 2.5, 1.2),
        (PK, B, 7000.0, -2.5, 3.0)),
    _eq("HE400se Supergrills + ASMR whisper clarity", "HE400se (Supergrills) correction plus 'ASMR: whisper clarity'.",
        (LS, B, 105.0, 6.7, 0.7), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (HS, B, 10000.0, -2.6, 0.7), (PK, B, 20.0, -3.0, 1.6), (PK, B, 3000.0, 3.2, 2.0),
        (HP, B, 60.0, 0.0, 0.7071), (PK, B, 200.0, -3.0, 1.0), (PK, B, 3000.0, 2.5, 1.2),
        (PK, B, 7000.0, -2.5, 3.0)),
    _eq("DT 990 Pro + ASMR soft & sleepy", "DT 990 Pro correction plus 'ASMR: soft & sleepy'.",
        (LS, B, 105.0, 12.1, 0.70), (PK, B, 541.0, 3.1, 0.52), (PK, B, 64.0, -7.4, 0.38),
        (PK, B, 7731.0, -4.6, 1.06), (HS, B, 10000.0, -13.8, 0.70), (PK, B, 7136.0, 3.1, 2.08),
        (PK, B, 5897.0, -4.8, 5.30), (PK, B, 4193.0, 1.1, 1.51), (PK, B, 9436.0, 2.7, 3.53),
        (PK, B, 6500.0, -3.5, 2.0)),
    _eq("DT 990 Pro + ASMR tingles", "DT 990 Pro correction plus 'ASMR: tingles / detail'.",
        (LS, B, 105.0, 10.1, 0.70), (PK, B, 541.0, 3.1, 0.52), (PK, B, 64.0, -7.4, 0.38),
        (PK, B, 640.0, 0.6, 2.47), (HS, B, 10000.0, -6.8, 0.70), (PK, B, 5897.0, -4.8, 5.30),
        (PK, B, 4193.0, 1.1, 1.51), (HP, B, 35.0, 0.0, 0.7071), (PK, B, 250.0, -1.5, 1.0),
        (PK, B, 4000.0, 2.0, 1.0)),
    _eq("DT 990 Pro + ASMR whisper clarity", "DT 990 Pro correction plus 'ASMR: whisper clarity'.",
        (LS, B, 105.0, 10.1, 0.70), (PK, B, 541.0, 3.1, 0.52), (PK, B, 64.0, -7.4, 0.38),
        (PK, B, 640.0, 0.6, 2.47), (HS, B, 10000.0, -9.8, 0.70), (PK, B, 5897.0, -4.8, 5.30),
        (HP, B, 60.0, 0.0, 0.7071), (PK, B, 200.0, -3.0, 1.0), (PK, B, 3000.0, 2.5, 1.2),
        (PK, B, 7000.0, -2.5, 3.0)),
    _eq("HE400se + ASMR natural breathing", "HE400se (stock) correction plus 'ASMR: natural breathing'.",
        (LS, B, 105.0, 6.7, 0.70), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 861.0, -2.3, 1.86),
        (PK, B, 101.0, -2.4, 0.32), (HS, B, 10000.0, -4.6, 0.70), (HP, B, 80.0, 0.0, 0.7071),
        (PK, B, 120.0, -2.0, 0.9), (PK, B, 650.0, 2.0, 0.8), (PK, B, 1600.0, 1.5, 1.2),
        (PK, B, 3500.0, -2.0, 1.8)),
    _eq("HE400se Supergrills + ASMR natural breathing", "HE400se (Supergrills) correction plus 'ASMR: natural breathing'.",
        (LS, B, 105.0, 6.7, 0.7), (PK, B, 1968.0, 5.1, 2.37), (PK, B, 101.0, -2.4, 0.32),
        (HS, B, 10000.0, -4.6, 0.7), (PK, B, 20.0, -3.0, 1.6), (PK, B, 1100.0, 1.5, 2.0),
        (PK, B, 3000.0, 3.2, 2.0), (HP, B, 80.0, 0.0, 0.7071), (PK, B, 1600.0, 1.5, 1.2),
        (PK, B, 3500.0, -2.0, 1.8)),
    _eq("DT 990 Pro + ASMR natural breathing", "DT 990 Pro correction plus 'ASMR: natural breathing'.",
        (LS, B, 105.0, 10.1, 0.70), (PK, B, 541.0, 3.1, 0.52), (PK, B, 64.0, -7.4, 0.38),
        (HS, B, 10000.0, -11.8, 0.70), (PK, B, 5897.0, -4.8, 5.30), (HP, B, 80.0, 0.0, 0.7071),
        (PK, B, 120.0, -2.0, 0.9), (PK, B, 650.0, 2.0, 0.8), (PK, B, 1600.0, 1.5, 1.2),
        (PK, B, 3500.0, -2.0, 1.8)),
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
