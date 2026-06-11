<#
.SYNOPSIS
  Windows PowerShell peer of scripts/release/to-vhdx.sh.

.DESCRIPTION
  Converts a raw release disk image to a Hyper-V dynamic VHDX. Mirrors
  the bash peer's arg surface (`--in` / `--out` / `--block-size` /
  `--no-verify`) and self-verifying contract (post-convert `qemu-img
  info` + `qemu-img compare`).

  Tool path: qemu-img.exe is the byte-content parity producer (same
  `convert -f raw -O vhdx -o subformat=dynamic,block_size=N` invocation
  the bash peer uses). Hyper-V module presence is reported in stderr
  for environment context only -- raw->VHDX via `Convert-VHD` requires
  VHD input, not raw, and `New-VHD -SourceDisk` accepts only physical
  disks. Either path would produce different bytes than the Linux peer
  even on the same input.

  Schema: docs/release/boot-artifact-manifest.md (vm_image_metadata)
  Bash peer: scripts/release/to-vhdx.sh

.NOTES
  PowerShell 5.1+ compatible. Requires qemu-img.exe on PATH (install
  via the qemu-tools package or `choco install qemu-img`). Without
  qemu-img.exe the script fails closed with [ERROR]; there is no
  silent degradation to a non-parity tool.

  Exit codes (mirror bash peer):
    0  VHDX produced, info OK, byte-content compares equal
    1  conversion or verification failed
    2  usage / missing tool / invalid block_size
#>


$ErrorActionPreference = 'Stop'

# $args bypass the PS parameter binder so `--Out` etc. can't trigger
# the "advanced function -> common parameter" auto-prefix collision.
$Rest = @($args | ForEach-Object { [string] $_ })

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot  = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir '..\..'))
Set-Location $RepoRoot

function Write-Err  { param([string] $M) [Console]::Error.WriteLine("[ERROR] $M") }
function Write-Note { param([string] $M) [Console]::Error.WriteLine("[to-vhdx] $M") }

function Show-Usage {
    [Console]::Error.WriteLine(@"
Usage: to-vhdx.ps1 [--In PATH] [--Out PATH] [--BlockSize BYTES] [--NoVerify]

Options:
  --In PATH         Input raw image (default: build/release/disk.img)
  --Out PATH        Output VHDX (default: build/release/disk.vhdx)
  --BlockSize N     VHDX block size in bytes (default: 4194304 = 4 MiB).
                    Legal values: power-of-two from 1 MiB to 256 MiB.
  --NoVerify        Skip post-convert qemu-img info + compare.
"@)
}

# ---- arg parse --------------------------------------------------------------

$InImg     = 'build/release/disk.img'
$OutImg    = 'build/release/disk.vhdx'
$BlockSize = 4194304
$Verify    = $true

$i = 0
while ($i -lt $Rest.Count) {
    $a = $Rest[$i]
    switch -Exact ($a) {
        '--In'         { $InImg = $Rest[$i + 1]; $i += 2; continue }
        '--in'         { $InImg = $Rest[$i + 1]; $i += 2; continue }
        '--Out'        { $OutImg = $Rest[$i + 1]; $i += 2; continue }
        '--out'        { $OutImg = $Rest[$i + 1]; $i += 2; continue }
        '--BlockSize'  { $BlockSize = $Rest[$i + 1]; $i += 2; continue }
        '--block-size' { $BlockSize = $Rest[$i + 1]; $i += 2; continue }
        '--NoVerify'   { $Verify = $false; $i += 1; continue }
        '--no-verify'  { $Verify = $false; $i += 1; continue }
        '-h'           { Show-Usage; exit 0 }
        '--help'       { Show-Usage; exit 0 }
        default        { Write-Err "unknown arg: $a"; Show-Usage; exit 2 }
    }
}

# ---- preflight --------------------------------------------------------------

if (-not (Test-Path -LiteralPath $InImg -PathType Leaf)) {
    Write-Err "missing input: $InImg (run scripts/release/build-image.sh)"
    exit 1
}

$qemuImg = Get-Command qemu-img.exe -ErrorAction SilentlyContinue
if (-not $qemuImg) { $qemuImg = Get-Command qemu-img -ErrorAction SilentlyContinue }
if (-not $qemuImg) {
    # Fallback: standard Windows install paths. The QEMU MSI / official
    # installer drops qemu-img.exe in `<ProgramFiles>\qemu\` by default
    # without prepending it to PATH. Don't fail-closed on a tool that
    # exists at a well-known install location -- the cost is one
    # Test-Path per fallback.
    $qemuFallbacks = @()
    foreach ($pfx in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramData)) {
        if ($pfx) {
            $qemuFallbacks += (Join-Path $pfx 'qemu\qemu-img.exe')
            $qemuFallbacks += (Join-Path $pfx 'chocolatey\bin\qemu-img.exe')
        }
    }
    foreach ($cand in $qemuFallbacks) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
            $qemuImg = [pscustomobject] @{ Path = $cand }
            break
        }
    }
}
if (-not $qemuImg) {
    Write-Err "missing tool: qemu-img.exe (install qemu-tools, 'choco install qemu-img', or place it on PATH)"
    exit 2
}

# String-then-cast so `--BlockSize foo` exits 2 with the canonical
# [ERROR] grammar instead of raising a PS conversion exception. Codex
# consistency dispatch flagged inline [int64] cast as a Windows/Linux
# arg-surface parity break (bash peer at to-vhdx.sh validates first).
if (([string]$BlockSize) -notmatch '^\d+$') {
    Write-Err "invalid --BlockSize: $BlockSize (must be a power-of-two from 1048576 to 268435456)"
    exit 2
}
$BlockSize = [int64] $BlockSize
$legalBlockSizes = @(1048576, 2097152, 4194304, 8388608, 16777216, 33554432, 67108864, 134217728, 268435456)
if ($legalBlockSizes -notcontains $BlockSize) {
    Write-Err "invalid --BlockSize: $BlockSize (must be a power-of-two from 1048576 to 268435456)"
    exit 2
}

$inCanon = (Resolve-Path -LiteralPath $InImg).ProviderPath
$outDir  = Split-Path -Parent $OutImg
if (-not $outDir) { $outDir = '.' }
if (-not (Test-Path -LiteralPath $outDir -PathType Container)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$outDirCanon = (Resolve-Path -LiteralPath $outDir).ProviderPath
$outCanon    = Join-Path $outDirCanon (Split-Path -Leaf $OutImg)
if ($inCanon -eq $outCanon) {
    Write-Err "--In and --Out resolve to the same canonical path ($inCanon); refusing to overwrite the source"
    exit 2
}

$hvAvail = $null -ne (Get-Module -ListAvailable -Name Hyper-V -ErrorAction SilentlyContinue)
Write-Note ("Hyper-V module {0}; using qemu-img.exe for byte-parity with the Linux peer" -f $(if ($hvAvail) { 'present' } else { 'absent' }))

if (Test-Path -LiteralPath $OutImg) { Remove-Item -LiteralPath $OutImg -Force }

# ---- UNC staging ------------------------------------------------------------
# Windows qemu-img.exe cannot read or write through `\\wsl.localhost\` (or
# `\\wsl$\`) UNC paths -- WSL's 9p file server returns a non-power-of-two
# `request_alignment`, which trips bdrv_open_driver's assertion in qemu's
# block layer regardless of the convert/info/compare subcommand. The
# .bat aggregator's `pushd "%~dp0"` UNC-to-drive auto-mapping doesn't help
# here because the mapped drive still routes through 9p.
#
# Detect a UNC input or output and stage to a local Windows temp dir so
# qemu-img only ever touches NTFS-backed files. Bytes are identical: the
# stage is a verbatim copy and the convert is the same invocation, so the
# byte-parity contract with the Linux peer (`to-vhdx.sh`) is preserved.

$isUncIn  = $inCanon  -match '^\\\\'
$isUncOut = $outCanon -match '^\\\\'
$stageDir = $null
if ($isUncIn -or $isUncOut) {
    $stageDir = Join-Path ([System.IO.Path]::GetTempPath()) ("to-vhdx-stage-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    Write-Note ("staging via {0} (input/output on UNC; qemu-img cannot operate over WSL 9p)" -f $stageDir)
}

if ($isUncIn) {
    $convertIn = Join-Path $stageDir 'in.img'
    Copy-Item -LiteralPath $inCanon -Destination $convertIn -Force
} else {
    $convertIn = $inCanon
}
if ($isUncOut) {
    $convertOut = Join-Path $stageDir ('out_' + (Split-Path -Leaf $outCanon))
} else {
    $convertOut = $outCanon
}

try {

# ---- convert ----------------------------------------------------------------

Write-Note "input  $InImg"
Write-Note ("output $OutImg (subformat=dynamic block_size={0})" -f $BlockSize)

$opts = "subformat=dynamic,block_size=$BlockSize"
& $qemuImg.Path convert -f raw -O vhdx -o $opts -- $convertIn $convertOut
if ($LASTEXITCODE -ne 0) {
    Write-Err "qemu-img convert failed (exit=$LASTEXITCODE)"
    exit 1
}

# ---- verify -----------------------------------------------------------------

if ($Verify) {
    $infoJson = & $qemuImg.Path info --output=json $convertOut
    if ($LASTEXITCODE -ne 0) {
        Write-Err "qemu-img info failed on $OutImg"
        exit 1
    }
    $info = ($infoJson -join "`n") | ConvertFrom-Json
    if ($info.format -ne 'vhdx') {
        Write-Err ("qemu-img info reports format={0}, expected vhdx" -f $info.format)
        exit 1
    }
    Write-Note ("qemu-img info OK (format={0})" -f $info.format)

    & $qemuImg.Path compare -f raw -F vhdx -- $convertIn $convertOut | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Err "qemu-img compare reports byte-content drift between $InImg and $OutImg"
        exit 1
    }
    Write-Note 'qemu-img compare OK (byte-content identical)'
}

# Move the staged VHDX back to the user-facing output path (UNC if that's
# what the caller asked for) only after convert+verify pass; on failure
# we leave $OutImg empty so retry semantics are obvious.
if ($isUncOut) {
    Move-Item -LiteralPath $convertOut -Destination $outCanon -Force
}

} finally {
    if ($stageDir -and (Test-Path -LiteralPath $stageDir)) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---- summary ----------------------------------------------------------------

$virtSize = (Get-Item -LiteralPath $InImg).Length
Write-Note ("virtual_size_bytes={0} block_size_bytes={1}" -f $virtSize, $BlockSize)

# Mirrors the bash peer's key=value summary so build-manifest.ps1 can pick
# up vm_image_metadata fields without re-running qemu-img info.
Write-Output "vm_image_format=vhdx"
Write-Output "vm_image_subformat=dynamic"
Write-Output "vm_image_block_size_bytes=$BlockSize"
Write-Output "vm_image_virtual_size_bytes=$virtSize"
Write-Output "vm_image_path=$OutImg"
exit 0
