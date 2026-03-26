# run-qemu-secureboot.ps1 -- Launch Impossible OS with UEFI Secure Boot enforcement
#
# Boot chain:
#   OVMF_CODE_4M.snakeoil.fd (matched snakeoil firmware -- Secure Boot enforcing)
#   OVMF_VARS_4M.snakeoil.fd (snakeoil PK/KEK/db pre-enrolled; OVMF trusts snakeoil-signed shim)
#     -> EFI/BOOT/BOOTX64.EFI  (shimx64.efi, re-signed with OVMF snakeoil key)
#         -> EFI/BOOT/grubx64.efi (our bootloader, signed with MOK.key, trusted via VENDOR_CERT_FILE)
#             -> kernel
#
# The build-sb-test-disk.sh helper re-signs the shim with the snakeoil key on
# every run, so no manual BIOS key enrollment is needed.
#
# Parameters:
#   -Xres        Horizontal resolution (default: 1280)
#   -Yres        Vertical resolution   (default: 720)
#   -Accel       Accelerator: auto, whpx, tcg (default: auto)
#   -NoRebuild   Skip rebuilding the SB test disk (use last build)
Param(
    [int]$Xres = 1280,
    [int]$Yres = 720,
    [ValidateSet('auto','whpx','tcg')]
    [string]$Accel = 'auto',
    [switch]$NoRebuild
)

$ErrorActionPreference = "Stop"

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"

$DISK_SB    = Join-Path $BUILD "system-disk-secureboot.img"
$OVMF_CODE  = Join-Path $BUILD "OVMF_CODE_4M.snakeoil.fd"
$OVMF_VARS  = Join-Path $BUILD "OVMF_VARS_4M.snakeoil.fd"
$VARS_DEST  = Join-Path $env:TEMP "OVMF_VARS_4M.snakeoil.fd"

Write-Host ""
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "  Impossible OS -- Secure Boot Test" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "  OVMF: secboot.fd + snakeoil VARS" -ForegroundColor DarkGray
Write-Host "  Chain: snakeoil-signed shim -> MOK-signed loader -> kernel" -ForegroundColor DarkGray
Write-Host ""

# --- Step 1: Build the Secure Boot test disk ---
if (-not $NoRebuild) {
    Write-Host "Building Secure Boot test disk (sign shim with OVMF snakeoil key)..." -ForegroundColor Yellow
    & wsl.exe bash ~/impossible-os/scripts/secure-boot/build-sb-test-disk.sh
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Secure Boot disk build failed." -ForegroundColor Red
        pause; exit 1
    }
} else {
    Write-Host "-NoRebuild: using existing $DISK_SB" -ForegroundColor DarkGray
}

# --- Step 2: Verify disk image exists ---
if (-not (Test-Path $DISK_SB)) {
    Write-Host "Missing: $DISK_SB" -ForegroundColor Red
    Write-Host "Run without -NoRebuild to build it." -ForegroundColor Yellow
    pause; exit 1
}

# --- Step 3: Verify OVMF secboot firmware (build-sb-test-disk.sh copies it) ---
if (-not (Test-Path $OVMF_CODE)) {
    Write-Host "Copying OVMF snakeoil firmware pair from WSL..." -ForegroundColor Yellow
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_CODE_4M.snakeoil.fd ~/impossible-os/build/'
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_VARS_4M.snakeoil.fd ~/impossible-os/build/'
    if (-not (Test-Path $OVMF_CODE)) {
        Write-Host "OVMF_CODE_4M.snakeoil.fd not found. Install: sudo apt install ovmf" -ForegroundColor Red
        pause; exit 1
    }
}

# --- Step 4: Fresh writable copy of snakeoil VARS (snakeoil PK/KEK/db enrolled) ---
Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST -Force
Write-Host "VARS: snakeoil PK/KEK/db enrolled (Secure Boot enforcing)" -ForegroundColor Green

# --- Step 5: Resolve display and accelerator ---
$Scale = if ($Yres -gt 2160) { 3 } elseif ($Yres -gt 1080) { 2 } else { 1 }
$VgaDevice = if ($Yres -ge 2160) {
    "bochs-display"
} elseif ($Yres -ge 1440) {
    "VGA,vgamem_mb=32"
} elseif ($Yres -ge 1080) {
    "VGA,vgamem_mb=16"
} else {
    "VGA"
}
$CpuModel      = if ($Accel -eq 'tcg') { 'qemu64' } else { 'Haswell' }
$TimerExpected = if ($Accel -eq 'tcg') { 'PIT (TCG)' } else { 'LAPIC (HW accel)' }

Write-Host "Launching at ${Xres}x${Yres} (scale=${Scale}x)..." -ForegroundColor Green
Write-Host "  Accel: $Accel  |  Timer: $TimerExpected" -ForegroundColor DarkGray
Write-Host "  Disk:  $DISK_SB" -ForegroundColor DarkGray
Write-Host "  Firm:  $OVMF_CODE" -ForegroundColor DarkGray
Write-Host ""

# --- Step 6: Find QEMU ---
$QEMU = "qemu-system-x86_64.exe"
if (-not (Get-Command $QEMU -ErrorAction SilentlyContinue)) {
    $QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (-not (Test-Path $QEMU)) {
        Write-Host "QEMU not found. Install from https://qemu.weilnetz.de/w64/" -ForegroundColor Red
        pause; exit 1
    }
}

# --- Step 7: Build QEMU arguments ---
$QemuArgs = @()

switch ($Accel) {
    'tcg'  { $QemuArgs += '-accel', 'tcg' }
    'whpx' { $QemuArgs += '-accel', 'whpx' }
    'auto' { $QemuArgs += '-accel', 'whpx', '-accel', 'tcg' }
}

$QemuArgs += @(
    '-cpu',    $CpuModel,
    '-drive',  "if=pflash,format=raw,readonly=on,file=$OVMF_CODE",
    '-drive',  "if=pflash,format=raw,file=$VARS_DEST",
    '-drive',  "id=disk0,file=$DISK_SB,format=raw,if=none",
    '-device', 'ich9-ahci,id=ahci0',
    '-device', 'ide-hd,drive=disk0,bus=ahci0.0',
    '-m',      '2G',
    '-serial', 'stdio',
    '-vga',    'none',
    '-device', ($VgaDevice + ',xres=' + $Xres + ',yres=' + $Yres),
    '-device', 'rtl8139,netdev=net0',
    '-netdev', 'user,id=net0',
    '-device', 'virtio-tablet-pci',
    '-rtc',    'base=localtime',
    '-no-reboot'
)

& $QEMU @QemuArgs
