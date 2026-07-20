#!/usr/bin/env python3
"""Request DykoDAC2 to reboot into the i.MX RT1011 ROM USB SDP loader."""

import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Missing hidapi. Install once with: py -m pip install hidapi")

VID = 0x1209
PID = 0xD2A3
COMMAND = b"DYKO-ROM-BOOT-01"

matches = [d for d in hid.enumerate(VID, PID) if d.get("usage_page") == 0xFF00]
if not matches:
    sys.exit("DykoDAC2 maintenance HID not found (expected 1209:D2A3)")

device = hid.device()
device.open_path(matches[0]["path"])
try:
    # Report ID 0 followed by the command and zero padding for the 64-byte
    # bidirectional maintenance report declared by the HID descriptor.
    report = bytes([0]) + COMMAND.ljust(64, b"\0")
    if device.write(report) < 0:
        raise OSError("first HID report failed")
    time.sleep(0.1)
    if device.write(report) < 0:
        raise OSError("confirmation HID report failed")
finally:
    device.close()

print("ROM serial downloader requested; wait for the NXP USB HID device.")
