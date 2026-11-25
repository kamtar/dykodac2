# Flashing over USB (no case opening)

One command flashes a new image through the same USB cable used for audio:

```powershell
python -m pip install spsdk hidapi     # once
python tools/flash_firmware.py          # flashes build/gmake2/bin/Release/dykodac2.bin
```

## How it works

The firmware never writes its own flash. That avoids the i.MX RT problem that
broke the earlier A/B boot manager: erasing or programming the FlexSPI NOR
while the CPU is executing (XIP) from that same flash.

1. The script sends `DYKO-ROM-BOOT-01` to the maintenance HID interface
   (`1209:D2A3`). The main loop makes the audio path safe (relay open, DAC
   muted, SAI/eDMA stopped), detaches USB, stores a request in SNVS `LPGPR2/3`
   and resets.
2. The first call in `main()` sees the request, clears it, and calls the
   RT1011 ROM API `runBootloader(0xEB100000)` from a near-reset state. The boot
   ROM enumerates as the NXP serial downloader `1FC9:0145`.
3. `sdphost` loads NXP's RAM flashloader (`ivt_flashloader.bin`) and jumps to
   it. It enumerates as `15A2:0073`.
4. `blhost` configures FlexSPI with option word `0xC0000002` (Quad SPI SDR,
   chip auto-detected from its SFDP table, 50 MHz). The image's own FCB
   cannot be used here: it carries only the read sequence the ROM needs for
   XIP, and erasing with it hangs the flashloader. It erases the image range, writes everything
   from `0x1000` up, writes the first 4 KiB (FCB and IVT) **last**, reads the
   image back to verify it, then resets.

The script resumes from whichever stage it finds on USB, so a rerun after an
interruption picks up where things are.

## The flashloader binary

The RT1011 boot ROM can only load code into RAM, so a RAM flashloader is
needed. Use NXP's RT1010 `ivt_flashloader.bin`, the same file MCU Boot
Utility and MCUXpresso Secure Provisioning Tool load. Copy it to
`tools/flashloader/ivt_flashloader.bin` (`*.bin` is git-ignored) or pass
`--flashloader PATH`. The script reads the load and entry address from the
file's IVT.

## Power loss

- Before or during erase: the old image or an erased boot header remains.
- During the body write: the boot header is still erased, so the boot ROM has
  no valid image.
- During the 4 KiB header write: the FCB/IVT are incomplete.

In the last two cases, the RT1011 boot ROM falls back to the serial
downloader when internal boot finds no valid image. Running the script again
continues from step 3. If a board's fuses or boot pins disable that fallback,
the boot-mode pins remain the last-resort recovery. **Confirm the fallback
once on hardware** (see below).

## First hardware check

1. Flash this build once with MCU Boot Utility as before.
2. Run `python tools/flash_firmware.py` and confirm it reaches each stage
   (`1FC9:0145`, then `15A2:0073`) and the DAC re-enumerates afterwards.
3. Optional fallback check: unplug USB during the "Writing application" step,
   replug, and confirm `1FC9:0145` appears and a rerun recovers.

The script uses the RT1011 flashloader from MCU Boot Utility 6.5.0
(`src/targets/MIMXRT1011/ivt_flashloader.bin`). It loads at `0x20205800`, and
the scratch address `0x20202000` is MCU Boot Utility's own free-RAM slot.
