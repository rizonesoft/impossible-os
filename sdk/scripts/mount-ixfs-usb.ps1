# mount-ixfs-usb.ps1 -- Detect USB drives with IXFS partitions and mount them.
#
# Strategy: Extract IXFS partition from USB to a temp image file (requires admin),
# then mount the image as normal user (visible in Explorer).
#
# Usage: mount-ixfs-usb.bat
# Unmount: unmount-ixfs.bat

param(
    [string]$Phase = "",
    [string]$ImageFile = ""
)

$ErrorActionPreference = "Continue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SdkDir    = Split-Path -Parent $ScriptDir
$IxfsMount = Join-Path $SdkDir "tools\ixfs-mount.exe"
$IXFS_MAGIC = 0x49584653
$TempImage = Join-Path $env:TEMP "ixfs-usb-partition.img"
$ResultFile = Join-Path $env:TEMP "ixfs-usb-result.txt"

# -- Helpers --
function Open-RawDrive {
    param([string]$Path)
    try {
        return New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    } catch { return $null }
}

function Read-Bytes {
    param([System.IO.FileStream]$Stream, [uint64]$Offset, [int]$Count)
    $sectorOff = $Offset - ($Offset % 512)
    $intraOff = $Offset - $sectorOff
    $readLen = [math]::Ceiling(($intraOff + $Count) / 512.0) * 512
    $Stream.Seek($sectorOff, [System.IO.SeekOrigin]::Begin) | Out-Null
    $buf = New-Object byte[] $readLen
    $bytesRead = $Stream.Read($buf, 0, $readLen)
    if ($bytesRead -lt ($intraOff + $Count)) { return $null }
    $result = New-Object byte[] $Count
    [Array]::Copy($buf, $intraOff, $result, 0, $Count)
    return $result
}

function Find-IXFSPartition {
    param([string]$DrivePath)
    $fs = Open-RawDrive -Path $DrivePath
    if (-not $fs) { return $null }
    try {
        $hdr = Read-Bytes -Stream $fs -Offset 512 -Count 92
        if (-not $hdr) { $fs.Close(); return $null }
        $sig = [System.Text.Encoding]::ASCII.GetString($hdr, 0, 8)
        if ($sig -ne "EFI PART") { $fs.Close(); return $null }
        $entryLBA = [BitConverter]::ToUInt64($hdr, 72)
        $numEntries = [BitConverter]::ToUInt32($hdr, 80)
        $entrySize = [BitConverter]::ToUInt32($hdr, 84)
        if ($entrySize -lt 128) { $entrySize = 128 }
        $partIndex = 0
        for ($i = 0; $i -lt $numEntries; $i++) {
            $entryOffset = $entryLBA * 512 + $i * $entrySize
            $entry = Read-Bytes -Stream $fs -Offset $entryOffset -Count 128
            if (-not $entry) { break }
            $allZero = $true
            for ($j = 0; $j -lt 16; $j++) {
                if ($entry[$j] -ne 0) { $allZero = $false; break }
            }
            if ($allZero) { continue }
            $partIndex++
            $startLBA = [BitConverter]::ToUInt64($entry, 32)
            $endLBA = [BitConverter]::ToUInt64($entry, 40)
            $magicBuf = Read-Bytes -Stream $fs -Offset ($startLBA * 512) -Count 4
            if ($magicBuf) {
                $magic = [BitConverter]::ToUInt32($magicBuf, 0)
                if ($magic -eq $IXFS_MAGIC) {
                    $fs.Close()
                    return @{ Index = $partIndex; StartLBA = $startLBA; EndLBA = $endLBA }
                }
            }
        }
        $fs.Close()
    } catch { try { $fs.Close() } catch {} }
    return $null
}

# ============================================================================
# Phase: extract -- runs elevated, extracts IXFS partition to temp image file
# ============================================================================
function Run-Extract {
    Write-Host "  Scanning for IXFS partitions..." -ForegroundColor Cyan

    $drives = Get-WmiObject Win32_DiskDrive | Where-Object {
        $_.InterfaceType -eq "USB" -or $_.MediaType -match "Removable"
    }

    if (-not $drives -or @($drives).Count -eq 0) {
        Write-Host "  No USB/removable drives found" -ForegroundColor Red
        return $false
    }

    Write-Host "  Found $(@($drives).Count) removable device(s)"

    foreach ($drive in $drives) {
        $devPath = $drive.Name
        $model = $drive.Model
        $sizeMB = [math]::Round($drive.Size / 1MB)
        Write-Host "  Scanning $devPath ($model, ${sizeMB} MB)..." -ForegroundColor DarkGray

        $result = Find-IXFSPartition -DrivePath $devPath
        if ($result) {
            Write-Host "  FOUND " -ForegroundColor Green -NoNewline
            Write-Host "IXFS partition $($result.Index)"
            Write-Host ""

            # Extract partition to temp image file
            $partBytes = ($result.EndLBA - $result.StartLBA + 1) * 512
            $partMB = [math]::Round($partBytes / 1MB)
            Write-Host "  Extracting partition ($partMB MB) to temp file..." -ForegroundColor Cyan

            $fs = Open-RawDrive -Path $devPath
            if (-not $fs) {
                Write-Host "  Failed to open drive" -ForegroundColor Red
                return $false
            }

            try {
                $outFs = [System.IO.File]::Create($TempImage)
                $fs.Seek($result.StartLBA * 512, [System.IO.SeekOrigin]::Begin) | Out-Null

                $bufSize = 1048576  # 1 MB chunks
                $buf = New-Object byte[] $bufSize
                $remaining = $partBytes
                $written = 0

                while ($remaining -gt 0) {
                    $toRead = [math]::Min($bufSize, $remaining)
                    $bytesRead = $fs.Read($buf, 0, $toRead)
                    if ($bytesRead -le 0) { break }
                    $outFs.Write($buf, 0, $bytesRead)
                    $remaining -= $bytesRead
                    $written += $bytesRead

                    # Progress every 10 MB
                    $pct = [math]::Floor($written * 100 / $partBytes)
                    if ($written % (10 * 1048576) -lt $bufSize) {
                        $writtenMB = [math]::Round($written / 1MB)
                        Write-Host "`r  Extracted $writtenMB / $partMB MB ($pct%)   " -NoNewline -ForegroundColor DarkGray
                    }
                }

                $outFs.Close()
                $fs.Close()

                Write-Host "`r  Extracted $partMB / $partMB MB (100%)   " -ForegroundColor DarkGray
                Write-Host "  OK " -ForegroundColor Green -NoNewline
                Write-Host "Partition saved to temp file"

                # Write result file so Phase 2 knows the image path
                Set-Content -Path $ResultFile -Value $TempImage
                return $true

            } catch {
                Write-Host "  Extraction failed: $_" -ForegroundColor Red
                try { $outFs.Close() } catch {}
                try { $fs.Close() } catch {}
                return $false
            }
        }
    }

    Write-Host "  No IXFS partitions found" -ForegroundColor Red
    return $false
}

# ============================================================================
# Phase: mount -- runs as normal user, mounts the extracted image file
# ============================================================================
function Run-Mount {
    param([string]$Image)

    if (-not (Test-Path $Image)) {
        Write-Host "  Image file not found: $Image" -ForegroundColor Red
        return
    }

    if (-not (Test-Path $IxfsMount)) {
        Write-Host "  ixfs-mount.exe not found. Run: sdk\build.bat" -ForegroundColor Red
        return
    }

    # Find free drive letter
    foreach ($letter in [char[]]('I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z')) {
        $drive = "${letter}:"
        if (-not (Test-Path $drive)) { $driveLetter = $drive; break }
    }
    if (-not $driveLetter) {
        Write-Host "  No free drive letters" -ForegroundColor Red
        return
    }

    # Ensure WinFsp DLL is findable
    $winfspBin = "C:\Program Files (x86)\WinFsp\bin"
    if (Test-Path $winfspBin) { $env:PATH = "$winfspBin;$env:PATH" }

    # The image IS the partition (no GPT wrapper), so partition index = 0
    # But ixfs-mount.exe expects a GPT image with partition index.
    # We need to pass the raw image directly. Use partition index 0 as a signal
    # to skip GPT parsing and treat the whole file as the IXFS volume.
    $mountArgs = "$driveLetter `"$Image`" 0"
    Write-Host "  Mounting as $driveLetter..." -ForegroundColor Cyan

    $errLog = Join-Path $env:TEMP "ixfs-mount-err.log"
    $proc = Start-Process -FilePath $IxfsMount -ArgumentList $mountArgs -NoNewWindow -PassThru -RedirectStandardError $errLog

    Start-Sleep -Seconds 3

    if (-not $proc.HasExited) {
        if (Test-Path "$driveLetter\") {
            Write-Host ""
            Write-Host "  Mounted IXFS as $driveLetter" -ForegroundColor Green
            Write-Host ""

            # Open Explorer
            Start-Process "explorer.exe" $driveLetter

            Write-Host "  Keep this window open -- closing it unmounts the drive." -ForegroundColor Yellow
            Write-Host "  Unmount: close this window or run unmount-ixfs.bat"
            Write-Host ""

            # Block to keep mount alive
            Read-Host "  Press Enter to unmount"

            # Cleanup
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 1
            Remove-Item $Image -Force -ErrorAction SilentlyContinue
            Write-Host "  Unmounted and cleaned up." -ForegroundColor Green
        } else {
            Write-Host "  Process running but $driveLetter not visible" -ForegroundColor Yellow
            if (Test-Path $errLog) {
                $err = Get-Content $errLog -Raw
                if ($err) { Write-Host "  $err" -ForegroundColor DarkGray }
            }
        }
    } else {
        Write-Host "  Mount failed (exit code $($proc.ExitCode))" -ForegroundColor Red
        if (Test-Path $errLog) {
            $err = Get-Content $errLog -Raw
            if ($err) { Write-Host "  $err" }
        }
        Remove-Item $Image -Force -ErrorAction SilentlyContinue
    }
}

# ============================================================================
# Main
# ============================================================================
Write-Host ""
Write-Host "  IXFS USB Mount" -ForegroundColor White
Write-Host ("=" * 50)
Write-Host ""

# Kill any existing ixfs-mount.exe processes
$existing = Get-Process -Name "ixfs-mount" -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "  Stopping previous ixfs-mount..." -ForegroundColor DarkGray
    $existing | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

# Clean up stale temp image
if (Test-Path $TempImage) {
    Remove-Item $TempImage -Force -ErrorAction SilentlyContinue
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($Phase -eq "extract") {
    # Phase 1: elevated, extract partition
    $ok = Run-Extract
    if (-not $ok) { Set-Content -Path $ResultFile -Value "FAILED" }
    exit 0
}

if ($Phase -eq "mount" -and $ImageFile) {
    # Phase 2: normal user, mount image
    Run-Mount -Image $ImageFile
    exit 0
}

# Default: orchestrate both phases
# Phase 1: extract (needs admin)
if (-not $isAdmin) {
    Write-Host "  Requesting admin to read USB drive..." -ForegroundColor Yellow
    $thisScript = $MyInvocation.MyCommand.Path
    Start-Process -FilePath "powershell.exe" `
        -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$thisScript`" -Phase extract" `
        -Verb RunAs -Wait
} else {
    Run-Extract
}

# Check result
if (-not (Test-Path $ResultFile)) {
    Write-Host "  Extraction failed or was cancelled" -ForegroundColor Red
    exit 1
}

$imageResult = (Get-Content $ResultFile -Raw).Trim()
Remove-Item $ResultFile -Force -ErrorAction SilentlyContinue

if ($imageResult -eq "FAILED" -or -not (Test-Path $imageResult)) {
    Write-Host "  Extraction failed" -ForegroundColor Red
    exit 1
}

Write-Host ""

# Phase 2: mount as normal user (this context, not elevated)
if ($isAdmin) {
    # We're already admin -- mount here (may not show in Explorer but will work)
    Run-Mount -Image $imageResult
} else {
    # Normal user -- mount directly (visible in Explorer!)
    Run-Mount -Image $imageResult
}
