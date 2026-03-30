# mount-ixfs-usb.ps1 -- Detect USB drives with IXFS partitions and mount them.
#
# Usage:
#   mount-ixfs-usb.bat              Auto-detect and mount
#
# Unmount: unmount-ixfs.bat

$ErrorActionPreference = "Continue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SdkDir    = Split-Path -Parent $ScriptDir
$IxfsMount = Join-Path $SdkDir "tools\ixfs-mount.exe"
$IXFS_MAGIC = 0x49584653  # "IXFS"

if (-not (Test-Path $IxfsMount)) {
    Write-Host "  ERROR" -ForegroundColor Red -NoNewline
    Write-Host " ixfs-mount.exe not found at $IxfsMount"
    Write-Host "        Run: sdk\build.bat"
    exit 1
}

# Find next available drive letter (Z: down to G:)
function Get-FreeDriveLetter {
    foreach ($letter in [char[]]('I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z')) {
        $drive = "${letter}:"
        if (-not (Test-Path $drive)) { return $drive }
    }
    return $null
}

# Read 4 bytes from a raw device at a given byte offset
function Read-Magic {
    param([string]$DevicePath, [uint64]$Offset)

    try {
        $fs = [System.IO.File]::Open($DevicePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $fs.Seek($Offset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $buf = New-Object byte[] 4
        $fs.Read($buf, 0, 4) | Out-Null
        $fs.Close()
        $magic = [BitConverter]::ToUInt32($buf, 0)
        return $magic
    } catch {
        return 0
    }
}

# Open a raw physical drive for reading (requires admin on some systems)
function Open-RawDrive {
    param([string]$Path)

    # Use .NET FileStream with explicit sharing flags for raw device access
    # FileAccess.Read = 1, FileShare.ReadWrite = 3, FileMode.Open = 3
    try {
        return New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    } catch {
        return $null
    }
}

# Read bytes from a FileStream at a given offset (sector-aligned for raw drives)
function Read-Bytes {
    param([System.IO.FileStream]$Stream, [uint64]$Offset, [int]$Count)

    # Raw drives require sector-aligned reads; read a full sector then extract
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

# Parse GPT to find IXFS partitions on a physical drive
function Find-IXFSPartition {
    param([string]$DrivePath)

    $fs = Open-RawDrive -Path $DrivePath
    if (-not $fs) { return $null }

    try {
        # Read GPT header at LBA 1 (offset 512)
        $hdr = Read-Bytes -Stream $fs -Offset 512 -Count 92
        if (-not $hdr) { $fs.Close(); return $null }

        # Check GPT signature "EFI PART"
        $sig = [System.Text.Encoding]::ASCII.GetString($hdr, 0, 8)
        if ($sig -ne "EFI PART") {
            $fs.Close()
            return $null
        }

        $entryLBA = [BitConverter]::ToUInt64($hdr, 72)
        $numEntries = [BitConverter]::ToUInt32($hdr, 80)
        $entrySize = [BitConverter]::ToUInt32($hdr, 84)
        if ($entrySize -lt 128) { $entrySize = 128 }

        $partIndex = 0
        for ($i = 0; $i -lt $numEntries; $i++) {
            $entryOffset = $entryLBA * 512 + $i * $entrySize
            $entry = Read-Bytes -Stream $fs -Offset $entryOffset -Count 128
            if (-not $entry) { break }

            # Check if entry is used (type GUID not all zeros)
            $allZero = $true
            for ($j = 0; $j -lt 16; $j++) {
                if ($entry[$j] -ne 0) { $allZero = $false; break }
            }
            if ($allZero) { continue }

            $partIndex++
            $startLBA = [BitConverter]::ToUInt64($entry, 32)

            # Check IXFS magic at the start of this partition
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

# Main
Write-Host "Scanning for USB drives with IXFS partitions..."
Write-Host ""

# Enumerate physical drives
$drives = Get-WmiObject Win32_DiskDrive | Where-Object {
    $_.InterfaceType -eq "USB" -or $_.MediaType -match "Removable"
}

if (-not $drives -or $drives.Count -eq 0) {
    Write-Host "  " -NoNewline
    Write-Host "No USB/removable drives found" -ForegroundColor Red
    exit 1
}

$driveCount = @($drives).Count
Write-Host "  Found " -NoNewline
Write-Host "$driveCount" -ForegroundColor Cyan -NoNewline
Write-Host " removable device(s)"

$found = $null
$foundDrive = $null

foreach ($drive in $drives) {
    # Win32_DiskDrive.Name is \\.\PHYSICALDRIVE7 format
    $devPath = $drive.Name
    $model = $drive.Model
    $sizeMB = [math]::Round($drive.Size / 1MB)

    Write-Host "  Scanning " -ForegroundColor DarkGray -NoNewline
    Write-Host "$devPath" -ForegroundColor Cyan -NoNewline
    Write-Host " ($model, ${sizeMB} MB)" -ForegroundColor DarkGray

    $result = Find-IXFSPartition -DrivePath $devPath
    if ($result) {
        Write-Host "  FOUND " -ForegroundColor Green -NoNewline
        Write-Host "IXFS partition $($result.Index) on $devPath"
        $found = $result
        $foundDrive = $devPath
        break
    }
}

if (-not $found) {
    Write-Host ""
    Write-Host "  No IXFS partitions found on USB drives" -ForegroundColor Red
    exit 1
}

# Find free drive letter
$driveLetter = Get-FreeDriveLetter
if (-not $driveLetter) {
    Write-Host "  ERROR" -ForegroundColor Red -NoNewline
    Write-Host " No free drive letters available"
    exit 1
}

Write-Host ""
Write-Host "  Mounting as " -NoNewline
Write-Host "$driveLetter" -ForegroundColor Cyan -NoNewline
Write-Host "..."

# Launch ixfs-mount.exe and capture output for debugging
$driveNum = [regex]::Match($foundDrive, '\d+$').Value
$mountArgs = "$driveLetter \\.\PhysicalDrive$driveNum $($found.Index)"
Write-Host "  Command: $IxfsMount $mountArgs" -ForegroundColor DarkGray

# Ensure WinFsp DLL is findable (add system WinFsp bin to PATH if needed)
$winfspBin = "C:\Program Files (x86)\WinFsp\bin"
if (Test-Path $winfspBin) {
    $env:PATH = "$winfspBin;$env:PATH"
}

# IMPORTANT: If running as admin, the WinFsp mount won't be visible in Explorer
# (different security context). Drop to non-elevated for the mount process.
$errLog = Join-Path $env:TEMP "ixfs-mount-err.log"
$outLog = Join-Path $env:TEMP "ixfs-mount-out.log"

# Check if we're elevated
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($isAdmin) {
    Write-Host "  NOTE " -ForegroundColor Yellow -NoNewline
    Write-Host "Running as admin -- mount may not be visible in Explorer"
    Write-Host "       Try running mount-ixfs-usb.bat WITHOUT admin privileges" -ForegroundColor DarkGray
    Write-Host "       (raw drive scan needs admin, but mount does not)" -ForegroundColor DarkGray
}

$proc = Start-Process -FilePath $IxfsMount -ArgumentList $mountArgs -NoNewWindow -PassThru -RedirectStandardError $errLog -RedirectStandardOutput $outLog

Start-Sleep -Seconds 3

if (-not $proc.HasExited) {
    if (Test-Path "$driveLetter\") {
        Write-Host "  " -NoNewline
        Write-Host "Mounted IXFS from PhysicalDrive$driveNum, partition $($found.Index) as $driveLetter" -ForegroundColor Green
        Write-Host ""
        Write-Host "  Unmount: unmount-ixfs.bat $driveLetter"
    } else {
        Write-Host "  " -NoNewline
        Write-Host "Process running but $driveLetter not visible" -ForegroundColor Yellow
        Write-Host ""
        if ($isAdmin) {
            Write-Host "  This is likely because you ran as Administrator." -ForegroundColor Yellow
            Write-Host "  The mount exists but is in the admin context." -ForegroundColor DarkGray
            Write-Host "  Try: run mount-ixfs-usb.bat as normal user (not admin)" -ForegroundColor DarkGray
        }
        # Show any output from ixfs-mount
        if (Test-Path $errLog) {
            $err = Get-Content $errLog -Raw
            if ($err) { Write-Host "  $err" -ForegroundColor DarkGray }
        }
    }
} else {
    Write-Host "  " -NoNewline
    Write-Host "Mount failed (exit code $($proc.ExitCode))" -ForegroundColor Red
    if (Test-Path $errLog) {
        $err = Get-Content $errLog -Raw
        if ($err) { Write-Host ""; Write-Host "  $err" }
    }
    if (Test-Path $outLog) {
        $out = Get-Content $outLog -Raw
        if ($out) { Write-Host "  $out" }
    }
}
