# Hardware map

The working 2022 firmware is authoritative until the assembled board is
measured. “Unknown” is deliberate; it must not be filled from inference.

| Signal | Working firmware | Schematic | Measured/as-built | Safe/reset behavior |
|---|---|---|---|---|
| `!DCDC_EN` | GPIO_AD_00 / GPIO1_IO14 | GPIO_AD_03 / GPIO1_IO17 | Package pin 60 bodge | Low during early boot, then high enables DC/DC |
| `DCDC_CLK` | Pad muxed as GPIO_AD_04 / GPIO1_IO18, no validated clock | GPIO_AD_04 / GPT2_COMPARE1 | Unknown | Unknown |
| `D_MCK` monitor | Not implemented | GPIO_AD_05 / GPT2_CAPTURE1 | Unknown | Input; keep disabled |
| `LED_G_ST` | GPIO_01 / GPIO1_IO01 | GPIO_00 / GPIO1_IO00 | Swapped on assembled board | Low |
| `LED_R_ERR` | GPIO_00 / GPIO1_IO00 | GPIO_01 / GPIO1_IO01 | Swapped on assembled board | Low |
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

## Current DAC bootstrap transactions

After holding reset for 10 ms, firmware waits 1 us after releasing reset and
sends these three-byte writes at 6 MHz FlexIO SPI:

1. `98 08 C0`
2. `98 02 12`
3. `98 07 B0`
4. `98 08 40`
5. `98 04 18`

The first write enables the control port while keeping the DAC powered down;
it is completed before the approximately 218-MCLK-cycle stand-alone power-up
sequence. The second write selects quad-speed I2S. The final write disables
automatic silence muting and explicitly mutes both channels until stream
startup is stable.

The software output stage follows the USB clock family: 44.1-kHz input becomes
176.4-kHz PCM from the 22.5792-MHz oscillator, while 48-kHz input becomes
192-kHz PCM from the 24.576-MHz oscillator. GPIO1_IO05 selects the external
oscillator and SAI1 receives it on the MCLK1 input path. The CS4398 remains in
quad-speed I2S mode (`98 02 12`) for both families; register 07 remains the
fast-roll-off internal interpolation selection (`98 07 B0`). The CS4398 does
not provide an external digital-filter bypass; “external filter” therefore
means the board's analog output filter, not a bypass of the DAC's digital
interpolator.

## Verified bring-up record (2026-07-19)

The Debug FlexSPI NOR image with SHA-256
`432AE83F08FF63C79F65666CED79044FDB6440EF21C851A65D4527900BF1C412` boots when
programmed at offset zero using MCU Boot Utility. Red and green LEDs alternate
once per second. Full GPIO initialization before `BOARD_InitBootClocks()` was
observed to stall clock setup; only GPIO_01 (green on the assembled board) may be initialized before it
until the responsible signal is isolated. Preserve `__MCUXPRESSO` for VTOR.

## Historical verified DAC SPI and SAI measurements (2026-07-19)

The diagnostic image passed the second hardware gate. FlexIO SPI operates at
2 MHz on the preserved legacy pins and produced `98 08 C0`, `98 02 10`,
`98 07 B0`, `98 08 40`, followed by the explicit two-channel mute write
`98 04 D8`. DAC reset and chip-select behavior were correct. With the relay
held disconnected, SAI1/eDMA continuously produced stereo I2S silence with
24 valid bits in 32-bit slots: 22.5792 MHz MCLK, 2.8224 MHz BCLK, and 44.1 kHz
LRCLK. These measurements are the baseline for USB integration.
