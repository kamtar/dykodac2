param([string]$Configuration = "Release", [UInt32]$Version = 1)
$ErrorActionPreference = "Stop"
premake5 gmake2
$env:CC = "arm-none-eabi-gcc"
$env:CXX = "arm-none-eabi-g++"
$env:AR = "arm-none-eabi-ar"
mingw32-make -C build/gmake2 -j1 dykodac2_fw_a dykodac2_fw_b dykodac2_boot ("config=" + $Configuration.ToLowerInvariant())
if ($LASTEXITCODE -ne 0) { throw "firmware build failed" }
python tools/package_update.py --build-dir ("build/gmake2/bin/" + $Configuration) --version $Version
if ($LASTEXITCODE -ne 0) { throw "update packaging failed" }
