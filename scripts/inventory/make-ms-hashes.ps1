<#
.SYNOPSIS
Makes scripts\inventory\ms-hashes.txt from the Windows 11 reference VM.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 4.

Runs guest\hash-assets.ps1 on a clean restore of the reference VM through
ref-run.ps1 and copies its list over ms-hashes.txt. Run it again when the
reference build changes (REFERENCE.md "What is the rule for a new build?") or
asset-types.txt grows; commit the result, so check-no-ms-assets.py runs in CI
with no VM.

    powershell.exe -File scripts\inventory\make-ms-hashes.ps1
#>
[CmdletBinding()]
param([string]$Out = 'R:\11\out-hashes')
$ErrorActionPreference = 'Stop'
$stage = Join-Path ([IO.Path]::GetTempPath()) ("ios-hash-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
# No exit inside the try: cmdlets in a finally entered by exit are skipped.
$rc = 1
try {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'guest\hash-assets.ps1') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'asset-types.txt') -Destination $stage
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'ref-run.ps1') `
        -In $stage -Out $Out -TimeoutSec 3600 `
        -Command 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ios\in\hash-assets.ps1'
    $list = Join-Path $Out 'ms-hashes.txt'
    if ($LASTEXITCODE -ne 0) {
        Write-Host "make-ms-hashes: ref-run exited $LASTEXITCODE"; $rc = $LASTEXITCODE
    } elseif (-not (Test-Path -LiteralPath $list)) {
        Write-Host 'make-ms-hashes: the guest wrote no ms-hashes.txt'
    } else {
        Copy-Item -LiteralPath $list -Destination (Join-Path $PSScriptRoot 'ms-hashes.txt') -Force
        Write-Host "make-ms-hashes: wrote $(Join-Path $PSScriptRoot 'ms-hashes.txt')"
        $rc = 0
    }
} finally {
    try { Remove-Item -LiteralPath $stage -Recurse -Force } catch { Write-Host "make-ms-hashes: left $stage behind: $($_.Exception.Message)" }
}
exit $rc
