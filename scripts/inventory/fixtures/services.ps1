# Capture fixture: puts Windows Update into a state the offline reference VM
# never reaches by itself, with no update ever delivered:
#   update-checking  the update source is a local port that accepts the
#                    connection and never answers, so the scan stays at
#                    "Checking for updates..."
#   update-failed    the update source is a local port nothing listens on, so
#                    the scan ends in "Error encountered" with Retry
# The source is set by the update policy only for the scan, then removed, so the
# page shows no "managed by your organization" line the capture would record.
# Settings is restarted afterwards, so the page opens on the new state.
# A second run stops the listener it owns and starts again, leaving the same state.
# -State prints the owned state as one JSON line and changes nothing.
# Runs elevated in ref's interactive session (capture.py --prepare, fixtures/run.py).
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 23.
# Variants: update-checking, update-failed
param(
    [ValidateSet('update-checking', 'update-failed')][string]$Variant = 'update-checking',
    [switch]$State
)
$ErrorActionPreference = 'Stop'

$Policy = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'
$Port = 8530
$Source = "http://127.0.0.1:$Port"
# Names the listener process, so a later run finds the one it owns.
$Tag = 'IosSampleUpdateSource'
$Marker = 'C:\ios\fixture-services.txt'
$ScanWait = 40

function Get-Listener {
    @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
        Where-Object { $_.CommandLine -like "*$Tag*" })
}

function Get-Owned {
    [ordered]@{
        variant  = if (Test-Path -LiteralPath $Marker) { (Get-Content -LiteralPath $Marker -Raw).Trim() } else { $null }
        listener = [bool](Get-Listener).Count
        policy   = Test-Path -LiteralPath $Policy
    }
}

if ($State) { (Get-Owned) | ConvertTo-Json -Compress; exit 0 }

foreach ($p in Get-Listener) { Stop-Process -Id $p.ProcessId -Force }
Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path -LiteralPath $Policy) { Remove-Item -LiteralPath $Policy -Recurse -Force }

if ($Variant -eq 'update-checking') {
    # Accepts every connection and holds it open, answering nothing.
    $body = "`$null = '$Tag'; `$l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port); `$l.Start(); `$held = New-Object System.Collections.ArrayList; while (`$true) { [void]`$held.Add(`$l.AcceptTcpClient()) }"
    Start-Process powershell.exe -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $body
    $deadline = (Get-Date).AddSeconds(20)
    while (-not (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)) {
        if ((Get-Date) -gt $deadline) { Write-Output "fixture services: the listener on $Port never started"; exit 1 }
        Start-Sleep -Milliseconds 500
    }
}

New-Item -Path "$Policy\AU" -Force | Out-Null
Set-ItemProperty -LiteralPath $Policy -Name WUServer -Value $Source
Set-ItemProperty -LiteralPath $Policy -Name WUStatusServer -Value $Source
Set-ItemProperty -LiteralPath "$Policy\AU" -Name UseWUServer -Value 1 -Type DWord
try {
    & usoclient.exe StartInteractiveScan
    Start-Sleep -Seconds $ScanWait
} finally {
    Remove-Item -LiteralPath $Policy -Recurse -Force
}
Set-Content -LiteralPath $Marker -Value $Variant -Encoding ASCII
Write-Output "fixture services: $Variant"
exit 0
