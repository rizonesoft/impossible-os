<#
.SYNOPSIS
  Windows PowerShell peer of scripts/release/build-iso.sh.

.DESCRIPTION
  Produces a UEFI-only hybrid ISO from the release disk image. Mirrors
  the bash peer's pipeline: extract the ESP partition from the raw
  disk image, stage /IPOS/manifest.json + EFI/esp.img + placeholder
  installer/recovery directories, run xorriso with the same flags, and
  emit deterministic byte-identical output when SOURCE_DATE_EPOCH=0.

  Tool path: xorriso.exe is the byte-parity producer. ADK oscdimg.exe
  presence is reported in stderr for environment context only --
  oscdimg's ISO9660 framing differs from xorriso, so it cannot
  reproduce the same bytes from the same input even with timestamp
  pinning.

  Schema: docs/release/boot-artifact-manifest.md (vm_image_metadata,
  iso format)
  Bash peer: scripts/release/build-iso.sh

.NOTES
  PowerShell 5.1+ compatible. Requires xorriso.exe + python.exe (or
  py -3) on PATH, and build-manifest.ps1 + verify-esp.sh peers in the
  same tree. Without xorriso.exe the script fails closed with [ERROR];
  there is no silent degradation to a non-parity tool.

  Exit codes (mirror bash peer):
    0  ISO produced
    1  xorriso / ESP extract / manifest / verify-esp failure
    2  usage / missing tool
#>


param(
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $Rest
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot  = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir '..\..'))
Set-Location $RepoRoot

function Write-Err  { param([string] $M) [Console]::Error.WriteLine("[ERROR] $M") }
function Write-Note { param([string] $M) [Console]::Error.WriteLine("[to-iso] $M") }

function Show-Usage {
    [Console]::Error.WriteLine(@"
Usage: to-iso.ps1 [--In PATH] [--Out PATH]

Options:
  --In PATH    Input raw release image (default: build/release/disk.img)
  --Out PATH   Output ISO (default: build/release/disk.iso)
"@)
}

# ---- arg parse --------------------------------------------------------------

$InImg  = 'build/release/disk.img'
$OutIso = 'build/release/disk.iso'

$i = 0
while ($i -lt $Rest.Count) {
    $a = $Rest[$i]
    switch -Exact ($a) {
        '--In'    { $InImg = $Rest[$i + 1]; $i += 2; continue }
        '--in'    { $InImg = $Rest[$i + 1]; $i += 2; continue }
        '--Out'   { $OutIso = $Rest[$i + 1]; $i += 2; continue }
        '--out'   { $OutIso = $Rest[$i + 1]; $i += 2; continue }
        '-h'      { Show-Usage; exit 0 }
        '--help'  { Show-Usage; exit 0 }
        default   { Write-Err "unknown arg: $a"; Show-Usage; exit 2 }
    }
}

# ---- preflight --------------------------------------------------------------

if (-not (Test-Path -LiteralPath $InImg -PathType Leaf)) {
    Write-Err "missing input: $InImg (run scripts/release/build-image.sh)"
    exit 1
}

$xorriso = Get-Command xorriso.exe -ErrorAction SilentlyContinue
if (-not $xorriso) { $xorriso = Get-Command xorriso -ErrorAction SilentlyContinue }
if (-not $xorriso) {
    Write-Err 'missing tool: xorriso.exe. Install via Cygwin/MSYS2 or download a Windows xorriso build.'
    Write-Err 'oscdimg.exe (Windows ADK) cannot be substituted -- its ISO9660 framing differs from xorriso so byte-parity with the Linux peer is impossible.'
    exit 2
}

$buildManifest = Join-Path $RepoRoot 'scripts\release\build-manifest.ps1'
if (-not (Test-Path -LiteralPath $buildManifest)) {
    Write-Err "missing peer: $buildManifest"
    exit 2
}

# Refuse --in == --out (irreversible source-overwrite).
$inCanon = (Resolve-Path -LiteralPath $InImg).ProviderPath
$outDir  = Split-Path -Parent $OutIso
if (-not $outDir) { $outDir = '.' }
if (-not (Test-Path -LiteralPath $outDir -PathType Container)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$outDirCanon = (Resolve-Path -LiteralPath $outDir).ProviderPath
$outCanon    = Join-Path $outDirCanon (Split-Path -Leaf $OutIso)
if ($inCanon -eq $outCanon) {
    Write-Err "--In and --Out resolve to the same canonical path ($inCanon); refusing to overwrite the source"
    exit 2
}

# Environment detection (informational only).
$adkOscdimg = Get-Command oscdimg.exe -ErrorAction SilentlyContinue
Write-Note ("ADK oscdimg.exe {0}; using xorriso.exe for byte-parity with the Linux peer" -f $(if ($adkOscdimg) { 'present' } else { 'absent' }))

# Determinism epoch (mirrors bash peer's SOURCE_DATE_EPOCH=0 + TZ=UTC).
$env:SOURCE_DATE_EPOCH = '0'
$env:TZ = 'UTC'
$env:LC_ALL = 'C'

# ---- staging directory ------------------------------------------------------

$workDir = Join-Path $outDir ('to-iso-work.' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $workDir -Force | Out-Null
try {
    # Pin the source image into workDir so a concurrent rebuild cannot
    # mutate $InImg between extraction and verification (TOCTOU). On
    # NTFS we use a hardlink (atomic inode pin); cross-volume falls back
    # to a copy. Same pattern as the bash peer.
    $pinned = Join-Path $workDir 'source.img'
    $linked = $false
    try {
        cmd /c mklink /H "`"$pinned`"" "`"$InImg`"" 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $pinned)) { $linked = $true }
    } catch { }
    if (-not $linked) {
        Write-Note 'hardlink pin failed (cross-volume?); falling back to copy'
        Copy-Item -LiteralPath $InImg -Destination $pinned
    }
    $InImg = $pinned

    # ---- extract ESP from raw image ---------------------------------------
    # ESP layout from build-image.sh: LBA 2048..133119, 64 MiB.
    $espLbaFirst = 2048
    $espSectors  = 131072
    $espBytes    = [int64]$espSectors * 512
    $espImg      = Join-Path $workDir 'esp.img'

    $srcSize = (Get-Item -LiteralPath $InImg).Length
    $minSrcSize = [int64]$espLbaFirst * 512 + $espBytes
    if ($srcSize -lt $minSrcSize) {
        Write-Err ("input {0} is {1} bytes; need at least {2} bytes for ESP extraction" -f $InImg, $srcSize, $minSrcSize)
        exit 1
    }

    Write-Note ("extracting ESP from {0} (LBA {1}, {2} sectors)" -f $InImg, $espLbaFirst, $espSectors)
    $inFs = [System.IO.File]::OpenRead($InImg)
    $outFs = [System.IO.File]::Create($espImg)
    try {
        $inFs.Seek([int64]$espLbaFirst * 512, [System.IO.SeekOrigin]::Begin) | Out-Null
        $buf = New-Object byte[] (1MB)
        $remaining = $espBytes
        while ($remaining -gt 0) {
            $toRead = if ($remaining -gt $buf.Length) { $buf.Length } else { $remaining }
            $n = $inFs.Read($buf, 0, [int]$toRead)
            if ($n -le 0) { break }
            $outFs.Write($buf, 0, $n)
            $remaining -= $n
        }
    } finally {
        $inFs.Close(); $outFs.Close()
    }

    $espOutSize = (Get-Item -LiteralPath $espImg).Length
    if ($espOutSize -ne $espBytes) {
        Write-Err ("ESP extract short: got {0} bytes, expected {1}" -f $espOutSize, $espBytes)
        exit 1
    }

    # ---- stage ISO root ---------------------------------------------------
    $isoStage = Join-Path $workDir 'iso_stage'
    New-Item -ItemType Directory -Path (Join-Path $isoStage 'IPOS\installer') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $isoStage 'IPOS\recovery')  -Force | Out-Null

    $manifestPath = Join-Path $isoStage 'IPOS\manifest.json'
    Write-Note 'generating artifact manifest at /IPOS/manifest.json (--Format iso)'
    & pwsh -NoProfile -File $buildManifest 'build' '--Out' $manifestPath '--Format' 'iso' | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Err "build-manifest.ps1 build --Format iso failed (exit=$LASTEXITCODE)"
        exit 1
    }

    # Bind manifest to the ESP that will be embedded: assert each
    # entries[] file declared in the manifest hash-matches the actual
    # bytes inside esp.img. The bash peer runs verify-esp.sh; on
    # Windows we either delegate to bash if available OR run a native
    # PowerShell equivalent. Failing closed is mandatory: a stale
    # --In combined with a fresh manifest would otherwise produce an
    # ISO whose /IPOS/manifest.json describes files that aren't
    # actually inside EFI/esp.img, defeating the release-artifact
    # integrity contract. The previous version silently skipped this
    # check when bash was absent -- Codex flagged High at section ship.
    $verifyEsp = Join-Path $RepoRoot 'scripts\release\verify-esp.sh'
    $bash = Get-Command bash.exe -ErrorAction SilentlyContinue
    if ($bash -and (Test-Path -LiteralPath $verifyEsp)) {
        Write-Note 'binding /IPOS/manifest.json to ESP from input image (verify-esp.sh --manifest)'
        & $bash.Path $verifyEsp $InImg '--manifest' $manifestPath | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Err 'verify-esp.sh failed; refusing to write a manifest that does not match the embedded ESP'
            exit 1
        }
    } else {
        Write-Note 'bash not available; running native PowerShell manifest-to-ESP cross-check'
        $mtype = Get-Command mtype.exe -ErrorAction SilentlyContinue
        if (-not $mtype) { $mtype = Get-Command mtype -ErrorAction SilentlyContinue }
        if (-not $mtype) {
            Write-Err 'neither bash+verify-esp.sh nor mtype.exe is available; cannot bind manifest to ESP'
            Write-Err 'install bash (Git Bash / WSL) OR mtools (Cygwin / MSYS2 mtype.exe) to run to-iso.ps1 standalone'
            exit 1
        }
        $env:MTOOLS_SKIP_CHECK = '1'
        $manifestObj = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $entries = @($manifestObj.entries)
        $checked = 0
        foreach ($entry in $entries) {
            $name = [string] $entry.name
            $espRel = ([string] $entry.path).TrimStart('\').Replace('\', '/')
            $expected = ([string] $entry.sha256).ToLowerInvariant()
            # Spawn mtype with binary-safe stdout capture (same approach as
            # write-usb.ps1's Get-MtypeFileHash). Native PS object pipeline
            # would corrupt binary content before SHA256.
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = $mtype.Path
            $psi.UseShellExecute = $false
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError  = $true
            $psi.CreateNoWindow = $true
            $psi.ArgumentList.Add('-i')
            $psi.ArgumentList.Add($InImg + '@@1048576')
            $psi.ArgumentList.Add('::' + $espRel)
            $proc = [System.Diagnostics.Process]::Start($psi)
            $ms = New-Object System.IO.MemoryStream
            try {
                $proc.StandardOutput.BaseStream.CopyTo($ms)
                # Bounded wait: 30s cap turns a hung mtype into a clean
                # failure instead of an indefinite ISO build hang.
                if (-not $proc.WaitForExit(30000)) {
                    try { $proc.Kill() } catch { }
                    Write-Err ("mtype timed out reading /{0} from ESP for entry name={1}" -f $espRel, $name)
                    exit 1
                }
                if ($proc.ExitCode -ne 0) {
                    Write-Err ("mtype could not read /{0} from ESP for entry name={1}" -f $espRel, $name)
                    exit 1
                }
                $bytes = $ms.ToArray()
                $sha = [System.Security.Cryptography.SHA256]::Create()
                try {
                    $hash = $sha.ComputeHash($bytes)
                } finally {
                    $sha.Dispose()
                }
                $actual = (($hash | ForEach-Object { $_.ToString('x2') }) -join '').ToLowerInvariant()
                if ($actual -ne $expected) {
                    Write-Err ("manifest-to-ESP MISMATCH for entry name={0} path=/{1}" -f $name, $espRel)
                    Write-Err ("  expected (manifest): {0}" -f $expected)
                    Write-Err ("  actual   (ESP)     : {0}" -f $actual)
                    exit 1
                }
                $checked++
            } finally {
                $ms.Dispose()
                $proc.Dispose()
            }
        }
        Write-Note ("manifest-to-ESP cross-check OK ({0} entries verified)" -f $checked)
    }

    # placeholder markers so /IPOS/installer and /IPOS/recovery survive
    # ISO toolchain dropping empty directories.
    New-Item -ItemType File -Path (Join-Path $isoStage 'IPOS\installer\.placeholder') -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $isoStage 'IPOS\recovery\.placeholder')  -Force | Out-Null

    # stage ESP image inside ISO tree
    New-Item -ItemType Directory -Path (Join-Path $isoStage 'EFI') -Force | Out-Null
    Copy-Item -LiteralPath $espImg -Destination (Join-Path $isoStage 'EFI\esp.img')

    # touch -h -d "@SOURCE_DATE_EPOCH" sweep so xorriso's directory-record
    # mtimes match the determinism contract (every file + dir touched).
    # The bash peer uses `find -exec touch`. PowerShell equivalent:
    $epoch = [datetime]::SpecifyKind([datetime]::ParseExact('1970-01-01 00:00:00', 'yyyy-MM-dd HH:mm:ss', $null), [DateTimeKind]::Utc)
    Get-ChildItem -LiteralPath $isoStage -Recurse -Force | ForEach-Object {
        try {
            $_.LastWriteTimeUtc = $epoch
            $_.CreationTimeUtc  = $epoch
        } catch { }
    }
    (Get-Item -LiteralPath $isoStage).LastWriteTimeUtc = $epoch

    # ---- run xorriso ------------------------------------------------------
    Write-Note "running xorriso -> $OutIso"
    if (Test-Path -LiteralPath $OutIso) { Remove-Item -LiteralPath $OutIso -Force }

    & $xorriso.Path -as mkisofs `
        -iso-level 3 `
        -V 'IPOS_INSTALL' `
        -joliet `
        -rational-rock `
        -no-emul-boot `
        -e 'EFI/esp.img' `
        -isohybrid-gpt-basdat `
        -o $OutIso `
        $isoStage 2>&1 | ForEach-Object { [Console]::Error.WriteLine("[xorriso] $_") }
    if ($LASTEXITCODE -ne 0) {
        Write-Err "xorriso failed (exit=$LASTEXITCODE)"
        exit 1
    }

    # ---- final report ----------------------------------------------------
    $sha = (Get-FileHash -LiteralPath $OutIso -Algorithm SHA256).Hash.ToLowerInvariant()
    $sz  = (Get-Item -LiteralPath $OutIso).Length
    Write-Note ("OUTPUT {0}  size={1}  sha256={2}" -f $OutIso, $sz, $sha)

    Write-Output ("iso_path=" + $OutIso)
    Write-Output ("iso_size_bytes=" + $sz)
    Write-Output ("iso_sha256=" + $sha)
    exit 0
} finally {
    Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
}
