# mount-ixfs-usb.ps1 -- Detect USB drives with IXFS partitions and mount them.
#
# Must run as Administrator (raw disk access requires it).
# The WinFsp mount will be visible in Explorer windows opened from this session.
#
# Usage:
#   Right-click mount-ixfs-usb.bat -> Run as administrator
#
# Unmount: unmount-ixfs.bat

$ErrorActionPreference = "Continue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SdkDir    = Split-Path -Parent $ScriptDir
$IxfsMount = Join-Path $SdkDir "tools\ixfs-mount.exe"
$IXFS_MAGIC = 0x49584653

if (-not (Test-Path $IxfsMount)) {
    Write-Host "  ERROR" -ForegroundColor Red -NoNewline
    Write-Host " ixfs-mount.exe not found. Run: sdk\build.bat"
    exit 1
}

# -- Check admin --
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "  Requesting administrator privileges (required for raw disk access)..." -ForegroundColor Yellow
    Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`"" -Verb RunAs
    exit 0
}

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
    } catch { try { $fs.Close() } catch {} }
    return $null
}

function Get-FreeDriveLetter {
    foreach ($letter in [char[]]('I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z')) {
        $drive = "${letter}:"
        if (-not (Test-Path $drive)) { return $drive }
    }
    return $null
}

# -- Main --
Write-Host ""
Write-Host "  IXFS USB Mount" -ForegroundColor White
Write-Host ("=" * 50)
Write-Host ""

# Scan for USB drives
Write-Host "  Scanning for IXFS partitions on USB drives..." -ForegroundColor Cyan

$drives = Get-WmiObject Win32_DiskDrive | Where-Object {
    $_.InterfaceType -eq "USB" -or $_.MediaType -match "Removable"
}

if (-not $drives -or @($drives).Count -eq 0) {
    Write-Host "  No USB/removable drives found" -ForegroundColor Red
    exit 1
}

Write-Host "  Found $(@($drives).Count) removable device(s)"

$foundInfo = $null
$foundDriveNum = $null

foreach ($drive in $drives) {
    $devPath = $drive.Name
    $model = $drive.Model
    $sizeMB = [math]::Round($drive.Size / 1MB)
    Write-Host "  Scanning $devPath ($model, ${sizeMB} MB)..." -ForegroundColor DarkGray

    $result = Find-IXFSPartition -DrivePath $devPath
    if ($result) {
        $foundDriveNum = [regex]::Match($devPath, '\d+$').Value
        $foundInfo = $result
        Write-Host "  FOUND " -ForegroundColor Green -NoNewline
        Write-Host "IXFS partition $($result.Index) on PhysicalDrive$foundDriveNum"
        break
    }
}

if (-not $foundInfo) {
    Write-Host "  No IXFS partitions found on USB drives" -ForegroundColor Red
    exit 1
}

# Mount
$driveLetter = Get-FreeDriveLetter
if (-not $driveLetter) {
    Write-Host "  ERROR: No free drive letters" -ForegroundColor Red
    exit 1
}

# Ensure WinFsp DLL is findable
$winfspBin = "C:\Program Files (x86)\WinFsp\bin"
if (Test-Path $winfspBin) { $env:PATH = "$winfspBin;$env:PATH" }

$mountArgs = "$driveLetter \\.\PhysicalDrive$foundDriveNum $($foundInfo.Index)"
Write-Host ""
Write-Host "  Mounting IXFS as $driveLetter..." -ForegroundColor Cyan
Write-Host "  Command: ixfs-mount.exe $mountArgs" -ForegroundColor DarkGray

$errLog = Join-Path $env:TEMP "ixfs-mount-err.log"
$proc = Start-Process -FilePath $IxfsMount -ArgumentList $mountArgs -NoNewWindow -PassThru -RedirectStandardError $errLog

Start-Sleep -Seconds 3

if (-not $proc.HasExited) {
    if (Test-Path "$driveLetter\") {
        Write-Host ""
        Write-Host "  Mounted IXFS as $driveLetter" -ForegroundColor Green
        Write-Host ""
        Write-Host "  The drive is mounted in the admin session." -ForegroundColor DarkGray
        Write-Host "  To browse: type $driveLetter\ in this window, or open an admin Explorer." -ForegroundColor DarkGray
        Write-Host ""
        Write-Host "  Quick test:" -ForegroundColor DarkGray
        Write-Host "    dir $driveLetter\" -ForegroundColor White
        Write-Host ""
        Write-Host "  Unmount: unmount-ixfs.bat" -ForegroundColor DarkGray
        Write-Host ""
        Write-Host "  Press any key to keep the mount alive (closing this window unmounts)..."
        cmd /c "dir $driveLetter\ 2>nul"
    } else {
        Write-Host "  Process running but $driveLetter not yet visible" -ForegroundColor Yellow
        Write-Host "  Try: dir $driveLetter\" -ForegroundColor DarkGray
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
}
