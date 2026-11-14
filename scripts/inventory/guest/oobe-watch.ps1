# Runs as SYSTEM at each boot while setup is unfinished. It reports the first
# setup page to the host through Hyper-V key-value exchange, and completes setup
# when the host asks: the host sets ios-oobe-go in the External pool and the
# watcher runs oobe.cmd from the OOBE answer media. No keyboard input is used:
# keys typed into a prompt over the setup page land in the page instead (seen
# 2026-11-14). Installed by ios-setup.cmd in the specialize pass; oobe.cmd
# removes it. Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.
$out = 'HKLM:\SOFTWARE\Microsoft\Virtual Machine\Guest'      # guest to host
$in = 'HKLM:\SOFTWARE\Microsoft\Virtual Machine\External'    # host to guest
function Put([string]$Name, [string]$Value) {
    if (-not (Test-Path $out)) { New-Item -Path $out -Force | Out-Null }
    Set-ItemProperty -Path $out -Name $Name -Value $Value
}
Put 'ios-boot' (Get-Date -Format o)
$since = $null
# No deadline: a restore of the pre-oobe checkpoint resumes this loop, possibly
# days later and with the clock moved.
while ($true) {
    # The setup page is up when one of its hosts runs. In this sequence no desktop
    # can exist before the host sends ios-oobe-go, and the host also requires two
    # matching thumbnails. Registry and shell signals were tried and do not mark
    # the page on build 26300 (seen 2026-11-14: msoobe.exe absent, OOBEInProgress
    # 0, ImageState IMAGE_STATE_COMPLETE, explorer.exe running); they are still
    # reported so a later build that changes them is visible.
    $image = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Setup\State').ImageState
    $setup = Get-ItemProperty -Path 'HKLM:\SYSTEM\Setup'
    $shell = [bool](Get-Process -Name explorer -ErrorAction SilentlyContinue)  # fail-direction: evidence only, decides nothing
    Put 'ios-setup-state' ("$image oobe=$($setup.OOBEInProgress) system=$($setup.SystemSetupInProgress) explorer=$shell")
    $hosts = @(Get-Process -Name msoobe, WWAHost, CloudExperienceHostBroker -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } | Sort-Object -Unique)  # fail-direction: none running means no page on screen
    Put 'ios-oobe-hosts' ($hosts -join ',')
    if ($hosts.Count) {
        if (-not $since) { $since = Get-Date }
        if (((Get-Date) - $since).TotalSeconds -ge 90) { Put 'ios-oobe' 'ready' } else { Put 'ios-oobe' 'waiting' }
    } else {
        $since = $null
        Put 'ios-oobe' 'waiting'
    }
    $go = $null
    if (Test-Path $in) { $go = (Get-ItemProperty -Path $in).'ios-oobe-go' }
    if ($go -eq '1') {
        foreach ($d in [IO.DriveInfo]::GetDrives()) {
            $cmd = Join-Path $d.RootDirectory.FullName 'oobe.cmd'
            if ($d.IsReady -and (Test-Path -LiteralPath $cmd)) {
                Put 'ios-oobe' 'started'
                & cmd.exe /c $cmd
                exit 0
            }
        }
        Put 'ios-oobe' 'no-media'
    }
    Start-Sleep -Seconds 10
}
