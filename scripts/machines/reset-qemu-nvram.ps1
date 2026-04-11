# reset-qemu-nvram.ps1 - Reset QEMU UEFI NVRAM to factory defaults
#
# Use after a clean build or when NVRAM is corrupted.
# Deletes all TEMP copies so the next QEMU launch gets fresh ones from build/.

$files = @(
    (Join-Path $env:TEMP "OVMF_VARS_4M.fd"),
    (Join-Path $env:TEMP "OVMF_VARS_secureboot.fd")
)

$deleted = 0
foreach ($f in $files) {
    if (Test-Path $f) {
        Remove-Item -Path $f -Force
        Write-Host "Deleted: $f" -ForegroundColor Green
        $deleted++
    }
}

if ($deleted -gt 0) {
    Write-Host "NVRAM reset: $deleted file(s) deleted." -ForegroundColor Green
    Write-Host "Next QEMU launch will use fresh OVMF_VARS from build/." -ForegroundColor DarkGray
} else {
    Write-Host "NVRAM already clean (no TEMP copies exist)." -ForegroundColor Yellow
}
