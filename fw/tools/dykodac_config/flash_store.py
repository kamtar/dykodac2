"""Save settings to the DAC's QSPI NOR through the ROM + RAM flashloader.

The RT1011 has no EEPROM and the firmware never erases or programs the flash
it executes from. Instead, exactly like a firmware update
(tools/flash_firmware.py, docs/firmware-update.md):

  1. running firmware (1209:D2A3) -- HID 'DYKO-ROM-BOOT-01' --> reset
  2. ROM serial downloader (1FC9:0145): load NXP's RAM flashloader, jump
  3. flashloader (15A2:0073): configure FlexSPI from SFDP, read both
     settings sectors, erase+program the one NOT holding the newest record,
     read it back, reset
  4. firmware boots and loads the newest valid record (A/B, CRC-32)

Only the two settings sectors at 0x600FE000..0x600FFFFF are ever erased; the
image ends below them (linker ASSERT). A power loss at any point leaves the
previous record intact, and the boot ROM stays reachable for a retry.
"""

from __future__ import annotations

import struct
import sys
import time
import zlib
from pathlib import Path
from typing import Callable

import hid

from . import protocol as p

SDP_ID = (0x1FC9, 0x0145)
FLASHLOADER_ID = (0x15A2, 0x0073)
ENTER_ROM = b"DYKO-ROM-BOOT-01"

FLASH_BASE = 0x60000000
SECTOR = 0x1000
REGION = FLASH_BASE + 0xFE000
SECTORS = 2
RECORD_MAGIC = 0x54534B44  # "DKST"
_RECORD_HEADER = struct.Struct("<IIII")
FLASH_OPTION = 0xC0000002  # QuadSPI SDR, SFDP auto-detect, 50 MHz (as flash_firmware.py)
SCRATCH = 0x20202000       # RAM for the option word; below the flashloader at 0x20205800


class FlashStoreError(RuntimeError):
    pass


def _resource_dir() -> Path:
    # PyInstaller one-file bundles unpack to sys._MEIPASS.
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent.parent))


def default_flashloader() -> Path | None:
    candidates = [
        _resource_dir() / "flashloader" / "ivt_flashloader.bin",
        Path(sys.executable).resolve().parent / "ivt_flashloader.bin",
        Path(__file__).resolve().parent.parent / "flashloader" / "ivt_flashloader.bin",
    ]
    return next((c for c in candidates if c.exists()), None)


def make_record(settings_blob: bytes, sequence: int) -> bytes:
    if len(settings_blob) != p.SETTINGS_SIZE:
        raise FlashStoreError("settings block has the wrong size")
    crc = zlib.crc32(settings_blob) & 0xFFFFFFFF
    record = _RECORD_HEADER.pack(RECORD_MAGIC, sequence & 0xFFFFFFFF, p.SETTINGS_SIZE, crc) + settings_blob
    return record + b"\xFF" * (SECTOR - len(record))


def parse_record(sector: bytes) -> tuple[int, bytes] | None:
    magic, sequence, length, crc = _RECORD_HEADER.unpack_from(sector, 0)
    blob = sector[_RECORD_HEADER.size:_RECORD_HEADER.size + p.SETTINGS_SIZE]
    if magic != RECORD_MAGIC or length != p.SETTINGS_SIZE or zlib.crc32(blob) & 0xFFFFFFFF != crc:
        return None
    return sequence, blob


def choose_target(sectors: list[bytes]) -> tuple[int, int]:
    """(sector index to write, next sequence): never overwrite the newest record."""
    newest = None
    for i, data in enumerate(sectors):
        rec = parse_record(data)
        # Same wrap-safe "newer" test as the firmware: (int32)(a - b) > 0.
        if rec and (newest is None or 0 < ((rec[0] - newest[1]) & 0xFFFFFFFF) < 0x80000000):
            newest = (i, rec[0])
    if newest is None:
        return 0, 1
    return (1 if newest[0] == 0 else 0), newest[1] + 1


def _present(ids) -> bool:
    return bool(hid.enumerate(*ids))


def _wait_for(ids, what: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _present(ids):
            time.sleep(0.5)
            return
        time.sleep(0.2)
    raise FlashStoreError(f"timed out waiting for {what} ({ids[0]:04X}:{ids[1]:04X})")


def _find_ivt(data: bytes) -> tuple[int, int]:
    for offset in range(0, min(len(data), 0x2000), 4):
        tag, length, version = data[offset], struct.unpack_from(">H", data, offset + 1)[0], data[offset + 3]
        if tag == 0xD1 and length == 0x20 and 0x40 <= version <= 0x4F:
            return offset, struct.unpack_from("<I", data, offset + 0x14)[0]
    raise FlashStoreError("flashloader file has no i.MX IVT (expected NXP RT1010 ivt_flashloader.bin)")


def save_settings(settings: p.Settings, flashloader: Path | None = None,
                  progress: Callable[[str], None] = print, timeout: float = 15.0) -> int:
    """Write `settings` as the newest flash record. Returns its sequence number.

    The DAC reboots twice (into the ROM and back); audio stops meanwhile.
    """
    from spsdk.mboot.interfaces.usb import MbootUSBInterface
    from spsdk.mboot.mcuboot import McuBoot
    from spsdk.sdp.interfaces.usb import SdpUSBInterface
    from spsdk.sdp.sdp import SDP

    problems = settings.problems()
    if problems:
        raise FlashStoreError("; ".join(problems))
    blob = settings.pack()
    loader_path = flashloader or default_flashloader()
    if not loader_path or not Path(loader_path).exists():
        raise FlashStoreError("NXP RT1010 ivt_flashloader.bin not found (put it next to the .exe "
                              "or in tools/flashloader/)")
    loader = Path(loader_path).read_bytes()

    if not _present(FLASHLOADER_ID):
        if not _present(SDP_ID):
            devices = p.find_devices()
            if not devices:
                raise FlashStoreError("DykoDAC2 not found")
            dev = hid.device()
            dev.open_path(devices[0]["path"])
            try:
                dev.write(b"\0" + ENTER_ROM.ljust(64, b"\0"))
            finally:
                dev.close()
            progress("Rebooting the DAC into its boot ROM...")
            _wait_for(SDP_ID, "the RT1011 boot ROM", timeout)
        ivt_offset, ivt_address = _find_ivt(loader)
        progress("Starting the RAM flashloader...")
        # Right after re-enumeration Windows may still be binding the ROM's HID
        # interface; retry the open/load briefly instead of failing the save.
        deadline, last = time.monotonic() + 5.0, "boot ROM vanished from USB"
        while True:
            try:
                interfaces = SdpUSBInterface.scan(device_id=f"0x{SDP_ID[0]:04x},0x{SDP_ID[1]:04x}")
                if interfaces:
                    with SDP(interfaces[0]) as sdp:
                        if sdp.write_file(ivt_address - ivt_offset, loader):
                            sdp.jump_and_run(ivt_address)
                            break
                        last = f"could not load the flashloader into RAM: {sdp.status_code}"
            except Exception as e:  # noqa: BLE001 - spsdk raises various transport errors
                last = f"could not load the flashloader into RAM: {e}"
            if time.monotonic() > deadline:
                raise FlashStoreError(last)
            time.sleep(0.5)
        _wait_for(FLASHLOADER_ID, "the NXP RAM flashloader", timeout)

    interfaces = MbootUSBInterface.scan(device_id=f"0x{FLASHLOADER_ID[0]:04x},0x{FLASHLOADER_ID[1]:04x}",
                                        timeout=30000)
    if not interfaces:
        raise FlashStoreError("flashloader vanished from USB")
    with McuBoot(interfaces[0]) as mb:
        def ok(result, what):
            if not result:
                raise FlashStoreError(f"{what} failed: {mb.status_string}")

        progress("Configuring the QSPI flash...")
        ok(mb.write_memory(SCRATCH, struct.pack("<I", FLASH_OPTION)), "write option")
        ok(mb.configure_memory(SCRATCH, 9), "configure-memory")
        current = mb.read_memory(REGION, SECTORS * SECTOR)
        if current is None:
            raise FlashStoreError(f"reading the settings sectors failed: {mb.status_string}")
        target, sequence = choose_target([current[i * SECTOR:(i + 1) * SECTOR] for i in range(SECTORS)])
        address = REGION + target * SECTOR
        assert REGION <= address < REGION + SECTORS * SECTOR  # never anything but the settings sectors
        image = make_record(blob, sequence)
        progress(f"Writing settings record #{sequence} to sector {'AB'[target]}...")
        ok(mb.flash_erase_region(address, SECTOR), "erase")
        ok(mb.write_memory(address, image[:256]), "program")
        back = mb.read_memory(address, SECTOR)
        if back != image:
            raise FlashStoreError("verify failed; the previous record is still intact. Retry Save.")
        progress("Restarting the DAC...")
        mb.reset(reopen=False)

    _wait_for((p.VID, p.PID), "the DAC firmware", timeout + 10.0)
    return sequence
