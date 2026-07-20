# Pinned dependencies

TinyUSB is pinned to release 0.18.0, commit
`86ad6e56c1700e85f1c5678607a762cfe3aa2f47`. Run
`tools/fetch_dependencies.ps1` to reproduce the checkout. The production build
uses TinyUSB's UAC2 device class and ChipIdea high-speed DCD; the legacy NXP USB
audio class is not compiled.
