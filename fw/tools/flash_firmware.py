#!/usr/bin/env python3
"""One-command USB flashing for DykoDAC2 (i.MX RT1011), no case opening.

Sequence (every step is driven automatically):

  1. Running firmware (1209:D2A3)  -- HID 'DYKO-ROM-BOOT-01' -->  reset
  2. Boot ROM serial downloader (1FC9:0145) -- sdphost loads NXP's RAM
     flashloader and jumps to it
  3. RAM flashloader (15A2:0073) -- blhost configures FlexSPI with the
     SFDP auto-detection, erases, writes everything except the first 4 KiB,
     writes the first 4 KiB (FCB + IVT) last, verifies, resets.

All flash programming runs from RAM/ROM, so there is no XIP conflict. If
power is lost mid-update, the boot header is either still erased or the
previous image is untouched, and the ROM serial downloader remains
reachable; rerun this script (it also starts from steps 2 or 3 directly).

Requirements (once):
    python -m pip install spsdk hidapi
and NXP's RT1010 'ivt_flashloader.bin' (the one MCU Boot Utility / MCUXpresso
Secure Provisioning Tool use), placed at tools/flashloader/ivt_flashloader.bin
or passed with --flashloader.
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import sysconfig
import tempfile
import time
from pathlib import Path

try:
    import hid
except ImportError:
    sys.exit("Missing hidapi. Install once with: python -m pip install spsdk hidapi")

APP_ID = (0x1209, 0xD2A3)
SDP_ID = (0x1FC9, 0x0145)          # RT1011 ROM serial downloader
FLASHLOADER_ID = (0x15A2, 0x0073)  # NXP i.MX RT RAM flashloader (MCU-Boot)
ENTER_ROM = b"DYKO-ROM-BOOT-01"

FLASH_BASE = 0x60000000
SECTOR = 0x1000
FCB_OFFSET = 0x400
FCB_SIZE = 0x200
HERE = Path(__file__).resolve().parent
DEFAULT_IMAGE = HERE.parent / "build" / "gmake2" / "bin" / "Release" / "dykodac2.bin"
DEFAULT_FLASHLOADER = HERE / "flashloader" / "ivt_flashloader.bin"


def present(ids: tuple[int, int]) -> bool:
    return bool(hid.enumerate(*ids))


def wait_for(ids: tuple[int, int], what: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if present(ids):
            time.sleep(0.5)  # let Windows finish binding the HID driver
            return
        time.sleep(0.2)
    sys.exit(f"Timed out waiting for {what} ({ids[0]:04X}:{ids[1]:04X}).")


def tool(name: str) -> list[str]:
    """Prefer SPSDK's console script from this Python, not an unrelated blhost.exe on PATH."""
    scripts = Path(sysconfig.get_path("scripts"))
    for candidate in (scripts / f"{name}.exe", scripts / name):
        if candidate.exists():
            return [str(candidate)]
    try:
        import spsdk  # noqa: F401
    except ImportError:
        sys.exit("Missing SPSDK. Install once with: python -m pip install spsdk")
    found = shutil.which(name)
    if found:
        return [found]
    return [sys.executable, "-m", f"spsdk.apps.{name}"]


def run(cmd: list[str]) -> None:
    print("  >", " ".join(Path(c).name if i == 0 else c for i, c in enumerate(cmd)))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        sys.exit(f"Command failed with exit code {result.returncode}.")


def usb_arg(ids: tuple[int, int]) -> str:
    return f"0x{ids[0]:04x},0x{ids[1]:04x}"


def find_ivt(data: bytes) -> tuple[int, int]:
    """Return (file offset, absolute address) of the i.MX IVT in a flashloader image."""
    for offset in range(0, min(len(data), 0x2000), 4):
        tag, length, version = data[offset], struct.unpack_from(">H", data, offset + 1)[0], data[offset + 3]
        if tag == 0xD1 and length == 0x20 and 0x40 <= version <= 0x4F:
            self_address = struct.unpack_from("<I", data, offset + 0x14)[0]
            return offset, self_address
    sys.exit("Flashloader file has no i.MX IVT; is it the RT1010 'ivt_flashloader.bin'?")


def request_rom(timeout: float) -> None:
    devices = [d for d in hid.enumerate(*APP_ID) if d.get("usage_page") == 0xFF00]
    if not devices:
        sys.exit("DykoDAC2 not found (1209:D2A3 maintenance HID) and no NXP boot device present.")
    device = hid.device()
    device.open_path(devices[0]["path"])
    try:
        if device.write(bytes([0]) + ENTER_ROM.ljust(64, b"\0")) < 0:
            sys.exit("Failed to send the ROM-boot request.")
    finally:
        device.close()
    print("Requested reboot into the ROM serial downloader.")
    wait_for(SDP_ID, "the RT1011 boot ROM", timeout)


def load_flashloader(path: Path, timeout: float) -> tuple[int, int]:
    data = path.read_bytes()
    ivt_offset, ivt_address = find_ivt(data)
    load_address = ivt_address - ivt_offset
    print(f"Loading flashloader to 0x{load_address:08X} (IVT 0x{ivt_address:08X}).")
    sdphost = tool("sdphost") + ["-u", usb_arg(SDP_ID)]
    run(sdphost + ["write-file", hex(load_address), str(path)])
    run(sdphost + ["jump-address", hex(ivt_address)])
    wait_for(FLASHLOADER_ID, "the NXP RAM flashloader", timeout)
    return load_address, len(data)


def program(image: bytes, scratch: int, flash_option: int, workdir: Path) -> None:
    blhost = tool("blhost") + ["-u", usb_arg(FLASHLOADER_ID), "-t", "30000"]
    option = workdir / "option.bin"
    body = workdir / "body.bin"
    head = workdir / "head.bin"
    readback = workdir / "readback.bin"
    option.write_bytes(struct.pack("<I", flash_option))
    body.write_bytes(image[SECTOR:])
    head.write_bytes(image[:SECTOR])
    erase_size = (len(image) + SECTOR - 1) // SECTOR * SECTOR

    # The image's FCB only carries the read sequence the ROM needs for XIP; it
    # has no erase/program commands, and using it hangs the flashloader. Let
    # the flashloader build a full configuration from the chip's SFDP table.
    print(f"Configuring FlexSPI NOR (option 0x{flash_option:08X}, SFDP auto-detect).")
    run(blhost + ["write-memory", hex(scratch), str(option)])
    run(blhost + ["configure-memory", "9", hex(scratch)])
    print(f"Erasing 0x{FLASH_BASE:08X}..0x{FLASH_BASE + erase_size:08X}.")
    run(blhost + ["flash-erase-region", hex(FLASH_BASE), hex(erase_size)])
    print("Writing application, then boot header last.")
    run(blhost + ["write-memory", hex(FLASH_BASE + SECTOR), str(body)])
    run(blhost + ["write-memory", hex(FLASH_BASE), str(head)])
    print("Verifying.")
    run(blhost + ["read-memory", hex(FLASH_BASE), hex(len(image)), str(readback)])
    if readback.read_bytes()[:len(image)] != image:
        sys.exit("VERIFY FAILED. The device stays in the flashloader; rerun this script.")
    run(blhost + ["reset"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("image", nargs="?", type=Path, default=DEFAULT_IMAGE,
                        help=f"raw NOR image at offset 0 (default: {DEFAULT_IMAGE})")
    parser.add_argument("--flashloader", type=Path, default=DEFAULT_FLASHLOADER,
                        help=f"NXP RT1010 ivt_flashloader.bin (default: {DEFAULT_FLASHLOADER})")
    parser.add_argument("--scratch", type=lambda v: int(v, 0), default=0x20202000,
                        help="RAM address for the flash option word handed to the flashloader")
    parser.add_argument("--flash-option", type=lambda v: int(v, 0), default=0xC0000002,
                        help="FlexSPI NOR option0: 0xC0000002 = QuadSPI SDR, SFDP, 50 MHz")
    parser.add_argument("--timeout", type=float, default=15.0, help="seconds to wait for each USB re-enumeration")
    args = parser.parse_args()

    image = args.image.read_bytes()
    if image[FCB_OFFSET:FCB_OFFSET + 4] != b"FCFB":
        sys.exit(f"{args.image} has no FlexSPI FCB at 0x400; expected a full NOR image based at 0x60000000.")
    if image[SECTOR:SECTOR + 4] != bytes((0xD1, 0x00, 0x20, 0x41)):
        sys.exit(f"{args.image} has no boot IVT at 0x1000.")
    if not args.flashloader.exists():
        sys.exit(f"Flashloader not found: {args.flashloader}\n"
                 "Copy NXP's RT1010 ivt_flashloader.bin (from MCU Boot Utility or MCUXpresso "
                 "Secure Provisioning Tool) there, or pass --flashloader PATH.")

    if present(FLASHLOADER_ID):
        print("Flashloader already running; programming directly.")
    else:
        if not present(SDP_ID):
            request_rom(args.timeout)
        load_address, size = load_flashloader(args.flashloader, args.timeout)
        if load_address <= args.scratch < load_address + size:
            sys.exit(f"--scratch 0x{args.scratch:08X} overlaps the flashloader; pick another RAM address.")

    with tempfile.TemporaryDirectory() as tmp:
        program(image, args.scratch, args.flash_option, Path(tmp))
    print(f"Done: {args.image.name} ({len(image)} bytes) flashed and verified; device is rebooting.")


if __name__ == "__main__":
    main()
