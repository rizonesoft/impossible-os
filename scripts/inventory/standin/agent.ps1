# Stand-in agent: runs at ref's logon on the stand-in machine (the reference disk
# booted under QEMU with stand-in devices). Waits for a removable volume holding
# ios-job\job.ps1, runs it in this interactive session with its output in
# ios-job\out, writes ios-job\done.txt with the exit code, then shuts down so the
# host reads the result. With no job volume (the disk under Hyper-V) it exits.
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 21.
$ErrorActionPreference = 'Stop'
$deadline = (Get-Date).AddMinutes(10)
$job = $null
while (-not $job -and (Get-Date) -lt $deadline) {
    foreach ($v in @(Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemLabel -eq 'IOSJOB' })) {
        $p = "$($v.DriveLetter):\ios-job\job.ps1"
        if (Test-Path -LiteralPath $p) { $job = $p }
    }
    if (-not $job) {
        # Under Hyper-V there is never a job volume; give up early there.
        if ((Get-CimInstance Win32_ComputerSystem).Manufacturer -match 'Microsoft') { exit 0 }
        Start-Sleep 5
    }
}
if (-not $job) { exit 0 }
$root = Split-Path -Parent $job
$out = Join-Path $root 'out'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$rc = 1
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $job -Out $out *> (Join-Path $out 'job.log')
    $rc = $LASTEXITCODE
} finally {
    Set-Content -LiteralPath (Join-Path $root 'done.txt') -Value "$rc" -Encoding ASCII
    Write-VolumeCache -DriveLetter $root[0]
    Stop-Computer -Force
}
