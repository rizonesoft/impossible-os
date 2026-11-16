# Capture fixture: puts Windows Update into a state the offline reference VM
# never reaches by itself, with no update ever delivered:
#   update-checking  the update source is a local port that accepts the
#                    connection and never answers, so the scan stays at
#                    "Checking for updates..."
#   update-failed    the update source is a local port nothing listens on, so
#                    the scan ends in "Error encountered" with Retry
# The source is set by the update policy only for the scan, then removed, so the
# page shows no "managed by your organization" line the capture would record.
# The fixture then opens Settings > Windows Update and succeeds only once the
# page shows the variant's status, so a scan that went elsewhere fails here.
# A second run stops the listener it owns and starts again, leaving the same state.
# -State prints the owned state, with the status the page shows, as one JSON
# line; it opens and closes Settings to read it and changes nothing else.
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
# How long the page may take to show the status after the scan starts.
$StatusWait = 90
# What the page shows for each variant, and every status it is read against.
$Want = @{ 'update-checking' = 'Checking for updates'; 'update-failed' = 'Error encountered' }
$Statuses = @('Checking for updates', 'Error encountered', "You're up to date", 'Updates available')

# The status line Settings > Windows Update shows, or $null when none of
# $Statuses is on screen. Reads the page through UI Automation.
function Get-PageStatus {
    Add-Type -AssemblyName UIAutomationClient
    $all = [Windows.Automation.AutomationElement]::RootElement.FindAll(
        [Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Text)))
    foreach ($e in $all) {
        $n = $e.Current.Name
        foreach ($st in $Statuses) { if ($n -like "$st*") { return $st } }
    }
    return $null
}

# Opens the page fresh, waits up to $Seconds for `$Expect` (or any status when
# $Expect is empty), closes Settings and returns what it last read.
function Read-UpdatePage([string]$Expect, [int]$Seconds) {
    Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Process 'ms-settings:windowsupdate'
    $deadline = (Get-Date).AddSeconds($Seconds)
    $seen = $null
    do {
        Start-Sleep -Seconds 2
        $seen = Get-PageStatus
    } until (($seen -and (-not $Expect -or $seen -eq $Expect)) -or (Get-Date) -gt $deadline)
    Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -Force
    return $seen
}

function Get-Listener {
    @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
        Where-Object { $_.CommandLine -like "*$Tag*" })
}

function Get-Owned {
    [ordered]@{
        variant  = if (Test-Path -LiteralPath $Marker) { (Get-Content -LiteralPath $Marker -Raw).Trim() } else { $null }
        listener = [bool](Get-Listener).Count
        policy   = Test-Path -LiteralPath $Policy
        page     = Read-UpdatePage '' 30
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
    # usoclient is a windowed program: PowerShell neither waits for it nor sets
    # $LASTEXITCODE, so its exit code is read from the process.
    $scan = Start-Process usoclient.exe -ArgumentList 'StartInteractiveScan' -Wait -PassThru -WindowStyle Hidden
    if ($scan.ExitCode -ne 0) { Write-Output "fixture services: usoclient StartInteractiveScan exited $($scan.ExitCode)"; exit 1 }
    # The scan reads the policy when it starts; wait for the page to show the
    # variant's status before the policy goes.
    $seen = Read-UpdatePage $Want[$Variant] $StatusWait
} finally {
    Remove-Item -LiteralPath $Policy -Recurse -Force
}
if ($seen -ne $Want[$Variant]) { Write-Output "fixture services: the page shows '$seen', not '$($Want[$Variant])'"; exit 1 }
# Read again with the policy gone: the state the capture opens must hold.
$seen = Read-UpdatePage $Want[$Variant] 30
if ($seen -ne $Want[$Variant]) { Write-Output "fixture services: with the policy removed the page shows '$seen', not '$($Want[$Variant])'"; exit 1 }
Set-Content -LiteralPath $Marker -Value $Variant -Encoding ASCII
Write-Output "fixture services: $Variant (the page shows '$seen')"
exit 0
