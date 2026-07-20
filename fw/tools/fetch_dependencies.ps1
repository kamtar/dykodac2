$ErrorActionPreference = 'Stop'
$TinyUsbCommit = 'dae3f9a366bfcddbf9dcf1b48d7500286a849539'
$Destination = Join-Path $PSScriptRoot '..\third_party\tinyusb'

if (-not (Test-Path -LiteralPath $Destination)) {
    git clone https://github.com/hathach/tinyusb.git $Destination
}
git -C $Destination fetch --depth 1 origin $TinyUsbCommit
git -C $Destination checkout --detach $TinyUsbCommit
if ((git -C $Destination rev-parse HEAD) -ne $TinyUsbCommit) {
    throw 'TinyUSB revision verification failed'
}
