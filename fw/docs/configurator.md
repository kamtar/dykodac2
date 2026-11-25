# DykoDAC2 Configurator

`tools/dist/DykoDAC2-Configurator.exe` (built by `make_configurator.bat`) is a
single-file Windows GUI that talks to the DAC over the existing maintenance HID
interface (`1209:D2A3`, usage page `0xFF00`). No driver is needed. The same
functions are scriptable with `python -m dykodac_config` from `tools/`.

## Controls

| Button | What it does |
|---|---|
| Read | Loads the settings the DAC is running (RAM) into the panel. |
| Write to RAM | Validates and applies the panel immediately. Lost at power-off unless saved. *Live apply* does this on every change. |
| Save to flash | Makes the running settings permanent. The DAC reboots through its boot ROM (about 5 s, audio stops). |
| Load from flash | Discards RAM changes and runs the saved settings. |
| Defaults | Factory settings into RAM. Flash is unchanged until Save. |

The **Diagnostics** tab polls the existing maintenance reports twice per second.
It shows status lamps, strip charts (ring-buffer fill, asynchronous feedback,
refill CPU load and worst case, USB packet rate, new errors per second),
counters since boot, and the recent USB event trace.

EQ presets live only on the PC (`%APPDATA%\DykoDAC2\eq-presets\*.json`). The
DAC stores exactly one settings set. Applying a preset replaces only the EQ;
reconstruction, headroom, ceiling, balance and crossfeed stay as they are.
*Import AutoEQ* reads Equalizer APO / AutoEQ `ParametricEQ.txt` files. Their
`Preamp` is ignored because the DAC removes the EQ's boost from the headroom
itself.

## Settings

| Setting | Range | Default | Notes |
|---|---|---|---|
| Upsampler | 4x interpolator / bypass | 4x | Bypass runs the CS4398 single-speed at 44.1/48 kHz (512 fs MCLK) and uses its internal filter. |
| CS4398 filter | fast / slow roll-off | fast | Register 07h FILT_SEL. |
| Digital headroom | 0 to -24 dB | -3.25 dB | Applied in float before the single dithered 24-bit quantisation. |
| DAC ceiling | 0 to -40 dB, 0.5 dB steps | -9 dB | CS4398 attenuation; the analog safety limit. |
| Balance | ±12 dB | 0 | Attenuates one side only. |
| Polarity | per channel | off | |
| Crossfeed | off, bs2b default, Chu Moy, Jan Meier | off | bs2b first-order crossfeed. |
| Parametric EQ | 10 bands: peak, low/high shelf (Q form), low/high pass, notch; per-band L/R/both | off | ±20 dB, 10 Hz to 20 kHz, Q 0.1 to 20. |

**Boost rule.** The firmware evaluates the worst-case magnitude of
EQ, crossfeed and balance (192 log-spaced points up to fs/2, plus every band
centre, +0.1 dB margin). It removes that boost from the headroom
automatically, as a makeup gain at the end of the source-rate chain, so the
compensation is time-aligned with the EQ and glides together with it. The app shows the same number, computed by
`tools/dykodac_config/eq_math.py`, which mirrors the firmware.

**EQ filters.** Each band runs as a trapezoidal state-variable filter (Simper)
with exactly the RBJ cookbook response. A direct-form biquad in float32
(the RT1011 FPU is single precision only) leaves an error near -80 dBFS on
loud music once a band sits at 20-30 Hz; the SVF stays near -140 dBFS, at
the 24-bit dither floor. Every change made while playing (band parameters,
bands added or removed, balance, polarity, crossfeed, makeup) glides over
20 ms, so dragging a control does not click.

**Neutral means untouched.** With the EQ off, or only 0 dB peak or shelf bands,
no crossfeed, balance or polarity, the processing chain is never entered. A
host test built the previous commit's interpolator next to the new one: with
factory settings, both produce byte-identical output at 44.1/176.4 kHz and
48/192 kHz. In bypass, unity gain (host volume 0 dB, headroom 0 dB) and a
neutral chain copy samples bit for bit.

## Where the processing runs

```
USB 24-bit -> float -> [EQ (SVF) -> crossfeed -> balance/polarity x makeup(-boost)]   (source rate, main context)
          -> 4x FFT interpolator (or bypass) -> x gain(volume + headroom)
          -> TPDF dither -> one round to 24 bit -> SAI -> CS4398
```

Measured on hardware at 44.1 kHz (average refill CPU per 1 ms, worst refill):

| Mode | CPU | Worst refill |
|---|---|---|
| 4x, neutral | 34.3 % | 509 µs |
| 4x, 10-band EQ, crossfeed, balance | 37.1 % | 509 µs |
| Bypass, 10-band EQ | 10.0 % | 166 µs |

No underruns, drops or DMA errors occurred while switching modes during
playback.

## Persistence

The RT1011 has no EEPROM or internal flash. Settings are a 208-byte record
(`DKST` magic, sequence number, length, CRC-32, settings) in one of two 4 KiB
sectors at `0x600FE000` and `0x600FF000`, outside the image. The linker script
ends FLASH there and asserts it. At boot the firmware loads the newest valid
record; otherwise it uses defaults.

The firmware never erases or programs flash. On this chip the ROM FlexSPI
driver table sits at API tree word 3, and its member order does not match
NXP's SDK header. Calling it by guess could have run `erase_all`. *Save*
therefore uses the proven firmware-update path in `flash_store.py`:

1. Ask the firmware to reboot into the boot ROM.
2. Load NXP's RAM flashloader.
3. Configure FlexSPI from SFDP.
4. Read both settings sectors, erase and program the one that does **not**
   hold the newest record, read it back.
5. Reset the DAC, then confirm it runs the saved record.

A power loss at any point leaves the previous record intact. A firmware update
(`flash.bat`) erases only the image, so saved settings survive. This was
verified on hardware.

## HID protocol (v1)

64-byte request `DYKO-CFG`, opcode, tag, offset, length, 48 data bytes. The
64-byte response is `DKCR`, echoing opcode and tag, plus status, aux and data.
Opcodes:

| Code | Opcode |
|---|---|
| 1 | Info |
| 2 | Read (active settings) |
| 3 | Write (staging buffer) |
| 4 | Apply (validate staging, make it active) |
| 5 | Save (unsupported in firmware: use the host flashloader path) |
| 6 | Load |
| 7 | Defaults |
| 0x20 | PeekRom (read-only diagnostic) |

Layouts: `usb/config_protocol.hpp`, `config/user_settings.hpp`; host mirror in
`tools/dykodac_config/protocol.py`.

## Hardware items still to confirm

- The CS4398 FILT_SEL bit (07h bit 2) is taken from the datasheet register map
  and is not yet checked by ear or with measurements. The register write itself
  is covered by host tests.
- Single-speed bypass at 512 fs was verified by playback at 44.1 kHz. 48 kHz
  bypass has not been played yet.
