# Bring-up test plan

## Automated checks

- Host tests must pass for format lookup, unsupported-rate rejection, pin
  polarity, DAC command encoding, attenuation conversion, and safety states.
- Debug and Release target links must report flash/RAM use and produce ELF,
  BIN, HEX, MAP, and disassembly outputs.
- `.boot_hdr` must start at `0x60000000`, IVT at `0x60001000`, boot data at
  `0x60001020`, and vectors/application text at `0x60002000`.

## First hardware flash: safe-idle image

Do not flash this image to the only known-working board without retaining the
prototype binary and a recovery method.

Before power-on, monitor relay drive, DAC reset, DC/DC enable, green LED, and
red LED. Expected behavior:

1. Relay drive takes the legacy safe-low level as early as possible.
2. DAC reset remains asserted low.
3. DAC chip-select remains inactive high.
4. Analog DC/DC enable remains low.
5. Oscillator selector takes the legacy 44.1-family low level.
6. Red turns on before the run-clock transition. Solid red alone means the
   clock routine did not return. Red and green together mean clocks returned
   but SysTick initialization did not complete.
7. Red and green alternate once per second after clock/SysTick startup. This
   intentionally overrides the eventual product LED policy for bring-up only.

The analog output must remain disconnected. Stop testing if relay polarity is
opposite to the provisional assumption or any monitored line glitches into an
unsafe state.

## Gate before audio-clock work

Record the measured electrical level and physical meaning for relay open,
oscillator family, DAC reset, and DC/DC enable in `hardware-map.md`. Confirm
the board can be recovered through the selected probe or ROM boot procedure.

## Second hardware flash: DAC SPI and SAI silence diagnostic

This checkpoint preserves the verified startup order above. Expected behavior:

1. Red is initialized before clock setup; after SysTick is live, red and green
   continue alternating at one-second intervals.
2. Relay drive remains at the disconnected level for the entire run; firmware
   contains no path that closes it.
3. DAC reset starts asserted low, remains asserted for 10 ms after SysTick
   starts, then releases high. It is reasserted on SPI or DMA failure.
4. CS4398 chip select is normally high. Four legacy 2 MHz MSB-first writes appear,
   exactly `98 08 C0`, `98 02 10`, `98 07 B0`, and `98 08 40`, separated by
   monotonic-timer deadlines, followed by `98 04 D8` to explicitly mute both
   DAC channels. The resulting default configuration stays muted.
5. `D_OSC_SELECT` remains at the known-working 44.1-kHz-family low level.
   External MCLK should measure 22.5792 MHz at GPIO_08.
6. SAI1/eDMA continuously transmits zeros in 32-bit stereo I2S slots with 24
   valid bits. BCLK should measure 2.8224 MHz and LRCLK 44.1 kHz.
7. Analog DC/DC enable remains low. Do not connect the relay or attach an analog
   load during this checkpoint.

Verify reset, CS, MOSI, SCK, MCLK, BCLK, and LRCLK on a logic analyzer or scope
before any USB integration or relay-close path is added.
