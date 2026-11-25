"""Entry point for the packaged DykoDAC2 Configurator (.exe)."""
import sys


def selftest(out_path: str) -> int:
    """Import everything Save-to-flash needs inside the frozen bundle."""
    lines = []
    try:
        from spsdk.mboot.interfaces.usb import MbootUSBInterface  # noqa: F401
        from spsdk.mboot.mcuboot import McuBoot  # noqa: F401
        from spsdk.sdp.interfaces.usb import SdpUSBInterface  # noqa: F401
        from spsdk.sdp.sdp import SDP  # noqa: F401
        from dykodac_config import flash_store, protocol
        lines.append("spsdk: ok")
        lines.append(f"flashloader: {flash_store.default_flashloader()}")
        lines.append(f"dac: {'found' if protocol.find_devices() else 'not found'}")
        code = 0
    except Exception as e:  # noqa: BLE001
        lines.append(f"FAIL: {e!r}")
        code = 1
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return code


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--selftest":
        sys.exit(selftest(sys.argv[2]))
    from dykodac_config.gui import main
    sys.exit(main())
