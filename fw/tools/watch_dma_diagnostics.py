#!/usr/bin/env python3
"""Continuously display the latched/live DykoDAC2 SAI/eDMA state."""

import json
import struct
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Missing hidapi. Install once with: py -m pip install hidapi")

VID, PID = 0x1209, 0xD2A3
COMMAND = b"DYKO-DMA-GET-001"
REPORT = struct.Struct("<IBBH11I6H")
interval = float(sys.argv[1]) if len(sys.argv) > 1 else 0.25


def decode(raw):
    values = REPORT.unpack(raw)
    (magic, version, size, sequence, captured_ms, chcfg, es, saddr, daddr,
     nbytes, tcsr, tcr1, tcr2, tcr3, tfr0, erq, interrupt, hrs, citer_raw,
     biter_raw, csr) = values
    if (magic, version, size) != (0x414D4444, 1, REPORT.size):
        raise ValueError("unrecognized DMA diagnostic report")
    citer, biter = citer_raw & 0x7FFF, biter_raw & 0x7FFF
    path = ((chcfg & 0x7F) == 20 and bool(chcfg & 0x80000000) and
            bool(erq & 1) and bool(tcsr & 1) and bool(tcr3 & 0x10000))
    if es:
        assessment = "eDMA error"
    elif interrupt & 1 or csr & 0x80:
        assessment = "major loop complete; inspect IRQ path"
    elif biter and citer < biter:
        assessment = "DMA progressed then stalled; inspect SAI clock/FIFO"
    elif path:
        assessment = "request path enabled, but no minor-loop service"
    else:
        assessment = "DMA request path not fully enabled"
    return {
        "seq": sequence, "captured_ms": captured_ms, "assessment": assessment,
        "chcfg0": f"0x{chcfg:08x}", "es": f"0x{es:08x}",
        "erq0": bool(erq & 1), "hrs0": bool(hrs & 1), "int0": bool(interrupt & 1),
        "citer": citer, "biter": biter, "nbytes": nbytes,
        "saddr": f"0x{saddr:08x}", "daddr": f"0x{daddr:08x}", "csr": f"0x{csr:04x}",
        "tcsr": f"0x{tcsr:08x}", "frde": bool(tcsr & 1), "frf": bool(tcsr & 0x10000),
        "fef": bool(tcsr & 0x40000), "bce": bool(tcsr & 0x10000000), "te": bool(tcsr & 0x80000000),
        "watermark": tcr1 & 0x1F, "tcr2": f"0x{tcr2:08x}", "tce0": bool(tcr3 & 0x10000),
        "fifo_rfp": tfr0 & 0x3F, "fifo_wfp": (tfr0 >> 16) & 0x3F,
    }


matches = [d for d in hid.enumerate(VID, PID) if d.get("usage_page") == 0xFF00]
if not matches:
    sys.exit("DykoDAC2 maintenance HID not found (expected 1209:D2A3)")
device = hid.device()
device.open_path(matches[0]["path"])
try:
    while True:
        output = bytes([0]) + COMMAND.ljust(64, b"\0")
        if device.write(output) < 0:
            raise OSError("HID request failed")
        raw = bytes(device.read(REPORT.size, 2000))
        if len(raw) != REPORT.size:
            raise OSError(f"expected {REPORT.size} bytes, received {len(raw)}")
        print(json.dumps(decode(raw), separators=(",", ":")), flush=True)
        time.sleep(interval)
except KeyboardInterrupt:
    pass
finally:
    device.close()
