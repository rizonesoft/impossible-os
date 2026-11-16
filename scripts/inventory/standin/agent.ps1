# Stand-in agent: runs at ref's logon on the stand-in machine (the reference disk
# booted under QEMU with stand-in devices). Waits for a removable volume holding
# ios-job\job.ps1, runs it in this interactive session with its output in
# ios-job\out, writes ios-job\done.txt with the exit code, then shuts down so the
# host reads the result. Under Hyper-V (never a job volume) it exits at once;
# under QEMU with no job after 10 minutes it shuts down.
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
# Under QEMU a missing job is a failed run: shut down now, so run.py reports it
# at once instead of waiting out its timeout.
if (-not $job) { Stop-Computer -Force; exit 0 }
# A cold sign-in leaves no window with focus, where clean's desktop has it; Win+D
# gives it to the desktop, so the job starts from clean's state.
Add-Type -Namespace Ios -Name Keys -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);'
$KEYUP = 2
[Ios.Keys]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero)
[Ios.Keys]::keybd_event(0x44, 0, 0, [UIntPtr]::Zero)
[Ios.Keys]::keybd_event(0x44, 0, $KEYUP, [UIntPtr]::Zero)
[Ios.Keys]::keybd_event(0x5B, 0, $KEYUP, [UIntPtr]::Zero)
Start-Sleep -Seconds 2
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
