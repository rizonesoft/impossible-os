# run-nvme-test.ps1 — Launch Impossible OS with an emulated NVMe drive
#
# Attaches a 128 MiB raw NVMe drive alongside the regular AHCI system disk.
# Use this to develop and test the NVMe storage driver (TODO-08).
#
# Parameters:
#   -Accel   Accelerator: auto, whpx, tcg (default: auto)
#   -Build   Build before testing (runs build.sh in WSL) (default: $false)
#
# Usage:
#   Double-click run-nvme-test.bat
#   powershell -File run-nvme-test.ps1
#   powershell -File run-nvme-test.ps1 -Build
#
# Prerequisites:
#   1. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
#   2. Build in WSL2: bash scripts/build.sh
Param(
    [ValidateSet('auto','whpx','tcg')]
    # TCG required: QEMU's WHPX backend processes NVMe doorbell MMIO writes
    # asynchronously through its event loop. The vCPU polls the CQ before
    # the main thread processes the command, causing intermittent timeouts.
    # Not a driver bug — real hardware and KVM handle this synchronously.
    [string]$Accel = 'tcg',
    [switch]$Build = $false
)

$ErrorActionPreference = "Stop"

# Resolve project root: scripts/vm/storage -> parent -> parent -> parent
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$VM_DIR     = Split-Path -Parent $SCRIPT_DIR
$PROJECT    = Split-Path -Parent (Split-Path -Parent $VM_DIR)
$BuildDir   = Join-Path $PROJECT "build"

$SYSTEM_DISK = Join-Path $BuildDir "system-disk.img"
$NVME_DISK   = Join-Path $BuildDir "test-nvme.img"
$OVMF_CODE   = Join-Path $BuildDir "OVMF_CODE_4M.fd"
$OVMF_VARS   = Join-Path $BuildDir "OVMF_VARS_4M.fd"
$VARS_DEST   = Join-Path $env:TEMP "OVMF_VARS_4M_nvme.fd"

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

# --- Generate NVMe test disk (if missing) ---
if (-not (Test-Path $NVME_DISK)) {
    Write-Host "Creating 128 MiB NVMe test disk in WSL2..." -ForegroundColor Yellow
    & wsl.exe bash -c 'cd ~/impossible-os && make test-nvme-img'
    if (-not (Test-Path $NVME_DISK)) {
        Write-Host "NVMe test disk creation failed: $NVME_DISK" -ForegroundColor Red
        pause; exit 1
    }
    Write-Host "NVMe test disk ready: $NVME_DISK" -ForegroundColor Green
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
$diskSizeMB = [math]::Round((Get-Item $NVME_DISK).Length / 1MB, 1)
Write-Host '' -ForegroundColor Cyan
Write-Host '  Impossible OS - NVMe Storage Test' -ForegroundColor Cyan
Write-Host '  ==================================' -ForegroundColor Cyan
Write-Host "  NVMe disk:   $NVME_DISK ($diskSizeMB MiB, FAT32)" -ForegroundColor DarkGray
Write-Host "  Controller:  NVMe (QEMU emulated)" -ForegroundColor DarkGray
Write-Host "  Accel:       $Accel" -ForegroundColor DarkGray
Write-Host '' -ForegroundColor Cyan
Write-Host '  The NVMe controller will appear as PCI class 01:08:02.' -ForegroundColor Yellow
Write-Host '  Look for [NVMe] lines in serial output.' -ForegroundColor Yellow
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
$QemuArgs += '-smp', '2'
$QemuArgs += '-drive', "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
$QemuArgs += '-drive', "if=pflash,format=raw,file=$VARS_DEST"
# System disk on AHCI port 0 (boot drive)
$QemuArgs += '-drive', "id=disk0,file=$SYSTEM_DISK,format=raw,if=none"
$QemuArgs += '-device', 'ich9-ahci,id=ahci0'
$QemuArgs += '-device', 'ide-hd,drive=disk0,bus=ahci0.0'
# NVMe test drive
$QemuArgs += '-drive', "id=nvme0,file=$NVME_DISK,format=raw,if=none"
$QemuArgs += '-device', 'nvme,serial=ImpOS-NVMe-Test,drive=nvme0'
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
