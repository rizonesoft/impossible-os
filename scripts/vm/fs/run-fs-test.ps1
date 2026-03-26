# run-fs-test.ps1 — Launch Impossible OS with a filesystem test disk
#
# Attaches a test disk image on AHCI port 1 alongside the system disk
# on port 0. The kernel auto-detects the filesystem and runs self-tests
# when the volume label matches the expected test pattern (e.g. "NTFS_TEST").
#
# Parameters:
#   -Disk    Filesystem to test: ntfs, fat32, ext2, ext4, ixfs (default: ntfs)
#   -Accel   Accelerator: auto, whpx, tcg (default: auto)
#   -Build   Build before testing (runs build.sh in WSL) (default: $false)
#   -GenDisk Force-regenerate the test disk image (default: $false)
#
# Usage:
#   Double-click run-ntfs-test.bat                → test NTFS with WHPX
#   powershell -File run-fs-test.ps1 -Disk fat32  → test FAT32
#   powershell -File run-fs-test.ps1 -Build       → build first, then test
#
# Prerequisites:
#   1. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
#   2. Build in WSL2: bash scripts/build.sh
#   3. Generate test disks in WSL2: bash tools/make-test-disks.sh build/test-disks build
Param(
    [ValidateSet('ntfs','fat32','ext2','ext3','ext4','exfat','ixfs','mbr','gpt')]
    [string]$Disk = 'ntfs',
    [ValidateSet('auto','whpx','tcg')]
    [string]$Accel = 'auto',
    [switch]$Build = $false,
    [switch]$GenDisk = $false
)

$ErrorActionPreference = "Stop"

# Resolve project root: scripts/vm/fs -> parent -> parent -> parent
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$VM_DIR     = Split-Path -Parent $SCRIPT_DIR
$PROJECT    = Split-Path -Parent (Split-Path -Parent $VM_DIR)
$BuildDir   = Join-Path $PROJECT "build"

$SYSTEM_DISK = Join-Path $BuildDir "system-disk.img"
$TEST_DISK   = Join-Path $BuildDir "test-disks\$Disk.img"
$OVMF_CODE   = Join-Path $BuildDir "OVMF_CODE_4M.fd"
$OVMF_VARS   = Join-Path $BuildDir "OVMF_VARS_4M.fd"
$VARS_DEST   = Join-Path $env:TEMP "OVMF_VARS_4M_test.fd"
$SERIAL_LOG  = Join-Path $BuildDir "test-disks\$Disk-serial.log"

# --- Build (optional) ---
if ($Build) {
    Write-Host "Building Impossible OS in WSL2..." -ForegroundColor Yellow
    & wsl.exe bash ~/impossible-os/scripts/build.sh
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build failed!" -ForegroundColor Red
        pause; exit 1
    }
    Write-Host "Build OK" -ForegroundColor Green
}

# --- Check system disk ---
if (-not (Test-Path $SYSTEM_DISK)) {
    Write-Host "Missing: $SYSTEM_DISK" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# --- Generate test disk (if missing or forced) ---
if (-not (Test-Path $TEST_DISK) -or $GenDisk) {
    Write-Host "Generating $Disk test disk in WSL2..." -ForegroundColor Yellow
    if ($Disk -eq 'ntfs') {
        & wsl.exe bash ~/impossible-os/scripts/make-ntfs-test.sh build/test-disks
    } else {
        & wsl.exe bash ~/impossible-os/tools/make-test-disks.sh build/test-disks build
    }
    if (-not (Test-Path $TEST_DISK)) {
        Write-Host "Test disk generation failed: $TEST_DISK" -ForegroundColor Red
        pause; exit 1
    }
    Write-Host "Test disk ready: $TEST_DISK" -ForegroundColor Green
}

# --- OVMF firmware ---
if (-not (Test-Path $OVMF_CODE) -or -not (Test-Path $OVMF_VARS)) {
    Write-Host "Copying OVMF firmware to build/..." -ForegroundColor Yellow
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/'
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/'
    if (-not (Test-Path $OVMF_CODE)) {
        Write-Host "Failed to copy OVMF." -ForegroundColor Red
        pause; exit 1
    }
}

Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST -Force

# --- Find QEMU ---
$QEMU = "qemu-system-x86_64.exe"
if (-not (Get-Command $QEMU -ErrorAction SilentlyContinue)) {
    $QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (-not (Test-Path $QEMU)) {
        Write-Host "QEMU not found. Install from https://qemu.weilnetz.de/w64/" -ForegroundColor Red
        pause; exit 1
    }
}

# --- Display test info ---
$diskSizeMB = [math]::Round((Get-Item $TEST_DISK).Length / 1MB, 1)
$diskLabel  = $Disk.ToUpper()
Write-Host '' -ForegroundColor Cyan
Write-Host '  Impossible OS - Filesystem Test' -ForegroundColor Cyan
Write-Host '  ================================' -ForegroundColor Cyan
Write-Host "  Filesystem:  $diskLabel" -ForegroundColor White
Write-Host "  Test disk:   $TEST_DISK ($diskSizeMB MiB)" -ForegroundColor DarkGray
Write-Host "  Serial log:  $SERIAL_LOG" -ForegroundColor DarkGray
Write-Host "  Accel:       $Accel" -ForegroundColor DarkGray
Write-Host '' -ForegroundColor Cyan
Write-Host '  Look for [PASS]/[FAIL] lines in the serial output.' -ForegroundColor Yellow
Write-Host '  Test suite runs automatically on volume detection.' -ForegroundColor Yellow
Write-Host ''

# --- Build QEMU arguments ---
$QemuArgs = @()

switch ($Accel) {
    'tcg'  { $QemuArgs += '-accel', 'tcg' }
    'whpx' { $QemuArgs += '-accel', 'whpx' }
    'auto' { $QemuArgs += '-accel', 'whpx', '-accel', 'tcg' }
}

$CpuModel = if ($Accel -eq 'tcg') { 'qemu64' } else { 'Haswell' }

$QemuArgs += '-cpu', $CpuModel
$QemuArgs += '-drive', "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
$QemuArgs += '-drive', "if=pflash,format=raw,file=$VARS_DEST"
# System disk on AHCI port 0
$QemuArgs += '-drive', "id=disk0,file=$SYSTEM_DISK,format=raw,if=none"
# Test disk on AHCI port 1
$QemuArgs += '-drive', "id=testdisk,file=$TEST_DISK,format=raw,if=none"
$QemuArgs += '-device', 'ich9-ahci,id=ahci0'
$QemuArgs += '-device', 'ide-hd,drive=disk0,bus=ahci0.0'
$QemuArgs += '-device', 'ide-hd,drive=testdisk,bus=ahci0.1'
$QemuArgs += '-m', '2G'
$QemuArgs += '-serial', 'stdio'
$QemuArgs += '-vga', 'none'
$QemuArgs += '-device', 'VGA,xres=1280,yres=720'
$QemuArgs += '-device', 'rtl8139,netdev=net0'
$QemuArgs += '-netdev', 'user,id=net0'
$QemuArgs += '-device', 'virtio-tablet-pci'
$QemuArgs += '-rtc', 'base=localtime'
$QemuArgs += '-no-reboot'

& $QEMU @QemuArgs
