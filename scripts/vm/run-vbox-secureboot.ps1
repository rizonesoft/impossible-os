# run-vbox-secureboot.ps1 -- Test Secure Boot shim chain in VirtualBox
#
# What this tests:
#   1. shimx64.efi (BOOTX64.EFI) loads from UEFI firmware
#   2. Shim verifies grubx64.efi against embedded VENDOR_CERT (keys/MOK.cer)
#   3. MOK-signed bootloader boots Impossible OS
#
# The normal system-disk.img already carries the shim chain -- no special
# disk preparation is needed:
#   EFI\BOOT\BOOTX64.EFI  = shimx64.efi  (shim, VENDOR_CERT = MOK.cer)
#   EFI\BOOT\grubx64.efi  = BOOTX64.EFI  (our bootloader, MOK-signed)
#   EFI\BOOT\mmx64.efi    = MokManager   (key enrollment UI)
#
# VirtualBox Secure Boot enforcement:
#   VirtualBox 7.x supports --uefi-secureboot-enabled on|off.
#   This script attempts to enable it.  With SB on, the shim is rejected
#   because it is not Microsoft-signed -- that itself demonstrates SB
#   enforcement working.  To test a passing boot, enroll keys/MOK.der via
#   the EFI shell (see docs/guides/secure-boot-keys.md) or temporarily
#   disable SB.
#
# Parameters:
#   -NoSecureBoot   Skip the SB enforcement config (test shim chain only)
#   -DebugBoot      Inject DEBUG flag (live boot text instead of splash)

param(
    [switch]$NoSecureBoot,
    [switch]$DebugBoot
)

$ErrorActionPreference = "Stop"

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"

$DISK_RAW   = Join-Path $BUILD "system-disk.img"
$DISK_VDI   = Join-Path $BUILD "system-disk-secureboot.vdi"

$VM_NAME    = "ImpossibleOS-SB"

Write-Host ""
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "  Impossible OS -- Secure Boot Chain Test (VBox)" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "  Chain: shimx64.efi -> MOK.cer -> grubx64.efi -> kernel" -ForegroundColor DarkGray
if ($NoSecureBoot) {
    Write-Host "  Mode:  Shim MOK verification only (UEFI enforcement off)" -ForegroundColor DarkGray
} else {
    Write-Host "  Mode:  SB enforcement ON (shim rejection expected without MOK enroll)" -ForegroundColor Yellow
}
Write-Host ""

# ---- Preflight ----
if (-not (Test-Path $DISK_RAW)) {
    Write-Host "Missing: $DISK_RAW" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# Check MOK.key exists for the shim verification test to be meaningful
$MOK_CER = Join-Path $PROJECT "keys\MOK.cer"
if (-not (Test-Path $MOK_CER)) {
    Write-Host "WARNING: keys/MOK.cer not found -- shim MOK verification may fail." -ForegroundColor Yellow
    Write-Host "         Run 'openssl req ...' to generate keys (see docs/guides/secure-boot-keys.md)" -ForegroundColor Yellow
}

# Find VBoxManage
$VBOX = "VBoxManage.exe"
if (-not (Get-Command $VBOX -ErrorAction SilentlyContinue)) {
    $VBOX = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
    if (-not (Test-Path $VBOX)) {
        Write-Host "VBoxManage not found. Install VirtualBox from https://www.virtualbox.org/" -ForegroundColor Red
        pause; exit 1
    }
}

# ---- Debug boot flag ----
if ($DebugBoot) {
    $LOG_OFFSET = 68157440
    & wsl.exe -e bash -c "echo -n debug | mcopy -o -i ~/impossible-os/build/system-disk.img@@${LOG_OFFSET} - ::DEBUG"
    Write-Host "  [OK] DEBUG flag injected" -ForegroundColor Green
} else {
    & wsl.exe -e bash -c "mdel -i ~/impossible-os/build/system-disk.img@@68157440 ::DEBUG 2>/dev/null" 2>$null
}

# ---- Convert raw disk to VDI ----
Write-Host "Converting disk image to VDI..." -ForegroundColor Cyan

try { & $VBOX controlvm $VM_NAME poweroff 2>$null } catch {}
Start-Sleep -Milliseconds 500
try { & $VBOX storageattach $VM_NAME --storagectl "AHCI" --port 0 --medium none 2>$null } catch {}
try { & $VBOX closemedium disk $DISK_VDI 2>$null } catch {}
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
    try { & $VBOX controlvm $VM_NAME poweroff 2>$null } catch {}
    Start-Sleep -Seconds 1
} else {
    Write-Host "Creating VM '$VM_NAME'..." -ForegroundColor Green
    & $VBOX createvm --name $VM_NAME --ostype "Other_64" --register
    & $VBOX storagectl $VM_NAME --name "AHCI" --add sata --controller IntelAhci --portcount 2
}

# ---- VM Settings ----
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

& $VBOX setextradata $VM_NAME "GUI/Input/MachineMouseIntegration" "false"
& $VBOX setextradata $VM_NAME "CustomVideoMode1" "1280x720x32"
& $VBOX setextradata $VM_NAME "VBoxInternal2/EfiGraphicsResolution" "1280x720"

# ---- VirtualBox Secure Boot enforcement (VBox 7.x) ----
if (-not $NoSecureBoot) {
    Write-Host "Enabling EFI Secure Boot enforcement (VBox 7.x)..." -ForegroundColor Yellow
    try {
        & $VBOX modifyvm $VM_NAME --uefi-secureboot-enabled on 2>$null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  [OK] Secure Boot enforcement enabled." -ForegroundColor Green
            Write-Host "  NOTE: shimx64.efi is not MS-signed -- SB will reject it." -ForegroundColor Yellow
            Write-Host "        To test a passing boot, enroll keys/MOK.der via EFI shell:" -ForegroundColor Yellow
            Write-Host "        See docs/guides/secure-boot-keys.md" -ForegroundColor Yellow
        } else {
            Write-Host "  [WARN] --uefi-secureboot-enabled not supported by this VBox version." -ForegroundColor DarkYellow
            Write-Host "         Testing shim MOK chain without UEFI enforcement." -ForegroundColor DarkGray
        }
    } catch {
        Write-Host "  [WARN] SB enforcement flag not supported by this VBox version." -ForegroundColor DarkYellow
        Write-Host "         Testing shim MOK chain without UEFI enforcement." -ForegroundColor DarkGray
    }
}

# ---- Attach boot disk ----
& $VBOX storageattach $VM_NAME `
    --storagectl "AHCI" `
    --port 0 `
    --type hdd `
    --medium $DISK_VDI

for ($p = 1; $p -le 15; $p++) {
    try { & $VBOX storageattach $VM_NAME --storagectl "AHCI" --port $p --medium none 2>$null } catch {}
}

# ---- Launch ----
Write-Host ""
Write-Host "Launching '$VM_NAME'..." -ForegroundColor Green
Write-Host "  Disk:   $DISK_VDI" -ForegroundColor DarkGray
Write-Host "  Serial: $BUILD\serial.log" -ForegroundColor DarkGray
Write-Host ""
Write-Host "Expected outcome:" -ForegroundColor Cyan
if ($NoSecureBoot) {
    Write-Host "  shimx64.efi loads -> verifies grubx64.efi (MOK.cer) -> OS boots" -ForegroundColor Green
} else {
    Write-Host "  With SB on and no MOK enrollment: VBox rejects shimx64.efi (proves enforcement)" -ForegroundColor Yellow
    Write-Host "  After enrolling keys/MOK.der via EFI shell: full chain boots" -ForegroundColor Green
}
Write-Host ""

& $VBOX startvm $VM_NAME
