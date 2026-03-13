# gen-test-disk.ps1 -- Generate filesystem test disk images via WSL2
#
# Usage:
#   .\scripts\test-fs\gen-test-disk.ps1              Generate ALL test disks
#   .\scripts\test-fs\gen-test-disk.ps1 fat32         Generate only FAT32
#   .\scripts\test-fs\gen-test-disk.ps1 optical/iso9660  Generate only ISO 9660

param(
    [Parameter(Position=0)]
    [string]$Disk = "all"
)

$ErrorActionPreference = "Continue"

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$TEST_DIR   = Join-Path $PROJECT "build\test-disks"

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

# Convert Windows UNC path to WSL path directly
# \\wsl.localhost\Ubuntu\home\user\project -> /home/user/project
$wslPath = $PROJECT
if ($wslPath -match '^\\\\wsl') {
    $wslPath = $wslPath -replace '^\\\\wsl\.[^\\]+\\[^\\]+', ''
}
$wslPath = $wslPath -replace '\\', '/'

Write-Host "Project:  $PROJECT" -ForegroundColor DarkGray
Write-Host "WSL path: $wslPath" -ForegroundColor DarkGray
Write-Host ""

if ($Disk -eq "all") {
    Write-Host "Generating ALL test disk images..." -ForegroundColor Yellow
    Write-Host ""

    $cmd = "cd '$wslPath' && bash scripts/build.sh 2>&1 | tail -3 && echo '' && bash tools/make-test-disks.sh build/test-disks build"
    wsl.exe bash -c $cmd

} else {
    Write-Host "Target: $Disk" -ForegroundColor Yellow
    Write-Host ""

    # Determine the image file to delete so it gets regenerated
    if ($Disk -like "optical/*") {
        $delFile = "build/test-disks/$Disk.iso"
        $checkPath = Join-Path $TEST_DIR "$Disk.iso"
    } else {
        $delFile = "build/test-disks/$Disk.img"
        $checkPath = Join-Path $TEST_DIR "$Disk.img"
    }

    # Build + delete old + regenerate
    Write-Host "Building OS and regenerating $Disk..." -ForegroundColor Cyan
    $cmd = "cd '$wslPath' && bash scripts/build.sh 2>&1 | tail -3 && rm -f '$delFile' && bash tools/make-test-disks.sh build/test-disks build"
    wsl.exe bash -c $cmd

    Write-Host ""

    # Verify
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
