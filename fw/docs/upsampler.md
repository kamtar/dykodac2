# 8192-tap exact-4x FFT interpolator

The production USB paths are fixed at 44.1 to 176.4 kHz and 48 to 192 kHz.
Both use one 8192-tap symmetric linear-phase prototype decomposed exactly as
`Hp[k] = h[4*k+p]`, for four 2048-tap branches corresponding to source-sample
fractions 0, 1/4, 1/2, and 3/4. There is no phase accumulator, 64-phase bank,
phase rounding, or adaptive SRC ratio. Native 176.4 and 192 kHz configurations
remain byte-for-byte bypasses.

## Filter and convolution

The imported sequential float32 FIR source declares four interpolation phases
and no embedded interpolation gain. The command `tools/generate_polyphase_coefficients.py --input <header>` applies its documented x4 zero-stuff gain, then decomposes the
effective taps on the 4x output grid. Its source-normalized cutoff is
0.47677267573696145 cycles/source-sample (0.11919316893424036 on the output
grid); after the gain is applied, all four phase branches have unity DC gain.
The same effective filter serves both clock families.

Each 2048-tap branch is split into eight 256-tap partitions. A 512-point
CMSIS-DSP packed real FFT implements uniform frequency-domain delay-line
convolution with overlap-add. Each stereo source block needs two forward FFTs,
shared by all phases. For a phase, each QSPI filter value is read once and used
for both left and right complex accumulations, followed by two inverse FFTs.
The second 256 samples of every inverse result are retained as overlap, so the
result is linear convolution rather than circular convolution.

To bound main-loop refill time, a source-block FFT and each of the four phase
slices are separate work units. Normal DMA calls execute at most one phase
slice (16 KiB of contiguous coefficient reads). Four slices finish before the
next 256-source-frame block can fill. The packed output queue is sized from a
long-run cadence proof: its worst reservation is 1920 frames at 48 kHz and
1764 frames at 44.1 kHz. It cannot overwrite output still owned by the
consumer.

## Memory and flash traffic

- QSPI: 65,536-byte `[phase][partition][packed bin]` coefficient table,
  contiguous in consumption order.
- OCRAM: 32,768-byte stereo frequency-domain delay line plus 7,680 bytes of
  the packed output queue.
- DTCM: accumulators, FFT scratch, input block, four-phase stereo overlap, the
  3,840-byte queue extension, and existing eDMA state.
- ITCM: CMSIS RFFT/CFFT kernels and interpolation hot functions.
- Heap: unused in the audio path.

At 48 kHz a 256-frame partition arrives 187.5 times/s. Traversing all 64 KiB
of spectra once per source block is 12.288 MB/s of coefficient traffic. Stereo
sharing prevents that traffic from doubling. At 44.1 kHz it is 11.290 MB/s.

The Release map reports 119,564 bytes flash, 65,164/65,504 bytes OCRAM,
24,064/32,768 bytes linked DTCM before the fixed 8 KiB stack, and
4,576/32,768 bytes ITCM. The linked DTCM state ends 512 bytes below the stack.

## Reset and latency

Every `configure()` call clears the input block, FFT history, overlap, spectral
accumulators, output queue, scheduling state, and startup delay. Audio start
always configures the converter, so startup, stream restart, USB rate change,
and clock-family restart cannot retain old filter state. The existing clock
switch/mute/SAI/eDMA sequence is unchanged.

The FIR group delay is 4095.5 output samples: 23.217120 ms at 176.4 kHz and
21.330729 ms at 192 kHz. Bounded FFT collection/scheduling adds exactly 10 ms
of digital-zero startup. The existing four-buffer DMA ring contributes
3.990930 ms at 176.4 kHz or 4.000000 ms at 192 kHz. Total pipeline latency is
therefore 37.208050 ms and 35.330729 ms respectively.

## Verification

The C++ host suite runs the actual CMSIS streaming core and compares its float
output against binary64 direct convolution using the exact float32 prototype.
It covers impulse, DC, 1/10/19/20 kHz, sweep, multitone, deterministic random
input, all 256-frame partition boundaries, both DMA cadences, phase ordering,
native bypass, and reset. The measured worst absolute FFT/direct difference is
0.000452063 for test amplitudes of 1000 (4.52e-7 relative); the worst vector RMS
difference is 7.12953e-5.

`tests/host/verify_fft_interpolator.py` writes detailed response and known
high-rate reconstruction results to `docs/fft-interpolator-verification.json`.
Passband ripple through 20 kHz at 44.1 kHz is 1.354e-5 dB, the 20 kHz first
image is -138.66 dB relative at 44.1 kHz and -125.59 dB at 48 kHz, and the
worst stopband from 22.05 kHz is -121.10 dB for the effective float32
prototype. Known-reference multitone reconstruction RMS error is 9.12e-8 at
44.1 kHz and 9.13e-8 at 48 kHz.

Target CPU usage and worst refill cycles cannot be truthfully measured without
running this image on the RT1011. The existing maintenance HID refill-timing
report exposes count, total, maximum, last cycles, maximum pending depth, and
coalesced callbacks. Those hardware values must be captured after flashing;
no host timing is presented as target timing.
