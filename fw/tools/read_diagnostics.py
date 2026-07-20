#!/usr/bin/env python3
"""Read and decode one DykoDAC2 maintenance diagnostic report."""

import json
import struct
import sys

try:
    import hid
except ImportError:
    sys.exit("Missing hidapi. Install once with: py -m pip install hidapi")

VID = 0x1209
PID = 0xD2A3
COMMAND = b"DYKO-DIAG-GET-01"
EVENT_COMMAND = b"DYKO-EVENT-GET-1"
REPORT = struct.Struct("<IBBH9I5H10B")
EVENT_HEADER = struct.Struct("<IBBBB")
EVENT_RECORD = struct.Struct("<BBHI")

APP_STATES = [
    "BootSafe", "Initializing", "IdleMuted", "PreparingStream", "Playing",
    "SwitchingRate", "Stopping", "FaultMuted",
]
START_STEPS = [
    "None", "RelayRelease", "ClockSettle", "DmaProgress", "Prefill",
    "Stable", "DacSettle", "Playing",
]
STOP_REASONS = [
    "None", "HostStop", "Detached", "Suspended", "HostMuted",
    "UnsupportedRate", "UsbInit", "DacConfigure", "EngineStart",
    "DmaNoProgress", "Underrun", "DmaError", "DacUnmute",
    "FinalInvariant", "VolumeControl",
]
EVENTS = [
    "None", "Mounted", "Detached", "Suspended", "Resumed", "Start", "Stop",
    "Rate", "Controls", "Diagnostics", "EventTrace", "BootloaderArm",
]
CONTROLS = ["None", "SampleRate", "Mute", "Volume", "InterfaceAlternate"]


def named(values, index):
    return values[index] if index < len(values) else f"Unknown({index})"


matches = [d for d in hid.enumerate(VID, PID) if d.get("usage_page") == 0xFF00]
if not matches:
    sys.exit("DykoDAC2 maintenance HID not found (expected 1209:D2A3)")

device = hid.device()
device.open_path(matches[0]["path"])
try:
    def query(command):
        output = bytes([0]) + command.ljust(64, b"\0")
        if device.write(output) < 0:
            raise OSError(f"HID request failed: {command!r}")
        response = bytes(device.read(REPORT.size, 2000))
        if len(response) != REPORT.size:
            raise OSError(f"Expected {REPORT.size} bytes, received {len(response)}")
        return response

    raw = query(COMMAND)
    event_raw = query(EVENT_COMMAND)
finally:
    device.close()

if len(raw) != REPORT.size:
    sys.exit(f"Expected {REPORT.size} diagnostic bytes, received {len(raw)}")

(
    magic, version, size, sequence,
    uptime_ms, last_set_cur_rate_hz, desired_rate_hz, active_rate_hz,
    feedback_16_16, usb_packet_count, dropped_packets, dropped_events,
    dma_completions, dma_errors, underruns, overflows, padding_errors,
    ring_fill_bytes, last_packet_bytes, minimum_packet_bytes,
    maximum_packet_bytes, start_step, last_stop_step, stop_reason,
    event_queue_depth, last_interface_alt, state, flags,
) = REPORT.unpack(raw)

if magic != 0x32444B44 or version != 1 or size != REPORT.size:
    sys.exit(f"Unrecognized diagnostic report: magic={magic:#x}, version={version}, size={size}")

app_state = state & 0x0F
decoded = {
    "sequence": sequence,
    "uptime_ms": uptime_ms,
    "rates_hz": {
        "last_set_cur": last_set_cur_rate_hz,
        "desired": desired_rate_hz,
        "active": active_rate_hz,
    },
    "clock": {
        "selected_family": "48k" if state & 0x10 else "44k1",
        "stable": bool(flags & 0x04),
        "raw_oscillator_select_high": bool(flags & 0x08),
        "uac2_clock_valid": bool(flags & 0x80),
        "feedback_16_16": feedback_16_16,
        "feedback_frames_per_microframe": feedback_16_16 / 65536.0,
    },
    "usb": {
        "mounted": bool(flags & 0x01),
        "suspended": bool(flags & 0x02),
        "last_interface_alt": last_interface_alt,
        "alt_1_selected": bool(state & 0x20),
        "stream_requested": bool(state & 0x40),
        "packet_count": usb_packet_count,
        "packet_bytes": {"last": last_packet_bytes, "minimum": minimum_packet_bytes, "maximum": maximum_packet_bytes},
        "padding_errors": padding_errors,
        "dropped_packets": dropped_packets,
        "dropped_events": dropped_events,
        "event_queue_depth": event_queue_depth,
    },
    "playback": {
        "app_state": named(APP_STATES, app_state),
        "start_step": named(START_STEPS, start_step),
        "last_stop_step": named(START_STEPS, last_stop_step),
        "stop_reason": named(STOP_REASONS, stop_reason),
        "host_muted": bool(state & 0x80),
        "dac_muted": bool(flags & 0x10),
        "relay_connected": bool(flags & 0x20),
        "engine_running": bool(flags & 0x40),
        "ring_fill_bytes": ring_fill_bytes,
        "dma_completions": dma_completions,
        "dma_errors": dma_errors,
        "underruns": underruns,
        "overflows": overflows,
    },
}

event_magic, event_version, event_size, event_count, _ = EVENT_HEADER.unpack_from(event_raw)
if event_magic != 0x56454B44 or event_version != 1 or event_size != REPORT.size or event_count > 7:
    sys.exit("Unrecognized USB event-trace report")
recent_events = []
for index in range(event_count):
    event, control, timestamp_ms_mod_65536, value = EVENT_RECORD.unpack_from(
        event_raw, EVENT_HEADER.size + index * EVENT_RECORD.size
    )
    item = {
        "timestamp_ms_mod_65536": timestamp_ms_mod_65536,
        "event": named(EVENTS, event),
        "control": named(CONTROLS, control),
        "value": value,
    }
    if control == 3 and value & 0x80000000:
        item["signed_value"] = value - 0x100000000
    recent_events.append(item)
decoded["usb"]["recent_events_oldest_to_newest"] = recent_events

print(json.dumps(decoded, indent=2))
