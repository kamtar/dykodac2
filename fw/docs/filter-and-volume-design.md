# Interpolation filter and volume: design decisions

This records what was changed in the 44.1/48 kHz playback path, why, and what
each decision costs. All filter figures are measured by
`tools/generate_polyphase_coefficients.py` and
`tests/host/verify_fft_interpolator.py` on the rounded binary32 coefficients
that ship in the image. Nothing here was measured on the board; the hardware
was not available.

## Requirements

1. Reconstruct the original band-limited waveform as faithfully as possible;
   time-domain behaviour matters.
2. Stop band of at least 130 dB starting at the source Nyquist frequency
   (fs/2), so nothing above fs/2 survives to be imaged.
3. A multistage structure.
4. Take the CS4398's own filter into account so the FIR and the DAC work
   together.
5. Volume control in the MCU, dithered, with a fixed hardware attenuation in
   the DAC kept as protection.

## What was wrong before

The previous filter was a single 8192-tap prototype (imported coefficients of
undocumented origin, not reproducible from the repository).

| | Previous | Now |
|---|---|---|
| Stop band from fs/2 | -121.1 dB | -141.0 dB |
| Pass band (44.1 kHz source) | flat to 21.34 kHz | flat to 21.38 kHz |
| FIR + DAC flatness, 0-20 kHz | about +/-0.01 dB (DAC uncorrected) | within 0.00003 dB of the DAC model |
| Group delay at 176.4 kHz | 23.2 ms | 5.98 ms |
| Pre-ringing down to -100 dB | 22.2 ms | 4.4 ms |
| Intersample peaks above 0 dBFS | hard-clipped | pass unclipped |
| Coefficient table | 64 KiB | 8 KiB + 32 floats |
| FFT history | 32 KiB | 8 KiB |
| Release image (flash) | 126.8 KB | 72.0 KB |
| OCRAM used | 65,164 B | 44,940 B |
| Reproducible from the repo | no | yes |

The previous filter missed the 130 dB requirement, rang five times longer
than its pass band justified, and clipped any source whose reconstructed
waveform exceeds full scale (common on loud masters: a full-scale fs/4 sine
reconstructs to +3.0 dBFS, a clipped two-tone to +2.8 dBFS).

## How the DAC shaped the design

Source: Cirrus Logic CS4398 datasheet DS568F3, filter tables on pages 10-11
and response plots in figures 36-39.

The firmware runs the DAC in quad-speed mode (176.4 or 192 kHz input). In that
mode the DAC's internal interpolator is deliberately gentle:

- -3 dB at 0.49 Fs (86 kHz at 176.4 kHz), stop band from 0.635 Fs (112 kHz),
  90 dB attenuation, group delay 4.7 samples (27 microseconds).
- Pass band: +0.010 dB hump at 0.065 Fs, back to 0 dB at 0.105 Fs, then
  falling (-0.02 dB at 0.125 Fs).

Consequences, each of which is a decision in this design:

1. **The DAC does no reconstruction work below 86 kHz, so the FIR must do all
   of it.** Everything between source Nyquist and 88 kHz reaches the analog
   output untouched by the DAC. That is why the stop band requirement applies
   to the whole range from fs/2 up to the 4x Nyquist, and why the second
   stage is a full 146 dB half-band rather than something short: the images
   it removes (66-110 kHz at 176.4 kHz) sit exactly where the DAC still
   passes signal.
2. **The DAC is left only the images around its own input rate.** After the
   FIR the first content above the audio band is at 154 kHz and up (0.875 Fs).
   The DAC's fast roll-off filter attenuates that by at least 90 dB, and the
   analog stage adds more. This 90 dB is the weakest link of the whole chain
   and cannot be improved in firmware, because the chip has no filter bypass.
3. **Fast roll-off stays selected.** The slow roll-off filter starts its stop
   band at 0.868 Fs with only 75 dB, which barely covers the 0.875 Fs images.
   It is 15 dB worse for no time-domain gain, since the DAC filter is already
   only a few samples long at this rate.
4. **The DAC's pass-band deviation is pre-compensated.** Stage 1 carries the
   inverse of the figure 39 curve. Because the curve is normalised to the DAC
   rate, one coefficient set is correct for both clock families. At 44.1 kHz
   the DAC alone deviates from -0.006 to +0.010 dB over 0-20 kHz; the FIR and
   DAC model together are flat within 0.00003 dB. The curve was read off a
   printed plot (about +/-0.003 dB reading error) and describes a typical
   part, so the real residual is a few thousandths of a dB. The correction is
   inaudible; it is included because it is free and removes a known error.
   The analog output stage is not included (no measurement exists);
   `DAC_RESPONSE_DB` in the generator is the single place to put a measured
   curve.
   *Is the filter tuned to a typical part?* Only in this one respect, and the
   exposure is bounded. The datasheet states the filter response is
   guaranteed by design (note 5): the pass-band shape comes from the digital
   interpolator, which is identical in every chip, and from a
   switched-capacitor stage set by capacitor ratios and the clock. The guaranteed
   limit is +/-0.01 dB from 10 Hz to 20 kHz. If a real chip had no deviation
   at all, the correction would leave -0.010 to +0.006 dB, the same size of
   error as not correcting. Stop band, transition, ringing, delay and headroom
   do not depend on this curve or on any part-to-part variation. Setting
   `DAC_RESPONSE_DB = None` and regenerating removes the correction.
5. **Interpolating to the quad-speed rate is the right target.** It is the
   mode in which the DAC's own filter has the least influence on the impulse
   response: the FIR defines the time-domain behaviour and the DAC adds 27
   microseconds of symmetric smoothing.
6. **Nothing in the datasheet promises internal headroom for peaks above
   full scale.** The 3.25 dB of headroom applied in the MCU (see below) keeps the
   reconstructed waveform below full scale at the DAC input too.
7. **130 dB is beyond what the DAC can show.** Its dynamic range is 120 dB
   A-weighted. The stop band depth is set by the requirement, not by anything
   the DAC could resolve; it costs nothing extra.
8. **Direct DSD is not an option.** It is the only real bypass of the DAC's
   digital filter, but the DSD pins (1, 2, 28) are tied to ground on this
   board, and the mode disables volume control and loses 3 dB of range.

## Filter design

Two stages, both linear phase, both Kaiser-windowed, same normalised
coefficients for 44.1 and 48 kHz sources.

**Stage 1, source rate to 2x.** 1024 taps on the 2x grid, Kaiser beta 15,
evaluated by partitioned FFT convolution (2 phases, 2 partitions of 256 per
phase, 512-point real FFT). It does all the sharp work: flat to 21.38 kHz,
-6 dB at 21.64 kHz, below -130 dB from 22.046 kHz (44.1 kHz source). The
generator places the cutoff as high as the stop band target allows, with a
10 dB design margin.

**Stage 2, 2x to 4x.** 63-tap true half-band, Kaiser beta 15, evaluated in the
time domain. One of its two phases is a pure delay, so every stage-1 sample
passes through bit-exact in float and only the in-between samples are
computed, with 32 multiplies each.

Decisions and their reasons:

- **Linear phase.** A band-limited waveform is reconstructed correctly only if
  all its components keep their relative timing. Minimum phase would remove
  the pre-ringing but shift the phase of the upper octave and change the
  shape of every transient. Rejected for that reason.
- **Kaiser window, not equiripple.** For a given stop band depth the Kaiser
  window concentrates the impulse response in time almost optimally, and its
  stop band keeps falling with frequency. Equiripple designs carry
  non-decaying echo taps at the ends of the impulse response.
- **Stop band begins at fs/2, not above it.** Any filter still passing signal
  at fs/2 lets images of the top of the band fold back just above fs/2. With
  the stop band at fs/2 there are none, at the cost of not passing the last
  670 Hz below Nyquist.
- **Length: 1024 taps for stage 1.** This passes 97% of the band. The
  alternative is 512 taps: flat only to 20 kHz, about -137 dB, half the delay
  and ringing. Both meet the requirement; 1024 was chosen because the stated
  priority is fidelity to the original waveform. `STAGE1_TAPS` in the
  generator switches it; the C++ takes its sizes from the generated header.
- **Why multistage.** The two-stage cascade has the same response and impulse
  shape as an equivalent single 2109-tap 4x filter. Multistage does not sound
  different; it needs an eighth of the frequency-domain multiplies and of
  the coefficient memory of the previous design.
- **About the ringing.** The filter rings at about 21.6 kHz for 2.3 ms either
  side of a transient (to -60 dB). This is the unavoidable consequence of
  requirement 2: a 130 dB stop band at fs/2 fixes how narrow the transition
  is, and the transition width fixes the ringing length. It is only excited
  by content within a few hundred hertz of 21.6 kHz.

## Volume, dither and headroom

**Where volume is applied.** In the MCU, on the float output of stage 2,
immediately before the one and only conversion to 24-bit integers:
`sample = round(value * gain + dither)`, then saturation.

**Headroom.** Host volume 0 dB maps to an MCU gain of -3.25 dB. The float
pipeline cannot clip; only the final conversion can. Exactly 3.0 dB is not
quite enough: a full-scale fs/4 tone reconstructs to +3.0003 dBFS in steady
state and +3.09 dBFS at its onset, so the host test for it fails at -3.0.
-3.25 dB is the smallest value on the 1/256 dB grid with a margin, and
reconstructed peaks up to +3.25 dBFS pass intact at every volume setting.

**Dither.** TPDF, +/-1 LSB peak at 24 bits, independent for left and right.
It makes the quantisation error signal-independent noise at about -141 dBFS
instead of distortion. No noise shaping: the dither floor is already 20 dB
below the DAC's own noise, so shaping would add complexity for nothing.
A float32 only resolves 0.5 LSB above half scale, so the quantiser splits
each value into its exact integer and fractional parts and adds the dither to
the fraction, which keeps the dither exact at every level.

**Volume changes.** The gain moves toward its target by a slew-limited linear
ramp (full range in 20 ms), replacing the DAC's soft ramp. The gain survives
stream restarts.

**DAC attenuation as a protective ceiling.** The CS4398 attenuation registers
are written once with a fixed value (`board::dac_ceiling_db_8_8`, default
-9 dB) and never by the host. The TPA6120 has far more gain than needed, so
if the firmware ever emitted full-scale garbage the ceiling bounds how loud it
can get. Maximum possible output is DAC full scale plus the ceiling. Host
0 dB is ceiling plus headroom, 12.25 dB below DAC full scale. The default is a
guess: set it to the loudest level you would ever want.

**What this does not do.** It does not recover dynamic range. MCU volume and
DAC volume are both digital attenuation ahead of the same analog noise floor,
so the signal-to-noise ratio at a given loudness is unchanged. Only a hardware
change (lower amplifier gain, or attenuation after the DAC) fixes that. The
gains are the removed clipping, known and dithered arithmetic, and a hard
upper bound on output level.

**Known limitation.** The native 176.4/192 kHz bypass path copies samples
unchanged and therefore ignores MCU volume; it would play at the ceiling. It
is not reachable from USB (only 44.1 and 48 kHz are advertised). Volume must
be added to it before any native-rate format is exposed.

## Sound quality: an honest assessment

- Audible: removing intersample clipping on loud masters.
- Measurable, not audible: 20 dB deeper stop band, 0.01 dB flatter response,
  dither replacing rounding distortion at -146 dBFS.
- Practical: 17 ms less latency, an eighth of the frequency-domain multiplies,
  about 24 KiB of RAM and 55 KiB of flash freed.
- Unchanged: dynamic range, which is set by the DAC's analog noise and the
  amplifier gain.

## Verification status

Run on a PC with the real streaming code (`build.bat`: Release ARM build plus
host tests):

- Host suite passes, 822,894 checks. The streaming output matches binary64
  direct convolution with the composite prototype: worst absolute error
  6.19e-4 and worst RMS 1.09e-4 for amplitude-1000 inputs.
- Gain measured on the 24-bit output is within 2e-5 of target at 0, -6, -20
  and -60.5 dB.
- Dither error: mean below 0.002 LSB, variance 0.2495 LSB^2 (theory 0.25),
  bounded by 1.5 LSB, left/right correlation 0.01.
- A full-scale fs/4 tone at 45 degrees does not saturate at host 0 dB.
- Volume ramps never exceed the slew limit, and the gain survives a stream
  restart.
- Host volume produces no DAC register writes; the ceiling writes registers
  05h and 06h with 0x12 (-9 dB).

Not verified:

- Nothing has run on the RT1011. CPU time per refill, the analog output and
  the DAC's behaviour with the ceiling are unmeasured.
- `platform/nxp/main.cpp` and `audio_engine.cpp` are not part of the host
  build. The volume routing, the two ceiling write points (after DAC
  bootstrap and after every format configuration, both before unmute) and the
  safe stop on a failed write were checked by reading the code.

## Regenerating

```
python tools/generate_polyphase_coefficients.py
python tests/host/verify_fft_interpolator.py
```

The first writes `audio/fft_filter_spectra.hpp`,
`audio/halfband_coefficients.hpp`, `tests/host/fft_prototype_coefficients.hpp`
and `tests/host/fft_filter_metrics.json`, and fails if the stop band misses
130 dB. The second writes `docs/fft-interpolator-verification.json`.
