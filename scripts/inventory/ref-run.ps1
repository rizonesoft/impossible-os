<#
.SYNOPSIS
Runs one capture command on the Windows 11 reference VM and copies its result out.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 2.

Restores the checkpoint (clean by default), copies -In into C:\ios\in over
PowerShell Direct, runs -Command inside ref's interactive desktop session (an
interactive scheduled task, so UI Automation, input and screen capture see the
real shell), waits for its exit code, copies C:\ios\out to -Out and turns the VM
off, so the next run starts from the same checkpoint. Exits with the command's
exit code; 2 when -Out is not a new empty folder, 3 when the VM never became
reachable, 4 when another run holds the VM, 5 when the guest is not the build
docs/design/inventory/reference.json names.

    powershell.exe -File scripts\inventory\ref-run.ps1 -Command 'python C:\ios\in\capture-ui.py --surface notepad-smoke --out C:\ios\out\notepad.json' -In tools -Out R:\11\out
    powershell.exe -File scripts\inventory\ref-run.ps1 -Proof -Out R:\11\out   # the Notepad UI Automation and screenshot proof
#>
[CmdletBinding()]
param(
    [string]$Command,
    [string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Checkpoint = 'clean',
    [string]$VmPath = 'R:\11\ios-win11-ref',
    [string]$Name = 'ios-win11-ref',
    [int]$TimeoutSec = 900,
    [switch]$Proof
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ref-common.ps1')

if ($Proof) { $Command = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\ios\probe\probe-notepad.ps1' }
if (-not $Command) { Write-Host 'ref-run: give -Command or -Proof'; exit 2 }
# Each run gets its own empty folder, so its output never mixes with an earlier one.
if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) {
    Write-Host "ref-run: $Out is not empty; give a new folder for each run"; exit 2
}
New-Item -ItemType Directory -Path $Out -Force | Out-Null
Enter-RefLock $Name

$s = $null
$code = 3
try {
    # Inside the try, so a failure while starting still turns the VM off. No exit
    # here: cmdlets in a finally entered by exit are skipped (Stop-VM left the VM
    # Saved, seen 2026-11-14), so the code is set and the work skipped instead.
    try { $s = Start-RefFrom $Name $VmPath $Checkpoint } catch { Write-Host "ref-run: $($_.Exception.Message)" }
    if ($s) {
        Invoke-Command -Session $s -ScriptBlock {
            foreach ($d in 'C:\ios\in', 'C:\ios\out') {
                if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
                New-Item -ItemType Directory -Path $d | Out-Null
            }
        }
        $wrong = @(Test-RefIdentity $s)
        if ($wrong.Count) {
            foreach ($w in $wrong) { Write-Host "ref-run: $w" }
            $code = 5
        } else {
            if ($In) { Send-RefTree $s $In 'C:\ios\in' }
            $r = Invoke-RefInteractive $s $Command $TimeoutSec
            $n = Receive-RefTree $s 'C:\ios\out' $Out
            Set-Content -LiteralPath (Join-Path $Out 'stdout.txt') -Value $r.Output
            Write-Host "ref-run: exit $($r.ExitCode); $n file(s) in $Out"
            if ($r.Output) { Write-Host $r.Output.Trim() }
            $code = $r.ExitCode
        }
    }
} finally {
    if ($s) { Remove-PSSession $s }
    if ((Get-VM -Name $Name).State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
}
exit $code
