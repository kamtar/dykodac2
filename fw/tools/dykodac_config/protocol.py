"""DykoDAC2 HID configuration protocol (host side).

Mirrors usb/config_protocol.hpp and config/user_settings.hpp. Every packet is
one 64-byte HID report on the maintenance interface (usage page 0xFF00).
"""

from __future__ import annotations

import json
import struct
import time
from dataclasses import asdict, dataclass, field

VID = 0x1209
PID = 0xD2A3
USAGE_PAGE = 0xFF00

PROTOCOL_VERSION = 1
SETTINGS_MAGIC = 0x46434B44  # "DKCF"
SETTINGS_VERSION = 1
SETTINGS_SIZE = 192
EQ_BANDS = 10
CHUNK = 48
REQUEST_PREFIX = b"DYKO-CFG"
RESPONSE_MAGIC = 0x52434B44  # "DKCR"

OP_INFO, OP_READ, OP_WRITE, OP_APPLY, OP_SAVE, OP_LOAD, OP_DEFAULTS = range(1, 8)
OP_PEEK_ROM = 0x20

STATUS_NAMES = {
    0: "OK", 1: "bad request", 2: "bad range", 3: "invalid settings",
    4: "flash error", 5: "no saved settings in flash", 6: "flash API unavailable",
}
INVALID_NAMES = [
    "none", "header", "upsampler mode", "DAC filter", "crossfeed", "headroom", "DAC ceiling",
    "balance", "polarity", "reserved bytes", "band type", "band channels", "band frequency",
    "band gain", "band Q", "leveler", "loudness compensation", "proximity", "transient shaper",
]
FLASH_RESULT_NAMES = [
    "OK", "no record", "unavailable", "init failed", "erase failed", "program failed",
    "verify failed", "bad argument",
]
APP_STATES = [
    "BootSafe", "Initializing", "IdleMuted", "PreparingStream", "Playing",
    "SwitchingRate", "Stopping", "FaultMuted",
]

FLAG_FLASH_AVAILABLE = 0x01
FLAG_FLASH_RECORD = 0x02
FLAG_UNSAVED = 0x04
FLAG_STREAMING = 0x08
FLAG_BIT_EXACT = 0x10
FLAG_STAGING_DIRTY = 0x20

FILTER_OFF, FILTER_PEAKING, FILTER_LOW_SHELF, FILTER_HIGH_SHELF, FILTER_LOW_PASS, FILTER_HIGH_PASS, FILTER_NOTCH = range(7)
FILTER_NAMES = ["Off", "Peak", "Low shelf", "High shelf", "Low pass", "High pass", "Notch"]
CH_LEFT, CH_RIGHT, CH_BOTH = 1, 2, 3
LEVELER_NAMES = ["Off", "Slow (4 s / 30 s)", "Medium (1.5 s / 10 s)", "Fast (0.5 s / 3 s)"]
CROSSFEED_NAMES = ["Off", "bs2b default (700 Hz, 4.5 dB)", "Chu Moy (700 Hz, 6 dB)", "Jan Meier (650 Hz, 9.5 dB)",
                   "Natural (flat centre)", "Virtual speakers ±30°", "Virtual speakers ±45°",
                   "Spacious (keeps reverb)"]

# Device limits (config/user_settings.hpp).
MIN_HEADROOM_DB = -24.0
MIN_CEILING_DB = -40.0
MAX_BALANCE_DB = 12.0
MIN_FREQ, MAX_FREQ = 10.0, 20000.0
MAX_GAIN = 20.0
MIN_Q, MAX_Q = 0.1, 20.0
MIN_LEVELER_TARGET, MAX_LEVELER_TARGET = -36.0, -6.0
MIN_LEVELER_CUT, MAX_LEVELER_CUT = 1, 30
MIN_LOUDNESS_AMOUNT, MAX_LOUDNESS_AMOUNT = 10, 100
MIN_LOUDNESS_REFERENCE = -60.0
MIN_LOUDNESS_BOOST, MAX_LOUDNESS_BOOST = 1, 20
MIN_DETAIL_AMOUNT, MAX_DETAIL_AMOUNT = 10, 100
MIN_TRANSIENT_HZ, MAX_TRANSIENT_HZ = 1000, 8000  # 100 Hz steps on the wire
MAX_TRANSIENT_BOOST_DB = 10.0
DETAIL_PROXIMITY, DETAIL_TRANSIENT = 0x01, 0x02
# ISO 226 fit used by the firmware (audio/source_dsp.hpp).
LOUDNESS_BASS_HZ, LOUDNESS_BASS_Q, LOUDNESS_BASS_PER_DB = 111.0, 0.38, 0.56
LOUDNESS_TREBLE_HZ, LOUDNESS_TREBLE_Q, LOUDNESS_TREBLE_PER_DB = 10200.0, 1.22, 0.17

_HEADER = struct.Struct("<IHHBBBBhhhBBBBhBBhBBBB")
_BAND = struct.Struct("<BBHfff")
_REQUEST = struct.Struct("<8sBBHB3x48s")
_RESPONSE = struct.Struct("<IBBBBHBBI48s")
_INFO = struct.Struct("<HHHBBIIIhhffBBBBfff")
assert _HEADER.size + EQ_BANDS * _BAND.size == SETTINGS_SIZE
assert _REQUEST.size == 64 and _RESPONSE.size == 64


class ProtocolError(RuntimeError):
    pass


def _q88(db: float) -> int:
    return int(round(db * 256.0))


@dataclass
class EqBand:
    type: int = FILTER_OFF
    channels: int = CH_BOTH
    frequency_hz: float = 1000.0
    gain_db: float = 0.0
    q: float = 0.7071

    def active(self) -> bool:
        return self.type != FILTER_OFF


@dataclass
class Settings:
    upsampler_bypass: bool = False
    dac_slow_rolloff: bool = False
    eq_enabled: bool = False
    crossfeed: int = 0
    headroom_db: float = -3.25
    dac_ceiling_db: float = -9.0
    balance_db: float = 0.0  # > 0 attenuates left, < 0 attenuates right
    invert_left: bool = False
    invert_right: bool = False
    bands: list[EqBand] = field(default_factory=lambda: [EqBand() for _ in range(EQ_BANDS)])
    # Loudness leveler (0 = off, 1..3 = slow/medium/fast) and compensation.
    leveler_mode: int = 0
    leveler_target_db: float = -18.0  # LUFS
    leveler_max_cut_db: int = 12
    loudness_enabled: bool = False
    loudness_amount_pct: int = 70
    loudness_reference_db: float = -10.0  # host volume where the curve is flat
    loudness_max_boost_db: int = 12
    # Detail stage: proximity (interaural level difference expansion) and
    # the high-band transient shaper.
    proximity_enabled: bool = False
    proximity_amount_pct: int = 50  # 100 % doubles the level difference
    transient_enabled: bool = False
    transient_amount_pct: int = 50  # 100 % = MAX_TRANSIENT_BOOST_DB on onsets
    transient_crossover_hz: int = 2500

    # --- wire format -------------------------------------------------------
    def pack(self) -> bytes:
        polarity = (CH_LEFT if self.invert_left else 0) | (CH_RIGHT if self.invert_right else 0)
        out = _HEADER.pack(
            SETTINGS_MAGIC, SETTINGS_VERSION, SETTINGS_SIZE,
            1 if self.upsampler_bypass else 0, 1 if self.dac_slow_rolloff else 0,
            1 if self.eq_enabled else 0, self.crossfeed,
            _q88(self.headroom_db), _q88(round(self.dac_ceiling_db * 2.0) / 2.0), _q88(self.balance_db),
            polarity,
            (DETAIL_PROXIMITY if self.proximity_enabled else 0) | (DETAIL_TRANSIENT if self.transient_enabled else 0),
            self.leveler_mode, int(self.leveler_max_cut_db), _q88(self.leveler_target_db),
            1 if self.loudness_enabled else 0, int(self.loudness_amount_pct), _q88(self.loudness_reference_db),
            int(self.loudness_max_boost_db),
            int(self.proximity_amount_pct), int(self.transient_amount_pct),
            int(round(self.transient_crossover_hz / 100.0)),
        )
        bands = (self.bands + [EqBand() for _ in range(EQ_BANDS)])[:EQ_BANDS]
        for b in bands:
            out += _BAND.pack(b.type, b.channels if b.type else CH_BOTH, 0,
                              float(b.frequency_hz), float(b.gain_db), float(b.q))
        return out

    @classmethod
    def unpack(cls, data: bytes) -> "Settings":
        if len(data) != SETTINGS_SIZE:
            raise ProtocolError(f"settings block is {len(data)} bytes, expected {SETTINGS_SIZE}")
        (magic, version, size, bypass, slow, eq, xf, headroom, ceiling, balance, polarity, detail,
         lev_mode, lev_cut, lev_target, loud_on, loud_amount, loud_ref, loud_boost,
         prox_amount, tr_amount, tr_xover) = _HEADER.unpack_from(data, 0)
        if magic != SETTINGS_MAGIC or version != SETTINGS_VERSION or size != SETTINGS_SIZE:
            raise ProtocolError("device settings layout is not supported by this tool")
        bands = []
        for i in range(EQ_BANDS):
            t, ch, _res, f, g, q = _BAND.unpack_from(data, _HEADER.size + i * _BAND.size)
            bands.append(EqBand(t, ch, round(f, 4), round(g, 4), round(q, 4)))
        s = cls(bool(bypass), bool(slow), bool(eq), xf, headroom / 256.0, ceiling / 256.0,
                balance / 256.0, bool(polarity & CH_LEFT), bool(polarity & CH_RIGHT), bands)
        # Records from before these features hold zeros (= off): keep the
        # default parameters so switching them on starts somewhere sensible.
        s.leveler_mode = lev_mode
        if lev_cut:
            s.leveler_max_cut_db, s.leveler_target_db = lev_cut, lev_target / 256.0
        s.loudness_enabled = bool(loud_on)
        if loud_amount:
            s.loudness_amount_pct, s.loudness_reference_db, s.loudness_max_boost_db = \
                loud_amount, loud_ref / 256.0, loud_boost
        s.proximity_enabled = bool(detail & DETAIL_PROXIMITY)
        s.transient_enabled = bool(detail & DETAIL_TRANSIENT)
        if prox_amount or tr_amount or tr_xover:
            s.proximity_amount_pct, s.transient_amount_pct = prox_amount, tr_amount
            s.transient_crossover_hz = tr_xover * 100
        return s

    # --- preset files ------------------------------------------------------
    def to_json(self) -> str:
        return json.dumps({"dykodac2_settings": 1, **asdict(self)}, indent=2)

    @classmethod
    def from_json(cls, text: str) -> "Settings":
        raw = json.loads(text)
        raw.pop("dykodac2_settings", None)
        bands = [EqBand(**b) for b in raw.pop("bands", [])]
        s = cls(**raw)
        s.bands = (bands + [EqBand() for _ in range(EQ_BANDS)])[:EQ_BANDS]
        return s

    def problems(self) -> list[str]:
        """Same checks as settings::validate() on the device, in words."""
        p = []
        if not MIN_HEADROOM_DB <= self.headroom_db <= 0:
            p.append(f"Headroom must be between {MIN_HEADROOM_DB:g} and 0 dB")
        if not MIN_CEILING_DB <= self.dac_ceiling_db <= 0:
            p.append(f"DAC ceiling must be between {MIN_CEILING_DB:g} and 0 dB")
        if abs(self.balance_db) > MAX_BALANCE_DB:
            p.append(f"Balance must be within ±{MAX_BALANCE_DB:g} dB")
        if self.leveler_mode:
            if not MIN_LEVELER_TARGET <= self.leveler_target_db <= MAX_LEVELER_TARGET:
                p.append(f"Leveler target must be between {MIN_LEVELER_TARGET:g} and {MAX_LEVELER_TARGET:g} LUFS")
            if not MIN_LEVELER_CUT <= self.leveler_max_cut_db <= MAX_LEVELER_CUT:
                p.append(f"Leveler maximum cut must be {MIN_LEVELER_CUT}..{MAX_LEVELER_CUT} dB")
        if self.loudness_enabled:
            if not MIN_LOUDNESS_AMOUNT <= self.loudness_amount_pct <= MAX_LOUDNESS_AMOUNT:
                p.append(f"Loudness amount must be {MIN_LOUDNESS_AMOUNT}..{MAX_LOUDNESS_AMOUNT} %")
            if not MIN_LOUDNESS_REFERENCE <= self.loudness_reference_db <= 0:
                p.append(f"Loudness reference must be between {MIN_LOUDNESS_REFERENCE:g} and 0 dB")
            if not MIN_LOUDNESS_BOOST <= self.loudness_max_boost_db <= MAX_LOUDNESS_BOOST:
                p.append(f"Loudness maximum boost must be {MIN_LOUDNESS_BOOST}..{MAX_LOUDNESS_BOOST} dB")
        if self.proximity_enabled and not MIN_DETAIL_AMOUNT <= self.proximity_amount_pct <= MAX_DETAIL_AMOUNT:
            p.append(f"Proximity amount must be {MIN_DETAIL_AMOUNT}..{MAX_DETAIL_AMOUNT} %")
        if self.transient_enabled:
            if not MIN_DETAIL_AMOUNT <= self.transient_amount_pct <= MAX_DETAIL_AMOUNT:
                p.append(f"Transient amount must be {MIN_DETAIL_AMOUNT}..{MAX_DETAIL_AMOUNT} %")
            if not MIN_TRANSIENT_HZ <= self.transient_crossover_hz <= MAX_TRANSIENT_HZ:
                p.append(f"Transient crossover must be {MIN_TRANSIENT_HZ}..{MAX_TRANSIENT_HZ} Hz")
        for i, b in enumerate(self.bands, 1):
            if not b.active():
                continue
            if not MIN_FREQ <= b.frequency_hz <= MAX_FREQ:
                p.append(f"Band {i}: frequency {b.frequency_hz:g} Hz outside {MIN_FREQ:g}..{MAX_FREQ:g}")
            if abs(b.gain_db) > MAX_GAIN:
                p.append(f"Band {i}: gain {b.gain_db:g} dB outside ±{MAX_GAIN:g}")
            if not MIN_Q <= b.q <= MAX_Q:
                p.append(f"Band {i}: Q {b.q:g} outside {MIN_Q:g}..{MAX_Q:g}")
        return p


@dataclass
class Info:
    protocol: int
    settings_size: int
    settings_version: int
    eq_bands: int
    flags: int
    flash_sequence: int
    source_rate_hz: int
    output_rate_hz: int
    host_volume_db: float
    headroom_db: float
    dsp_boost_db: float
    effective_headroom_db: float
    app_state: str
    upsampler_bypass_active: bool
    last_flash_status: str
    leveler_gain_db: float = 0.0
    loudness_bass_db: float = 0.0
    loudness_treble_db: float = 0.0

    @property
    def flash_available(self) -> bool: return bool(self.flags & FLAG_FLASH_AVAILABLE)
    @property
    def flash_record(self) -> bool: return bool(self.flags & FLAG_FLASH_RECORD)
    @property
    def unsaved(self) -> bool: return bool(self.flags & FLAG_UNSAVED)
    @property
    def streaming(self) -> bool: return bool(self.flags & FLAG_STREAMING)
    @property
    def bit_exact(self) -> bool: return bool(self.flags & FLAG_BIT_EXACT)


class DeviceError(RuntimeError):
    pass


def find_devices():
    import hid
    return [d for d in hid.enumerate(VID, PID) if d.get("usage_page") == USAGE_PAGE]


class Device:
    """One open maintenance HID connection. Not thread-safe."""

    def __init__(self, path: bytes | None = None):
        import hid
        if path is None:
            found = find_devices()
            if not found:
                raise DeviceError("DykoDAC2 not found (USB 1209:D2A3, maintenance interface)")
            path = found[0]["path"]
        self._dev = hid.device()
        self._dev.open_path(path)
        self._tag = 0

    def close(self) -> None:
        try:
            self._dev.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # --- transport ---------------------------------------------------------
    def transact(self, opcode: int, offset: int = 0, length: int = 0, data: bytes = b"",
                 timeout_s: float = 2.0):
        self._tag = (self._tag + 1) & 0xFF
        request = _REQUEST.pack(REQUEST_PREFIX, opcode, self._tag, offset, length, data.ljust(CHUNK, b"\0"))
        # Drop stale IN reports (diagnostics replies of other tools etc.).
        while self._dev.read(64, 1):
            pass
        if self._dev.write(b"\0" + request) < 0:
            raise DeviceError("HID write failed (device unplugged?)")
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            raw = bytes(self._dev.read(64, 100))
            if len(raw) < 64:
                continue
            magic, op, tag, status, _r0, off, n, _r1, aux, payload = _RESPONSE.unpack(raw[:64])
            if magic != RESPONSE_MAGIC or tag != self._tag:
                continue
            return status, aux, payload[:n]
        raise DeviceError("no response from the DAC (firmware without configuration support?)")

    def _check(self, status: int, aux: int, what: str):
        if status == 0:
            return
        detail = STATUS_NAMES.get(status, f"status {status}")
        if status == 3 and aux < len(INVALID_NAMES):
            detail += f": {INVALID_NAMES[aux]}"
        raise DeviceError(f"{what} failed: {detail}")

    # --- operations --------------------------------------------------------
    def info(self) -> Info:
        status, aux, data = self.transact(OP_INFO)
        self._check(status, aux, "Info")
        f = _INFO.unpack_from(data.ljust(_INFO.size, b"\0"))
        info = Info(f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8] / 256.0, f[9] / 256.0,
                    f[10], f[11], APP_STATES[f[12]] if f[12] < len(APP_STATES) else str(f[12]),
                    bool(f[13]), FLASH_RESULT_NAMES[f[14]] if f[14] < len(FLASH_RESULT_NAMES) else str(f[14]),
                    f[16], f[17], f[18])
        if info.protocol != PROTOCOL_VERSION or info.settings_size != SETTINGS_SIZE:
            raise DeviceError(f"firmware protocol {info.protocol}/{info.settings_size} B is not supported")
        return info

    def read_settings(self) -> Settings:
        blob = b""
        for offset in range(0, SETTINGS_SIZE, CHUNK):
            n = min(CHUNK, SETTINGS_SIZE - offset)
            status, aux, data = self.transact(OP_READ, offset, n)
            self._check(status, aux, "Read")
            blob += data
        return Settings.unpack(blob)

    def write_settings(self, settings: Settings) -> None:
        """Upload to the staging buffer and apply ("write into RAM")."""
        blob = settings.pack()
        for offset in range(0, SETTINGS_SIZE, CHUNK):
            chunk = blob[offset:offset + CHUNK]
            status, aux, _ = self.transact(OP_WRITE, offset, len(chunk), chunk)
            self._check(status, aux, "Write")
        status, aux, _ = self.transact(OP_APPLY)
        self._check(status, aux, "Apply")

    def save(self) -> int:
        status, aux, _ = self.transact(OP_SAVE, timeout_s=5.0)
        self._check(status, aux, "Save to flash")
        return aux

    def load(self) -> Settings:
        status, aux, _ = self.transact(OP_LOAD)
        self._check(status, aux, "Load from flash")
        return self.read_settings()

    def defaults(self) -> Settings:
        status, aux, _ = self.transact(OP_DEFAULTS)
        self._check(status, aux, "Defaults")
        return self.read_settings()

    def peek_rom(self, address: int, length: int) -> bytes:
        status, aux, data = self.transact(OP_PEEK_ROM, 0, length, struct.pack("<I", address))
        self._check(status, aux, "ROM read")
        return data
