# Fixed-rate polyphase output

The RT1011 sends the CS4398 a fixed 192 kHz stream. The USB input formats
remain 44.1 and 48 kHz; each output sample advances an integer source position
and a modulo-192,000 phase remainder by `input_rate_hz`. Thus 44.1 kHz is the
exact repeating ratio 147 input samples per 640 output samples, while 48 kHz
is exactly one input sample per four output samples. The phase bank is selected
from the fractional position, so input-rate changes naturally skip/select
different phases rather than requiring a separate converter per rate. The
nearest phase row is selected once per output sample. Phase rounding uses
the endpoint row and bounds timing error to 1/128 of an input sample; the tap
loop itself contains only native M7 single-precision FMAs. Fractional-delay
kernels above phase 0.5 are exact reversals of their complementary lower-phase
kernels, so only phases 0 through 32 are stored. The stored bank is copied from
the factory image into DTCM at startup. The process function and its stereo FIR
kernel run from ITCM.

SAI/eDMA transfers use 192 output frames per block, exactly 1 ms at 192 kHz.

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

The inner loop performs 192,000 x 128 x 2 = 49.15 million sample MACs per
second using the 500-MHz Cortex-M7 FPv5 hardware FPU. Binary32 exactly
represents every 24-bit input integer, phase selection occurs once per output,
and convolution walks the power-of-two history ring without per-tap bounds
checks. The filter needs 64 future input frames, adding about 1.45 ms at
44.1 kHz or 1.33 ms at 48 kHz to the pipeline. The maintenance HID performance
report exposes average, maximum, and last refill cycles plus the peak pending-
refill depth.

Four one-millisecond output buffers form a permanent eDMA scatter/gather ring.
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
the exact 44.1-kHz accumulator count, 48-kHz scheduling, DC response after FIR
warm-up, bit-exact equivalence between one-millisecond and larger streaming
calls across history wraps, and CS4398 register mode writes.
