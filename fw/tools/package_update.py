#!/usr/bin/env python3
"""Package slot-specific payloads and create the one-time factory image."""

import argparse
import hashlib
import subprocess
import struct
import zlib
from pathlib import Path

IMAGE_MAGIC = 0x4F4B5944
PRODUCT_ID = 0x1010D2A3
HEADER_SIZE = 512
SLOT_A = 0x60010000
SLOT_B = 0x60070000
SLOT_SIZE = 384 * 1024
FLASH_BASE = 0x60000000
PACKAGE = struct.Struct("<8sIIII")
FIXED_HEADER = struct.Struct("<IHHIIIII32sI")


def image(payload: bytes, slot: int, version: int) -> bytes:
    digest = hashlib.sha256(payload).digest()
    fixed = FIXED_HEADER.pack(
        IMAGE_MAGIC, 1, HEADER_SIZE, PRODUCT_ID, version, len(payload),
        HEADER_SIZE, slot + HEADER_SIZE, digest, 0,
    )
    header = bytearray(fixed.ljust(HEADER_SIZE, b"\xff"))
    crc_offset = FIXED_HEADER.size - 4
    struct.pack_into("<I", header, crc_offset, 0)
    struct.pack_into("<I", header, crc_offset, zlib.crc32(header) & 0xFFFFFFFF)
    return bytes(header) + payload


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--version", type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()
    directory = args.build_dir
    payload_a = (directory / "dykodac2_a.payload.bin").read_bytes()
    payload_b = (directory / "dykodac2_b.payload.bin").read_bytes()
    image_a = image(payload_a, SLOT_A, args.version)
    image_b = image(payload_b, SLOT_B, args.version)
    if len(image_a) > SLOT_SIZE or len(image_b) > SLOT_SIZE:
        raise SystemExit("application image exceeds AT25SF081B slot size")
    package = PACKAGE.pack(b"DYKUPKG1", 1, args.version, len(image_a), len(image_b)) + image_a + image_b
    (directory / "dykodac2.dykoupd").write_bytes(package)

    boot = (directory / "dykodac2_boot.bin").read_bytes()
    if len(boot) > SLOT_A - FLASH_BASE:
        raise SystemExit("boot image exceeds protected boot region")
    factory = boot + b"\xff" * (SLOT_A - FLASH_BASE - len(boot)) + image_a
    (directory / "dykodac2_factory.bin").write_bytes(factory)
    factory_elf = directory / "dykodac2_factory.elf"
    factory_object = directory / "dykodac2_factory.tmp.o"
    subprocess.run([
        "arm-none-eabi-objcopy", "-I", "binary", "-O", "elf32-littlearm",
        "-B", "arm", str(directory / "dykodac2_factory.bin"), str(factory_object),
    ], check=True)
    subprocess.run([
        "arm-none-eabi-ld", f"-Tdata={hex(FLASH_BASE)}", "-e", hex(FLASH_BASE + 0x2000),
        "-o", str(factory_elf), str(factory_object),
    ], check=True)
    factory_object.unlink()
    print(f"Created {directory / 'dykodac2.dykoupd'} ({len(package)} bytes)")
    print(f"Created {directory / 'dykodac2_factory.bin'} ({len(factory)} bytes)")
    print(f"Created {factory_elf}")


if __name__ == "__main__":
    main()
