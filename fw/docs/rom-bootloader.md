# Entering the RT1011 ROM USB downloader

Firmware PID `1209:D2A3` includes a vendor-defined HID maintenance interface,
which uses the Windows/macOS/Linux inbox HID driver. To avoid accidental entry,
the exact 16-byte output report `DYKO-ROM-BOOT-01` must be received twice within
two seconds. USB callbacks only enqueue the request.

On confirmation, the cooperative loop opens the relay, mutes the DAC, stops
SAI/eDMA, clears the stream, asserts DAC reset, disables interrupts and invokes
the RT1011 ROM bootloader API at the documented ROM API-tree pointer with
argument `0xEB100000`. The ROM resets into USB HID serial-downloader/SDP mode.

From Windows:

```powershell
py -m pip install hidapi
py tools/enter_rom_bootloader.py
```

The application device will disconnect and an NXP ROM USB HID device should
appear. MCU Boot Utility or `sdphost` can then load the flashloader/programmer.
A normal reset or power cycle returns to the application unless the ROM tool is
actively holding or reconfiguring the target.
