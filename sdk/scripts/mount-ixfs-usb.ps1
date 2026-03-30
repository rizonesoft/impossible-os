# mount-ixfs-usb.ps1 -- Detect USB drives with IXFS partitions and mount them.
#
# Split workflow:
#   Phase 1: Runs elevated (admin) to scan raw physical drives for IXFS partitions
#   Phase 2: Runs as normal user to mount via WinFsp (visible in Explorer)
#
# Usage:
#   mount-ixfs-usb.bat              Auto-detect and mount
#
# Unmount: unmount-ixfs.bat

param(
    [string]$Phase = "",
    [string]$ScanResult = ""
)

$ErrorActionPreference = "Continue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SdkDir    = Split-Path -Parent $ScriptDir
$IxfsMount = Join-Path $SdkDir "tools\ixfs-mount.exe"
$IXFS_MAGIC = 0x49584653

# ============================================================================
# Phase 1: Elevated scan -- find IXFS partition on USB drives
# ============================================================================

function Open-RawDrive {
    param([string]$Path)
    try {
        return New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    } catch {
        return $null
    }
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
            $magicBuf = Read-Bytes -Stream $fs -Offset ($startLBA * 512) -Count 4
            if ($magicBuf) {
                $magic = [BitConverter]::ToUInt32($magicBuf, 0)
                if ($magic -eq $IXFS_MAGIC) {
                    $fs.Close()
                    return @{ Index = $partIndex; StartLBA = $startLBA }
                }
            }
        }
        $fs.Close()
    } catch {
        try { $fs.Close() } catch {}
    }
    return $null
}

function Run-Scan {
    Write-Host "  Phase 1: Scanning for IXFS partitions (elevated)..." -ForegroundColor Cyan
    Write-Host ""

    $drives = Get-WmiObject Win32_DiskDrive | Where-Object {
        $_.InterfaceType -eq "USB" -or $_.MediaType -match "Removable"
    }

    if (-not $drives -or @($drives).Count -eq 0) {
        Write-Host "  No USB/removable drives found" -ForegroundColor Red
        return $null
    }

    Write-Host "  Found $(@($drives).Count) removable device(s)"

    foreach ($drive in $drives) {
        $devPath = $drive.Name
        $model = $drive.Model
        $sizeMB = [math]::Round($drive.Size / 1MB)
        Write-Host "  Scanning $devPath ($model, ${sizeMB} MB)..." -ForegroundColor DarkGray

        $result = Find-IXFSPartition -DrivePath $devPath
        if ($result) {
            $driveNum = [regex]::Match($devPath, '\d+$').Value
            Write-Host "  FOUND " -ForegroundColor Green -NoNewline
            Write-Host "IXFS partition $($result.Index) on PhysicalDrive$driveNum"
            return "$driveNum`:$($result.Index)"
        }
    }

    Write-Host "  No IXFS partitions found" -ForegroundColor Red
    return $null
}

# ============================================================================
# Phase 2: Non-elevated mount -- launch ixfs-mount.exe as normal user
# ============================================================================

function Get-FreeDriveLetter {
    foreach ($letter in [char[]]('I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z')) {
        $drive = "${letter}:"
        if (-not (Test-Path $drive)) { return $drive }
    }
    return $null
}

function Run-Mount {
    param([string]$ScanInfo)

    $parts = $ScanInfo -split ":"
    $driveNum = $parts[0]
    $partIdx = $parts[1]

    $driveLetter = Get-FreeDriveLetter
    if (-not $driveLetter) {
        Write-Host "  ERROR: No free drive letters" -ForegroundColor Red
        return
    }

    Write-Host "  Phase 2: Mounting IXFS as $driveLetter..." -ForegroundColor Cyan

    # Ensure WinFsp DLL is findable
    $winfspBin = "C:\Program Files (x86)\WinFsp\bin"
    if (Test-Path $winfspBin) { $env:PATH = "$winfspBin;$env:PATH" }

    $mountArgs = "$driveLetter \\.\PhysicalDrive$driveNum $partIdx"
    Write-Host "  Command: ixfs-mount.exe $mountArgs" -ForegroundColor DarkGray

    $errLog = Join-Path $env:TEMP "ixfs-mount-err.log"
    $outLog = Join-Path $env:TEMP "ixfs-mount-out.log"

    $proc = Start-Process -FilePath $IxfsMount -ArgumentList $mountArgs -NoNewWindow -PassThru -RedirectStandardError $errLog -RedirectStandardOutput $outLog

    Start-Sleep -Seconds 3

    if (-not $proc.HasExited) {
        if (Test-Path "$driveLetter\") {
            Write-Host "  " -NoNewline
            Write-Host "Mounted IXFS from PhysicalDrive$driveNum, partition $partIdx as $driveLetter" -ForegroundColor Green
            Write-Host ""
            Write-Host "  Browse: open $driveLetter\ in Explorer"
            Write-Host "  Unmount: unmount-ixfs.bat $driveLetter"
        } else {
            Write-Host "  Process running -- $driveLetter should appear in Explorer shortly" -ForegroundColor Yellow
        }
    } else {
        Write-Host "  Mount failed (exit code $($proc.ExitCode))" -ForegroundColor Red
        if (Test-Path $errLog) {
            $err = Get-Content $errLog -Raw
            if ($err) { Write-Host "  $err" }
        }
        if (Test-Path $outLog) {
            $out = Get-Content $outLog -Raw
            if ($out) { Write-Host "  $out" }
        }
    }
}

# ============================================================================
# Main entry point -- orchestrates Phase 1 (elevated) and Phase 2 (normal)
# ============================================================================

Write-Host ""
Write-Host "  IXFS USB Mount" -ForegroundColor White
Write-Host ("=" * 50)

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$resultFile = Join-Path $env:TEMP "ixfs-usb-scan.txt"

if ($Phase -eq "mount" -and $ScanResult) {
    # Phase 2: called back as normal user with scan result
    Run-Mount -ScanInfo $ScanResult
    exit 0
}

if ($Phase -eq "scan") {
    # Phase 1: running elevated, do the scan and save result
    $info = Run-Scan
    if ($info) {
        Set-Content -Path $resultFile -Value $info
        Write-Host ""
        Write-Host "  Scan complete. Launching mount as normal user..." -ForegroundColor DarkGray
    } else {
        Set-Content -Path $resultFile -Value "NONE"
    }
    exit 0
}

# Default entry: orchestrate both phases

if (-not $isAdmin) {
    # Not admin -- try scan directly first (may work if user has raw disk perms)
    $info = Run-Scan
    if ($info) {
        Run-Mount -ScanInfo $info
    } else {
        # Scan failed -- need elevation for raw drive access
        Write-Host ""
        Write-Host "  Raw drive access requires admin. Requesting elevation..." -ForegroundColor Yellow
        Write-Host ""

        # Launch Phase 1 elevated
        $scanScript = $MyInvocation.MyCommand.Path
        Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$scanScript`" -Phase scan" -Verb RunAs -Wait

        # Read scan result
        if (Test-Path $resultFile) {
            $info = (Get-Content $resultFile -Raw).Trim()
            Remove-Item $resultFile -Force -ErrorAction SilentlyContinue
            if ($info -and $info -ne "NONE") {
                # Phase 2: mount as normal user (current context)
                Run-Mount -ScanInfo $info
            } else {
                Write-Host "  No IXFS partitions found" -ForegroundColor Red
            }
        } else {
            Write-Host "  Scan failed or was cancelled" -ForegroundColor Red
        }
    }
} else {
    # Already admin -- scan here, then warn about mount visibility
    $info = Run-Scan
    if ($info) {
        Write-Host ""
        Write-Host "  WARNING: Running as admin -- mount may not show in Explorer" -ForegroundColor Yellow
        Write-Host "  Mounting anyway (use Explorer's address bar to type the drive letter)..." -ForegroundColor DarkGray
        Write-Host ""
        Run-Mount -ScanInfo $info
    }
}
