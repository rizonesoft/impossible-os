# run-windows.ps1 — Launch Impossible OS in QEMU on Windows
#
# Parameters:
#   -Xres  Horizontal resolution (default: 1280)
#   -Yres  Vertical resolution   (default: 720)
#
# Usage:
#   Double-click run-windows.bat             → 1280×720  (default)
#   Double-click run-windows-1080p.bat       → 1920×1080  scale=1×
#   Double-click run-windows-1440p.bat       → 2560×1440  scale=2×
#   Double-click run-windows-4k.bat          → 3840×2160  scale=2×
#
# Prerequisites:
#   1. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
#   2. Build in WSL2 first: bash scripts/build.sh
Param(
    [int]$Xres = 1280,
    [int]$Yres = 720,
    [switch]$DebugBoot  # Inject X:\DEBUG flag to show live boot output instead of splash
)

$ErrorActionPreference = "Stop"

# Resolve project root from this script's location (scripts/ -> parent)
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent $SCRIPT_DIR
$BUILD      = Join-Path $PROJECT "build"

$DISK      = Join-Path $BUILD "system-disk.img"
$OVMF_CODE = Join-Path $BUILD "OVMF_CODE_4M.fd"
$OVMF_VARS = Join-Path $BUILD "OVMF_VARS_4M.fd"
$VARS_DEST = Join-Path $env:TEMP "OVMF_VARS_4M.fd"

# Ensure disk image exists
if (-not (Test-Path $DISK)) {
    Write-Host "Missing: $DISK" -ForegroundColor Red
    Write-Host "Run 'make all' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# Auto-copy OVMF firmware from WSL system path if not in build/
if (-not (Test-Path $OVMF_CODE) -or -not (Test-Path $OVMF_VARS)) {
    Write-Host "Copying OVMF firmware to build/..." -ForegroundColor Yellow
    & wsl.exe -e bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/ && cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/"
    if (-not (Test-Path $OVMF_CODE)) {
        Write-Host "Failed to copy OVMF." -ForegroundColor Red
        pause; exit 1
    }
}

# OVMF_VARS needs a writable copy
Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST -Force

$Scale = if ($Yres -gt 2160) { 3 } elseif ($Yres -gt 1080) { 2 } else { 1 }
# Per-resolution VRAM: 1440p needs 32MB (14.7MB fb), 1080p needs 16MB.
# 720p fits in the default 8MB VGA VRAM.
# 4K uses bochs-display which manages VRAM automatically.
$VgaDevice = if ($Yres -ge 2160) { "bochs-display" } `
        elseif ($Yres -ge 1440) { "VGA,vgamem_mb=32" } `
        elseif ($Yres -ge 1080) { "VGA,vgamem_mb=16" } `
        else                    { "VGA" }

Write-Host "Launching Impossible OS at ${Xres}x${Yres} (HiDPI scale=${Scale}x)..." -ForegroundColor Green
Write-Host "  Disk:   $DISK" -ForegroundColor DarkGray
Write-Host "  Device: $VgaDevice" -ForegroundColor DarkGray
Write-Host "  Serial: [??] SPLASH  ${Xres}x${Yres}  scale=${Scale}x" -ForegroundColor DarkCyan
if ($DebugBoot) {
    Write-Host "  Mode:   DEBUG (splash disabled, live text on screen)" -ForegroundColor Yellow
}
Write-Host ""

# ---- Debug boot flag: inject DEBUG file into Logs partition ----
if ($DebugBoot) {
    $LOG_OFFSET = 68157440  # Must match Makefile LOG_OFFSET
    & wsl.exe -e bash -c "echo -n debug | mcopy -i ~/impossible-os/build/system-disk.img@@${LOG_OFFSET} - ::DEBUG 2>/dev/null; echo -n debug | mcopy -o -i ~/impossible-os/build/system-disk.img@@${LOG_OFFSET} - ::DEBUG"
    Write-Host "  [OK] DEBUG flag injected into Logs partition" -ForegroundColor Green
} else {
    # Remove DEBUG flag if it exists (normal boot)
    & wsl.exe -e bash -c "mdel -i ~/impossible-os/build/system-disk.img@@68157440 ::DEBUG 2>/dev/null" 2>$null
}

# Find QEMU executable
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
    -device "$VgaDevice,xres=$Xres,yres=$Yres" `
    -device rtl8139,netdev=net0 `
    -netdev user,id=net0 `
    -device virtio-tablet-pci `
    -rtc base=localtime `
    -no-reboot
