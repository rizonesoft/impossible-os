# run-qemu.ps1 — Launch Impossible OS in QEMU on Windows
#
# Parameters:
#   -Xres   Horizontal resolution (default: 1280)
#   -Yres   Vertical resolution   (default: 720)
#   -Accel  Accelerator: auto, whpx, tcg (default: auto)
#
# UTS Timer Behavior:
#   auto/whpx: CPUID detects hypervisor → LAPIC timer selected
#   tcg:       CPUID → "TCGTCGTCGTCG" → PIT timer selected
#
# Disk images are copied from WSL to a local Windows directory to avoid
# the \\wsl.localhost\ 9P bridge which adds massive I/O latency.
#
# Usage:
#   Double-click run-qemu-kvm.bat          → 1280×720, WHPX accel
#   Double-click run-qemu-tcg.bat          → 1280×720, TCG (PIT timer)
#   Double-click run-windows-1080p.bat     → 1920×1080  scale=1×
#   Double-click run-windows-1440p.bat     → 2560×1440  scale=2×
#   Double-click run-windows-4k.bat        → 3840×2160  scale=2×
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

# ---- Paths ----
# Source: WSL build directory (via 9P bridge — slow for disk I/O)
$SCRIPT_DIR  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT     = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD       = Join-Path $PROJECT "build"

# Destination: local Windows directory (NTFS — fast disk I/O for QEMU)
$LOCAL_DIR   = "C:\Users\DerickPayne\ImpossibleOS"

$SRC_DISK      = Join-Path $BUILD "system-disk.img"
$SRC_OVMF_CODE = Join-Path $BUILD "OVMF_CODE_4M.fd"
$SRC_OVMF_VARS = Join-Path $BUILD "OVMF_VARS_4M.fd"

$DISK      = Join-Path $LOCAL_DIR "system-disk.img"
$OVMF_CODE = Join-Path $LOCAL_DIR "OVMF_CODE_4M.fd"
$OVMF_VARS = Join-Path $LOCAL_DIR "OVMF_VARS_4M.fd"

# ---- Ensure source exists ----
if (-not (Test-Path $SRC_DISK)) {
    Write-Host "Missing: $SRC_DISK" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# ---- Auto-copy OVMF firmware from WSL if not in build/ ----
if (-not (Test-Path $SRC_OVMF_CODE) -or -not (Test-Path $SRC_OVMF_VARS)) {
    Write-Host "Copying OVMF firmware to build/..." -ForegroundColor Yellow
    & wsl.exe -e bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/ && cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/"
    if (-not (Test-Path $SRC_OVMF_CODE)) {
        Write-Host "Failed to copy OVMF." -ForegroundColor Red
        pause; exit 1
    }
}

# ---- Copy images to local Windows directory ----
# Delete stale images first, then copy fresh ones.
# This avoids booting an outdated kernel after a rebuild.
if (-not (Test-Path $LOCAL_DIR)) {
    New-Item -ItemType Directory -Path $LOCAL_DIR -Force | Out-Null
    Write-Host "Created $LOCAL_DIR" -ForegroundColor DarkGray
}

Write-Host "Syncing disk images to $LOCAL_DIR ..." -ForegroundColor Yellow

# Always delete + re-copy disk image (ensures fresh after rebuild)
if (Test-Path $DISK) { Remove-Item $DISK -Force }
Copy-Item -Path $SRC_DISK -Destination $DISK -Force
Write-Host "  system-disk.img  copied" -ForegroundColor DarkGray

# OVMF_CODE is read-only, only copy if missing or older
if (-not (Test-Path $OVMF_CODE) -or
    (Get-Item $SRC_OVMF_CODE).LastWriteTime -gt (Get-Item $OVMF_CODE).LastWriteTime) {
    Copy-Item -Path $SRC_OVMF_CODE -Destination $OVMF_CODE -Force
    Write-Host "  OVMF_CODE_4M.fd  copied" -ForegroundColor DarkGray
}

# OVMF_VARS needs a fresh writable copy every run (NVRAM state)
if (Test-Path $OVMF_VARS) { Remove-Item $OVMF_VARS -Force }
Copy-Item -Path $SRC_OVMF_VARS -Destination $OVMF_VARS -Force
Write-Host "  OVMF_VARS_4M.fd  copied" -ForegroundColor DarkGray

# ---- Display settings ----
$Scale = if ($Yres -gt 2160) { 3 } elseif ($Yres -gt 1080) { 2 } else { 1 }
$VgaDevice = if ($Yres -ge 2160) { "bochs-display" } `
        elseif ($Yres -ge 1440) { "VGA,vgamem_mb=32" } `
        elseif ($Yres -ge 1080) { "VGA,vgamem_mb=16" } `
        else                    { "VGA" }

# ---- Accelerator & CPU ----
$CpuModel = if ($Accel -eq 'tcg') { 'qemu64' } else { 'Haswell' }
$TimerExpected = if ($Accel -eq 'tcg') { 'PIT (TCG path)' } else { 'LAPIC (HW accel path)' }

Write-Host ""
Write-Host "Launching Impossible OS at ${Xres}x${Yres} (HiDPI scale=${Scale}x)..." -ForegroundColor Green
Write-Host "  Accel:  $Accel" -ForegroundColor DarkGray
Write-Host "  Timer:  $TimerExpected" -ForegroundColor DarkGray
Write-Host "  Disk:   $DISK (local)" -ForegroundColor DarkGray
Write-Host "  Device: $VgaDevice" -ForegroundColor DarkGray
Write-Host ""

# ---- Find QEMU ----
$QEMU = "qemu-system-x86_64.exe"
if (-not (Get-Command $QEMU -ErrorAction SilentlyContinue)) {
    $QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (-not (Test-Path $QEMU)) {
        Write-Host "QEMU not found. Install from https://qemu.weilnetz.de/w64/" -ForegroundColor Red
        pause; exit 1
    }
}

# ---- Build QEMU arguments ----
$QemuArgs = @()

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
    '-drive', "if=pflash,format=raw,file=$OVMF_VARS",
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
