"""Command-line front end, mainly for scripting and hardware bring-up.

    python -m dykodac_config info|read|defaults|load|save|probe
    python -m dykodac_config write preset.json [--save]
    python -m dykodac_config gui
"""

from __future__ import annotations

import argparse
import struct
import sys
import time

from . import protocol as p


def print_info(info: p.Info) -> None:
    print(f"State           : {info.app_state}{' (streaming)' if info.streaming else ''}")
    if info.source_rate_hz:
        mode = "bypass" if info.upsampler_bypass_active else "4x interpolator"
        print(f"Rate            : {info.source_rate_hz} Hz -> {info.output_rate_hz} Hz ({mode})"
              f"{', bit-perfect' if info.bit_exact else ''}")
    print(f"Host volume     : {info.host_volume_db:.2f} dB")
    print(f"Headroom        : {info.headroom_db:.2f} dB set, EQ boost {info.dsp_boost_db:.2f} dB, "
          f"effective {info.effective_headroom_db:.2f} dB")
    print(f"Flash           : {'record #%d' % info.flash_sequence if info.flash_record else 'no saved settings'}, "
          f"{'unsaved changes' if info.unsaved else 'in sync'}")


def probe(dev: p.Device) -> None:
    """Dump the ROM API tree and FlexSPI NOR driver table (read-only)."""
    tree_ptr = struct.unpack("<I", dev.peek_rom(0x0020001C, 4))[0]
    print(f"API tree pointer: 0x{tree_ptr:08X}")
    words = struct.unpack("<8I", dev.peek_rom(tree_ptr, 32))
    for i, w in enumerate(words):
        print(f"  tree[{i}] = 0x{w:08X}")
    drv = words[4]
    names = ["version", "init", "program", "erase_all", "erase", "read", "clear_cache",
             "xfer", "update_lut", "get_config", "erase_sector", "erase_block"]
    table = struct.unpack(f"<{len(names)}I", dev.peek_rom(drv, 4 * len(names)))
    print(f"FlexSPI NOR driver @ 0x{drv:08X}:")
    for n, v in zip(names, table):
        rom = 0x00200000 <= (v & ~1) < 0x00240000
        print(f"  {n:<12} 0x{v:08X} {'' if n == 'version' else ('thumb/ROM' if rom and v & 1 else '!! not a ROM thumb pointer')}")


def save_to_flash(dev: p.Device) -> None:
    from . import flash_store
    settings = dev.read_settings()
    dev.close()
    sequence = flash_store.save_settings(settings, progress=lambda m: print(m))
    time.sleep(1.0)
    with p.Device() as again:
        loaded = again.read_settings()
        info = again.info()
    if loaded.pack() != settings.pack() or info.flash_sequence != sequence:
        raise p.DeviceError("after reboot the DAC is not running the saved settings")
    print(f"Saved to flash as record #{sequence}; the DAC rebooted and loaded it.")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="dykodac_config", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["info", "read", "write", "save", "load", "defaults", "probe", "gui"])
    ap.add_argument("file", nargs="?", help="preset JSON for write")
    ap.add_argument("--save", action="store_true", help="after write, also save to flash")
    args = ap.parse_args(argv)
    if args.command == "gui":
        from .gui import main as gui_main
        return gui_main()
    try:
        with p.Device() as dev:
            if args.command == "info":
                print_info(dev.info())
            elif args.command == "read":
                print(dev.read_settings().to_json())
            elif args.command == "write":
                if not args.file:
                    ap.error("write needs a preset file")
                s = p.Settings.from_json(open(args.file, encoding="utf-8").read())
                dev.write_settings(s)
                print("Applied to RAM.")
                if args.save:
                    save_to_flash(dev)
            elif args.command == "save":
                save_to_flash(dev)
            elif args.command == "load":
                print(dev.load().to_json())
            elif args.command == "defaults":
                print(dev.defaults().to_json())
            elif args.command == "probe":
                print_info(dev.info())
                probe(dev)
    except (p.DeviceError, p.ProtocolError, OSError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
