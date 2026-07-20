$ErrorActionPreference = 'Stop'
$TinyUsbCommit = '86ad6e56c1700e85f1c5678607a762cfe3aa2f47'
$Destination = Join-Path $PSScriptRoot '..\third_party\tinyusb'

if (-not (Test-Path -LiteralPath $Destination)) {
    git clone https://github.com/hathach/tinyusb.git $Destination
}
git -C $Destination fetch --depth 1 origin $TinyUsbCommit
git -C $Destination checkout --detach $TinyUsbCommit
if ((git -C $Destination rev-parse HEAD) -ne $TinyUsbCommit) {
    throw 'TinyUSB revision verification failed'
}
