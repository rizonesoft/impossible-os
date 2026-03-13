# test-fs.ps1 — Filesystem test harness for VirtualBox on Windows
#
# Prerequisites:
#   1. Install VirtualBox: https://www.virtualbox.org/
#   2. Build in WSL2 first: bash scripts/build.sh clean
#   3. Generate test disks in WSL2: bash scripts/test-fs.sh gen
#
# Usage:
#   .\scripts\test-fs.ps1                     List available test disks
#   .\scripts\test-fs.ps1 fat32               Attach FAT32 on AHCI port 1 + launch
#   .\scripts\test-fs.ps1 ntfs                Attach NTFS on AHCI port 1 + launch
#   .\scripts\test-fs.ps1 optical/iso9660     Attach ISO as DVD drive + launch
#   .\scripts\test-fs.ps1 optical/udf         Attach UDF ISO as DVD drive + launch
#   .\scripts\test-fs.ps1 detach              Remove test disk, launch with system disk only
#
# Test disk images are generated in WSL2 via tools/make-test-disks.sh.
# This script converts .img → .vdi (or uses .iso directly for optical)
# and attaches them to the ImpossibleOS VM on AHCI port 1.

param(
    [Parameter(Position=0)]
    [string]$Disk = ""
)

$ErrorActionPreference = "Stop"

# ---- Paths ----
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent $SCRIPT_DIR
$BUILD      = Join-Path $PROJECT "build"
$TEST_DIR   = Join-Path $BUILD "test-disks"
$DISK_VDI   = Join-Path $BUILD "system-disk.vdi"

$VM_NAME    = "ImpossibleOS"

# ---- Find VBoxManage ----
$VBOX = "VBoxManage.exe"
if (-not (Get-Command $VBOX -ErrorAction SilentlyContinue)) {
    $VBOX = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
    if (-not (Test-Path $VBOX)) {
        Write-Host "VBoxManage not found. Install VirtualBox." -ForegroundColor Red
        pause; exit 1
    }
}

# ---- Check VM exists ----
function Test-VMExists {
    try {
        $null = & $VBOX showvminfo $VM_NAME 2>$null
        return ($LASTEXITCODE -eq 0)
    } catch { return $false }
}

# ---- Ensure AHCI has enough ports ----
function Ensure-PortCount {
    param([int]$Ports)
    try {
        & $VBOX storagectl $VM_NAME --name "AHCI" --portcount $Ports 2>$null
    } catch {}
}

# ---- List available test disks ----
function Show-TestDisks {
    Write-Host ""
    Write-Host "══════════════════════════════════════════════════" -ForegroundColor White
    Write-Host " Available test disks" -ForegroundColor Cyan
    Write-Host "══════════════════════════════════════════════════" -ForegroundColor White
    Write-Host ""

    Write-Host " Block devices (AHCI port 1):" -ForegroundColor White
    $found = $false
    Get-ChildItem -Path "$TEST_DIR\*.img" -ErrorAction SilentlyContinue | ForEach-Object {
        $name = $_.BaseName
        $size = "{0:N1} MB" -f ($_.Length / 1MB)
        Write-Host ("   {0,-16} {1}" -f $name, $size)
        $found = $true
    }
    if (-not $found) {
        Write-Host "   (none — run in WSL2: bash scripts/test-fs.sh gen)" -ForegroundColor DarkGray
    }

    Write-Host ""
    Write-Host " Optical media (DVD drive):" -ForegroundColor White
    $found = $false
    $optDir = Join-Path $TEST_DIR "optical"
    Get-ChildItem -Path "$optDir\*.iso" -ErrorAction SilentlyContinue | ForEach-Object {
        $name = "optical/$($_.BaseName)"
        $size = "{0:N1} MB" -f ($_.Length / 1MB)
        Write-Host ("   {0,-24} {1}" -f $name, $size)
        $found = $true
    }
    if (-not $found) {
        Write-Host "   (none — run in WSL2: bash scripts/test-fs.sh gen)" -ForegroundColor DarkGray
    }

    Write-Host ""
    Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray
    Write-Host " Usage:  .\scripts\test-fs.ps1 <disk_name>" -ForegroundColor White
    Write-Host " Example: .\scripts\test-fs.ps1 fat32" -ForegroundColor White
    Write-Host "          .\scripts\test-fs.ps1 optical/iso9660" -ForegroundColor White
    Write-Host "──────────────────────────────────────────────────" -ForegroundColor DarkGray
}

# ---- Detach any test media from port 1 ----
function Detach-TestDisk {
    # Power off if running
    try { & $VBOX controlvm $VM_NAME poweroff 2>$null } catch {}
    Start-Sleep -Milliseconds 500

    # Detach AHCI port 1 (HDD test disk)
    try { & $VBOX storageattach $VM_NAME --storagectl "AHCI" --port 1 --medium none 2>$null } catch {}

    # Detach IDE DVD if present
    try { & $VBOX storageattach $VM_NAME --storagectl "IDE" --port 0 --device 0 --medium none 2>$null } catch {}
}

# ---- Attach block device test disk ----
function Attach-BlockDisk {
    param([string]$DiskName)

    $imgPath  = Join-Path $TEST_DIR "$DiskName.img"
    $vdiPath  = Join-Path $TEST_DIR "$DiskName.vdi"

    if (-not (Test-Path $imgPath)) {
        Write-Host "Test disk not found: $imgPath" -ForegroundColor Red
        Write-Host "Run in WSL2: bash scripts/test-fs.sh gen" -ForegroundColor Yellow
        pause; exit 1
    }

    Write-Host "Converting $DiskName.img → $DiskName.vdi ..." -ForegroundColor Cyan

    # Unregister old VDI
    try { & $VBOX closemedium disk $vdiPath 2>$null } catch {}
    if (Test-Path $vdiPath) { Remove-Item $vdiPath -Force }

    & $VBOX convertfromraw $imgPath $vdiPath --format VDI
    if (-not (Test-Path $vdiPath)) {
        Write-Host "Failed to convert disk image." -ForegroundColor Red
        pause; exit 1
    }

    Detach-TestDisk
    Start-Sleep -Milliseconds 500
    Ensure-PortCount 3

    Write-Host "Attaching $DiskName on AHCI port 1..." -ForegroundColor Cyan
    & $VBOX storageattach $VM_NAME `
        --storagectl "AHCI" `
        --port 1 `
        --type hdd `
        --medium $vdiPath
}

# ---- Attach optical test disk ----
function Attach-OpticalDisk {
    param([string]$IsoName)

    $isoPath = Join-Path $TEST_DIR "$IsoName.iso"

    if (-not (Test-Path $isoPath)) {
        Write-Host "Test ISO not found: $isoPath" -ForegroundColor Red
        Write-Host "Run in WSL2: bash scripts/test-fs.sh gen" -ForegroundColor Yellow
        pause; exit 1
    }

    Detach-TestDisk
    Start-Sleep -Milliseconds 500

    # Ensure IDE controller exists for DVD
    try {
        & $VBOX storagectl $VM_NAME --name "IDE" --add ide --controller PIIX4 2>$null
    } catch {}

    Write-Host "Attaching $IsoName as DVD drive..." -ForegroundColor Cyan
    & $VBOX storageattach $VM_NAME `
        --storagectl "IDE" `
        --port 0 --device 0 `
        --type dvddrive `
        --medium $isoPath
}

# ---- Launch VM ----
function Start-TestVM {
    param([string]$DiskLabel, [string]$Mode)

    # Ensure system disk is attached on port 0
    try {
        & $VBOX storageattach $VM_NAME `
            --storagectl "AHCI" `
            --port 0 `
            --type hdd `
            --medium $DISK_VDI 2>$null
    } catch {}

    Write-Host ""
    Write-Host "══════════════════════════════════════════════════" -ForegroundColor White
    Write-Host " Filesystem Test — VirtualBox" -ForegroundColor Cyan
    Write-Host "══════════════════════════════════════════════════" -ForegroundColor White
    Write-Host "  VM:        $VM_NAME" -ForegroundColor DarkGray
    Write-Host "  Test disk: $DiskLabel" -ForegroundColor DarkGray
    Write-Host "  Mode:      $Mode" -ForegroundColor DarkGray
    Write-Host "  Serial:    $BUILD\serial.log" -ForegroundColor DarkGray
    Write-Host ""

    & $VBOX startvm $VM_NAME
}

# ---- Main ----
if (-not (Test-VMExists)) {
    Write-Host "VM '$VM_NAME' not found." -ForegroundColor Red
    Write-Host "Run .\scripts\run-vbox.ps1 first to create the VM." -ForegroundColor Yellow
    pause; exit 1
}

if (-not (Test-Path $DISK_VDI)) {
    Write-Host "System disk not found: $DISK_VDI" -ForegroundColor Red
    Write-Host "Run .\scripts\run-vbox.ps1 first to convert the system disk." -ForegroundColor Yellow
    pause; exit 1
}

if ($Disk -eq "" -or $Disk -eq "list") {
    Show-TestDisks
    exit 0
}

if ($Disk -eq "detach") {
    Detach-TestDisk
    Write-Host "Test disk detached." -ForegroundColor Green
    Start-TestVM "none (system disk only)" "AHCI port 0 only"
    exit 0
}

if ($Disk -like "optical/*") {
    Attach-OpticalDisk $Disk
    Start-TestVM $Disk "IDE DVD drive"
} else {
    Attach-BlockDisk $Disk
    Start-TestVM $Disk "AHCI port 1"
}
