# Architecture checkpoint

The application owns safety policy through an event-driven state machine.
USB callbacks will publish events and audio packets only. They will not write
GPIO, select oscillators, or access the DAC bus.

The currently implemented boundary contains:

- a named compile-time board map preserving prototype mappings;
- one source of truth for the two initial audio formats;
- integer UAC2 attenuation conversion and typed DAC commands;
- safety outputs for boot, preparation, playback, switching, stop, and fault;
- native tests that verify the core invariants without NXP registers.

The target build now reproduces the legacy flash/RAM map, startup vector table,
XIP boot header placement, and clock initialization under GNU Arm Embedded.
It also reproduces the working Cortex-M7 MPU/cache policy before peripheral
access; this is required for safe speculative-access behavior during XIP.
The bootable safe-idle image binds semantic relay and clock-selection services
to the named board map and uses a 1 ms SysTick only as a monotonic time source.
Timed behavior stays in the cooperative main loop.

The target build temporarily reads the unmodified prototype NXP SDK sources.
The next checkpoint is to import and pin the minimal SDK subset, add the typed
FlexIO DAC transport, then bring up generated SAI/eDMA silence while the relay
remains open. USB integration follows only after clocks are measured.
