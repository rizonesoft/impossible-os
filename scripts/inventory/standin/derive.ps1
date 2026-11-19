<#
.SYNOPSIS
Derives the stand-in disk from the reference VM's clean checkpoint.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 21.

Restores clean, installs the stand-in agent (a logon task that runs a job from
a volume labelled IOSJOB and shuts down), drops clean's pending OneDrive setup
(Clear-RefPendingSetup), stages the virtio GPU display driver in the driver
store (drivers.json pins the virtio-win ISO by SHA-256; the driver binds only on
a machine with virtio-gpu heads, the two-display variant), pauses device encryption, turns
hibernation off (a hybrid-boot image does not resume on other hardware) and
display and sleep timeouts off, lets the task run on battery, shuts
the guest down cleanly and merges its disk chain into one standalone VHDX at
-Out. The checkpoint itself is unchanged: the next ref-run restores it.
qemu-img convert -O qcow2 then gives standin/run.py its disk.

    powershell.exe -File scripts\inventory\standin\derive.ps1 -Out D:\11\standin\standin.vhdx
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Out,
    [string]$VmPath = 'D:\11\ios-win11-ref',
    [string]$Name = 'ios-win11-ref'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\ref-common.ps1')
if (Test-Path -LiteralPath $Out) { Write-Host "derive: $Out exists; give a new path"; exit 2 }
$pin = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'drivers.json') | ConvertFrom-Json).'virtio-win'
if (-not (Test-Path -LiteralPath $pin.iso)) { Write-Host "derive: no $($pin.iso); fetch $($pin.url)"; exit 2 }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pin.iso).Hash.ToLowerInvariant()
if ($hash -ne $pin.sha256) { Write-Host "derive: $($pin.iso) has SHA-256 $hash, drivers.json pins $($pin.sha256)"; exit 2 }
Enter-RefLock $Name
$s = $null
$mounted = $false
$code = 3
try {
    $s = Start-RefFrom $Name $VmPath 'clean'
    $wrong = @(Test-RefIdentity $s $Name)
    if ($wrong.Count) { foreach ($w in $wrong) { Write-Host "derive: $w" }; $code = 5 }
    else {
        Invoke-Command -Session $s -ScriptBlock { New-Item -ItemType Directory -Path 'C:\ios\standin' -Force | Out-Null }
        Send-RefFile $s (Join-Path $PSScriptRoot 'agent.ps1') 'C:\ios\standin\agent.ps1'
        Clear-RefPendingSetup $s
        $vol = Mount-DiskImage -ImagePath $pin.iso -PassThru | Get-Volume
        $mounted = $true
        foreach ($f in $pin.display.files) {
            Send-RefFile $s (Join-Path "$($vol.DriveLetter):\$($pin.display.folder)" $f) "C:\ios\standin\viogpudo\$f"
        }
        Dismount-DiskImage -ImagePath $pin.iso | Out-Null
        $mounted = $false
        $added = Invoke-Command -Session $s -ScriptBlock {
            $o = & pnputil.exe /add-driver 'C:\ios\standin\viogpudo\viogpudo.inf' 2>&1 | Out-String
            $c = $LASTEXITCODE
            Remove-Item -LiteralPath 'C:\ios\standin\viogpudo' -Recurse -Force
            [pscustomobject]@{ Code = $c; Output = $o }
        }
        if ($added.Code -ne 0) { throw "pnputil /add-driver viogpudo.inf exited $($added.Code): $($added.Output.Trim())" }
        Invoke-Command -Session $s -ScriptBlock {
            $ErrorActionPreference = 'Stop'
            & powercfg.exe /h off
            if ($LASTEXITCODE -ne 0) { throw "powercfg /h off exited $LASTEXITCODE" }
            # On a variant's battery the display would turn off and the guest sleep
            # before a page is captured: never time out, on AC or on battery.
            foreach ($t in 'monitor-timeout-ac', 'monitor-timeout-dc', 'standby-timeout-ac', 'standby-timeout-dc') {
                & powercfg.exe /change $t 0
                if ($LASTEXITCODE -ne 0) { throw "powercfg /change $t exited $LASTEXITCODE" }
            }
            # Automatic device encryption is still converting C: in clean; on the
            # stand-in every boot would rewrite the whole disk into its overlay
            # (16 GB in 10 minutes, measured). Pause it and stop it resuming.
            $bl = 'HKLM:\SYSTEM\CurrentControlSet\Control\BitLocker'
            # New-Item -Force on an existing key tries to recreate it and fails.
            if (-not (Test-Path -LiteralPath $bl)) { New-Item -Path $bl | Out-Null }
            Set-ItemProperty -LiteralPath $bl -Name PreventDeviceEncryption -Value 1 -Type DWord
            $st = & manage-bde.exe -status C: | Out-String
            if ($st -match 'Encryption in Progress') {
                & manage-bde.exe -pause C: | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "manage-bde -pause exited $LASTEXITCODE" }
            }
            $a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\ios\standin\agent.ps1'
            $t = New-ScheduledTaskTrigger -AtLogOn -User 'ref'
            $p = New-ScheduledTaskPrincipal -UserId 'ref' -LogonType Interactive -RunLevel Highest
            # A task's defaults refuse to start on battery and stop it when AC goes
            # away; the discharging and saver variants run on battery.
            $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
            Register-ScheduledTask -TaskName 'ios-standin-agent' -Action $a -Trigger $t -Principal $p -Settings $set -Force | Out-Null
        }
        Remove-PSSession $s; $s = $null
        Stop-VM -Name $Name -Force    # graceful: the guest shuts down
        $disk = (Get-VMHardDiskDrive -VMName $Name).Path
        New-Item -ItemType Directory -Path (Split-Path -Parent $Out) -Force | Out-Null
        Convert-VHD -Path $disk -DestinationPath $Out -VHDType Dynamic
        Write-Host "derive: $Out from $disk"
        $code = 0
    }
} finally {
    if ($mounted) { Dismount-DiskImage -ImagePath $pin.iso | Out-Null }
    if ($s) { Remove-PSSession $s }
    if ($script:RefTouched -and (Get-VM -Name $Name).State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
}
exit $code
