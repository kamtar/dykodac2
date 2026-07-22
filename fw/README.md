# DykoDAC2 firmware redesign

This directory is the clean-room successor to
`../original_evkmimxrt1010_dev_audio_speaker_bm`. The prototype remains a
read-only behavioral reference.

The semi-final checkpoint implements TinyUSB UAC2 high-speed playback,
asynchronous feedback, a bounded static audio ring, SAI/eDMA output, runtime
44.1/48-kHz switching, CS4398 mute/volume control, and the pop-safe cooperative
state machine. See `docs/usb-interface.md` for first-flash expectations.

## Build host tests

```powershell
premake5 gmake2
mingw32-make -C build/gmake2 dykodac2_host_tests config=hosttest
build/gmake2/bin/HostTest/dykodac2_host_tests.exe
```

## Build the RT1011 USB DAC and A/B updater

```powershell
build.bat 1
```

The optional argument is the packaged firmware version and defaults to `1`.
The script builds all Release firmware targets, creates the factory and HID
update packages, then builds and runs the host tests.

For a clean build, remove only the generated `build/gmake2` tree and rebuild
everything with:

```powershell
build.bat clean
```

An optional version may follow `clean`, for example `build.bat clean 5`.
The archived known-good image under `build/recovery` is preserved.

This produces `dykodac2_factory.elf` and `dykodac2_factory.bin` for the one-time
layout migration and `dykodac2.dykoupd` for subsequent driverless HID updates. See
[`docs/firmware-update.md`](docs/firmware-update.md) before programming the
factory image; the first board test must confirm the exact NOR command set.

This produces `.elf`, `.bin`, `.hex`, `.map`, and `.lst` files. The diagnostic
image holds the relay open and analog power disabled, configures the CS4398 over
2 MHz FlexIO SPI with cooperative deadlines, and continuously sends 44.1 kHz
stereo silence over SAI1/eDMA. The DAC remains digitally muted.

After flashing the maintenance-diagnostic image, capture live playback state
without interpreting LED cadence:

```powershell
py -m pip install hidapi
py tools/read_diagnostics.py
```

## Verified safe-idle hardware baseline

The 2026-07-19 Debug FlexSPI NOR image booted successfully when programmed as a
BIN at NOR offset zero with MCU Boot Utility. Its SHA-256 is
`432AE83F08FF63C79F65666CED79044FDB6440EF21C851A65D4527900BF1C412`; red and
green LEDs alternated once per second.

Initialization order is hardware-verified and safety-critical: ResetISR and
SystemInit, Cortex-M7 MPU/cache setup, red diagnostic LED only,
`BOARD_InitBootClocks()`, safe board GPIOs, SysTick, then the cooperative main
loop. Initializing every board GPIO before the clock routine caused that routine
to stall. `__MCUXPRESSO` must remain defined so SystemInit sets `SCB->VTOR`.

The target baseline temporarily compiles the prototype's NXP SDK/startup/XIP
sources in place. They remain unmodified. A pinned SDK subset will replace
these references after the baseline is verified on hardware.
