# reset-qemu-nvram.ps1 - Reset QEMU UEFI NVRAM to factory defaults
#
# Use after a clean build or when NVRAM is corrupted.
# Deletes the TEMP copy so the next QEMU launch gets a fresh one from build/.

$VARS_DEST = Join-Path $env:TEMP "OVMF_VARS_4M.fd"

if (Test-Path $VARS_DEST) {
    Remove-Item -Path $VARS_DEST -Force
    Write-Host "NVRAM reset: deleted $VARS_DEST" -ForegroundColor Green
    Write-Host "Next QEMU launch will use fresh OVMF_VARS from build/." -ForegroundColor DarkGray
} else {
    Write-Host "NVRAM already clean (no TEMP copy exists)." -ForegroundColor Yellow
}
