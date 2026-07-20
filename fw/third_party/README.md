# Pinned dependencies

TinyUSB is pinned to release 0.21.0, commit
`dae3f9a366bfcddbf9dcf1b48d7500286a849539`. Run
`tools/fetch_dependencies.ps1` to reproduce the checkout. The production build
uses TinyUSB's UAC2 device class and ChipIdea high-speed DCD; the legacy NXP USB
audio class is not compiled.

The `nxp-mcuxpresso` directory is the MIMXRT1011/EVK FlexSPI subset of the NXP
MCUXpresso SDK required by the firmware build. It includes the device headers,
CMSIS, selected peripheral drivers, startup code, board clocks, and FlexSPI boot
sources. Vendor copyright notices are retained in the copied files; upstream
sources are maintained by [NXP MCUXpresso](https://github.com/nxp-mcuxpresso).

The RT1010 device layer is vendored from
`mcux-devices-rt` commit `40ab7654b7fdc774e62e5f0234ab0a7cc6d1aa93`
(2026-06-25). This revision splits the device register definitions into the
`periph/` headers; those files are intentionally kept together with the updated
MIMXRT1011 device header.
