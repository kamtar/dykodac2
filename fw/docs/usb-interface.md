# DykoDAC2 USB Audio 2 interface

The diagnostic image enumerates as **DykoDAC2 UAC2**, VID `1209`, PID `D2A3`.
It is a high-speed, asynchronous USB Audio Class 2 playback device with one
stereo output interface, 24 valid PCM bits in 32-bit subslots, and explicit
16.16 feedback. The data endpoint is OUT `0x01`, up to 56 bytes every 125 us
(`bInterval=1`); feedback is IN `0x81`, four bytes every 1 ms
(`bInterval=4`). The programmable UAC2 clock source exposes exactly 44.1 kHz
and 48 kHz and rejects any other `SET_CUR` value.

The power-on default is 44.1 kHz. The oscillator selector remains at the
verified 44.1-family low level until the host explicitly requests 48 kHz and
starts the guarded switch; 48 kHz selects the verified high level while the
relay is open.

TinyUSB 0.21.0 at commit
`dae3f9a366bfcddbf9dcf1b48d7500286a849539` supplies the class and i.MX RT
ChipIdea high-speed device controller. The legacy NXP audio class is not linked.
The NXP clock and device headers remain the low-level silicon support.

Windows 10/11 should bind the inbox USB Audio 2.0 driver and show “DykoDAC2
UAC2” as a stereo output. Current macOS should show it in Audio MIDI Setup;
Linux should expose an ALSA UAC2 playback PCM. All hosts should offer only
44.1/48 kHz, stereo, 24-bit audio carried in four-byte subslots.

Mute and volume requests are validated in the USB callback and queued for the
cooperative loop. The loop applies one half-dB attenuation value to both DAC
channels. Callbacks never touch relay, oscillator, or DAC GPIO/SPI.

## Runtime safety behavior

- Start: relay open, DAC mute, oscillator select, 50 ms settle, SAI/eDMA
  silence, confirmed DMA progress, half-ring prefill, 100 ms stable streaming,
  DAC unmute, 20 ms DAC settle, then relay close.
- Stop, detach, or suspend: relay opens first, DAC mutes, SAI stops, stream and
  feedback state clear, and the clock is marked invalid.
- Rate change: the same complete stop/start sequence runs; repeated requests
  collapse to the latest supported rate. The relay stays open throughout.
- Overflow drops the complete incoming packet and increments counters.
  Underrun zero-fills; three underruns or any DMA error enter `FaultMuted`.
- Fault: relay remains open, DAC remains muted, and a fresh complete start is
  required before reconnecting the output.

## Maintenance diagnostics

The vendor-defined HID interface uses one 64-byte input/output report. Sending
the zero-padded command `DYKO-DIAG-GET-01` returns a versioned binary snapshot
containing the last host `SET_CUR` rate, desired and active rates, clock family
and raw selector pin level, interface alternate setting, start/stop stage,
mount/suspend/mute/relay state, USB packet sizes and counts, ring fill, feedback,
drops, DMA progress/errors, underruns, overflows, and nonzero low-padding-byte
count for 24-in-32 PCM. Read and decode it on Windows with:

```powershell
py -m pip install hidapi
py tools/read_diagnostics.py
```

`last_set_cur` remains zero until the host actually sends a sample-frequency
`SET_CUR`; setting the active format no longer overwrites it. Each queued rate,
mute, volume, and alternate-setting event carries its own value. The reader also
fetches a second report containing the seven most recent USB events in oldest-to-
newest order, including each control or alternate-setting payload and the low
16 bits of its monotonic millisecond timestamp.

The reader also requests `DYKO-DMA-GET-001`, a separate 64-byte SAI/eDMA
snapshot. On `DmaNoProgress` the firmware latches this snapshot before stopping
SAI or disabling DMAMUX, preserving CHCFG0, eDMA error/request/interrupt state,
the live TCD addresses and iteration counters, and the SAI control/FIFO pointers.
For a continuously refreshed compact view, run:

```powershell
py tools/watch_dma_diagnostics.py
```

## Indicators

- Before enumeration: the selected oscillator LED pulses for 100 ms every
  5 seconds. The power-on default is the green 44.1-kHz-family LED.
- Enumerated and idle: only the selected oscillator LED toggles every 500 ms.
- Playing: the selected oscillator LED is solid (green for 44.1 kHz, red for
  48 kHz).
- Fault: both LEDs toggle every 125 ms and override oscillator indication.

## Expected hardware signals

At 44.1 kHz, expect 22.5792 MHz MCLK, 2.8224 MHz BCLK, and 44.1 kHz LRCLK.
At 48 kHz, expect 24.576 MHz MCLK, 3.072 MHz BCLK, and 48 kHz LRCLK. SAI is
stereo I2S with 24 valid bits in 32-bit slots. At boot the CS4398 reset is held
for 10 ms, then the 2 MHz SPI sequence is `98 08 C0`, `98 02 10`, `98 07 B0`,
`98 08 40`, `98 04 D8`. Stream preparation adds format/mute/attenuation writes;
unmute is `98 04 D0`. Reset is asserted again on DAC bus failure.

## First-flash validation

1. Probe relay drive, DAC reset/CS/SCK/MOSI, oscillator select, MCLK, BCLK,
   LRCLK, and both LEDs. Flash the BIN at NOR offset zero.
2. Confirm the boot SPI sequence and that the relay remains open while idle.
3. Enumerate at high speed and verify only 44.1/48 kHz are offered.
4. Start a low-level 44.1-kHz stream. Confirm silence DMA starts before buffered
   audio, clocks match the values above, DAC unmute precedes relay close by at
   least 20 ms, and feedback packets occur every 1 ms.
5. Repeat at 48 kHz and confirm oscillator polarity changes while the relay is
   open. Exercise mute, volume, stop/start, rapid rate changes, suspend/resume,
   and unplug. The relay must open immediately for every safety transition.
