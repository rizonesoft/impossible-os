# run-vbox.ps1 — Launch Impossible OS in VirtualBox on Windows
#
# Prerequisites:
#   1. Install VirtualBox: https://www.virtualbox.org/
#   2. Build in WSL2 first: bash scripts/build.sh clean
#
# Usage: Double-click run-vbox.bat (or run this script from PowerShell)
#
# This script:
#   1. Converts the raw disk image to VDI format
#   2. Creates/updates a VirtualBox VM with correct settings
#   3. Launches the VM

param(
    [switch]$DebugBoot  # Inject X:\DEBUG flag to show live boot output instead of splash
)

$ErrorActionPreference = "Stop"

# ---- Paths ----
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"

$DISK_RAW   = Join-Path $BUILD "system-disk.img"
$DISK_VDI   = Join-Path $BUILD "system-disk.vdi"
$OVMF_CODE  = Join-Path $BUILD "OVMF_CODE_4M.fd"
$OVMF_VARS  = Join-Path $BUILD "OVMF_VARS_4M.fd"

$VM_NAME    = "ImpossibleOS"

# ---- Preflight checks ----
if (-not (Test-Path $DISK_RAW)) {
    Write-Host "Missing: $DISK_RAW" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh clean' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# Find VBoxManage
$VBOX = "VBoxManage.exe"
if (-not (Get-Command $VBOX -ErrorAction SilentlyContinue)) {
    $VBOX = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
    if (-not (Test-Path $VBOX)) {
        Write-Host "VBoxManage not found. Install VirtualBox." -ForegroundColor Red
        pause; exit 1
    }
}

# Auto-copy OVMF firmware from WSL if missing
if (-not (Test-Path $OVMF_CODE) -or -not (Test-Path $OVMF_VARS)) {
    Write-Host "Copying OVMF firmware to build/..." -ForegroundColor Yellow
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/'
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/'
}

# ---- Convert raw disk to VDI ----
# Always re-convert to pick up latest build changes.
# Must properly unregister the old VDI from VirtualBox's media registry
# before deleting, otherwise UUID mismatch errors occur.
# ---- Debug boot flag: inject into raw image BEFORE VDI conversion ----
if ($DebugBoot) {
    $LOG_OFFSET = 68157440  # Must match Makefile LOG_OFFSET
    & wsl.exe -e bash -c "echo -n debug | mcopy -i ~/impossible-os/build/system-disk.img@@${LOG_OFFSET} - ::DEBUG 2>/dev/null; echo -n debug | mcopy -o -i ~/impossible-os/build/system-disk.img@@${LOG_OFFSET} - ::DEBUG"
    Write-Host "  [OK] DEBUG flag injected into Logs partition" -ForegroundColor Green
} else {
    # Remove DEBUG flag if it exists (normal boot)
    & wsl.exe -e bash -c "mdel -i ~/impossible-os/build/system-disk.img@@68157440 ::DEBUG 2>/dev/null" 2>$null
}

Write-Host "Converting disk image to VDI..." -ForegroundColor Cyan

# Power off VM if running
try { & $VBOX controlvm $VM_NAME poweroff 2>$null } catch {}
Start-Sleep -Milliseconds 500

# Detach disk from VM
try { & $VBOX storageattach $VM_NAME --storagectl "AHCI" --port 0 --medium none 2>$null } catch {}

# Close medium in VirtualBox registry (by path, handles UUID mismatch)
try { & $VBOX closemedium disk $DISK_VDI 2>$null } catch {}

# Delete old VDI file
if (Test-Path $DISK_VDI) { Remove-Item $DISK_VDI -Force }

& $VBOX convertfromraw $DISK_RAW $DISK_VDI --format VDI
if (-not (Test-Path $DISK_VDI)) {
    Write-Host "Failed to convert disk image." -ForegroundColor Red
    pause; exit 1
}
Write-Host "  VDI: $DISK_VDI" -ForegroundColor DarkGray

# ---- Create or update VM ----
$vmExists = $false
try {
    $info = & $VBOX showvminfo $VM_NAME 2>$null
    if ($LASTEXITCODE -eq 0) { $vmExists = $true }
} catch {}

if ($vmExists) {
    Write-Host "Updating existing VM '$VM_NAME'..." -ForegroundColor Yellow
    # Power off if running
    try { & $VBOX controlvm $VM_NAME poweroff 2>$null } catch {}
    Start-Sleep -Seconds 1
} else {
    Write-Host "Creating VM '$VM_NAME'..." -ForegroundColor Green

    & $VBOX createvm --name $VM_NAME --ostype "Other_64" --register

    # Storage controller: AHCI (matches our AHCI driver)
    & $VBOX storagectl $VM_NAME --name "AHCI" --add sata --controller IntelAhci --portcount 2
}

# ---- VM Settings ----
# VMSVGA is required for UEFI guests (VBoxVGA has no EFI GOP support)
& $VBOX modifyvm $VM_NAME `
    --memory 2048 `
    --cpus 4 `
    --ioapic on `
    --firmware efi `
    --graphicscontroller vmsvga `
    --vram 128 `
    --mouse ps2 `
    --keyboard ps2 `
    --audio-driver none `
    --uart1 "0x3F8" "4" `
    --uart-mode1 file "$BUILD\serial.log" `
    --boot1 disk `
    --boot2 none `
    --boot3 none `
    --boot4 none

# Disable mouse integration — without Guest Additions, VirtualBox's
# absolute-to-relative coordinate conversion causes:
#   1. Pointer jumping 100-200px
#   2. Mouse hitting a "wall" at ~40% of desktop
#   3. Mouse escaping VM window without host key
# Clear stale key from earlier attempts, then set correct one:
try { & $VBOX setextradata $VM_NAME "VBoxInternal/Devices/pckbd/0/Config/DisableMouseIntegration" 2>$null } catch {}
& $VBOX setextradata $VM_NAME "GUI/Input/MachineMouseIntegration" "false"

# Set resolution hint for the VMSVGA adapter
& $VBOX setextradata $VM_NAME "CustomVideoMode1" "1280x720x32"
& $VBOX setextradata $VM_NAME "VBoxInternal2/EfiGraphicsResolution" "1280x720"

# Attach boot disk
& $VBOX storageattach $VM_NAME `
    --storagectl "AHCI" `
    --port 0 `
    --type hdd `
    --medium $DISK_VDI

# Detach any stale test disks from ports 1+ (left by test-filesystem scripts)
for ($p = 1; $p -le 15; $p++) {
    try { & $VBOX storageattach $VM_NAME --storagectl "AHCI" --port $p --medium none 2>$null } catch {}
}
# Detach any stale DVD (left by optical test scripts)
try { & $VBOX storageattach $VM_NAME --storagectl "IDE" --port 0 --device 0 --medium none 2>$null } catch {}

# ---- Launch ----
Write-Host ""
Write-Host "Launching Impossible OS in VirtualBox..." -ForegroundColor Green
Write-Host "  VM: $VM_NAME" -ForegroundColor DarkGray
Write-Host "  Disk: $DISK_VDI" -ForegroundColor DarkGray
Write-Host "  Display: VMSVGA 1280x720 (UEFI)" -ForegroundColor DarkGray
Write-Host "  Serial log: $BUILD\serial.log" -ForegroundColor DarkGray
if ($DebugBoot) {
    Write-Host "  Mode: DEBUG (splash disabled, live text on screen)" -ForegroundColor Yellow
}
Write-Host ""

& $VBOX startvm $VM_NAME
