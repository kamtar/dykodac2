#!/usr/bin/env python3
"""One-command DykoDAC2 A/B update over driverless USB HID."""

import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

try:
    import hid
except ImportError:
    sys.exit("Missing hidapi. Install once with: py -m pip install hidapi")

VID, APP_PID, UPDATER_PID = 0x1209, 0xD2A3, 0xD2A4
PACKAGE = struct.Struct("<8sIIII")
REPORT = struct.Struct("<IBBBBI48sI")
MAGIC = 0x554B5944


def find(pid: int):
    matches = hid.enumerate(VID, pid)
    return matches[0] if matches else None


def wait_for(pid: int, seconds: float):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        match = find(pid)
        if match:
            return match
        time.sleep(0.25)
    return None


def enter_updater() -> None:
    match = find(APP_PID)
    if not match:
        if find(UPDATER_PID):
            return
        raise RuntimeError("normal firmware and updater HID are both absent")
    device = hid.device(); device.open_path(match["path"])
    try:
        report = bytes([0]) + b"DYKO-FW-UPDATE01".ljust(64, b"\0")
        # The installed application intentionally requires two matching reports
        # within two seconds. Send several copies far enough apart for its main
        # loop to consume them. A disconnect/write error means reset started.
        sent = 0
        for _ in range(6):
            try:
                if device.write(report) > 0:
                    sent += 1
            except OSError:
                break
            time.sleep(0.30)
        if sent < 2 and not find(UPDATER_PID):
            raise RuntimeError("normal firmware did not accept updater request")
    finally:
        device.close()


class Updater:
    def __init__(self, path):
        self.device = hid.device(); self.device.open_path(path); self.sequence = 0

    def close(self): self.device.close()

    def command(self, command: int, offset=0, data=b"", timeout=15000):
        self.sequence = (self.sequence + 1) & 0xFF
        if len(data) > 48: raise ValueError("HID payload too large")
        packet = bytearray(REPORT.pack(MAGIC, command, self.sequence, 0, len(data), offset, data.ljust(48, b"\0"), 0))
        struct.pack_into("<I", packet, 60, zlib.crc32(packet[:60]) & 0xFFFFFFFF)
        if self.device.write(bytes([0]) + packet) < 0: raise OSError("HID write failed")
        raw = bytes(self.device.read(64, timeout))
        if len(raw) != 64: raise TimeoutError(f"updater did not respond to command {command}")
        magic, reply_cmd, seq, status, length, next_offset, payload, crc = REPORT.unpack(raw)
        check = bytearray(raw); struct.pack_into("<I", check, 60, 0)
        if magic != MAGIC or reply_cmd != command or seq != self.sequence or zlib.crc32(check[:60]) & 0xFFFFFFFF != crc:
            raise RuntimeError("invalid updater reply")
        if status != 0: raise RuntimeError(f"updater error {status}, accepted offset {next_offset}")
        return next_offset, payload


def load_package(path: Path):
    raw = path.read_bytes(); magic, fmt, version, size_a, size_b = PACKAGE.unpack_from(raw)
    if magic != b"DYKUPKG1" or fmt != 1: raise ValueError("not a DykoDAC2 update package")
    pos = PACKAGE.size; a = raw[pos:pos + size_a]; pos += size_a; b = raw[pos:pos + size_b]
    if len(a) != size_a or len(b) != size_b: raise ValueError("truncated update package")
    return version, (a, b)


def main():
    parser = argparse.ArgumentParser(); parser.add_argument("package", type=Path); args = parser.parse_args()
    version, images = load_package(args.package)
    enter_updater(); match = wait_for(UPDATER_PID, 30)
    if not match:
        if find(APP_PID):
            raise SystemExit("device stayed in normal firmware; updater request was not acted on")
        raise SystemExit("device reset but did not enumerate as firmware updater")
    updater = Updater(match["path"])
    try:
        _, info = updater.command(1)
        active = info[0]
        if active not in (0, 1): raise RuntimeError(f"invalid active slot {active}")
        jedec = struct.unpack_from("<I", info, 8)[0] & 0xFFFFFF
        print(f"Updater JEDEC ID: {jedec:06X}")
        target = 1 - active; image = images[target]
        print(f"Updating slot {'AB'[target]} to version {version} ({len(image)} bytes)")
        updater.command(2, data=struct.pack("<I", len(image)), timeout=60000)
        offset = 0
        while offset < len(image):
            page_remaining = 256 - (offset & 255)
            count = min(48, page_remaining, len(image) - offset)
            next_offset, _ = updater.command(3, offset, image[offset:offset + count])
            if next_offset != offset + count: raise RuntimeError("updater offset mismatch")
            offset = next_offset
            if offset % 4096 == 0 or offset == len(image): print(f"  {100 * offset // len(image):3d}%")
        updater.command(4, timeout=30000)
        try: updater.command(6)
        except (TimeoutError, OSError): pass
    finally:
        updater.close()
    if not wait_for(APP_PID, 15): raise SystemExit("trial firmware did not enumerate")
    print("Trial firmware is running. It will self-confirm after 60 healthy seconds;")
    print("three unconfirmed resets automatically return to the previous slot.")


if __name__ == "__main__":
    main()
