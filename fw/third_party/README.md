# Pinned dependencies

TinyUSB is pinned to release 0.21.0, commit
`dae3f9a366bfcddbf9dcf1b48d7500286a849539`. Run
`tools/fetch_dependencies.ps1` to reproduce the checkout. The production build
uses TinyUSB's UAC2 device class and ChipIdea high-speed DCD; the legacy NXP USB
audio class is not compiled.
