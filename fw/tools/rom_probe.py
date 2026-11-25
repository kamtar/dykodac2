#!/usr/bin/env python3
"""Debug ROM serial-downloader entry on DykoDAC2 without a debug probe.

    python tools/rom_probe.py info
    python tools/rom_probe.py enter [--arg 0xEB100000] [--method reset|direct] [--wait 20]

'enter' triggers ROM entry, watches USB for the ROM (1FC9:0145), and if the
application comes back instead, prints its boot info (reset reason, SNVS
breadcrumbs) so we can see where the handover stopped.
"""

import argparse
import struct
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Missing hidapi: python -m pip install hidapi")

APP = (0x1209, 0xD2A3)
ROM = (0x1FC9, 0x0145)
FLASHLOADER = (0x15A2, 0x0073)
INFO = struct.Struct("<IBBHI4II4I16s")
BREADCRUMBS = {
    0xB0000001: "RequestStored (reset issued, request not seen at next boot)",
    0xB0000002: "RequestSeen (stopped before ROM call)",
    0xB0000003: "CallingRom (ROM was called, then something reset the chip)",
    0xB0000004: "RomReturned (ROM call returned or API pointer invalid)",
    0xB0000013: "DirectCallingRom (direct ROM call made, then a reset)",
    0xB0000014: "DirectRomReturned (direct ROM call returned or API invalid)",
}
SRSR_BITS = ["ipp_reset_b (POR)", "lockup_sysresetreq", "csu_reset_b", "ipp_user_reset_b",
             "wdog_rst_b", "jtag_rst_b", "jtag_sw_rst", "wdog3_rst_b", "tempsense_rst_b"]


def app_device():
    found = [d for d in hid.enumerate(*APP) if d.get("usage_page") == 0xFF00]
    if not found:
        return None
    dev = hid.device()
    dev.open_path(found[0]["path"])
    return dev


def send(dev, payload: bytes) -> None:
    if dev.write(bytes([0]) + payload.ljust(64, b"\0")) < 0:
        raise OSError("HID write failed")


def read_info() -> bool:
    dev = app_device()
    if not dev:
        print("Application HID not present.")
        return False
    try:
        send(dev, b"DYKO-BOOT-INFO-1")
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            data = bytes(dev.read(64, 200))
            if len(data) >= 64 and data[:4] == b"DKBI":
                break
        else:
            print("No boot-info reply (firmware without DYKO-BOOT-INFO-1?).")
            return False
    finally:
        dev.close()
    magic, ver, size, _, srsr, g0, g1, g2, g3, tree, w0, w1, w2, w3, copyright = INFO.unpack(data[:64])
    bits = [name for i, name in enumerate(SRSR_BITS) if srsr & (1 << i)]
    print(f"reset reason SRSR=0x{srsr:08X}: {', '.join(bits) or 'none'}")
    print(f"mailbox at boot: arg=0x{g0:08X} breadcrumb=0x{g1:08X} magic=0x{g2:08X}/0x{g3:08X}")
    print(f"breadcrumb: {BREADCRUMBS.get(g1, 'none / unknown')}")
    print(f"ROM API tree *0x0020001C = 0x{tree:08X}")
    print(f"  version=0x{w0:08X} copyright=0x{w1:08X} runBootloader=0x{w2:08X} next=0x{w3:08X}")
    print(f"  copyright text: {copyright.split(b'\\0')[0].decode('ascii', 'replace')!r}")
    return True


def enter(argument: int, method: int, wait: float) -> None:
    dev = app_device()
    if not dev:
        sys.exit("Application HID not present.")
    try:
        if argument == 0xEB100000 and method == 0:
            send(dev, b"DYKO-ROM-BOOT-01")
        else:
            send(dev, b"DYKO-ROM-BOOT-XX" + struct.pack("<IB", argument, method))
    finally:
        dev.close()
    print(f"Sent ROM entry: arg=0x{argument:08X} method={'direct' if method else 'reset'}; watching USB...")
    start = time.monotonic()
    seen_gone = False
    while time.monotonic() - start < wait:
        t = time.monotonic() - start
        if hid.enumerate(*ROM):
            print(f"[{t:5.1f}s] ROM serial downloader 1FC9:0145 PRESENT -- entry works.")
            return
        if hid.enumerate(*FLASHLOADER):
            print(f"[{t:5.1f}s] Flashloader 15A2:0073 present.")
            return
        app_now = bool(hid.enumerate(*APP))
        if not app_now and not seen_gone:
            seen_gone = True
            print(f"[{t:5.1f}s] application detached")
        if app_now and seen_gone:
            print(f"[{t:5.1f}s] application came back instead of the ROM")
            time.sleep(1.0)
            read_info()
            return
        time.sleep(0.1)
    print(f"Nothing after {wait:.0f}s: no ROM, application {'absent' if seen_gone else 'never detached'}.")
    if not seen_gone:
        read_info()


def main() -> None:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    e = sub.add_parser("enter")
    e.add_argument("--arg", type=lambda v: int(v, 0), default=0xEB100000)
    e.add_argument("--method", choices=["reset", "direct"], default="reset")
    e.add_argument("--wait", type=float, default=20.0)
    a = p.parse_args()
    if a.cmd == "info":
        read_info()
    else:
        enter(a.arg, 1 if a.method == "direct" else 0, a.wait)


if __name__ == "__main__":
    main()
