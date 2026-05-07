<#
.SYNOPSIS
  Write Impossible OS to a USB flash drive (Windows host peer of
  scripts/deploy/write-usb.sh).

.DESCRIPTION
  Fail-closed USB writer. The pre-existing version of this script
  (commit c6a4dcca) had two safety regressions Codex flagged at
  ship-time:

    1. The disk filter fell back to size+removable heuristics when no
       BusType=USB disk was found, so a secondary internal SATA/NVMe
       SSD or SD card reader could appear in the selection list and be
       passed to a destructive raw write.
    2. Post-write verification only checked path existence; missing
       kernel.exe / boot.conf were WARNs and the script printed
       "Done!" regardless. A user could end up with a corrupt USB and
       a green completion message.

  This version is fail-closed:

    - **STRICT** `BusType -eq 'USB'` filter. No size/removable
      heuristics. If no USB disk is present, the script exits non-zero
      with [ERROR]; it never falls back to "any small non-system
      disk".
    - The SELECTED disk is re-queried via `Get-Disk -Number N`
      immediately before `Clear-Disk` and the raw write, and its
      BusType is re-asserted. If a USB stick was unplugged and an
      internal disk took its number between the menu and the write,
      the second check catches it. (TOCTOU defence.)
    - Post-write verification computes sha256 of every required ESP
      file (BOOTX64.EFI, kernel.exe, boot.conf) on the written drive
      and compares to the source-image hash captured BEFORE dd-style
      write. Any missing file or sha256 mismatch -> exit 1. The
      success banner is printed only after every check passes.
    - Source image hashes are captured before the write so a mid-write
      replacement of the source file cannot mask the defect.

  Exit codes:
    0  USB written and every required ESP file verified by sha256
    1  preflight, write, or verification failure (data is destroyed
       on the target if the failure occurred post-write -- the user
       should treat the drive as suspect and re-image)
    2  refusal: non-USB disk asked for, missing tool, missing source
       image
#>

#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $Rest
)

$ErrorActionPreference = 'Stop'

# ---- repo root + source image ---------------------------------------------

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..\..')
$BuildDir  = Join-Path $RepoRoot 'build'
$ReleaseImg = Join-Path $BuildDir 'release\disk.img'
$LegacyImg  = Join-Path $BuildDir 'system-disk.img'

function Write-Err  { param([string] $M) [Console]::Error.WriteLine("[ERROR] $M") }
function Write-Note { param([string] $M) [Console]::Error.WriteLine("[write-usb] $M") }

# Arg surface mirrors to-vhdx.ps1 / to-iso.ps1: --In/--in (image
# override, optional), --DiskImg/--disk-img (alias for parity with the
# old positional form). No --Out -- USB writers don't take an output
# argument; the user picks the target disk interactively.
$DiskImg = $null
$ai = 0
while ($ai -lt $Rest.Count) {
    $a = $Rest[$ai]
    switch -Exact ($a) {
        '--In'        { $DiskImg = $Rest[$ai + 1]; $ai += 2; continue }
        '--in'        { $DiskImg = $Rest[$ai + 1]; $ai += 2; continue }
        '--DiskImg'   { $DiskImg = $Rest[$ai + 1]; $ai += 2; continue }
        '--disk-img'  { $DiskImg = $Rest[$ai + 1]; $ai += 2; continue }
        '-h'          { [Console]::Error.WriteLine("Usage: write-usb.ps1 [--In PATH]`n  Default: build/release/disk.img (preferred) or build/system-disk.img"); exit 0 }
        '--help'      { [Console]::Error.WriteLine("Usage: write-usb.ps1 [--In PATH]`n  Default: build/release/disk.img (preferred) or build/system-disk.img"); exit 0 }
        default       { Write-Err "unknown arg: $a"; exit 2 }
    }
}

if (-not $DiskImg) {
    if ((Test-Path -LiteralPath $ReleaseImg) -and (Test-Path -LiteralPath $LegacyImg)) {
        $relMt = (Get-Item -LiteralPath $ReleaseImg).LastWriteTimeUtc
        $legMt = (Get-Item -LiteralPath $LegacyImg).LastWriteTimeUtc
        $DiskImg = if ($relMt -gt $legMt) { $ReleaseImg } else { $LegacyImg }
    } elseif (Test-Path -LiteralPath $ReleaseImg) {
        $DiskImg = $ReleaseImg
    } else {
        $DiskImg = $LegacyImg
    }
}

if (-not (Test-Path -LiteralPath $DiskImg -PathType Leaf)) {
    Write-Err "missing source image: $DiskImg"
    Write-Err "Build a release image: bash scripts/release/build-image.sh (in WSL)"
    Write-Err "Or the legacy system img: bash scripts/build.sh clean (in WSL)"
    exit 1
}

$ImgSize  = (Get-Item -LiteralPath $DiskImg).Length
$ImgSizeMB = [math]::Round($ImgSize / 1MB)

Write-Host 'Impossible OS USB Writer' -ForegroundColor Cyan
Write-Host '========================' -ForegroundColor Cyan
Write-Host ('  Image: {0} ({1} MB)' -f $DiskImg, $ImgSizeMB) -ForegroundColor DarkGray
Write-Host ''

# ---- pre-write source ESP hash capture ------------------------------------
# Capture must succeed before any write. Failing closed here means a missing
# mtype tool or a malformed source ESP cannot result in a destructive write
# whose verification step is later forced to be lenient. Same pattern as the
# bash peer.

$Required = @(
    'EFI/BOOT/BOOTX64.EFI',
    'boot/kernel.exe',
    'EFI/ImpossibleOS/boot.conf'
)

# Resolve mtype.exe (mtools port). Required for source-ESP hash extraction
# from the raw image. Without it we have no way to compute the expected
# hashes pre-write, so we fail closed.
$mtype = Get-Command mtype.exe -ErrorAction SilentlyContinue
if (-not $mtype) { $mtype = Get-Command mtype -ErrorAction SilentlyContinue }
if (-not $mtype) {
    Write-Err 'mtype not found on PATH. Install mtools for Windows (the Cygwin or MSYS2 mtools package ships mtype.exe).'
    Write-Err 'Without mtype the source ESP cannot be hashed pre-write; refusing to write without verifiable hashes.'
    exit 1
}

# Binary-safe mtype extraction. Native command stdout through the
# PowerShell object pipeline (`& mtype ... | Out-File`) is decoded as
# text and re-encoded -- BOOTX64.EFI and kernel.exe are binary, so the
# rewritten stream hashes differently than the file actually written
# to USB. Codex flagged the previous text-pipeline approach as High
# (post-write verification compares mismatched hashes after the target
# is already wiped).
#
# Fix: spawn mtype via [System.Diagnostics.Process], redirect stdout
# to BaseStream, and CopyTo a MemoryStream. SHA256.ComputeHash on the
# raw byte buffer matches the bytes mtype actually emitted.
function Get-MtypeFileHash {
    param(
        [string] $MtypeExe,
        [string] $ImagePath,
        [string] $InsidePath
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $MtypeExe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.CreateNoWindow = $true
    $psi.ArgumentList.Add('-i')
    $psi.ArgumentList.Add(($ImagePath + '@@1048576'))
    $psi.ArgumentList.Add('::' + $InsidePath)
    $proc = [System.Diagnostics.Process]::Start($psi)
    $ms = New-Object System.IO.MemoryStream
    try {
        $proc.StandardOutput.BaseStream.CopyTo($ms)
        # Bounded wait: mtype on a single ESP file is fast; a 30s cap
        # converts a hung mtype (degraded image, broken pipe) into a
        # clean failure instead of an indefinite hang. Codex flagged the
        # unbounded WaitForExit in post-commit review.
        if (-not $proc.WaitForExit(30000)) {
            try { $proc.Kill() } catch { }
            return $null
        }
        if ($proc.ExitCode -ne 0) {
            return $null
        }
        $bytes = $ms.ToArray()
        if ($bytes.Length -eq 0) { return $null }
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $hash = $sha.ComputeHash($bytes)
        } finally {
            $sha.Dispose()
        }
        return (($hash | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally {
        $ms.Dispose()
        $proc.Dispose()
    }
}

$ExpectedSha = @{}
$env:MTOOLS_SKIP_CHECK = '1'
foreach ($p in $Required) {
    $h = Get-MtypeFileHash -MtypeExe $mtype.Path -ImagePath $DiskImg -InsidePath $p
    if (-not $h) {
        Write-Err "could not derive source-image sha256 for $p"
        Write-Err "the source image at $DiskImg is missing a required ESP file or uses a different layout"
        exit 1
    }
    $ExpectedSha[$p] = $h
}
Write-Note ('captured source ESP hashes for {0} files' -f $Required.Count)

# ---- list disks (STRICT BusType=USB filter) -------------------------------
# No size/removable fallback. The pre-existing script accepted any disk
# under 256GB that wasn't system or boot, which can include internal SATA
# SSDs and SD card readers. Strict BusType=USB is the only safe filter
# here; Codex flagged the permissive fallback as Critical (irreversible
# data loss path) at section-ship time.

$UsbDisks = @(Get-Disk | Where-Object { $_.BusType -eq 'USB' })
if ($UsbDisks.Count -eq 0) {
    Write-Err 'no USB disks found (BusType=USB). Insert a USB flash drive and try again.'
    Write-Host ''
    Write-Host 'All disks detected by Windows:' -ForegroundColor DarkGray
    Get-Disk | Format-Table Number, FriendlyName, BusType, @{ L='Size GB'; E={ [math]::Round($_.Size/1GB, 1) } }, PartitionStyle -AutoSize
    exit 1
}

Write-Host 'Available USB drives:' -ForegroundColor Green
Write-Host ''
for ($i = 0; $i -lt $UsbDisks.Count; $i++) {
    $d = $UsbDisks[$i]
    $szMB = [math]::Round($d.Size / 1MB)
    $szGB = [math]::Round($d.Size / 1GB, 1)
    $tag  = if ($szMB -lt $ImgSizeMB) { ' [TOO SMALL]' } else { '' }
    Write-Host ("  [{0}] Disk {1}: {2}" -f ($i + 1), $d.Number, $d.FriendlyName) -ForegroundColor White
    Write-Host ("      Size: {0} GB ({1} MB)  Partitions: {2}{3}" -f $szGB, $szMB, $d.NumberOfPartitions, $tag) -ForegroundColor DarkGray
}
Write-Host ''

$selection = Read-Host ("Select drive [1-{0}]" -f $UsbDisks.Count)
$idx = [int]$selection - 1
if ($idx -lt 0 -or $idx -ge $UsbDisks.Count) {
    Write-Err 'invalid selection'
    exit 1
}
$Target = $UsbDisks[$idx]
$DiskNum = $Target.Number
$DiskName = $Target.FriendlyName
$DiskSizeMB = [math]::Round($Target.Size / 1MB)

if ($DiskSizeMB -lt $ImgSizeMB) {
    Write-Err ("drive too small ({0} MB, need {1} MB)" -f $DiskSizeMB, $ImgSizeMB)
    exit 1
}

# ---- double confirmation --------------------------------------------------

Write-Host ''
Write-Host 'WARNING: ALL DATA ON THIS DRIVE WILL BE DESTROYED!' -ForegroundColor Red
Write-Host ''
Write-Host ("  Target: Disk {0} - {1} ({2} MB)" -f $DiskNum, $DiskName, $DiskSizeMB) -ForegroundColor Yellow
Write-Host ("  Image:  {0} MB" -f $ImgSizeMB) -ForegroundColor DarkGray
Write-Host ''

$c1 = Read-Host "Type 'YES' to continue"
if ($c1 -ne 'YES') {
    Write-Note 'cancelled'
    exit 0
}
$c2 = Read-Host ("Type the disk number ({0}) to confirm" -f $DiskNum)
if ($c2 -ne "$DiskNum") {
    Write-Note 'cancelled - disk number mismatch'
    exit 0
}

# ---- TOCTOU re-check: re-query the selected disk RIGHT BEFORE the write ---
# Defense against a USB stick being unplugged between the menu and the
# write, with an internal disk taking its disk number. Re-verify BusType
# and FriendlyName match what we showed the user. If anything drifted,
# exit non-zero before touching the device.

$Recheck = Get-Disk -Number $DiskNum -ErrorAction SilentlyContinue
if (-not $Recheck) {
    Write-Err ("disk {0} no longer present at write time" -f $DiskNum)
    exit 1
}
if ($Recheck.BusType -ne 'USB') {
    Write-Err ("disk {0} is BusType={1}; only BusType=USB is allowed" -f $DiskNum, $Recheck.BusType)
    exit 1
}
if ($Recheck.FriendlyName -ne $DiskName) {
    Write-Err ("disk {0} FriendlyName changed from '{1}' to '{2}' between menu and write; aborting" -f $DiskNum, $DiskName, $Recheck.FriendlyName)
    exit 1
}

# ---- write ----------------------------------------------------------------

Write-Host ''
Write-Host 'Writing Impossible OS to USB...' -ForegroundColor Cyan

Write-Host ('  [1/4] Cleaning disk {0}...' -f $DiskNum) -ForegroundColor DarkGray
Set-Disk -Number $DiskNum -IsOffline $false -ErrorAction SilentlyContinue
Clear-Disk -Number $DiskNum -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue

Write-Host ('  [2/4] Writing {0} MB image (this may take a minute)...' -f $ImgSizeMB) -ForegroundColor DarkGray

$physPath = "\\.\PhysicalDrive$DiskNum"
$diskStream = $null
$imgStream  = $null
try {
    $diskStream = [System.IO.File]::Open($physPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $imgStream  = [System.IO.File]::OpenRead($DiskImg)

    $buffer = New-Object byte[] (1MB)
    $totalWritten = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (($n = $imgStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $diskStream.Write($buffer, 0, $n)
        $totalWritten += $n
        $pct = [math]::Round(($totalWritten / $ImgSize) * 100)
        $writtenMB = [math]::Round($totalWritten / 1MB)
        $elapsed = $sw.Elapsed.TotalSeconds
        $speed = if ($elapsed -gt 0) { [math]::Round(($totalWritten / 1MB) / $elapsed, 1) } else { 0 }
        Write-Host ("`r        {0}%  {1}/{2} MB  {3} MB/s  " -f $pct, $writtenMB, $ImgSizeMB, $speed) -NoNewline -ForegroundColor DarkCyan
    }
    $diskStream.Flush()
    Write-Host ''
    $sw.Stop()
    Write-Host ('  [3/4] Write complete - {0:N1}s, {1:N1} MB/s' -f $sw.Elapsed.TotalSeconds, ($ImgSizeMB / $sw.Elapsed.TotalSeconds)) -ForegroundColor Green
} catch {
    Write-Host ''
    Write-Err ("write failed: {0}" -f $_.Exception.Message)
    Write-Err 'Make sure no other program is using the USB drive.'
    exit 1
} finally {
    if ($imgStream)  { $imgStream.Close() }
    if ($diskStream) { $diskStream.Close() }
}

Write-Host ('  [4/4] Refreshing partition table...') -ForegroundColor DarkGray
Start-Sleep -Seconds 1
Set-Disk -Number $DiskNum -IsOffline $true  -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Set-Disk -Number $DiskNum -IsOffline $false -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# ---- verify (FAIL-CLOSED per-file sha256) ---------------------------------

Write-Host ''
Write-Host 'Verifying USB boot files...' -ForegroundColor Cyan

$VerifyFailures = 0

$espPart = Get-Partition -DiskNumber $DiskNum -ErrorAction SilentlyContinue |
    Where-Object { $_.Type -eq 'System' -or $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } |
    Select-Object -First 1

if (-not $espPart) {
    Write-Err 'EFI System Partition not detected on written disk'
    exit 1
}

$driveLetter = $espPart.DriveLetter
$tempAssigned = $false
if (-not $driveLetter) {
    $usedLetters = (Get-Volume).DriveLetter | Where-Object { $_ }
    $candidate = ([char[]](90..69)) | Where-Object { $_ -notin $usedLetters } | Select-Object -First 1
    if ($candidate) {
        $espPart | Set-Partition -NewDriveLetter $candidate -ErrorAction SilentlyContinue
        $driveLetter = $candidate
        $tempAssigned = $true
        Start-Sleep -Milliseconds 500
    }
}

if (-not $driveLetter) {
    Write-Err 'could not assign drive letter for verification'
    exit 1
}

try {
    foreach ($p in $Required) {
        $usbPath = "${driveLetter}:\$($p -replace '/', '\')"
        if (-not (Test-Path -LiteralPath $usbPath)) {
            Write-Err ("{0} not present on USB" -f $p)
            $VerifyFailures++
            continue
        }
        $usbHash = (Get-FileHash -LiteralPath $usbPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $expected = $ExpectedSha[$p]
        $usbKB = [math]::Round((Get-Item -LiteralPath $usbPath).Length / 1KB)
        if ($usbHash -ne $expected) {
            Write-Err ("{0} ({1} KB) sha256 MISMATCH" -f $p, $usbKB)
            Write-Err ("  expected: {0}" -f $expected)
            Write-Err ("  got:      {0}" -f $usbHash)
            $VerifyFailures++
        } else {
            Write-Host ("  [OK] {0} ({1} KB) sha256 matches source" -f $p, $usbKB) -ForegroundColor Green
        }
    }
} finally {
    if ($tempAssigned) {
        $espPart | Remove-PartitionAccessPath -AccessPath "${driveLetter}:\" -ErrorAction SilentlyContinue
    }
}

if ($VerifyFailures -gt 0) {
    Write-Host ''
    Write-Err ("USB write verification FAILED ({0} issue(s))." -f $VerifyFailures)
    Write-Err 'The image was written but post-write read-back did not match.'
    Write-Err 'Re-run after replacing the USB drive or re-imaging.'
    exit 1
}

Write-Host ''
Write-Host '==================================================' -ForegroundColor Green
Write-Host '  Done! USB drive is ready to boot.' -ForegroundColor Green
Write-Host '==================================================' -ForegroundColor Green
Write-Host ''
Write-Host 'To boot:' -ForegroundColor Cyan
Write-Host '  1. Insert USB into target machine'  -ForegroundColor DarkGray
Write-Host '  2. Enter BIOS/UEFI boot menu (F12, F2, or Del)' -ForegroundColor DarkGray
Write-Host '  3. Select the USB drive (UEFI mode)' -ForegroundColor DarkGray
Write-Host '  4. Impossible OS should boot.' -ForegroundColor DarkGray
Write-Host ''
exit 0
