# gen-test-disk.ps1 -- Generate filesystem test disk images via WSL2
#
# Usage:
#   .\scripts\test-filesystem\gen-test-disk.ps1              Generate ALL test disks
#   .\scripts\test-filesystem\gen-test-disk.ps1 fat32         Generate only FAT32
#   .\scripts\test-filesystem\gen-test-disk.ps1 optical/iso9660  Generate only ISO 9660

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

# Convert Windows UNC path to WSL path
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

    wsl.exe bash -c "cd '$wslPath' && bash tools/make-test-disks.sh build/test-disks build"

} else {
    Write-Host "Target: $Disk" -ForegroundColor Yellow
    Write-Host ""

    if ($Disk -like "optical/*") {
        $delFile = "build/test-disks/$Disk.iso"
        $checkPath = Join-Path $TEST_DIR "$Disk.iso"
    } else {
        $delFile = "build/test-disks/$Disk.img"
        $checkPath = Join-Path $TEST_DIR "$Disk.img"
    }

    Write-Host "Regenerating $Disk..." -ForegroundColor Cyan
    wsl.exe bash -c "cd '$wslPath' && rm -f '$delFile' && bash tools/make-test-disks.sh build/test-disks build"

    Write-Host ""

    if (Test-Path $checkPath) {
        $size = "{0:N1} MB" -f ((Get-Item $checkPath).Length / 1MB)
        Write-Host "OK: $Disk ($size)" -ForegroundColor Green
    } else {
        Write-Host "FAILED: $checkPath not created" -ForegroundColor Red
        Write-Host "Build the OS first in WSL2: bash scripts/build.sh" -ForegroundColor Yellow
    }
}

Write-Host ""
Read-Host "Press Enter to close"
