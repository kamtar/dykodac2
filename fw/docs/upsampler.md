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
loop itself contains only native M7 single-precision FMAs. The complete bank
is copied from the factory image into DTCM at startup, and the hot process
function is copied into ITCM, so coefficient and instruction reads do not
contend with XIP traffic.

SAI/eDMA transfers use 192 output frames per block, exactly 1 ms at 192 kHz.

The checked-in bank is 64 fractional phases plus a phase-1 endpoint, 64 taps
per phase, binary32 coefficients, and a Kaiser window (`beta=10`, cutoff
`0.47675` cycles/input-sample). It occupies 65 x 64 x 4 = 16,640 bytes in
DTCM. Runtime history uses separate left/right 128-frame rings, each mirrored
once to make every 64-tap window contiguous (2,048 bytes total); the 1,024-byte
source staging buffer is also in DTCM. The Release map uses 20,556 of 65,504
OCRAM bytes, 1,504 of 32 KiB ITCM, and DTCM through `0x20006000`; the fixed
8 KiB stack occupies `0x20006000` through `0x20008000`.

The inner loop performs 192,000 x 64 x 2 = 24.58 million sample MACs per
second using the Cortex-M7 FPv5 hardware FPU. Binary32 exactly represents every
24-bit input integer, phase selection occurs once per output, and convolution
walks the power-of-two history ring without per-tap bounds checks. A larger
phase bank or more taps should be benchmarked on hardware before adoption.
The maintenance HID performance report exposes average, maximum, and last
refill cycles plus the peak pending-refill depth.

`tools/generate_polyphase_coefficients.py` regenerates the exact checked-in
header without third-party packages. Host tests cover complete-block output,
the exact 44.1-kHz accumulator count, 48-kHz scheduling, DC response after FIR
warm-up, bit-exact equivalence between the production and bounded streaming
paths across history wraps, and CS4398 register mode writes.
