# Dual-family polyphase output

The RT1011 sends the CS4398 either 176.4 kHz PCM from the 22.5792-MHz
oscillator (44.1-kHz USB family) or 192 kHz PCM from the 24.576-MHz oscillator
(48-kHz USB family). Each output sample advances an integer source position and
a modulo-output-rate phase remainder by `input_rate_hz`. Thus 44.1 to 176.4
and 48 to 192 are exact 4x conversions; 88.2 to 176.4 and 96 to 192 are exact
2x conversions when configured by the converter API. Equal-rate configurations
copy PCM directly and bypass the FIR. The USB descriptor currently advertises
only 44.1 and 48 kHz, matching the existing product interface.

The phase bank is selected from the fractional position, so input-rate changes
naturally select different phases rather than requiring a separate converter per
rate. The nearest phase row is selected once per output sample. Phase rounding
uses the endpoint row and bounds timing error to 1/128 of an input sample; the
tap loop itself contains only native M7 single-precision FMAs. Fractional-delay
kernels above phase 0.5 are exact reversals of their complementary lower-phase
kernels, so only phases 0 through 32 are stored. The stored bank is copied from
the factory image into DTCM at startup. The process function and its stereo FIR
kernel run from ITCM.

SAI/eDMA transfers use 176 output frames for the 176.4-kHz family and 192
output frames for the 192-kHz family, keeping each block close to 1 ms.

The checked-in bank represents 64 fractional phases plus the phase-1 endpoint,
with 128 taps per phase, binary32 coefficients, and a Kaiser window (`beta=10`,
cutoff `0.47675` cycles/input-sample). Its 33 non-redundant stored rows occupy
33 x 128 x 4 = 16,896 bytes in DTCM. Runtime history uses separate left/right
128-frame rings, each mirrored once to make every 128-tap window contiguous
(2,048 bytes total); the worst-case 896-byte source staging buffer is also in
DTCM.

The standard RT1011 FlexRAM split remains unchanged: 32 KiB ITCM, 32 KiB DTCM,
and 64 KiB OCRAM. The Release link uses 62,624 of 392,704 application-slot
bytes, 24,684 of 65,504 OCRAM bytes, 22,784 of 32,768 DTCM bytes, and 1,984 of
32,768 ITCM bytes. The fixed 8-KiB stack occupies `0x20006000` through
`0x20008000`, leaving 1,792 DTCM bytes between linked sections and the stack.
The boot manager and both 384-KiB application slots remain unchanged.

The inner loop performs 176,400 x 128 x 2 = 45.16 million or 192,000 x 128 x 2
= 49.15 million sample MACs per second using the 500-MHz Cortex-M7 FPv5 hardware
FPU. Binary32 exactly
represents every 24-bit input integer, phase selection occurs once per output,
and convolution walks the power-of-two history ring without per-tap bounds
checks. The filter needs 64 future input frames, adding about 1.45 ms at
44.1 kHz or 1.33 ms at 48 kHz to the pipeline. The maintenance HID performance
report exposes average, maximum, and last refill cycles plus the peak pending-
refill depth.

Four near-one-millisecond output buffers form a permanent eDMA scatter/gather ring.
Each completion only retires buffer ownership in the interrupt; main context
regenerates those retired buffers while eDMA continues from the remaining
entries. The SDK's modulo TCD count is retained only as diagnostic evidence.
Each DMA interrupt releases exactly one software entry; delayed queue positions
are absorbed by subsequent interrupts instead of causing burst refills that
advance the resampler and USB ring ahead of the output timeline. Audio service
also runs between queued USB control and maintenance events.

A 512-tap hardware experiment was rejected because its direct stereo FIR was
projected from the measured 64-tap baseline to require about 816,000 average
and 914,000 worst-case cycles per one-millisecond refill, exceeding the
500,000-cycle deadline at 500 MHz. A 1024-tap direct FIR would be further over
budget. The 128-tap design is the largest conservative step from that baseline;
its actual target timing must still be confirmed through the performance
report after flashing.

`tools/generate_polyphase_coefficients.py` regenerates the exact checked-in
header without third-party packages. Host tests cover complete-block output,
both output families, exact 4x and 2x phase progression, native-rate bypass, DC
response after FIR warm-up, bit-exact equivalence between blocked and larger
streaming calls across history wraps, and CS4398 register mode writes.

USB and DAC clocks are independent physical domains. The firmware uses the
asynchronous USB feedback endpoint and a bounded ring-fill controller to make
the host adjust its packet cadence toward the selected DAC clock. The SRC ratio
itself remains the exact nominal rational ratio; it is not adaptively retuned.
Consequently, feedback handles long-term oscillator drift while the ring
absorbs short-term packet jitter. There is no hardware clock-lock or adaptive
SRC-ratio mechanism in this design.
