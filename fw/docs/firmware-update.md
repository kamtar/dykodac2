# A/B firmware update and recovery

The update system uses a protected boot manager and two directly executable
application slots. Routine updates only erase/program the inactive slot. The
known-good slot is never copied or modified during an update.

## Flash layout

| Region | Address | Size |
|---|---:|---:|
| Boot manager and RAM updater load image | `0x60000000` | 64 KiB |
| Application A | `0x60010000` | 384 KiB |
| Application B | `0x60070000` | 384 KiB |
| Boot-state copy 0 | `0x600D0000` | 4 KiB |
| Boot-state copy 1 | `0x600D1000` | 4 KiB |
| Reserved for future use | `0x600D2000` | 184 KiB |

Each application begins with a 512-byte Dyko image header. Its Cortex-M vector
table follows at slot offset `0x200`. Slot A and B are linked separately, and a
`.dykoupd` package contains both variants. The updater selects the inactive one.

## Build

From the firmware directory:

```powershell
tools/make_update.ps1 -Configuration Release -Version 1
```

Outputs under `build/gmake2/bin/Release`:

- `dykodac2_factory.bin`: one-time complete boot manager plus slot-A image.
- `dykodac2_factory.elf`: the same contiguous factory image based at
  `0x60000000`, for probes/tools that require ELF input.
- `dykodac2.dykoupd`: routine A/B update package.
- `dykodac2_boot.bin`: boot region only; do not use for routine updates.
- `dykodac2_a.payload.bin`, `dykodac2_b.payload.bin`: unwrapped build artifacts.

## First installation

The layout is incompatible with the previous offset-zero standalone firmware.
Install `dykodac2_factory.bin` once at NOR offset zero using MCU Boot Utility or
the existing RT1010 ROM flashloader. Keep the verified
`dykodac2_known_boot_recovery.bin` available until this build has passed the
first hardware gate.

Before the first A/B update, confirm the NOR JEDEC ID reported by updater HID.
The current writer intentionally uses the common 3-byte-address serial-NOR
commands `06h` write-enable, `05h` status, `20h` 4-KiB erase, and `02h` page
program. Do not test update erasure on a board whose flash data sheet requires a
different command set.

## Routine update

Install the Python HID dependency once, then run:

```powershell
python -m pip install hidapi
python tools/flash_update.py build/gmake2/bin/Release/dykodac2.dykoupd
```

The script requests update mode twice, waits for HID-only PID `1209:D2A4`,
programs and verifies the inactive slot, and reboots into a trial image. Normal
firmware remains PID `1209:D2A3`.

The candidate receives three boot attempts. In trial mode the application runs
a five-second RTWDOG and refreshes it from the main loop. After 60 seconds with
the DAC safely initialized, USB initialized, and no application fault, it asks
the boot manager to confirm it. Until confirmation, three resets cause the boot
manager to abandon the candidate and return to the previous slot.

## Power-loss behavior

- During upload: only the inactive slot is affected; the old image boots.
- Before complete SHA-256 verification: no pending image is recorded.
- During boot-state update: two alternating CRC-protected state sectors retain
  the preceding valid state.
- During trial: the old slot remains untouched and is selected after the trial
  budget is exhausted.
- If neither slot validates: the boot manager enumerates directly as the HID
  updater.

## Recovery

The old `DYKO-ROM-BOOT-01` maintenance command remains available and enters the
RT1010 ROM downloader. It is the recovery route for a damaged boot region or an
unsupported flash device. Routine HID updates cannot erase or program the first
64 KiB boot region.

## Required first hardware gate

1. Program `dykodac2_factory.bin` at offset zero and verify safe GPIO/relay
   behavior and normal USB audio enumeration.
2. Enter updater mode and record the JEDEC ID from the host response before
   sending `BEGIN`.
3. Update to a build with a visibly different version and verify slot B boots.
4. Let it run for 60 seconds and verify the confirmation reset returns to B.
5. Install another version into A.
6. Force three watchdog resets during its trial and verify rollback to B.
7. Repeat with power removed during erase, payload programming, final
   verification, and boot-state programming.

The software build and static layout checks cannot substitute for these
flash-specific and power-interruption tests.
