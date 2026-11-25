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
mode). Turns the VM off at the end and restores -Checkpoint again, so the VM
leaves the run as it started. Exits 0 when every step ran, 1 when a step
failed (the shots before it are kept and the failure is in shots.json), 2 on a
bad argument, 3 when the VM never became reachable, 4 when another run holds it.

Steps, one object each, by "op":
  guest   {cmd}               run a command line in ref's desktop session and wait for it
  admin   {script}            run PowerShell in the guest over PowerShell Direct (no desktop)
  fetch   {path, name}        copy a guest file into the output folder as name (over PowerShell Direct)
  key     {vk}                type one virtual key on the VM's keyboard
  chord   {mod, vk}           type vk with mod held (a virtual key or a list of them)
  text    {text}              type text
  secret  {}                  type ref's password, never written anywhere
  cad     {}                  Ctrl+Alt+Delete
  nmi     {}                  inject a non-maskable interrupt (Debug-VM), which stops Windows with a stop error
  click   {x, y, button}      move the mouse to a screen pixel and click (button 1 left, 2 right)
  wait    {sec}               sleep
  awaitcolour {x, y, rgb, timeout[, width, height]}  wait until that pixel (in 1920 by 1080) shows rgb, within 16-bit rounding, at that mode if given
  shot    {name}              one shot at the current mode
  burst   {name, count, every[, cadence, seconds, until, still, as, astimeout]} count shots every `every` seconds, named <name>-<n>; cadence: shots start no closer than that; seconds: stop after that long; until: stop at a frame showing that pixel; still: stop once the screen has not changed for that long, and fail if it never holds still; as: name the still shot of the page the burst ended on (the exact frame that met until or held still, else a shot, after a wait of astimeout s for until)
  settle  {sec, timeout}      wait until the screen has not changed for sec seconds
  reconnect {}               open a new PowerShell Direct session (after a sign-out or restart)
  user    {name, admin, flags} make a local account with a random password this run keeps in memory
                              (never written); flags go to net user (e.g. "/logonpasswordchg:yes")
  typeof  {name}              type that account's password
  renew   {name}              make a new random password for that account and type it (a change
                              form, or the first one of an account a setup form creates)

    powershell.exe -File scripts\inventory\ref-host.ps1 -Steps steps.json -Out D:\11\captures\session-x
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Steps,
    [Parameter(Mandatory)] [string]$Out,
    [string]$Checkpoint = 'clean',
    [string]$VmPath = 'D:\11\ios-win11-ref',
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
Assert-RefTarget $Name @($VmPath, $Out, $Steps)

if ((Test-Path -LiteralPath $Out) -and @(Get-ChildItem -LiteralPath $Out -Force).Count) {
    Write-Host "ref-host: $Out is not empty; give a new folder for each run"; exit 2
}
# Windows PowerShell 5.1's ConvertFrom-Json emits a JSON array as one object;
# the pipeline through ForEach-Object unrolls it into its steps.
try { $plan = @((Get-Content -LiteralPath $Steps -Raw | ConvertFrom-Json) | ForEach-Object { $_ }) } catch {
    Write-Host "ref-host: $Steps is not a JSON array of steps: $($_.Exception.Message)"; exit 2
}
# These become files in -Out, so each is a plain file name.
foreach ($step in $plan) {
    if ($step.op -isnot [string]) { Write-Host "ref-host: a step's op is one word, not '$($step.op)'"; exit 2 }
    $files = @(if ($step.op -in 'shot', 'burst', 'fetch') { "$($step.name)" }; if ($step.as) { "$($step.as)" })
    foreach ($f in $files) {
        if ($f -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\z') { Write-Host "ref-host: $($step.op): '$f' is not a plain file name"; exit 2 }
    }
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

# A shot whose mode changed while it was taken is taken again once at the new
# mode; a screen that has passed by then keeps the second shot, marked inexact.
# A restart takes the video head away for a moment; a shot then waits for it
# rather than failing the run, until the shot's deadline.
function Get-ShotMode() {
    for (;;) {
        try { return Get-RefVideoMode $Name $script:ShotUntil } catch {
            if ((Get-Date) -ge $script:ShotDeadline -or $_.Exception.Message -notlike 'no video head*') { throw }
            $script:ShotInterrupted = $true
            # The wait stops at the deadline, and no read starts after it
            # (review, 2026-11-18: a retry started past a burst's end).
            Start-Sleep -Milliseconds ([int][Math]::Max(0, [Math]::Min(500, ($script:ShotDeadline - (Get-Date)).TotalMilliseconds)))
            if ((Get-Date) -ge $script:ShotDeadline) { throw }
        }
    }
}
$script:ShotDeadline = Get-Date
$script:ShotInterrupted = $false
$script:ShotUntil = [datetime]::MaxValue
$script:LastShot = $null

# True when the pixel at (x, y) of a thumbnail of the given size shows rgb,
# within the 16-bit rounding of the host path (8 a channel); x and y are in
# 1920 by 1080 and scaled to the thumbnail's size.
function Test-ThumbnailColour([byte[]]$Bytes, [int]$Width, [int]$Height, $Want) {
    $p = Get-ThumbnailPixel $Bytes $Width $Height ([int]([int]$Want.x * $Width / 1920)) ([int]([int]$Want.y * $Height / 1080))
    return @(0, 1, 2 | Where-Object { [Math]::Abs($p[$_] - [int]$Want.rgb[$_]) -gt 8 }).Count -eq 0
}

function Save-Shot([string]$ShotName, [datetime]$Until = [datetime]::MaxValue) {
    $png = Join-Path $Out "$ShotName.png"
    # One deadline for the whole shot, shared by the mode reads and the
    # thumbnail, so a reset cannot hold a burst frame much past 15 s (review,
    # 2026-11-18: each read had its own budget, and the budget counted only
    # sleeps, not the host calls between them).
    # Until: a timed burst's own end, which a frame started late in it must
    # not run past (review, 2026-11-18).
    $script:ShotDeadline = (Get-Date).AddSeconds(15)
    # Past Until, no new host call starts: the frame is dropped and
    # ShotExpired tells the burst to end (review, 2026-11-18).
    if ($Until -lt $script:ShotDeadline) { $script:ShotDeadline = $Until }
    $script:ShotExpired = $false
    # ShotInterrupted: the helpers waited out a missing video head or an
    # invalid state inside this shot, which a still screen must not span
    # (review, 2026-11-19).
    $script:ShotInterrupted = $false
    # The helpers check the shot's own deadline, the earlier of its 15 s and
    # Until, between their host calls. Past Until the frame is dropped for
    # the burst to end; past the 15 s alone the shot fails, so a standalone
    # shot never disappears quietly (review, 2026-11-18).
    $script:ShotUntil = $script:ShotDeadline
    try { $taken = Get-ShotPixels } catch {
        if ($_.Exception.Message -notlike 'shot expired*') { throw }
        if ((Get-Date) -lt $Until) { throw "shot ${ShotName}: no frame within 15 s" }
        $script:ShotExpired = $true
        return
    } finally { $script:ShotUntil = [datetime]::MaxValue }
    $bytes, $before, $after, $exact, $try = $taken
    Save-Thumbnail $bytes $png $before[0] $before[1]
    $script:LastShot = @{ Bytes = $bytes; Width = $before[0]; Height = $before[1] }
    [void]$shots.Add([ordered]@{ name = $ShotName; file = "$ShotName.png"; width = $before[0]; height = $before[1]
        depth = 16; modeBefore = $before; modeAfter = $after; exact = $exact; tries = [Math]::Min($try, 2); at = (Get-Date).ToString('o') })
    Write-Host "ref-host: shot $ShotName $($before[0])x$($before[1])$(if (-not $exact) { ' (the mode changed during it twice)' })"
}

# A shot of the screen a step failed on, for the operator. It waits at most
# 3 s: a failure that left no video head would otherwise hold the VM lock
# for another 15 s before teardown (review, 2026-11-19).
function Save-FailureShot([int]$N) {
    try { Save-Shot "failed-step-$N" (Get-Date).AddSeconds(3) } catch { Write-Host "ref-host: no shot of the failure either: $($_.Exception.Message)"; return }
    if ($script:ShotExpired) { Write-Host "ref-host: no shot of the failure either: no frame within 3 s" }
}

# The thumbnail and the mode read before and after it, retried once at a new
# mode; every host call checks $script:ShotUntil through the helpers.
function Get-ShotPixels() {
    for ($try = 1; $try -le 2; ) {
        $before = Get-ShotMode
        try { $bytes = Get-RefThumbnail $Name $before[0] $before[1] $script:ShotUntil } catch {
            # 32775 is Hyper-V's "invalid state": the VM is between states as it
            # resets (after the boot walk's stop error, 2026-11-18). Wait it out
            # like a missing video head, up to 15 s, reading the mode again.
            if ((Get-Date) -ge $script:ShotDeadline -or $_.Exception.Message -notlike 'thumbnail failed: 32775*') { throw }
            $script:ShotInterrupted = $true
            Start-Sleep -Milliseconds ([int][Math]::Max(0, [Math]::Min(500, ($script:ShotDeadline - (Get-Date)).TotalMilliseconds)))
            if ((Get-Date) -ge $script:ShotDeadline) { throw }
            continue
        }
        # Pixels taken before an outage that the mode read after them waited
        # out are not this frame: take them again, still under the shot's
        # deadline (review, 2026-11-19: they started a still hold).
        $was = $script:ShotInterrupted; $script:ShotInterrupted = $false
        $after = Get-ShotMode
        $lost = $script:ShotInterrupted; $script:ShotInterrupted = $was -or $lost
        if ($lost) { continue }
        $exact = ($before[0] -eq $after[0]) -and ($before[1] -eq $after[1])
        if ($exact) { break }
        $try++
    }
    return $bytes, $before, $after, $exact, $try
}

function Invoke-Step($step) {
    if ($NoSession -and $step.op -in 'guest', 'admin', 'fetch', 'user', 'reconnect') { throw "a $($step.op) step needs a session, and this run has none (-NoSession)" }
    switch ($step.op) {
        'guest' {
            $r = Invoke-RefInteractive $script:Session $step.cmd 600
            if ($r.ExitCode -ne 0) { throw "guest command exit $($r.ExitCode): $($step.cmd) $($r.Output)" }
        }
        'admin' { Invoke-Command -Session $script:Session -ScriptBlock ([scriptblock]::Create($step.script)) | Out-Host }
        'fetch' {
            Copy-Item -FromSession $script:Session -LiteralPath $step.path -Destination (Join-Path $Out $step.name) -Force
        }
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
        'nmi' { Debug-VM -Name $Name -InjectNonMaskableInterrupt -Force -Confirm:$false }
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
            # seconds: also end when that long has passed, so the time watched
            # does not depend on how fast the host saves a shot; a frame that
            # is waiting for the VM when that time runs out is dropped and the
            # burst ends. cadence: each shot starts no sooner than that after
            # the one before it, which bounds the frames a fast host keeps;
            # count stays the ceiling on frames (review, 2026-11-18).
            # still: end once the screen has held still that long (the same
            # frame bytes), and fail if it never does within the burst.
            $end = if ($step.seconds) { (Get-Date).AddSeconds([double]$step.seconds) } else { [datetime]::MaxValue }
            $held = $null; $since = $null; $stilled = $false; $saved = 0; $met = $null
            for ($i = 1; $i -le [int]$step.count; $i++) {
                if ((Get-Date) -ge $end) { break }
                $began = Get-Date
                try { Save-Shot "$($step.name)-$i" $end } catch {
                    $m = $_.Exception.Message
                    $transient = $m -like 'no video head*' -or $m -like 'thumbnail failed: 32775*' -or $m -like 'shot *: no frame within 15 s'
                    if ($transient -and (Get-Date) -ge $end) { break }
                    # A timed burst outlives a video outage longer than one
                    # shot's 15 s: the same frame is tried again while the
                    # burst has time, and a screen held still before the
                    # outage counts afresh after it (review, 2026-11-19).
                    if ($transient -and $step.seconds) { $held = $null; $i--; continue }
                    throw
                }
                if ($script:ShotExpired) { break }
                $saved++
                # The hold restarts after an outage the shot waited out.
                if ($script:ShotInterrupted) { $held = $null }
                if ($step.still) {
                    $h = Get-ThumbnailHash $script:LastShot.Bytes
                    if ($h -ne $held) { $held = $h; $since = Get-Date }
                    elseif (((Get-Date) - $since).TotalSeconds -ge [double]$step.still) { $stilled = $true; if ($step.as) { $met = $shots[$shots.Count - 1] }; break }
                }
                # until: end the burst at the first frame that shows that
                # pixel, the boot menu's blue, whose 30 s countdown a fixed
                # burst could outrun (review, 2026-11-18).
                if ($step.until -and (Test-ThumbnailColour $script:LastShot.Bytes $script:LastShot.Width $script:LastShot.Height $step.until)) { if ($step.as) { $met = $shots[$shots.Count - 1] }; break }
                # No sleep after the last frame, and none past the end
                # (review, 2026-11-18: the repair burst slept 3 s after its
                # twelfth frame).
                if ($i -ge [int]$step.count -or (Get-Date) -ge $end) { break }
                if ($step.cadence) {
                    # At most one cadence, and never past the burst's end, so a
                    # step in the host's clock cannot stretch the sleep (review,
                    # 2026-11-18).
                    $wait = ($began.AddSeconds([double]$step.cadence) - (Get-Date)).TotalMilliseconds
                    $wait = [Math]::Min($wait, [Math]::Min([double]$step.cadence * 1000, ($end - (Get-Date)).TotalMilliseconds))
                    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
                }
                $rest = [Math]::Min([double]$step.every * 1000, ($end - (Get-Date)).TotalMilliseconds)
                if ($rest -gt 0) { Start-Sleep -Milliseconds ([int]$rest) }
            }
            # A burst that kept no frame cannot be published, so the walk
            # stops here and not after the steps that follow (review,
            # 2026-11-19).
            if ($saved -eq 0) { throw "burst $($step.name): no frame within the burst" }
            if ($step.still -and -not $stilled) { throw "burst $($step.name): the screen did not hold still for $($step.still) s" }
            # as: the still shot of the page the burst ended on. An exact
            # frame that met until or held still is that shot already, so the
            # walk keys on at once; the boot menu's 30 s countdown is running
            # (review, 2026-11-19: the menu was polled and shot again first).
            # Without one, an until page is awaited for up to astimeout
            # seconds, and the page is shot.
            if ($step.as) {
                if ($met -and $met.exact) {
                    Copy-Item -LiteralPath (Join-Path $Out $met.file) -Destination (Join-Path $Out "$($step.as).png") -Force
                    $copy = [ordered]@{}; foreach ($k in $met.Keys) { $copy[$k] = $met[$k] }
                    $copy.name = $step.as; $copy.file = "$($step.as).png"
                    [void]$shots.Add($copy)
                    Write-Host "ref-host: shot $($step.as) is $($met.name)"
                } else {
                    if ($step.until) { Invoke-Step ([pscustomobject]@{ op = 'awaitcolour'; x = $step.until.x; y = $step.until.y; rgb = $step.until.rgb; timeout = $step.astimeout }) }
                    Save-Shot $step.as
                }
            }
        }
        'awaitcolour' {
            # Wait until the pixel at (x, y) shows rgb, within the 16-bit
            # rounding of the host path: the boot walk's timer for the boot
            # manager's blue. A burst's length timed it, and then the mode,
            # which comes back while Windows' spinner still shows alone
            # (review and live run, 2026-11-18).
            # A 160 by 90 thumbnail is enough for one pixel of a uniform
            # background and needs no video mode, which a restart takes away
            # (review, 2026-11-18: a native-size image per poll, and a mode
            # read that gave up after 15 s of a 90 s wait).
            # With width and height, the head must also show that mode: the
            # Startup Settings menu is blue in the firmware's mode, and the
            # recovery page before it is blue at the display's (2026-11-18).
            $deadline = (Get-Date).AddSeconds([double]$step.timeout)
            while ((Get-Date) -lt $deadline) {
                try {
                    # The thumbnail keeps the screen's aspect: Hyper-V puts a
                    # 4:3 screen in a 16:9 one between black bars, so the
                    # corner read black on the 1024 by 768 menu (2026-11-18).
                    $mode = $true; $tw = 160; $th = 90
                    if ($step.width) {
                        $m = Get-RefVideoMode $Name $deadline; $mode = ($m[0] -eq [int]$step.width) -and ($m[1] -eq [int]$step.height)
                        $th = [int](160 * [int]$step.height / [int]$step.width)
                    }
                    # Each host call checks the wait's deadline, so a poll
                    # begun just before it starts no further call (review,
                    # 2026-11-18); its expiry is the wait's timeout.
                    if ($mode -and (Test-ThumbnailColour (Get-RefThumbnail $Name $tw $th $deadline) $tw $th $step)) { return }
                } catch {
                    if ($_.Exception.Message -like 'shot expired*') { break }
                    if ($_.Exception.Message -notlike 'thumbnail failed: 32775*' -and $_.Exception.Message -notlike 'no video head*') { throw }
                }
                Start-Sleep -Milliseconds ([int][Math]::Max(0, [Math]::Min(250, ($deadline - (Get-Date)).TotalMilliseconds)))
            }
            throw "the pixel at ($($step.x), $($step.y)) did not show $($step.rgb -join ',') within $($step.timeout) s"
        }
        'settle' {
            $quiet = [double]$step.sec; $deadline = (Get-Date).AddSeconds([double]$step.timeout)
            $last = $null; $since = Get-Date
            # Each poll's host calls check the deadline, and the sleep stops
            # at it (review, 2026-11-18), as in awaitcolour.
            while ((Get-Date) -lt $deadline) {
                try { $h = Get-ThumbnailHash (Get-RefThumbnail $Name 640 360 $deadline) } catch {
                    if ($_.Exception.Message -like 'shot expired*') { break }
                    # Hyper-V's invalid state is the VM between states: the
                    # quiet time starts again after it, inside the same
                    # timeout (review, 2026-11-19: one such read ended a 60 s
                    # settle).
                    if ($_.Exception.Message -notlike 'thumbnail failed: 32775*') { throw }
                    $last = $null; $h = $null
                }
                if ($null -eq $h) { }
                elseif ($h -ne $last) { $last = $h; $since = Get-Date }
                elseif (((Get-Date) - $since).TotalSeconds -ge $quiet) { return }
                Start-Sleep -Milliseconds ([int][Math]::Max(0, [Math]::Min(500, ($deadline - (Get-Date)).TotalMilliseconds)))
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
        # Without a session the guest cannot be asked its build, so the build
        # the VM was last verified as is checked instead (review, 2026-11-18).
        $wrong = if ($NoSession) { @(Test-RefBuildNote $Name) } else { @(Test-RefIdentity $script:Session $Name) }
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
                    Save-FailureShot $n
                    $code = 1
                    break
                }
            }
        }
    }
} finally {
    Close-Session
    if ($script:RefTouched) {
        # Put the VM back on its checkpoint, so what a walk changed (the boot
        # walk's boot entry and crash settings) does not outlive the run, on a
        # failed step too (review, 2026-11-18). Restore-RefCheckpoint turns it
        # off first and attaches the media again from the VM's own folder, which
        # a plain restore of pre-oobe would leave pointing at the build folder.
        try { Restore-RefCheckpoint $Name $Checkpoint $VmPath -NoStart } catch {
            $why = "the VM could not be restored to $Checkpoint after the run: $($_.Exception.Message)"
            Write-Host "ref-host: $why"
            if (-not $failed) { $failed = $why }
            if ($code -eq 0) { $code = 1 }
        }
        Exit-RefTurn
    }
    # thumbnail: the revision of Save-Thumbnail's reading of Hyper-V's bytes;
    # host-capture.py refuses shots from any other (revision 2 skips the 4
    # bytes before the pixels, 2026-11-18).
    $doc = [ordered]@{ checkpoint = $Checkpoint; thumbnail = $script:ThumbnailRevision; shots = $shots; failed = $failed }
    $doc | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Out 'shots.json') -Encoding UTF8
}
Write-Host "ref-host: exit $code; $($shots.Count) shot(s) in $Out"
exit $code
