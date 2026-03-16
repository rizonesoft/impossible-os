# write-usb.ps1 — Write Impossible OS to a USB flash drive
#
# Creates a bootable USB drive from the system-disk.img raw image.
# The image is a GPT disk with an EFI System Partition + IXFS partition.
#
# Usage: Right-click write-usb.bat → "Run as administrator"
#        Or from admin PowerShell: .\scripts\write-usb.ps1
#
# SAFETY: Only lists USB drives. Requires double confirmation.
#         Will NOT touch internal drives (SATA/NVMe/etc).

#Requires -RunAsAdministrator
$ErrorActionPreference = "Stop"

# ---- Paths ----
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent $SCRIPT_DIR
$BUILD      = Join-Path $PROJECT "build"
$DISK_IMG   = Join-Path $BUILD "system-disk.img"

# ---- Preflight ----
if (-not (Test-Path $DISK_IMG)) {
    Write-Host "Missing: $DISK_IMG" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh clean' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

$imgSize = (Get-Item $DISK_IMG).Length
$imgSizeMB = [math]::Round($imgSize / 1MB)
Write-Host "Impossible OS USB Writer" -ForegroundColor Cyan
Write-Host "========================" -ForegroundColor Cyan
Write-Host "  Image: $DISK_IMG `(${imgSizeMB} MB`)" -ForegroundColor DarkGray
Write-Host ""

# ---- List USB drives ----
@($usbDisks = Get-Disk | Where-Object { $_.BusType -eq "USB" })
$usbDisks = @($usbDisks)  # Force array even for single result

if ($usbDisks.Count -eq 0) {
    Write-Host "No USB drives found." -ForegroundColor Red
    Write-Host "Insert a USB flash drive and try again." -ForegroundColor Yellow
    pause; exit 1
}

Write-Host "Available USB drives:" -ForegroundColor Green
Write-Host ""
for ($i = 0; $i -lt $usbDisks.Count; $i++) {
    $disk = $usbDisks[$i]
    $sizeMB = [math]::Round($disk.Size / 1MB)
    $sizeGB = [math]::Round($disk.Size / 1GB, 1)
    $status = if ($sizeMB -lt $imgSizeMB) { " [TOO SMALL]" } else { "" }
    $num = $i + 1
    Write-Host "  [${num}] Disk $($disk.Number): $($disk.FriendlyName)" -ForegroundColor White
    Write-Host "      Size: ${sizeGB} GB `(${sizeMB} MB`)  Partitions: $($disk.NumberOfPartitions)${status}" -ForegroundColor DarkGray
}
Write-Host ""

# ---- Select drive ----
$selection = Read-Host "Select drive [1-$($usbDisks.Count)]"
$selection = [int]$selection

if ($selection -lt 1 -or $selection -gt $usbDisks.Count) {
    Write-Host "Invalid selection." -ForegroundColor Red
    pause; exit 1
}

$targetDisk = $usbDisks[$selection - 1]
$diskNumber = $targetDisk.Number
$diskName   = $targetDisk.FriendlyName
$diskSizeMB = [math]::Round($targetDisk.Size / 1MB)

if ($diskSizeMB -lt $imgSizeMB) {
    Write-Host "Drive too small `(${diskSizeMB} MB, need ${imgSizeMB} MB`)." -ForegroundColor Red
    pause; exit 1
}

# ---- Double confirmation ----
Write-Host ""
Write-Host "WARNING: ALL DATA ON THIS DRIVE WILL BE DESTROYED!" -ForegroundColor Red
Write-Host ""
Write-Host "  Target: Disk ${diskNumber} - ${diskName} `(${diskSizeMB} MB`)" -ForegroundColor Yellow
Write-Host "  Image:  $imgSizeMB MB" -ForegroundColor DarkGray
Write-Host ""
$confirm1 = Read-Host "Type 'YES' to continue"
if ($confirm1 -ne "YES") {
    Write-Host "Cancelled." -ForegroundColor Yellow
    pause; exit 0
}

$confirm2 = Read-Host "Type the disk number `(${diskNumber}`) to confirm"
if ($confirm2 -ne "$diskNumber") {
    Write-Host "Cancelled — disk number mismatch." -ForegroundColor Yellow
    pause; exit 0
}

# ---- Write image ----
Write-Host ""
Write-Host "Writing Impossible OS to USB..." -ForegroundColor Cyan

# Step 1: Take the disk offline and clear it
Write-Host "  [1/4] Cleaning disk $diskNumber..." -ForegroundColor DarkGray
Set-Disk -Number $diskNumber -IsOffline $false -ErrorAction SilentlyContinue
Clear-Disk -Number $diskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue

# Step 2: Write raw image to the physical disk
Write-Host "  [2/4] Writing ${imgSizeMB} MB image `(this may take a minute`)..." -ForegroundColor DarkGray

$physPath = "\\.\PhysicalDrive$diskNumber"
try {
    # Open physical disk for raw write
    $diskStream = [System.IO.File]::Open(
        $physPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None
    )
    $imgStream = [System.IO.File]::OpenRead($DISK_IMG)

    $buffer = New-Object byte[] (1MB)  # 1 MB chunks
    $totalWritten = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    while (($bytesRead = $imgStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $diskStream.Write($buffer, 0, $bytesRead)
        $totalWritten += $bytesRead
        $pct = [math]::Round(($totalWritten / $imgSize) * 100)
        $elapsed = $sw.Elapsed.TotalSeconds
        $speed = if ($elapsed -gt 0) { [math]::Round(($totalWritten / 1MB) / $elapsed, 1) } else { 0 }
        $writtenMB = [math]::Round($totalWritten / 1MB)
        Write-Host "`r        ${pct}%  ${writtenMB}/${imgSizeMB} MB  ${speed} MB/s  " -NoNewline -ForegroundColor DarkCyan
    }

    $diskStream.Flush()
    Write-Host ""
    $sw.Stop()
    $totalSec = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    $avgSpeed = [math]::Round($imgSizeMB / $sw.Elapsed.TotalSeconds, 1)
    Write-Host "  [3/4] Write complete - ${totalSec}s, ${avgSpeed} MB/s" -ForegroundColor Green

} catch {
    Write-Host ""
    Write-Host "  WRITE FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  Make sure no other program is using the USB drive." -ForegroundColor Yellow
    pause; exit 1
} finally {
    if ($imgStream)  { $imgStream.Close() }
    if ($diskStream) { $diskStream.Close() }
}

# Step 3: Refresh disk to pick up new partition table
Write-Host "  [4/4] Refreshing disk..." -ForegroundColor DarkGray
Start-Sleep -Seconds 1

# Force Windows to re-read the partition table
Set-Disk -Number $diskNumber -IsOffline $true  -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Set-Disk -Number $diskNumber -IsOffline $false -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# ---- Verify ----
Write-Host ""
Write-Host "Verifying USB boot files..." -ForegroundColor Cyan

$partition = Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue |
             Where-Object { $_.Type -eq "System" -or $_.GptType -eq "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}" } |
             Select-Object -First 1

if ($partition) {
    $driveLetter = $partition.DriveLetter
    $tempAssigned = $false
    if (-not $driveLetter) {
        $usedLetters = (Get-Volume).DriveLetter
        $driveLetter = [char[]](90..69) | Where-Object { $_ -notin $usedLetters } | Select-Object -First 1
        if ($driveLetter) {
            $partition | Set-Partition -NewDriveLetter $driveLetter -ErrorAction SilentlyContinue
            $tempAssigned = $true
            Start-Sleep -Milliseconds 500
        }
    }

    if ($driveLetter) {
        $efiPath    = "${driveLetter}:\EFI\BOOT\BOOTX64.EFI"
        $kernelPath = "${driveLetter}:\boot\kernel.exe"

        if (Test-Path $efiPath) {
            $efiKB = [math]::Round((Get-Item $efiPath).Length / 1KB)
            Write-Host "  [OK] BOOTX64.EFI `(${efiKB} KB`)" -ForegroundColor Green
        } else {
            Write-Host "  [FAIL] BOOTX64.EFI not found!" -ForegroundColor Red
        }

        if (Test-Path $kernelPath) {
            $kKB = [math]::Round((Get-Item $kernelPath).Length / 1KB)
            Write-Host "  [OK] kernel.exe `(${kKB} KB`)" -ForegroundColor Green
        } else {
            Write-Host "  [WARN] kernel.exe not found" -ForegroundColor Yellow
        }

        if ($tempAssigned) {
            $partition | Remove-PartitionAccessPath -AccessPath "${driveLetter}:\" -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "  [WARN] Could not assign drive letter for verification" -ForegroundColor Yellow
    }
} else {
    Write-Host "  [WARN] EFI System Partition not detected" -ForegroundColor Yellow
    Write-Host "  The USB is written but Windows couldn't verify the GPT." -ForegroundColor DarkGray
    Write-Host "  It should still boot on UEFI machines." -ForegroundColor DarkGray
}

# ---- Done ----
Write-Host ""
Write-Host "Done! USB drive is ready to boot." -ForegroundColor Green
Write-Host ""
Write-Host "To boot:" -ForegroundColor Cyan
Write-Host "  1. Insert USB into target machine" -ForegroundColor DarkGray
Write-Host "  2. Enter BIOS/UEFI boot menu `(usually F12, F2, or Del`)" -ForegroundColor DarkGray
Write-Host "  3. Select the USB drive `(UEFI mode`)" -ForegroundColor DarkGray
Write-Host "  4. Impossible OS should boot!" -ForegroundColor DarkGray
Write-Host ""
pause
