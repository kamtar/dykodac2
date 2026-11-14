# Two-stage exact-4x interpolator

The production USB paths are fixed at 44.1 to 176.4 kHz and 48 to 192 kHz.
Both use the same normalised coefficients. There is no phase accumulator,
phase rounding, or adaptive SRC ratio. The reasoning behind the filter design,
the DAC compensation, and the volume/dither stage is in
[filter-and-volume-design.md](filter-and-volume-design.md); this file
describes the mechanism.

## Signal flow

1. **Stage 1, source rate to 2x.** A 1024-tap linear-phase Kaiser prototype on
   the 2x grid, split exactly as `Hp[k] = h[2*k+p]` into two 512-tap branches.
   Each branch is two 256-tap partitions, convolved with a 512-point CMSIS-DSP
   packed real FFT (uniform frequency-domain delay line, overlap-add). Each
   stereo source block needs two forward FFTs, shared by both phases, and two
   inverse FFTs per phase. The result is 512 time-ordered float samples per
   channel per 256-frame source block.
2. **Stage 2, 2x to 4x.** A 63-tap true half-band in the time domain:
   `y[2m] = sum_j halfband_branch[j] * u[m-j]` (32 taps) and
   `y[2m+1] = u[m-15]` (exact pass-through, no multiply).
3. **Gain, dither, quantisation.** `round(value * gain + tpdf)` saturated to
   signed 24 bit. This is the only quantisation in the path. The gain is the
   host volume plus fixed headroom, slewed per output frame.

Sizes are taken from the generated headers (`audio/fft_filter_spectra.hpp`,
`audio/halfband_coefficients.hpp`), so regenerating with a different stage-1
length needs only a rebuild.

## Scheduling

To bound main-loop refill time, the work for one 256-frame source block is
split into one forward-FFT job and four slices, one slice per normal DMA
refill: stage-1 phase 0, stage-1 phase 1, then stage 2 with gain/dither for
each half of the block. All 1024 output frames of a block are published to
the packed output queue when the fourth slice finishes. This is the same
cadence as the previous single-stage design, so the output queue sizing is
unchanged: worst reservation 1920 frames at 48 kHz and 1764 at 44.1 kHz. The
queue cannot overwrite output still owned by the consumer.

## Memory

- QSPI: 8,192-byte `[phase][partition][packed bin]` stage-1 spectra.
- OCRAM: 8,192-byte stereo frequency-domain delay line, the stage-2 float
  block buffer with half-band history (4,352 bytes), and the main part of the
  packed output queue.
- DTCM: accumulators, FFT scratch, input block, overlap, half-band taps, the
  queue extension, and eDMA state.
- ITCM: CMSIS RFFT/CFFT kernels and the interpolation hot functions.
- Heap: unused in the audio path.

Release map of this revision: 72,012 bytes flash, 44,940/65,504 bytes OCRAM,
20,096/32,768 bytes linked DTCM (4,480 bytes below the 8 KiB stack), and
5,824/32,768 bytes ITCM.

## Reset and latency

Every `configure()` clears the input block, FFT history, overlap, stage-2
buffer and history, output queue, scheduling state and startup delay, and
reseeds the dither. The gain target and current gain survive it, so a stream
restart never changes loudness.

Composite group delay is 1054 output samples: 5.975 ms at 176.4 kHz and
5.490 ms at 192 kHz. Bounded FFT collection/scheduling adds exactly 10 ms of
digital-zero startup, and the four-buffer DMA ring about 4 ms. A volume
change takes effect when a slice is computed, up to about one block ahead of
playback.

## Verification

The C++ host suite runs the actual streaming core and compares its float
output (after stage 2, before gain and dither) against binary64 direct
convolution with the equivalent 2109-tap composite prototype. It covers
impulse, DC, sines, sweep, multitone, random input, all partition boundaries,
both DMA cadences, queue wrap, native bypass and reset, plus the stage-2
pass-through property, gain accuracy, headroom, TPDF statistics, ramp
behaviour, saturation and the DAC ceiling writes. Worst absolute FFT/direct
difference is 6.19e-4 for test amplitudes of 1000; worst RMS is 1.09e-4.

`tests/host/verify_fft_interpolator.py` writes the response, time-domain and
reconstruction figures to `docs/fft-interpolator-verification.json`.

Target CPU usage and worst refill cycles have not been measured: no RT1011
hardware was available. The maintenance HID refill-timing report exposes
count, total, maximum and last cycles; capture those after flashing.
