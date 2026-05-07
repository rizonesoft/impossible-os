<#
.SYNOPSIS
  Host-side tests for build-manifest.ps1 -- PowerShell peer of
  scripts/release/test-build-manifest.sh.

.DESCRIPTION
  Mirrors the 23 assertions in the bash peer one-for-one. The two test
  files run independently per host; cross-host BYTE parity of the
  manifest output is enforced by a separate harness:
    tools/bootimg/tests/cross_host/test_manifest_parity.sh

  This file only verifies that the .ps1 peer of build-manifest implements
  the same field-set, validator, and determinism contract as the bash
  peer. JSON mutation in negative tests is done with PowerShell's native
  ConvertFrom-Json / ConvertTo-Json (fixture-only -- the validator parses
  these too, so byte-identity is not required for fixtures).

  Exit code: 0 when all 23 assertions pass; non-zero otherwise.
#>


param()

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $PSCommandPath
$RepoRoot  = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir '..\..'))
Set-Location $RepoRoot

$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("build-manifest-ps-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $TmpDir -Force | Out-Null

$Script:Pass = 0
$Script:Fail = 0

function Note { param([string] $M) Write-Output $M }
function Ok   { param([string] $M) $Script:Pass++; Write-Output ("  [PASS] " + $M) }
function Bad  { param([string] $M) $Script:Fail++; Write-Output ("  [FAIL] " + $M) }

function Invoke-Manifest {
    param([string[]] $ScriptArgs)
    & pwsh -NoProfile -File (Join-Path $RepoRoot 'scripts\release\build-manifest.ps1') @ScriptArgs 2>&1
    return $LASTEXITCODE
}

function Invoke-ManifestCapture {
    # Returns @{ rc = N; out = <combined stderr+stdout> } so we can assert on
    # error text from check-mode failures.
    param([string[]] $ScriptArgs)
    $combined = & pwsh -NoProfile -File (Join-Path $RepoRoot 'scripts\release\build-manifest.ps1') @ScriptArgs 2>&1
    @{ rc = $LASTEXITCODE; out = ($combined -join "`n") }
}

function Get-ManifestObject {
    param([string] $Path)
    Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Save-MutatedManifest {
    # Write a hashtable-shaped manifest back as JSON. Fixture-only -- the
    # validator parses the same JSON we write here, so byte-identity is not
    # required.
    param([object] $Obj, [string] $Path)
    $Obj | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $Path -Encoding UTF8
}

# Cleanup at end.
try {

# ---- [1] build mode produces required fields ------------------------------

Note "[1] build mode produces required fields"
$mPath = Join-Path $TmpDir 'm.json'
$rc = Invoke-Manifest @('build', '--Out', $mPath) | Out-Null; $rc = $LASTEXITCODE
if ($rc -ne 0) { Bad "build invocation failed (rc=$rc)" }
$raw = Get-Content -LiteralPath $mPath -Raw
foreach ($f in @('bootloader_sha256', 'kernel_sha256', 'partition_map', 'secure_boot_status', 'boot_info_version', 'artifact_uuid')) {
    if ($raw -match ('"' + [regex]::Escape($f) + '"')) {
        Ok ($f + " present")
    } else {
        Bad ($f + " missing")
    }
}

# ---- [2] check mode passes on a fresh manifest ----------------------------

Note "[2] check mode passes on a fresh manifest"
$null = Invoke-Manifest @('check', $mPath) | Out-Null
if ($LASTEXITCODE -eq 0) { Ok "check exits 0" } else { Bad "check did not exit 0 (rc=$LASTEXITCODE)" }

# ---- [3] check fails when kernel_sha256 removed --------------------------

Note "[3] check mode fails with [ERROR] when kernel_sha256 removed"
$broken = Get-ManifestObject $mPath | Select-Object -Property * -ExcludeProperty kernel_sha256
$brokenPath = "$mPath.broken"
Save-MutatedManifest -Obj $broken -Path $brokenPath
$r = Invoke-ManifestCapture @('check', $brokenPath)
if ($r.rc -ne 0)            { Ok ("check exits non-zero (" + $r.rc + ") on missing kernel_sha256") } else { Bad "check should have failed" }
if ($r.out -match '\[ERROR\].*kernel_sha256') { Ok "stderr names missing field (kernel_sha256)" } else { Bad ("stderr did not name field; got: " + $r.out) }

# ---- [4a] reject manifest_version != 1 ------------------------------------

Note "[4a] check rejects manifest_version != 1"
$m = Get-ManifestObject $mPath
$m.manifest_version = 2
$bvPath = "$mPath.bad_ver"
Save-MutatedManifest -Obj $m -Path $bvPath
$r = Invoke-ManifestCapture @('check', $bvPath)
if ($r.rc -ne 0 -and $r.out -match 'manifest_version') { Ok "rejects manifest_version=2" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4b] reject unknown artifact_format ---------------------------------

Note "[4b] check rejects unknown artifact_format"
$m = Get-ManifestObject $mPath
$m.artifact_format = 'bogus'
$bfPath = "$mPath.bad_fmt"
Save-MutatedManifest -Obj $m -Path $bfPath
$r = Invoke-ManifestCapture @('check', $bfPath)
if ($r.rc -ne 0 -and $r.out -match 'artifact_format') { Ok "rejects artifact_format=bogus" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4c] reject malformed artifact_uuid ---------------------------------

Note "[4c] check rejects malformed artifact_uuid"
$m = Get-ManifestObject $mPath
$m.artifact_uuid = 'not-a-uuid'
$buPath = "$mPath.bad_uuid"
Save-MutatedManifest -Obj $m -Path $buPath
$r = Invoke-ManifestCapture @('check', $buPath)
if ($r.rc -ne 0 -and $r.out -match 'artifact_uuid') { Ok "rejects malformed artifact_uuid" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4d] reject syntactically valid but content-mismatched UUID ---------

Note "[4d] check rejects well-formed but wrong artifact_uuid"
$m = Get-ManifestObject $mPath
$m.artifact_uuid = '00000000-0000-5000-8000-000000000000'
$buSemPath = "$mPath.bad_uuid_sem"
Save-MutatedManifest -Obj $m -Path $buSemPath
$r = Invoke-ManifestCapture @('check', $buSemPath)
if ($r.rc -ne 0 -and $r.out -match 'deterministic UUID') { Ok "rejects mismatched-content artifact_uuid" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4e] reject malformed disk_guid -------------------------------------

Note "[4e] check rejects malformed disk_guid when present"
$m = Get-ManifestObject $mPath
$m | Add-Member -NotePropertyName disk_guid -NotePropertyValue 'not-a-guid' -Force
$bdgPath = "$mPath.bad_disk_guid"
Save-MutatedManifest -Obj $m -Path $bdgPath
$r = Invoke-ManifestCapture @('check', $bdgPath)
if ($r.rc -ne 0 -and $r.out -match 'disk_guid') { Ok "rejects malformed disk_guid" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4f] reject bogus sector_size ---------------------------------------

Note "[4f] check rejects bogus sector_size when present"
$m = Get-ManifestObject $mPath
$m | Add-Member -NotePropertyName sector_size -NotePropertyValue 999 -Force
$bsPath = "$mPath.bad_sector"
Save-MutatedManifest -Obj $m -Path $bsPath
$r = Invoke-ManifestCapture @('check', $bsPath)
if ($r.rc -ne 0 -and $r.out -match 'sector_size') { Ok "rejects sector_size=999" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4g] installer format derives media_role=installer ------------------

Note "[4g] build --Format installer emits media_role=installer"
$mInstPath = Join-Path $TmpDir 'm_inst.json'
$null = Invoke-Manifest @('build', '--Format', 'installer', '--Out', $mInstPath) | Out-Null
$rawI = Get-Content -LiteralPath $mInstPath -Raw
if ($rawI -match '"media_role":\s*"installer"') { Ok "installer format derives media_role=installer" } else { Bad "installer format did not set media_role=installer" }
$null = Invoke-Manifest @('check', $mInstPath) | Out-Null
if ($LASTEXITCODE -eq 0) { Ok "installer manifest passes check" } else { Bad "installer manifest should pass check" }

# ---- [4h] reject installer artifact_format with media_role=normal --------

Note "[4h] check rejects artifact_format=installer with media_role=normal"
$mI = Get-ManifestObject $mInstPath
$mI.media_role = 'normal'
$brPath = "$mInstPath.bad_role"
Save-MutatedManifest -Obj $mI -Path $brPath
$r = Invoke-ManifestCapture @('check', $brPath)
if ($r.rc -ne 0 -and $r.out -match 'artifact_format=installer requires media_role=installer') { Ok "rejects installer/normal mismatch" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4i] stale signing stamp -> unsigned --------------------------------

Note "[4i] secure_boot_status reports unsigned on stale stamp"
$fpTmp    = Join-Path $TmpDir 'sign.fingerprint'
$stampTmp = Join-Path $TmpDir 'sign.stamp'
Set-Content -LiteralPath $fpTmp -Value 'deadbeef' -NoNewline
Set-Content -LiteralPath $stampTmp -Value '' -NoNewline
$past = (Get-Date).AddHours(-1)
(Get-Item -LiteralPath $fpTmp).LastWriteTime    = $past
(Get-Item -LiteralPath $stampTmp).LastWriteTime = $past
$mStalePath = Join-Path $TmpDir 'm_stale.json'
$prevFp    = $env:SIGN_FINGERPRINT_FILE
$prevStamp = $env:SIGN_STAMP_FILE
$env:SIGN_FINGERPRINT_FILE = $fpTmp
$env:SIGN_STAMP_FILE       = $stampTmp
try {
    $null = Invoke-Manifest @('build', '--Out', $mStalePath) | Out-Null
} finally {
    $env:SIGN_FINGERPRINT_FILE = $prevFp
    $env:SIGN_STAMP_FILE       = $prevStamp
}
$rawS = Get-Content -LiteralPath $mStalePath -Raw
if ($rawS -match '"secure_boot_status":\s*"unsigned"') { Ok "stale signing stamp -> unsigned" } else { Bad "stale stamp should produce unsigned" }

# ---- [4j] stale ABI JSON refuses build -----------------------------------

Note "[4j] build refuses stale boot-info-abi.kernel.json (older than artifacts)"
$abiTmp = Join-Path $TmpDir 'abi.json'
Copy-Item -LiteralPath 'build/boot-info-abi.kernel.json' -Destination $abiTmp
(Get-Item -LiteralPath $abiTmp).LastWriteTime = (Get-Date).AddHours(-1)
$mStaleAbiPath = Join-Path $TmpDir 'm_stale_abi.json'
$prevAbi = $env:BOOT_INFO_ABI_FILE
$env:BOOT_INFO_ABI_FILE = $abiTmp
try {
    $r = Invoke-ManifestCapture @('build', '--Out', $mStaleAbiPath)
} finally {
    $env:BOOT_INFO_ABI_FILE = $prevAbi
}
if ($r.rc -ne 0 -and $r.out -match 'older than build artifacts') { Ok "stale ABI JSON refuses build" } else { Bad ("stale ABI JSON should fail; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4k] additive optional entry passes ---------------------------------

Note "[4k] check accepts additive optional entry name (v1 forward-compat)"
$m = Get-ManifestObject $mPath
$entries = @($m.entries) + @([pscustomobject] @{
    name       = 'future_payload'
    path       = '\IPOS\future.bin'
    sha256     = ('0' * 64)
    size_bytes = 4096
    optional   = $true
})
$m.entries = $entries
$addPath = "$mPath.additive"
Save-MutatedManifest -Obj $m -Path $addPath
$null = Invoke-Manifest @('check', $addPath) | Out-Null
if ($LASTEXITCODE -eq 0) { Ok "additive optional entry passes check" } else { Bad "additive optional entry should pass; check rejected it" }

# ---- [4l] reject unknown non-optional entry name -------------------------

Note "[4l] check rejects unknown entry name when optional=false"
$m = Get-ManifestObject $mPath
$entries = @($m.entries) + @([pscustomobject] @{
    name       = 'future_payload'
    path       = '\IPOS\future.bin'
    sha256     = ('0' * 64)
    size_bytes = 4096
    optional   = $false
})
$m.entries = $entries
$badAddPath = "$mPath.bad_additive"
Save-MutatedManifest -Obj $m -Path $badAddPath
$r = Invoke-ManifestCapture @('check', $badAddPath)
if ($r.rc -ne 0 -and $r.out -match 'must be one of') { Ok "rejects unknown non-optional entry name" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4m] reject size_mib above 2^32-1 (parity with bash bound) ----------

Note "[4m] check rejects partition_map[].size_mib > 2^32-1"
$m = Get-ManifestObject $mPath
$m.partition_map[0].size_mib = 4294967296
$bszPath = "$mPath.bad_size_mib"
Save-MutatedManifest -Obj $m -Path $bszPath
$r = Invoke-ManifestCapture @('check', $bszPath)
if ($r.rc -ne 0 -and $r.out -match 'size_mib') { Ok "rejects size_mib=4294967296" } else { Bad ("should have rejected; rc=" + $r.rc + ", err: " + $r.out) }

# ---- [4n] accept uint64-sized total_sectors (parity with Python int) -----

Note "[4n] check accepts total_sectors at and above Int64.MaxValue (uint64 parity)"
foreach ($val in @('9223372036854775808', '18446744073709551615')) {
    $m = Get-ManifestObject $mPath
    $m | Add-Member -NotePropertyName total_sectors -NotePropertyValue $val -Force
    $bigPath = "$mPath.big_$val"
    Save-MutatedManifest -Obj $m -Path $bigPath
    $r = Invoke-ManifestCapture @('check', $bigPath)
    if ($r.rc -eq 0) { Ok ("accepts total_sectors=$val") } else { Bad ("should have accepted total_sectors=$val; rc=" + $r.rc + ", err: " + $r.out) }
}

# ---- [5] deterministic build (byte-identical across runs) ----------------

Note "[5] deterministic build (artifact_uuid is reproducibility-friendly)"
$m2Path = Join-Path $TmpDir 'm2.json'
$null = Invoke-Manifest @('build', '--Out', $m2Path) | Out-Null
$h1 = (Get-FileHash -LiteralPath $mPath  -Algorithm SHA256).Hash
$h2 = (Get-FileHash -LiteralPath $m2Path -Algorithm SHA256).Hash
if ($h1 -eq $h2) { Ok "two consecutive builds are byte-identical" } else { Bad "consecutive builds differ; UUID is not deterministic" }

} finally {
    Remove-Item -Recurse -Force -LiteralPath $TmpDir -ErrorAction SilentlyContinue
}

Write-Output ""
Write-Output ("[summary] {0} pass, {1} fail" -f $Script:Pass, $Script:Fail)
if ($Script:Fail -ne 0) { exit 1 }
exit 0
