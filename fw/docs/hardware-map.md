# Hardware map

The working 2022 firmware is authoritative until the assembled board is
measured. “Unknown” is deliberate; it must not be filled from inference.

| Signal | Working firmware | Schematic | Measured/as-built | Safe/reset behavior |
|---|---|---|---|---|
| `!DCDC_EN` | GPIO_AD_00 / GPIO1_IO14 | GPIO_AD_03 / GPIO1_IO17 | Unknown | Legacy starts low, later high |
| `DCDC_CLK` | Pad muxed as GPIO_AD_04 / GPIO1_IO18, no validated clock | GPIO_AD_04 / GPT2_COMPARE1 | Unknown | Unknown |
| `D_MCK` monitor | Not implemented | GPIO_AD_05 / GPT2_CAPTURE1 | Unknown | Input; keep disabled |
| `LED_G_ST` | GPIO_00 / GPIO1_IO00, unused | Same | Unknown | Low |
| `LED_R_ERR` | GPIO_01 / GPIO1_IO01, loop-counter blink | Same | Unknown | Low |
| `OPTO_RELAY_JACK` | GPIO_03 / GPIO1_IO03 | Same | Unknown | Low at boot; high before USB; polarity needs measurement |
| `D_OSC_SELECT` | GPIO_05 / GPIO1_IO05 | Same | Unknown | Low used for 44.1-kHz family |
| `D_I2S_DATA` | GPIO_04 / SAI1_TX_DATA00 | Same | Unknown | Peripheral output |
| `D_I2S_SCK` | GPIO_06 / SAI1_TX_BCLK | Same | Unknown | Peripheral output |
| `D_I2S_FCK` | GPIO_07 / SAI1_TX_SYNC | Same | Unknown | Peripheral output |
| `D_MCK` | GPIO_08 / SAI1_MCLK input | Same | Unknown | External oscillator input |
| `D_SPI_MISO` | GPIO_09 / FLEXIO1_IO01 | Same | Unknown | Boot-ROM low-level hazard |
| `D_SPI_MOSI` | GPIO_10 / FLEXIO1_IO02 | Same | Unknown | Peripheral output |
| `D_SPI_SCK` | GPIO_11 / FLEXIO1_IO03 | Same | Unknown | Peripheral output |
| `D_DAC_CS` | GPIO_12 / GPIO1_IO12 | Same | Unknown | High inactive |
| `D_DAC_RST` | GPIO_13 / GPIO1_IO13 | Same | Unknown | Low asserted |

## Legacy DAC bootstrap transactions

At 2 MHz FlexIO SPI, the prototype sends these three-byte writes after reset:

1. `98 08 C0`
2. `98 02 10`
3. `98 07 B0`
4. `98 08 40`

The redesign then writes `98 04 D8`: register 04h retains the default PCM/DSD
auto-mute bits and explicitly sets both channel-mute bits. This safety write is
additional to, and deliberately does not alter, the known-working sequence.

Register semantics still require confirmation against the exact DAC datasheet.

## Verified bring-up record (2026-07-19)

The Debug FlexSPI NOR image with SHA-256
`432AE83F08FF63C79F65666CED79044FDB6440EF21C851A65D4527900BF1C412` boots when
programmed at offset zero using MCU Boot Utility. Red and green LEDs alternate
once per second. Full GPIO initialization before `BOARD_InitBootClocks()` was
observed to stall clock setup; only GPIO_01/red may be initialized before it
until the responsible signal is isolated. Preserve `__MCUXPRESSO` for VTOR.

## Verified DAC SPI and SAI measurements (2026-07-19)

The diagnostic image passed the second hardware gate. FlexIO SPI operates at
2 MHz on the preserved legacy pins and produced `98 08 C0`, `98 02 10`,
`98 07 B0`, `98 08 40`, followed by the explicit two-channel mute write
`98 04 D8`. DAC reset and chip-select behavior were correct. With the relay
held disconnected, SAI1/eDMA continuously produced stereo I2S silence with
24 valid bits in 32-bit slots: 22.5792 MHz MCLK, 2.8224 MHz BCLK, and 44.1 kHz
LRCLK. These measurements are the baseline for USB integration.
