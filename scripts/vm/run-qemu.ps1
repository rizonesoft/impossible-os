# run-qemu.ps1 - Launch Impossible OS in QEMU on Windows
#
# Parameters:
#   -Xres   Horizontal resolution (default: 1280)
#   -Yres   Vertical resolution   (default: 720)
#   -Accel  Accelerator: auto, whpx, tcg (default: auto)
#
# UTS Timer Behavior:
#   auto/whpx: CPUID detects hypervisor -> LAPIC timer selected
#   tcg:       CPUID -> "TCGTCGTCGTCG" -> PIT timer selected
#
# Usage:
#   Double-click run-qemu-kvm.bat          -> 1280x720, WHPX accel
#   Double-click run-qemu-tcg.bat          -> 1280x720, TCG (PIT timer)
#   Double-click run-windows-1080p.bat     -> 1920x1080  scale=1x
#   Double-click run-windows-1440p.bat     -> 2560x1440  scale=2x
#   Double-click run-windows-4k.bat        -> 3840x2160  scale=2x
#
# Prerequisites:
#   1. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
#   2. Build in WSL2 first: bash scripts/build.sh
Param(
    [int]$Xres = 1280,
    [int]$Yres = 720,
    [ValidateSet('auto','whpx','tcg')]
    [string]$Accel = 'auto'
)

$ErrorActionPreference = "Stop"

# Resolve project root from this script's location (scripts/vm -> parent -> parent)
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"

$DISK      = Join-Path $BUILD "system-disk.img"
$OVMF_CODE = Join-Path $BUILD "OVMF_CODE_4M.fd"
$OVMF_VARS = Join-Path $BUILD "OVMF_VARS_4M.fd"
$VARS_DEST = Join-Path $env:TEMP "OVMF_VARS_4M.fd"

# Ensure disk image exists
if (-not (Test-Path $DISK)) {
    Write-Host "Missing: $DISK" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# Auto-copy OVMF firmware from WSL system path if not in build/
if (-not (Test-Path $OVMF_CODE) -or -not (Test-Path $OVMF_VARS)) {
    Write-Host "Copying OVMF firmware to build/..." -ForegroundColor Yellow
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/'
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/'
    if (-not (Test-Path $OVMF_CODE)) {
        Write-Host "Failed to copy OVMF." -ForegroundColor Red
        pause; exit 1
    }
}

# OVMF_VARS needs a writable copy - preserve across runs for NVRAM persistence.
# If the source (build/) is newer than the TEMP copy, refresh it (clean build case).
if (-not (Test-Path $VARS_DEST)) {
    Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST
} elseif ((Get-Item $OVMF_VARS).LastWriteTime -gt (Get-Item $VARS_DEST).LastWriteTime) {
    Write-Host "OVMF_VARS updated (clean build?) - refreshing NVRAM copy" -ForegroundColor Yellow
    Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST -Force
}

$Scale = if ($Yres -gt 2160) { 3 } elseif ($Yres -gt 1080) { 2 } else { 1 }
# Per-resolution VRAM: 1440p needs 32MB (14.7MB fb), 1080p needs 16MB.
# 720p fits in the default 8MB VGA VRAM.
# 4K uses bochs-display which manages VRAM automatically.
$VgaDevice = if ($Yres -ge 2160) { "bochs-display" } `
        elseif ($Yres -ge 1440) { "VGA,vgamem_mb=32" } `
        elseif ($Yres -ge 1080) { "VGA,vgamem_mb=16" } `
        else                    { "VGA" }

# Determine accelerator and CPU model
$CpuModel = if ($Accel -eq 'tcg') { 'qemu64' } else { 'Haswell' }
$TimerExpected = if ($Accel -eq 'tcg') { 'PIT (TCG path)' } else { 'LAPIC (HW accel path)' }

Write-Host "Launching Impossible OS at ${Xres}x${Yres} (HiDPI scale=${Scale}x)..." -ForegroundColor Green
Write-Host "  Accel:  $Accel" -ForegroundColor DarkGray
Write-Host "  Timer:  $TimerExpected" -ForegroundColor DarkGray
Write-Host "  Disk:   $DISK" -ForegroundColor DarkGray
Write-Host "  Device: $VgaDevice" -ForegroundColor DarkGray
Write-Host ""

# Find QEMU executable
$QEMU = "qemu-system-x86_64.exe"
if (-not (Get-Command $QEMU -ErrorAction SilentlyContinue)) {
    $QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (-not (Test-Path $QEMU)) {
        Write-Host "QEMU not found. Install from https://qemu.weilnetz.de/w64/" -ForegroundColor Red
        pause; exit 1
    }
}

# Build QEMU arguments
$QemuArgs = @()

# Accelerator
switch ($Accel) {
    'tcg'  { $QemuArgs += '-accel', 'tcg' }
    'whpx' { $QemuArgs += '-accel', 'whpx' }
    'auto' {
        # Try WHPX first (near-native), fall back to TCG (slow but works)
        $QemuArgs += '-accel', 'whpx', '-accel', 'tcg'
    }
}

$QemuArgs += @(
    '-cpu', $CpuModel,
    '-drive', "if=pflash,format=raw,readonly=on,file=$OVMF_CODE",
    '-drive', "if=pflash,format=raw,file=$VARS_DEST",
    '-drive', "id=disk0,file=$DISK,format=raw,if=none",
    '-device', 'ich9-ahci,id=ahci0',
    '-device', 'ide-hd,drive=disk0,bus=ahci0.0',
    '-m', '2G',
    '-serial', 'stdio',
    '-vga', 'none',
    '-device', "$VgaDevice,xres=$Xres,yres=$Yres",
    '-device', 'rtl8139,netdev=net0',
    '-netdev', 'user,id=net0',
    '-device', 'virtio-tablet-pci',
    '-rtc', 'base=localtime',
    '-no-reboot'
)

& $QEMU @QemuArgs
