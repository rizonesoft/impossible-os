# gen-test-disk.ps1 -- Generate filesystem test disk images via WSL2
#
# Usage:
#   .\scripts\test-fs\gen-test-disk.ps1              Generate ALL test disks
#   .\scripts\test-fs\gen-test-disk.ps1 fat32         Generate only FAT32
#   .\scripts\test-fs\gen-test-disk.ps1 optical/iso9660  Generate only ISO 9660
#
# This script calls into WSL2 to build the OS (if needed) and run
# tools/make-test-disks.sh to create the requested disk image(s).

param(
    [Parameter(Position=0)]
    [string]$Disk = "all"
)

$ErrorActionPreference = "Continue"

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)

Write-Host ""
Write-Host "==================================================" -ForegroundColor White
Write-Host " Generating Test Disk Images" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor White
Write-Host ""

# Check WSL is available
if (-not (Get-Command "wsl.exe" -ErrorAction SilentlyContinue)) {
    Write-Host "WSL2 not found. Install WSL2 to generate test disks." -ForegroundColor Red
    Read-Host "Press Enter to exit"; exit 1
}

# Convert Windows path to WSL path
$WSL_PROJECT = wsl.exe wslpath -u ($PROJECT -replace '\\\\wsl.localhost\\Ubuntu', '')
if (-not $WSL_PROJECT -or $WSL_PROJECT -eq "") {
    # Fallback: try to detect from the project path
    $WSL_PROJECT = wsl.exe wslpath -u "$PROJECT"
}

Write-Host "Project (WSL): $WSL_PROJECT" -ForegroundColor DarkGray

if ($Disk -eq "all") {
    Write-Host "Generating ALL test disk images..." -ForegroundColor Yellow
    Write-Host ""

    # Build first, then generate all test disks
    wsl.exe -e bash -c "cd $WSL_PROJECT && bash scripts/build.sh 2>&1 | tail -3 && echo '' && bash tools/make-test-disks.sh build/test-disks build"

} else {
    # Generate only the requested disk
    Write-Host "Target: $Disk" -ForegroundColor Yellow
    Write-Host ""

    # Build first
    Write-Host "Building OS (if needed)..." -ForegroundColor DarkGray
    wsl.exe -e bash -c "cd $WSL_PROJECT && bash scripts/build.sh 2>&1 | tail -3"

    # Determine the image file to delete so it gets regenerated
    if ($Disk -like "optical/*") {
        $imgFile = "build/test-disks/$Disk.iso"
    } else {
        $imgFile = "build/test-disks/$Disk.img"
    }

    # Delete the existing image so make-test-disks.sh regenerates it
    Write-Host "Regenerating $Disk..." -ForegroundColor Cyan
    wsl.exe -e bash -c "cd $WSL_PROJECT && rm -f '$imgFile' && bash tools/make-test-disks.sh build/test-disks build"
}

Write-Host ""

# Verify the disk was created
if ($Disk -ne "all") {
    if ($Disk -like "optical/*") {
        $checkPath = Join-Path $PROJECT "build\test-disks\$Disk.iso"
    } else {
        $checkPath = Join-Path $PROJECT "build\test-disks\$Disk.img"
    }

    if (Test-Path $checkPath) {
        $size = "{0:N1} MB" -f ((Get-Item $checkPath).Length / 1MB)
        Write-Host "OK: $Disk ($size)" -ForegroundColor Green
    } else {
        Write-Host "FAILED: $checkPath not created" -ForegroundColor Red
        Write-Host "Check that the required tools are installed in WSL2." -ForegroundColor Yellow
    }
}

Write-Host ""
Read-Host "Press Enter to close"
