# run-hyperv.ps1 - Launch Impossible OS in Hyper-V Gen 2 on Windows
#
# Prerequisites:
#   1. Enable Hyper-V: Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All
#   2. Build in WSL2 first: bash scripts/build.sh clean
#
# Usage: Double-click run-hyperv.bat (or run this script as Admin from PowerShell)

param(
    [string]$VmName = "ImpossibleOS-Dev",
    [int]$MemoryMB = 512,
    [int]$CpuCount = 1,
    [switch]$NoConnect
)

$ErrorActionPreference = "Stop"

function Write-Status { param([string]$msg) Write-Host "[*] $msg" -ForegroundColor Cyan }
function Write-Ok     { param([string]$msg) Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Warn   { param([string]$msg) Write-Host "[!!] $msg" -ForegroundColor Yellow }
function Write-Fail   { param([string]$msg) Write-Host "[FAIL] $msg" -ForegroundColor Red }

# ---- Paths ----
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"
$DISK_RAW   = Join-Path $BUILD "system-disk.img"

# VHDX must live on Windows NTFS. WSL 9P filesystem does not support VHD ops.
$VHD_DIR    = Join-Path $env:USERPROFILE "ImpossibleOS"
$DISK_VHDX  = Join-Path $VHD_DIR "system-disk.vhdx"

# ---- Preflight checks ----
if (-not (Test-Path $DISK_RAW)) {
    Write-Fail "Missing: $DISK_RAW"
    Write-Host "Run in WSL2: bash scripts/build.sh clean" -ForegroundColor Yellow
    exit 1
}

try { $null = Get-Command Get-VM -ErrorAction Stop }
catch {
    Write-Fail "Hyper-V PowerShell module not available."
    Write-Host "Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All" -ForegroundColor Gray
    exit 1
}

# ---- Ensure VHD directory exists ----
if (-not (Test-Path $VHD_DIR)) {
    New-Item -ItemType Directory -Path $VHD_DIR -Force | Out-Null
    Write-Status "Created VHD directory: $VHD_DIR"
}

# ---- Delete existing VM (always start clean) ----
$vm = Get-VM -Name $VmName -ErrorAction SilentlyContinue
if ($vm) {
    if ($vm.State -eq "Running") {
        Write-Warn "Stopping running VM..."
        Stop-VM -Name $VmName -Force -TurnOff
        Start-Sleep -Seconds 2
    }
    elseif ($vm.State -eq "Saved") {
        Remove-VMSavedState -VMName $VmName
    }
    Write-Warn "Deleting existing VM..."
    Remove-VM -Name $VmName -Force
    Start-Sleep -Seconds 1
}

# ---- Convert raw disk to VHDX ----
Write-Status "Converting system-disk.img to VHDX..."
if (Test-Path $DISK_VHDX) { Remove-Item $DISK_VHDX -Force }

# Copy raw image to Windows filesystem first (WSL 9P -> NTFS)
$DISK_TEMP = Join-Path $VHD_DIR "system-disk.img"
Write-Status "Copying raw image to Windows filesystem..."
Copy-Item -Path $DISK_RAW -Destination $DISK_TEMP -Force

$rawSize = (Get-Item $DISK_TEMP).Length
$alignedSize = [math]::Ceiling($rawSize / 1MB) * 1MB

New-VHD -Path $DISK_VHDX -Fixed -SizeBytes $alignedSize | Out-Null

$vhd = Mount-VHD -Path $DISK_VHDX -Passthru
$diskNumber = $vhd.DiskNumber
$devicePath = "\\.\PhysicalDrive$diskNumber"

try {
    $source = [System.IO.File]::OpenRead($DISK_TEMP)
    $dest = [System.IO.FileStream]::new($devicePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    $buffer = New-Object byte[] (1MB)
    while (($read = $source.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $dest.Write($buffer, 0, $read)
    }
    $dest.Flush()
    $dest.Close()
    $source.Close()
}
finally {
    Dismount-VHD -Path $DISK_VHDX
}

Remove-Item $DISK_TEMP -Force -ErrorAction SilentlyContinue
Write-Ok "VHDX: $DISK_VHDX ($([math]::Round($alignedSize / 1MB)) MB)"

# ---- Create fresh VM ----
$MemoryBytes = [int64]$MemoryMB * 1MB
Write-Status "Creating Gen 2 VM: $VmName"

$vmSwitch = Get-VMSwitch -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vmSwitch) {
    Write-Status "Using network switch: $($vmSwitch.Name)"
    New-VM -Name $VmName -Generation 2 -MemoryStartupBytes $MemoryBytes -NoVHD -SwitchName $vmSwitch.Name | Out-Null
}
else {
    Write-Warn "No network switch found - creating VM without network"
    New-VM -Name $VmName -Generation 2 -MemoryStartupBytes $MemoryBytes -NoVHD | Out-Null
}

Set-VMProcessor -VMName $VmName -Count $CpuCount
Set-VMFirmware -VMName $VmName -EnableSecureBoot Off
Add-VMHardDiskDrive -VMName $VmName -Path $DISK_VHDX
$hdd = Get-VMHardDiskDrive -VMName $VmName
Set-VMFirmware -VMName $VmName -FirstBootDevice $hdd
Set-VM -VMName $VmName -CheckpointType Disabled
Set-VMMemory -VMName $VmName -DynamicMemoryEnabled $false

Write-Ok "VM created: Gen 2, $MemoryMB MB RAM, $CpuCount vCPU, Secure Boot OFF"

# ---- Start VM ----
Write-Status "Starting VM..."
Start-VM -Name $VmName
Write-Ok "VM started"

if (-not $NoConnect) {
    Write-Status "Connecting to console..."
    Start-Process "vmconnect.exe" -ArgumentList $env:COMPUTERNAME, $VmName
    Write-Ok "Console connected"
}

# ---- Summary ----
Write-Host ""
Write-Host "=======================================" -ForegroundColor DarkGray
Write-Host "  Impossible OS - Hyper-V Gen 2 Test" -ForegroundColor White
Write-Host "=======================================" -ForegroundColor DarkGray
Write-Host "  VM Name    : $VmName" -ForegroundColor Gray
Write-Host "  Generation : 2" -ForegroundColor Gray
Write-Host "  RAM        : $MemoryMB MB" -ForegroundColor Gray
Write-Host "  vCPUs      : $CpuCount" -ForegroundColor Gray
Write-Host "  Secure Boot: OFF" -ForegroundColor Gray
Write-Host "  Disk       : $DISK_VHDX" -ForegroundColor Gray
Write-Host "=======================================" -ForegroundColor DarkGray
Write-Host ""
Write-Host "To stop:   Stop-VM -Name $VmName -TurnOff" -ForegroundColor Yellow
Write-Host "To delete: Remove-VM -Name $VmName -Force" -ForegroundColor Yellow
