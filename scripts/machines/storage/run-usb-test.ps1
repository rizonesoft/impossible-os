# run-usb-test.ps1 -- Launch Impossible OS with xHCI + USB mass storage test disk
#
# Attaches a 64 MiB FAT32 USB disk via an emulated xHCI controller alongside
# the regular AHCI system disk. Use this to develop and test the xHCI + USB MSC
# driver (TODO-040.20).
#
# Parameters:
#   -Accel   Accelerator: auto, whpx, tcg (default: auto)
#   -Build   Build before testing (runs build.sh in WSL) (default: $false)
#
# Usage:
#   Double-click run-usb-test.bat
#   powershell -File run-usb-test.ps1
#   powershell -File run-usb-test.ps1 -Build
#
# Prerequisites:
#   1. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
#   2. Build in WSL2: bash scripts/build.sh
Param(
    [ValidateSet('auto','whpx','tcg')]
    # TCG required: WHPX hangs with xHCI device attached
    [string]$Accel = 'tcg',
    [switch]$Build = $false
)

$ErrorActionPreference = "Stop"

# Resolve project root: scripts/machines/storage -> parent -> parent -> parent
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$VM_DIR     = Split-Path -Parent $SCRIPT_DIR
$PROJECT    = Split-Path -Parent (Split-Path -Parent $VM_DIR)
$BuildDir   = Join-Path $PROJECT "build"

$SYSTEM_DISK = Join-Path $BuildDir "system-disk.img"
$USB_DISK    = Join-Path $BuildDir "test-usb.img"
$OVMF_CODE   = Join-Path $BuildDir "OVMF_CODE_4M.fd"
$OVMF_VARS   = Join-Path $BuildDir "OVMF_VARS_4M.fd"
$VARS_DEST   = Join-Path $env:TEMP "OVMF_VARS_4M_usb.fd"

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

# --- Generate USB test disk (if missing) ---
if (-not (Test-Path $USB_DISK)) {
    Write-Host "Creating 64 MiB FAT32 USB test disk in WSL2..." -ForegroundColor Yellow
    & wsl.exe bash -c 'cd ~/impossible-os && make test-usb-img'
    if (-not (Test-Path $USB_DISK)) {
        Write-Host "USB test disk creation failed: $USB_DISK" -ForegroundColor Red
        pause; exit 1
    }
    Write-Host "USB test disk ready: $USB_DISK" -ForegroundColor Green
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
$diskSizeMB = [math]::Round((Get-Item $USB_DISK).Length / 1MB, 1)
Write-Host '' -ForegroundColor Cyan
Write-Host '  Impossible OS - USB xHCI Test' -ForegroundColor Cyan
Write-Host '  ==============================' -ForegroundColor Cyan
Write-Host "  USB disk:    $USB_DISK ($diskSizeMB MiB, FAT32)" -ForegroundColor DarkGray
Write-Host "  Controller:  xHCI (qemu-xhci)" -ForegroundColor DarkGray
Write-Host "  Accel:       $Accel" -ForegroundColor DarkGray
Write-Host '' -ForegroundColor Cyan
Write-Host '  The xHCI controller will appear as PCI class 0C:03:30.' -ForegroundColor Yellow
Write-Host '  Look for [xHCI] and [USB-MSC] lines in serial output.' -ForegroundColor Yellow
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
# System disk on AHCI port 0
$QemuArgs += '-drive', "id=disk0,file=$SYSTEM_DISK,format=raw,if=none"
$QemuArgs += '-device', 'ich9-ahci,id=ahci0'
$QemuArgs += '-device', 'ide-hd,drive=disk0,bus=ahci0.0'
# USB test disk on xHCI
$QemuArgs += '-device', 'qemu-xhci,id=xhci0'
$QemuArgs += '-drive', "id=usbdisk0,file=$USB_DISK,format=raw,if=none"
$QemuArgs += '-device', 'usb-storage,bus=xhci0.0,drive=usbdisk0'
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
