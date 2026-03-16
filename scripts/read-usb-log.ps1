# read-usb-log.ps1 -- Read kernel logs from Impossible OS USB drive
#
# Finds the IXOS_LOG partition on a USB drive, copies serial.log
# to the local build directory, and displays the last 50 lines.
#
# Usage: Right-click read-usb-log.bat -> "Run as administrator"
#        Or from admin PowerShell: .\scripts\read-usb-log.ps1

$ErrorActionPreference = "Stop"

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent $SCRIPT_DIR
$BUILD      = Join-Path $PROJECT "build"

Write-Host "Impossible OS Log Reader" -ForegroundColor Cyan
Write-Host "========================" -ForegroundColor Cyan
Write-Host ""

# Find the IXOS_LOG volume
$logVol = Get-Volume | Where-Object { $_.FileSystemLabel -eq "IXOS_LOG" } | Select-Object -First 1

if (-not $logVol) {
    Write-Host "No IXOS_LOG partition found." -ForegroundColor Red
    Write-Host "Make sure the USB drive is plugged in." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Available volumes:" -ForegroundColor DarkGray
    Get-Volume | Where-Object { $_.DriveLetter } |
        Format-Table DriveLetter, FileSystemLabel, FileSystem, @{L="Size MB";E={[math]::Round($_.Size/1MB)}} -AutoSize
    exit 1
}

$letter = $logVol.DriveLetter
if (-not $letter) {
    Write-Host "IXOS_LOG found but no drive letter assigned." -ForegroundColor Yellow
    Write-Host "Attempting to assign one..." -ForegroundColor DarkGray

    $partition = Get-Partition | Where-Object {
        (Get-Volume -Partition $_ -ErrorAction SilentlyContinue).FileSystemLabel -eq "IXOS_LOG"
    } | Select-Object -First 1

    if ($partition) {
        $usedLetters = (Get-Volume).DriveLetter
        $letter = [char[]](90..69) | Where-Object { $_ -notin $usedLetters } | Select-Object -First 1
        if ($letter) {
            $partition | Set-Partition -NewDriveLetter $letter -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 500
        }
    }

    if (-not $letter) {
        Write-Host "Could not assign drive letter." -ForegroundColor Red
        exit 1
    }
}

$logPath = "${letter}:\serial.log"
Write-Host "  Drive: ${letter}:\ IXOS_LOG" -ForegroundColor Green

if (-not (Test-Path $logPath)) {
    Write-Host "  No serial.log found on the log partition." -ForegroundColor Yellow
    Write-Host "  The kernel may not have written logs yet." -ForegroundColor DarkGray
    exit 0
}

$logSize = (Get-Item $logPath).Length
$logKB = [math]::Round($logSize / 1KB, 1)
Write-Host "  Log: serial.log - ${logKB} KB" -ForegroundColor Green
Write-Host ""

# Copy to build directory
if (-not (Test-Path $BUILD)) {
    New-Item -ItemType Directory -Path $BUILD -Force | Out-Null
}

$dest = Join-Path $BUILD "serial.log"
Copy-Item -Path $logPath -Destination $dest -Force
Write-Host "  Copied to: $dest" -ForegroundColor DarkGray
Write-Host ""

# Display last 50 lines
Write-Host "--- Last 50 lines ---" -ForegroundColor Cyan
$lines = Get-Content $logPath -Tail 50
foreach ($line in $lines) {
    if ($line -match "ERROR|FATAL") {
        Write-Host $line -ForegroundColor Red
    } elseif ($line -match "WARN") {
        Write-Host $line -ForegroundColor Yellow
    } elseif ($line -match "DEBUG") {
        Write-Host $line -ForegroundColor DarkGray
    } else {
        Write-Host $line -ForegroundColor White
    }
}
Write-Host "--- End ---" -ForegroundColor Cyan
Write-Host ""
Write-Host "Full log saved to: $dest" -ForegroundColor Green
