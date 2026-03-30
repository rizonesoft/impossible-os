# run-qemu-kvm-usb.ps1 -- Launch Impossible OS with WHPX + xHCI USB mass storage
#
# Attaches a 64 MiB FAT32 USB disk via an emulated xHCI controller alongside
# the regular AHCI system disk. Use this to test xHCI + USB MSC before bare metal.
#
# The xHCI controller appears as PCI class 0C:03:30.
# Look for [xhci] and [usb] lines in serial output.
#
# Parameters:
#   -Accel   Accelerator: auto, whpx, tcg (default: auto)
#
# Usage:
#   Double-click run-qemu-kvm-usb.bat
#   powershell -File run-qemu-kvm-usb.ps1
#   powershell -File run-qemu-kvm-usb.ps1 -Accel tcg
Param(
    [ValidateSet('auto','whpx','tcg')]
    [string]$Accel = 'auto'
)

$ErrorActionPreference = "Stop"

# Resolve project root: scripts/vm -> parent -> parent
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"

$SYSTEM_DISK = Join-Path $BUILD "system-disk.img"
$USB_DISK    = Join-Path $BUILD "test-usb.img"
$OVMF_CODE   = Join-Path $BUILD "OVMF_CODE_4M.fd"
$OVMF_VARS   = Join-Path $BUILD "OVMF_VARS_4M.fd"
$VARS_DEST   = Join-Path $env:TEMP "OVMF_VARS_4M_usb.fd"

# --- Check system disk ---
if (-not (Test-Path $SYSTEM_DISK)) {
    Write-Host "Missing: $SYSTEM_DISK" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# --- Generate USB test disk (if missing) ---
if (-not (Test-Path $USB_DISK)) {
    Write-Host "Creating 64 MiB FAT32 USB test disk in WSL2..." -ForegroundColor Yellow
    & wsl.exe bash -c "cd ~/impossible-os && make test-usb-img"
    if (-not (Test-Path $USB_DISK)) {
        # Fallback: create a simple raw image via WSL
        Write-Host "make target not found, creating raw FAT32 image..." -ForegroundColor Yellow
        & wsl.exe bash -c "dd if=/dev/zero of=~/impossible-os/build/test-usb.img bs=1M count=64 2>/dev/null && mkfs.vfat ~/impossible-os/build/test-usb.img"
        if (-not (Test-Path $USB_DISK)) {
            Write-Host "USB test disk creation failed." -ForegroundColor Red
            pause; exit 1
        }
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

# OVMF_VARS: writable copy in TEMP (separate from non-USB to avoid NVRAM conflicts)
Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST -Force

# --- Display info ---
$diskSizeMB = [math]::Round((Get-Item $USB_DISK).Length / 1MB, 1)
$CpuModel = if ($Accel -eq 'tcg') { 'qemu64' } else { 'Haswell' }
$TimerExpected = if ($Accel -eq 'tcg') { 'PIT (TCG path)' } else { 'LAPIC (HW accel path)' }

Write-Host ""
Write-Host "  Impossible OS -- xHCI USB Test" -ForegroundColor Cyan
Write-Host "  ==============================" -ForegroundColor Cyan
Write-Host "  System disk: $SYSTEM_DISK" -ForegroundColor DarkGray
Write-Host "  USB disk:    $USB_DISK ($diskSizeMB MiB, FAT32)" -ForegroundColor DarkGray
Write-Host "  Controller:  xHCI (qemu-xhci)" -ForegroundColor DarkGray
Write-Host "  Accel:       $Accel ($CpuModel)" -ForegroundColor DarkGray
Write-Host "  Timer:       $TimerExpected" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  xHCI will appear as PCI class 0C:03:30." -ForegroundColor Yellow
Write-Host "  Look for [xhci] lines in serial output." -ForegroundColor Yellow
Write-Host ""

# --- Find QEMU ---
$QEMU = "qemu-system-x86_64.exe"
if (-not (Get-Command $QEMU -ErrorAction SilentlyContinue)) {
    $QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (-not (Test-Path $QEMU)) {
        Write-Host "QEMU not found. Install from https://qemu.weilnetz.de/w64/" -ForegroundColor Red
        pause; exit 1
    }
}

# --- Build QEMU arguments ---
$QemuArgs = @()

switch ($Accel) {
    'tcg'  { $QemuArgs += '-accel', 'tcg' }
    'whpx' { $QemuArgs += '-accel', 'whpx' }
    'auto' { $QemuArgs += '-accel', 'whpx', '-accel', 'tcg' }
}

$QemuArgs += @(
    '-cpu', $CpuModel,
    '-smp', '2',
    # UEFI firmware
    '-drive', "if=pflash,format=raw,readonly=on,file=$OVMF_CODE",
    '-drive', "if=pflash,format=raw,file=$VARS_DEST",
    # System disk on AHCI
    '-drive', "id=disk0,file=$SYSTEM_DISK,format=raw,if=none",
    '-device', 'ich9-ahci,id=ahci0',
    '-device', 'ide-hd,drive=disk0,bus=ahci0.0',
    # USB test disk on xHCI
    '-device', 'qemu-xhci,id=xhci0',
    '-drive', "id=usbdisk0,file=$USB_DISK,format=raw,if=none",
    '-device', 'usb-storage,bus=xhci0.0,drive=usbdisk0',
    # Memory, serial, display
    '-m', '2G',
    '-serial', 'stdio',
    '-vga', 'none',
    '-device', 'VGA,xres=1280,yres=720',
    '-device', 'rtl8139,netdev=net0',
    '-netdev', 'user,id=net0',
    '-device', 'virtio-tablet-pci',
    '-rtc', 'base=localtime',
    '-no-reboot'
)

& $QEMU @QemuArgs
