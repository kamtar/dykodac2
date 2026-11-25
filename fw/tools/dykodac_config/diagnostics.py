"""Decoders for the existing maintenance reports (usb/diagnostic_report.hpp,
usb/usb_device.hpp), shared with tools/read_diagnostics.py's layouts."""

from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field

DIAG_COMMAND = b"DYKO-DIAG-GET-01"
EVENT_COMMAND = b"DYKO-EVENT-GET-1"
PERF_COMMAND = b"DYKO-PERF-GET-01"
DIAG_MAGIC, EVENT_MAGIC, PERF_MAGIC = 0x32444B44, 0x56454B44, 0x46524550
RING_CAPACITY = 8 * 1024

_DIAG = struct.Struct("<IBBH9I5H10B")
_EVENT_HEADER = struct.Struct("<IBBBB")
_EVENT = struct.Struct("<BBHI")
_PERF = struct.Struct("<IBBHIIQIIIBBHII16x")

APP_STATES = ["BootSafe", "Initializing", "IdleMuted", "PreparingStream", "Playing",
              "SwitchingRate", "Stopping", "FaultMuted"]
START_STEPS = ["None", "RelayRelease", "ClockSettle", "DmaProgress", "Prefill", "Stable", "DacSettle", "Playing"]
STOP_REASONS = ["None", "HostStop", "Detached", "Suspended", "HostMuted", "UnsupportedRate", "UsbInit",
                "DacConfigure", "EngineStart", "DmaNoProgress", "Underrun", "DmaError", "DacUnmute",
                "FinalInvariant", "VolumeControl"]
EVENTS = ["None", "Mounted", "Detached", "Suspended", "Resumed", "Start", "Stop", "Rate", "Controls",
          "Diagnostics", "EventTrace", "DmaDiagnostics", "RefillTiming", "EnterRomBootloader", "BootInfo",
          "Config"]
CONTROLS = ["None", "SampleRate", "Mute", "Volume", "InterfaceAlternate"]


def _name(table, i):
    return table[i] if i < len(table) else f"#{i}"


@dataclass
class Diag:
    uptime_ms: int
    desired_rate: int
    active_rate: int
    feedback: float            # frames per 125 us microframe
    packets: int
    dropped_packets: int
    dropped_events: int
    dma_completions: int
    dma_errors: int
    underruns: int
    overflows: int
    padding_errors: int
    ring_fill: int
    packet_last: int
    packet_min: int
    packet_max: int
    start_step: str
    last_stop_step: str
    stop_reason: str
    app_state: str
    family_48k: bool
    alt1: bool
    stream_requested: bool
    host_muted: bool
    flags: dict = field(default_factory=dict)


@dataclass
class Perf:
    count: int
    average_us: float
    maximum_us: float
    last_us: float
    load_percent: float        # of a 1 ms refill period
    max_pending: int
    buffers: int
    coalesced: int
    drains: int


def decode_diag(raw: bytes) -> Diag | None:
    if len(raw) < 64:
        return None
    f = _DIAG.unpack(raw[:64])
    if f[0] != DIAG_MAGIC:
        return None
    state, flags = f[26], f[27]
    return Diag(
        uptime_ms=f[4], desired_rate=f[6], active_rate=f[7], feedback=f[8] / 65536.0, packets=f[9],
        dropped_packets=f[10], dropped_events=f[11], dma_completions=f[12], dma_errors=f[13], underruns=f[14],
        overflows=f[15], padding_errors=f[16], ring_fill=f[17], packet_last=f[18], packet_min=f[19],
        packet_max=f[20], start_step=_name(START_STEPS, f[21]), last_stop_step=_name(START_STEPS, f[22]),
        stop_reason=_name(STOP_REASONS, f[23]), app_state=_name(APP_STATES, state & 0x0F),
        family_48k=bool(state & 0x10), alt1=bool(state & 0x20), stream_requested=bool(state & 0x40),
        host_muted=bool(state & 0x80),
        flags={"USB mounted": bool(flags & 0x01), "Suspended": bool(flags & 0x02),
               "Clock stable": bool(flags & 0x04), "DAC muted": bool(flags & 0x10),
               "Relay connected": bool(flags & 0x20), "Engine running": bool(flags & 0x40),
               "UAC2 clock valid": bool(flags & 0x80)},
    )


def decode_perf(raw: bytes) -> Perf | None:
    if len(raw) < 64:
        return None
    f = _PERF.unpack(raw[:64])
    if f[0] != PERF_MAGIC:
        return None
    count, total, maximum, last, clock = f[5], f[6], f[7], f[8], f[9] or 500_000_000
    avg = total / count if count else 0.0
    us = 1e6 / clock
    return Perf(count, avg * us, maximum * us, last * us, avg * us / 10.0, f[10], f[11], f[13], f[14])


def decode_events(raw: bytes) -> list[str]:
    if len(raw) < 64:
        return []
    magic, _v, _s, count, _r = _EVENT_HEADER.unpack_from(raw)
    if magic != EVENT_MAGIC:
        return []
    out = []
    for i in range(min(count, 7)):
        ev, ctl, t, value = _EVENT.unpack_from(raw, _EVENT_HEADER.size + i * _EVENT.size)
        text = f"t={t:5d} ms  {_name(EVENTS, ev)}"
        if ctl:
            text += f"  {_name(CONTROLS, ctl)}"
        if ev == 7:
            text += f"  {value} Hz"
        elif ctl == 3:
            text += f"  {struct.unpack('<i', struct.pack('<I', value))[0] / 256:+.2f} dB"
        elif ctl:
            text += f"  {value}"
        out.append(text)
    return out


def query(dev, command: bytes, magic: int, timeout_s: float = 1.0) -> bytes | None:
    """Send one 16-byte maintenance command on a protocol.Device and wait for
    the 64-byte report whose first word is `magic`."""
    hd = dev._dev  # noqa: SLF001 - same HID handle, one request at a time
    while hd.read(64, 1):
        pass
    if hd.write(b"\0" + command.ljust(64, b"\0")) < 0:
        raise OSError("HID write failed")
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        raw = bytes(hd.read(64, 100))
        if len(raw) >= 64 and struct.unpack_from("<I", raw)[0] == magic:
            return raw
    return None
