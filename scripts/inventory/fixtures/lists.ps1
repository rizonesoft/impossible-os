# Capture fixture: fills the lists a capture walks with made-up entries, so a
# list's row template, controls, bounds and states are recorded while every
# payload stays fictional. One variant sets every list to the same amount:
#   files     C:\Users\ref\Documents\Sample folder (File Explorer)
#   apps      per-user uninstall entries (Settings > Apps > Installed apps)
#   devices   printers on the inbox PDF driver (Settings > Bluetooth & devices)
#   networks  per-user VPN connections (Settings > Network & internet > VPN)
#   accounts  local standard users (Settings > Accounts > Other users)
# Every entry carries a "Sample" name, and the fixture removes every entry it
# owns before it adds the variant's, so a second run leaves the same state.
# "empty" removes them all and leaves the sample folder, empty; a list Windows fills itself (the inbox apps and
# printers) then still shows its own entries, which the capture records as such.
# -State prints the owned entries as one JSON line and changes nothing.
# Runs elevated in ref's interactive session (capture.py --prepare, fixtures/run.py).
# Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 20.
# Variants: empty, populated, overflowing
param(
    [ValidateSet('empty', 'populated', 'overflowing')][string]$Variant = 'populated',
    [switch]$State
)
$ErrorActionPreference = 'Stop'

# Entries per list: populated shows a few rows, overflowing scrolls at 1920 by
# 1080 at every scale of the profile matrix.
$Counts = @{ empty = 0; populated = 4; overflowing = 40 }
$Folder = Join-Path $env:USERPROFILE 'Documents\Sample folder'
$Uninstall = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
$Prefix = 'IosSample'
$PdfDriver = 'Microsoft Print To PDF'
# Made-up files of the kinds a folder holds; each is a few bytes.
$FileKinds = '.txt', '.md', '.csv', '.log', '.json', '.xml'

# A fresh account has no per-user Uninstall key until something installs per user.
function Get-UninstallKeys() {
    if (-not (Test-Path -LiteralPath $Uninstall)) { return @() }
    return @(Get-ChildItem -LiteralPath $Uninstall)
}

function Get-Owned() {
    $files = @()
    if (Test-Path -LiteralPath $Folder) { $files = @(Get-ChildItem -LiteralPath $Folder -Force | Sort-Object Name | ForEach-Object { $_.Name }) }
    [ordered]@{
        # The folder itself is state: the empty variant keeps it, empty, for File Explorer to open.
        folder = (Test-Path -LiteralPath $Folder -PathType Container)
        files = $files
        apps = @(Get-UninstallKeys | Where-Object { $_.PSChildName -like "$Prefix*" } | Sort-Object PSChildName | ForEach-Object { (Get-ItemProperty -LiteralPath $_.PSPath).DisplayName })
        devices = @(Get-Printer | Where-Object { $_.Name -like 'Sample printer *' } | Sort-Object Name | ForEach-Object { "$($_.Name) on $(Split-Path -Leaf $_.PortName)" })
        networks = @(Get-VpnConnection | Where-Object { $_.Name -like 'Sample VPN *' } | Sort-Object Name | ForEach-Object { $_.Name })
        accounts = @(Get-LocalUser | Where-Object { $_.Name -like 'sample*' } | Sort-Object Name | ForEach-Object { $_.Name })
    }
}

if ($State) { (Get-Owned) | ConvertTo-Json -Compress; exit 0 }

# Remove every owned entry first. Each list is collected before anything is
# removed: Remove-LocalUser inside a live Get-LocalUser pipeline skips every
# second user (measured on the VM).
if (Test-Path -LiteralPath $Folder) { Remove-Item -LiteralPath $Folder -Recurse -Force }
foreach ($k in @(Get-UninstallKeys | Where-Object { $_.PSChildName -like "$Prefix*" })) { Remove-Item -LiteralPath $k.PSPath -Recurse -Force }
foreach ($p in @(Get-Printer | Where-Object { $_.Name -like 'Sample printer *' })) { Remove-Printer -Name $p.Name }
foreach ($p in @(Get-PrinterPort | Where-Object { $_.Name -like '*\Sample printer *.prn' })) { Remove-PrinterPort -Name $p.Name }
foreach ($v in @(Get-VpnConnection | Where-Object { $_.Name -like 'Sample VPN *' })) { Remove-VpnConnection -Name $v.Name -Force }
foreach ($u in @(Get-LocalUser | Where-Object { $_.Name -like 'sample*' })) { Remove-LocalUser -Name $u.Name }
$left = Get-Owned
if ($left.folder -or @($left.Keys | Where-Object { $_ -ne 'folder' -and @($left[$_]).Count })) { throw "entries left after removal: $($left | ConvertTo-Json -Compress)" }

$n = $Counts[$Variant]
New-Item -ItemType Directory -Path $Folder -Force | Out-Null
if ($n -gt 0) {
    New-Item -Path $Uninstall -Force | Out-Null
    # Users get a random password nobody keeps: they exist to be listed, never to sign in.
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    for ($i = 1; $i -le $n; $i++) {
        $nn = '{0:D2}' -f $i
        $name = "Sample file $nn$($FileKinds[($i - 1) % $FileKinds.Count])"
        Set-Content -LiteralPath (Join-Path $Folder $name) -Value "Sample content $nn" -Encoding ASCII
        $key = Join-Path $Uninstall "$Prefix$nn"
        New-Item -Path $key -Force | Out-Null
        foreach ($v in @(@('DisplayName', "Sample app $nn"), @('Publisher', 'Sample publisher'), @('DisplayVersion', "1.0.$i"),
                @('InstallDate', '20260101'), @('UninstallString', 'cmd.exe /c exit 0'))) {
            New-ItemProperty -LiteralPath $key -Name $v[0] -Value $v[1] -PropertyType String -Force | Out-Null
        }
        New-ItemProperty -LiteralPath $key -Name EstimatedSize -Value (1024 * $i) -PropertyType DWord -Force | Out-Null
        New-ItemProperty -LiteralPath $key -Name NoModify -Value 1 -PropertyType DWord -Force | Out-Null
        # Each printer gets its own local port: Settings shows one device per port,
        # so printers sharing a port collapse into one row (measured on the VM).
        $port = Join-Path $env:PUBLIC "Sample printer $nn.prn"
        Add-PrinterPort -Name $port
        Add-Printer -Name "Sample printer $nn" -DriverName $PdfDriver -PortName $port
        Add-VpnConnection -Name "Sample VPN $nn" -ServerAddress "vpn$nn.example.invalid" -TunnelType Automatic -RememberCredential:$false -Force
        $bytes = New-Object byte[] 24; $rng.GetBytes($bytes)
        $pw = ConvertTo-SecureString ([Convert]::ToBase64String($bytes) + 'a1!') -AsPlainText -Force
        New-LocalUser -Name "sample$nn" -FullName "Sample person $nn" -Password $pw -Description 'Capture fixture' | Out-Null
        # Settings > Other users lists members of Users; a bare local user is in no group.
        Add-LocalGroupMember -SID 'S-1-5-32-545' -Member "sample$nn"
    }
}
$owned = Get-Owned
Write-Output "fixture lists: $Variant ($n of each: $(@($owned.files).Count) files, $(@($owned.apps).Count) apps, $(@($owned.devices).Count) printers, $(@($owned.networks).Count) VPNs, $(@($owned.accounts).Count) users)"
exit 0
