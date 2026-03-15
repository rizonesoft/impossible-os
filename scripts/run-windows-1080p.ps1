# run-windows-1080p.ps1 — Launch Impossible OS in QEMU at 1920×1080
#
# HiDPI test: scale=1× (≤1080p) but at native 1080p resolution.
# Serial output: [??] SPLASH  1920x1080  scale=1x  font=16px
#
# Prerequisites:
#   1. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
#   2. Build in WSL2 first: bash scripts/build.sh
#
# Usage: Double-click run-windows-1080p.bat

$ErrorActionPreference = "Stop"

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent $SCRIPT_DIR
$BUILD      = Join-Path $PROJECT "build"

$DISK      = Join-Path $BUILD "system-disk.img"
$OVMF_CODE = Join-Path $BUILD "OVMF_CODE_4M.fd"
$OVMF_VARS = Join-Path $BUILD "OVMF_VARS_4M.fd"
$VARS_DEST = Join-Path $env:TEMP "OVMF_VARS_4M.fd"

if (-not (Test-Path $DISK)) {
    Write-Host "Missing: $DISK" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

if (-not (Test-Path $OVMF_CODE) -or -not (Test-Path $OVMF_VARS)) {
    Write-Host "Copying OVMF firmware to build/..." -ForegroundColor Yellow
    & wsl.exe -e bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/ && cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/"
    if (-not (Test-Path $OVMF_CODE)) {
        Write-Host "Failed to copy OVMF." -ForegroundColor Red
        pause; exit 1
    }
}

Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST -Force

Write-Host "Launching Impossible OS at 1920x1080 (HiDPI scale=1x)..." -ForegroundColor Green
Write-Host "  Disk: $DISK" -ForegroundColor DarkGray
Write-Host "  Watch serial output for: [??] SPLASH  1920x1080  scale=1x" -ForegroundColor DarkCyan
Write-Host ""

$QEMU = "qemu-system-x86_64.exe"
if (-not (Get-Command $QEMU -ErrorAction SilentlyContinue)) {
    $QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (-not (Test-Path $QEMU)) {
        Write-Host "QEMU not found. Install from https://qemu.weilnetz.de/w64/" -ForegroundColor Red
        pause; exit 1
    }
}

& $QEMU `
    -cpu Haswell `
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" `
    -drive "if=pflash,format=raw,file=$VARS_DEST" `
    -drive "id=disk0,file=$DISK,format=raw,if=none" `
    -device ich9-ahci,id=ahci0 `
    -device ide-hd,drive=disk0,bus=ahci0.0 `
    -m 2G `
    -serial stdio `
    -vga none `
    -device VGA,xres=1920,yres=1080 `
    -device rtl8139,netdev=net0 `
    -netdev user,id=net0 `
    -device virtio-tablet-pci `
    -rtc base=localtime `
    -no-reboot
