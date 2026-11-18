<#
.SYNOPSIS
Drives the Windows 11 reference VM from the host and shoots its framebuffer.

.DESCRIPTION
Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9.

For the screens UI Automation cannot reach (the lock and sign-in screens, the
secure desktop, setup, boot and recovery). Restores -Checkpoint, runs the steps
in -Steps (a JSON array) in order, writes each shot to -Out as <name>.png, and
writes -Out\shots.json with what each shot is: the video head's mode read just
before and just after it, the size it was taken at, and whether the two agree
(a shot is pixel for pixel only when the mode held and it was taken at that
mode). Turns the VM off at the end. Exits 0 when every step ran, 1 when a step
failed (the shots before it are kept and the failure is in shots.json), 2 on a
bad argument, 3 when the VM never became reachable, 4 when another run holds it.

Steps, one object each, by "op":
  guest   {cmd}               run a command line in ref's desktop session and wait for it
  admin   {script}            run PowerShell in the guest over PowerShell Direct (no desktop)
  key     {vk}                type one virtual key on the VM's keyboard
  chord   {mod, vk}           type vk with mod held (a virtual key or a list of them)
  text    {text}              type text
  secret  {}                  type ref's password, never written anywhere
  cad     {}                  Ctrl+Alt+Delete
  click   {x, y, button}      move the mouse to a screen pixel and click (button 1 left, 2 right)
  wait    {sec}               sleep
  shot    {name}              one shot at the current mode
  burst   {name, count, every} count shots every `every` seconds, named <name>-<n>
  settle  {sec, timeout}      wait until the screen has not changed for sec seconds
  reconnect {}               open a new PowerShell Direct session (after a sign-out or restart)
  user    {name, admin, flags} make a local account with a random password this run keeps in memory
                              (never written); flags go to net user (e.g. "/logonpasswordchg:yes")
  typeof  {name}              type that account's password
  renew   {name}              make a new random password for that account and type it (a change
                              form, or the first one of an account a setup form creates)

    powershell.exe -File scripts\inventory\ref-host.ps1 -Steps steps.json -Out R:\11\captures\session-x
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Steps,
    [Parameter(Mandatory)] [string]$Out,
    [string]$Checkpoint = 'clean',
    [string]$VmPath = 'R:\11\ios-win11-ref',
    [string]$Name = 'ios-win11-ref',
    # For a checkpoint before any account exists (pre-oobe): restore and drive
    # the screen only. No session opens, so guest, admin, user and reconnect
    # steps refuse, and the build is not read back from the guest (ref-vm.ps1
    # checked it when it took the checkpoint).
    [switch]$NoSession
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
. (Join-Path $PSScriptRoot 'ref-common.ps1')

if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) {
    Write-Host "ref-host: $Out is not empty; give a new folder for each run"; exit 2
}
# Windows PowerShell 5.1's ConvertFrom-Json emits a JSON array as one object;
# the pipeline through ForEach-Object unrolls it into its steps.
try { $plan = @((Get-Content -LiteralPath $Steps -Raw | ConvertFrom-Json) | ForEach-Object { $_ }) } catch {
    Write-Host "ref-host: $Steps is not a JSON array of steps: $($_.Exception.Message)"; exit 2
}
New-Item -ItemType Directory -Path $Out -Force | Out-Null
Enter-RefLock $Name

$shots = New-Object System.Collections.ArrayList
$script:Session = $null

# A session a sign-out or restart has ended cannot be closed cleanly; that is
# expected there, so it is reported and not treated as a failure.
function Close-Session() {
    if (-not $script:Session) { return }
    try { Remove-PSSession $script:Session -ErrorAction Stop } catch { Write-Host "ref-host: the old session was already gone ($($_.Exception.Message))" }
    $script:Session = $null
}

# Fixture accounts this run made: name -> password, in memory only. Each run
# restores the checkpoint, so an account and its password end with the run.
$script:Pw = @{}
function New-FixturePassword() {
    $bytes = New-Object byte[] 18
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    return 'Aa1!' + ([Convert]::ToBase64String($bytes) -replace '[+/=]', 'x')
}
function Send-Text([string]$Text) {
    [void](Invoke-CimMethod -InputObject (Get-RefKeyboard $Name) -MethodName TypeText -Arguments @{ asciiText = $Text })
}

function Get-RefMouse() {
    return Get-CimAssociatedInstance -InputObject (Get-RefSystem $Name) -ResultClassName Msvm_SyntheticMouse
}

function Save-Shot([string]$ShotName) {
    $before = Get-RefVideoMode $Name
    $bytes = Get-RefThumbnail $Name $before[0] $before[1]
    $after = Get-RefVideoMode $Name
    $png = Join-Path $Out "$ShotName.png"
    Save-Thumbnail $bytes $png $before[0] $before[1]
    $exact = ($before[0] -eq $after[0]) -and ($before[1] -eq $after[1])
    [void]$shots.Add([ordered]@{ name = $ShotName; file = "$ShotName.png"; width = $before[0]; height = $before[1]
        depth = 16; modeBefore = $before; modeAfter = $after; exact = $exact; at = (Get-Date).ToString('o') })
    Write-Host "ref-host: shot $ShotName $($before[0])x$($before[1])$(if (-not $exact) { ' (the mode changed during it)' })"
}

function Invoke-Step($step) {
    if ($NoSession -and $step.op -in 'guest', 'admin', 'user', 'reconnect') { throw "a $($step.op) step needs a session, and this run has none (-NoSession)" }
    switch ($step.op) {
        'guest' {
            $r = Invoke-RefInteractive $script:Session $step.cmd 600
            if ($r.ExitCode -ne 0) { throw "guest command exit $($r.ExitCode): $($step.cmd) $($r.Output)" }
        }
        'admin' { Invoke-Command -Session $script:Session -ScriptBlock ([scriptblock]::Create($step.script)) | Out-Host }
        'key' { Send-RefKey $Name ([uint32]$step.vk) }
        'chord' {
            # mod is one virtual key or a list of them, pressed in order and
            # released in reverse even when the typed key fails.
            $kb = Get-RefKeyboard $Name
            $mods = @($step.mod | ForEach-Object { [uint32]$_ })
            $down = New-Object System.Collections.ArrayList
            try {
                foreach ($m in $mods) { [void](Invoke-CimMethod -InputObject $kb -MethodName PressKey -Arguments @{ keyCode = $m }); [void]$down.Add($m) }
                [void](Invoke-CimMethod -InputObject $kb -MethodName TypeKey -Arguments @{ keyCode = [uint32]$step.vk })
            } finally {
                for ($i = $down.Count - 1; $i -ge 0; $i--) { [void](Invoke-CimMethod -InputObject $kb -MethodName ReleaseKey -Arguments @{ keyCode = $down[$i] }) }
            }
        }
        'text' { Send-Text ([string]$step.text) }
        'secret' {
            Send-Text (Get-RefCredential $VmPath).GetNetworkCredential().Password
        }
        'cad' { [void](Invoke-CimMethod -InputObject (Get-RefKeyboard $Name) -MethodName TypeCtrlAltDel) }
        'click' {
            $m = Get-RefMouse
            [void](Invoke-CimMethod -InputObject $m -MethodName SetAbsolutePosition -Arguments @{
                horizontalPosition = [uint32]$step.x; verticalPosition = [uint32]$step.y })
            Start-Sleep -Milliseconds 300
            $b = if ($step.button) { [uint32]$step.button } else { [uint32]1 }
            [void](Invoke-CimMethod -InputObject $m -MethodName ClickButton -Arguments @{ buttonIndex = $b })
        }
        'wait' { Start-Sleep -Milliseconds ([int]([double]$step.sec * 1000)) }
        'shot' { Save-Shot $step.name }
        'burst' {
            for ($i = 1; $i -le [int]$step.count; $i++) {
                Save-Shot "$($step.name)-$i"
                Start-Sleep -Milliseconds ([int]([double]$step.every * 1000))
            }
        }
        'settle' {
            $quiet = [double]$step.sec; $deadline = (Get-Date).AddSeconds([double]$step.timeout)
            $last = $null; $since = Get-Date
            while ((Get-Date) -lt $deadline) {
                $h = (Get-ThumbnailFacts (Get-RefThumbnail $Name)).Hash
                if ($h -ne $last) { $last = $h; $since = Get-Date }
                elseif (((Get-Date) - $since).TotalSeconds -ge $quiet) { return }
                Start-Sleep -Milliseconds 500
            }
            throw "the screen did not settle for $quiet s within $($step.timeout) s"
        }
        'user' {
            $pw = New-FixturePassword
            $script:Pw[$step.name] = $pw
            $flags = if ($step.flags) { [string]$step.flags } else { '' }
            $admin = [bool]$step.admin
            Invoke-Command -Session $script:Session -ArgumentList $step.name, $pw, $flags, $admin -ScriptBlock {
                param($n, $p, $f, $a)
                # /y answers net's question about passwords over 14 characters.
                $out = & net.exe user $n $p /add /y $f.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) 2>&1
                if ($LASTEXITCODE -ne 0) { throw "net user $n /add: $out" }
                if ($a) { & net.exe localgroup Administrators $n /add | Out-Null }
            }
        }
        'typeof' {
            if (-not $script:Pw.ContainsKey($step.name)) { throw "no fixture account $($step.name) in this run" }
            Send-Text $script:Pw[$step.name]
        }
        'renew' {
            # Also the first password of an account a setup form creates.
            $script:Pw[$step.name] = New-FixturePassword
            Send-Text $script:Pw[$step.name]
        }
        'reconnect' {
            Close-Session
            $script:Session = New-RefSession $Name $VmPath
        }
        default { throw "unknown step op '$($step.op)'" }
    }
}

$code = 3
$failed = $null
try {
    $ready = $false
    if ($NoSession) {
        try { Restore-RefCheckpoint $Name $Checkpoint $VmPath; $ready = $true } catch { Write-Host "ref-host: $($_.Exception.Message)" }
    } else {
        try { $script:Session = Start-RefFrom $Name $VmPath $Checkpoint; $ready = [bool]$script:Session } catch { Write-Host "ref-host: $($_.Exception.Message)" }
    }
    if ($ready) {
        $wrong = if ($NoSession) { @() } else { @(Test-RefIdentity $script:Session) }
        if ($wrong.Count) {
            foreach ($w in $wrong) { Write-Host "ref-host: $w" }
            $code = 5
        } else {
            $code = 0
            $n = 0
            foreach ($step in $plan) {
                $n++
                try { Invoke-Step $step } catch {
                    $failed = "step $n ($($step.op)): $($_.Exception.Message)"
                    Write-Host "ref-host: $failed"
                    try { Save-Shot "failed-step-$n" } catch { Write-Host "ref-host: no shot of the failure either: $($_.Exception.Message)" }
                    $code = 1
                    break
                }
            }
        }
    }
} finally {
    $doc = [ordered]@{ checkpoint = $Checkpoint; shots = $shots; failed = $failed }
    $doc | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Out 'shots.json') -Encoding UTF8
    Close-Session
    if ($script:RefTouched -and (Get-VM -Name $Name).State -ne 'Off') { Stop-VM -Name $Name -TurnOff -Force }
}
Write-Host "ref-host: exit $code; $($shots.Count) shot(s) in $Out"
exit $code
