# run-qemu.ps1 - Launch Impossible OS in QEMU on Windows
#
# Parameters:
#   -Xres   Horizontal resolution (default: 1280)
#   -Yres   Vertical resolution   (default: 720)
#   -Accel  Accelerator: auto, whpx, tcg (default: auto)
#
# UTS Timer Behavior:
#   auto/whpx: CPUID detects hypervisor -> LAPIC timer selected
#   tcg:       CPUID -> "TCGTCGTCGTCG" -> PIT timer selected
#
# Usage:
#   Double-click run-qemu-kvm.bat          -> 1280x720, WHPX accel
#   Double-click run-qemu-tcg.bat          -> 1280x720, TCG (PIT timer)
#   Double-click run-windows-1080p.bat     -> 1920x1080  scale=1x
#   Double-click run-windows-1440p.bat     -> 2560x1440  scale=2x
#   Double-click run-windows-4k.bat        -> 3840x2160  scale=2x
#
# Prerequisites:
#   1. Install QEMU for Windows: https://qemu.weilnetz.de/w64/
#   2. Build in WSL2 first: bash scripts/build.sh
Param(
    [int]$Xres = 1280,
    [int]$Yres = 720,
    [ValidateSet('auto','whpx','tcg')]
    [string]$Accel = 'auto',
    [int]$Smp = 0,  # 0 = auto (2 for WHPX/KVM, 1 for TCG)
    [switch]$DebugTests,  # boot with debug=1 (unit + boot tests)
    [switch]$TestOnly,    # boot with test=1 (unit tests, then shutdown)
    [ValidateSet('', 'mm', 'fs', 'sched', 'ob', 'security', 'ipc', 'boot', 'abi', 'storage', 'exec', 'x86', 'desktop', 'ex', 'nls', 'knf', 'except', 'quota')]
    [string]$TestSuite = '',  # category filter: must match a kernel TEST_CAT_* enum
    [switch]$Quiet,       # suppress PASS lines, show FAIL + summary only
    [string]$ExtraArgs = '',  # additional QEMU arguments (e.g., "-machine pc,i8042=on")
    [switch]$CrashTest,   # boot with crash_test=1 (deliberate BSOD after desktop)
    [switch]$ErrorScreenTest,  # boot with error_screen_test=1 (trigger boot_fatal for QR/BSOD)
    [string]$UtestFilter = '', # limit user-mode launcher to one binary/glob (e.g. "test_syscall.exe" or "test_smoke_*.exe")
    [switch]$NoKernelTests,    # set test_kernel_skip=1: skip kernel TEST_CAT_* sweep, run only the user-mode launcher
    [switch]$NoUsermodeTests,  # set test_usermode_skip=1: skip user-mode launcher, run only the kernel TEST_CAT_* sweep
    [switch]$Monitor,          # expose QEMU HMP monitor on 127.0.0.1:$MonitorPort for scripts/qemu-screenshot.sh (desktop-UI screendump)
    [ValidateRange(1, 65535)]
    [int]$MonitorPort = 4444,  # HMP monitor TCP port; auto-enables -Monitor if set explicitly
    [string]$MonitorSocket = '',  # HMP monitor UNIX-socket path (Linux/WSL QEMU builds only); auto-enables -Monitor when set. Windows-native QEMU keeps TCP because WSL's AF_UNIX interop across the WSL/Windows boundary is flaky.
    [switch]$Headless          # suppress the local QEMU window (-display none); see the IOS_HEADLESS note below
)

# Headless: suppress the QEMU UI window, keep everything else identical.
#
# The switch OR the IOS_HEADLESS=1 environment variable enables it. The env
# form is what makes an unattended sweep bearable: run-all-tests.bat chains
# roughly twenty per-category QEMU launches, each of which opens and closes its
# own window, so an operator running the aggregate gets a window storm they
# cannot dismiss (closing one only lets the next open). CMD exports the
# variable once and every nested bat and PowerShell child inherits it, with no
# edit to any per-category bat.
#
# This does NOT make the guest headless. `-display none` disables only the
# local UI; the VGA device is still emulated, the guest still renders its
# framebuffer, and the desktop layer's screenshots still work because those go
# through the HMP `screendump` command over -Monitor, never through the window.
# Serial is on stdio, so the log is unaffected -- which is the whole point.
if ($env:IOS_HEADLESS -eq '1') { $Headless = $true }

# UX consistency: if the caller explicitly passed -MonitorPort or
# -MonitorSocket but forgot -Monitor, the non-default transport would be
# silently ignored and the screenshot script would later fail with an
# unexplained "monitor unreachable" error. Treat explicit settings as
# implicitly enabling -Monitor. Codex [L] quality review.
if ($PSBoundParameters.ContainsKey('MonitorPort') -and -not $Monitor) {
    $Monitor = $true
}
if ($MonitorSocket -and -not $Monitor) {
    $Monitor = $true
}

$ErrorActionPreference = "Stop"

# Resolve project root from this script's location (scripts/machines -> parent -> parent)
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"

# WSL-side absolute path of the repo, derived from this script's location so
# WSL commands never depend on a hardcoded home-relative repo path.
$WSL_PROJECT = (& wsl.exe -e wslpath -a "$PROJECT").Trim()

$DISK      = Join-Path $BUILD "system-disk.img"
$OVMF_CODE = Join-Path $BUILD "OVMF_CODE_4M.fd"
$OVMF_VARS = Join-Path $BUILD "OVMF_VARS_4M.fd"
$VARS_DEST = Join-Path $env:TEMP "OVMF_VARS_4M.fd"

# Ensure disk image exists
if (-not (Test-Path $DISK)) {
    Write-Host "Missing: $DISK" -ForegroundColor Red
    Write-Host "Run 'bash scripts/build.sh' in WSL2 first." -ForegroundColor Yellow
    pause; exit 1
}

# Auto-copy OVMF firmware from WSL system path if not in build/
if (-not (Test-Path $OVMF_CODE) -or -not (Test-Path $OVMF_VARS)) {
    Write-Host "Copying OVMF firmware to build/..." -ForegroundColor Yellow
    & wsl.exe bash -c "cp /usr/share/OVMF/OVMF_CODE_4M.fd '$WSL_PROJECT/build/'"
    & wsl.exe bash -c "cp /usr/share/OVMF/OVMF_VARS_4M.fd '$WSL_PROJECT/build/'"
    if (-not (Test-Path $OVMF_CODE)) {
        Write-Host "Failed to copy OVMF." -ForegroundColor Red
        pause; exit 1
    }
}

# OVMF_VARS: writable copy in TEMP, preserved across runs for NVRAM persistence.
# Use reset-qemu-nvram.bat to force a fresh copy after clean builds.
if (-not (Test-Path $VARS_DEST)) {
    Copy-Item -Path $OVMF_VARS -Destination $VARS_DEST
}

$Scale = if ($Yres -gt 2160) { 3 } elseif ($Yres -gt 1080) { 2 } else { 1 }
# Per-resolution VRAM: 1440p needs 32MB (14.7MB fb), 1080p needs 16MB.
# 720p fits in the default 8MB VGA VRAM.
# 4K uses bochs-display which manages VRAM automatically.
$VgaDevice = if ($Yres -ge 2160) { "bochs-display" } `
        elseif ($Yres -ge 1440) { "VGA,vgamem_mb=32" } `
        elseif ($Yres -ge 1080) { "VGA,vgamem_mb=16" } `
        else                    { "VGA" }

# Determine accelerator and CPU model
$CpuModel = if ($Accel -eq 'tcg') { 'qemu64' } else { 'Haswell' }
$TimerExpected = if ($Accel -eq 'tcg') { 'PIT (TCG path)' } else { 'LAPIC (HW accel path)' }

# SMP: default 2 CPUs for HW-accel (test SMP), 1 for TCG (too slow for multi-CPU)
if ($Smp -eq 0) {
    $Smp = if ($Accel -eq 'tcg') { 1 } else { 2 }
}

Write-Host "Launching Impossible OS at ${Xres}x${Yres} (HiDPI scale=${Scale}x)..." -ForegroundColor Green
Write-Host "  Accel:  $Accel" -ForegroundColor DarkGray
Write-Host "  CPUs:   $Smp" -ForegroundColor DarkGray
Write-Host "  Timer:  $TimerExpected" -ForegroundColor DarkGray
Write-Host "  Disk:   $DISK" -ForegroundColor DarkGray
Write-Host "  Device: $VgaDevice" -ForegroundColor DarkGray
Write-Host ""

# Find QEMU executable
$QEMU = "qemu-system-x86_64.exe"
if (-not (Get-Command $QEMU -ErrorAction SilentlyContinue)) {
    $QEMU = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (-not (Test-Path $QEMU)) {
        Write-Host "QEMU not found. Install from https://qemu.weilnetz.de/w64/" -ForegroundColor Red
        pause; exit 1
    }
}

# Build QEMU arguments
$QemuArgs = @()

# Accelerator
switch ($Accel) {
    'tcg'  { $QemuArgs += '-accel', 'tcg' }
    'whpx' { $QemuArgs += '-accel', 'whpx' }
    'auto' {
        # Try WHPX first (near-native), fall back to TCG (slow but works)
        $QemuArgs += '-accel', 'whpx', '-accel', 'tcg'
    }
}

$QemuArgs += '-smp', "$Smp"

$QemuArgs += @(
    '-cpu', $CpuModel,
    '-drive', "if=pflash,format=raw,readonly=on,file=$OVMF_CODE",
    '-drive', "if=pflash,format=raw,file=$VARS_DEST",
    '-drive', "id=disk0,file=$DISK,format=raw,if=none",
    '-device', 'ich9-ahci,id=ahci0',
    '-device', 'ide-hd,drive=disk0,bus=ahci0.0',
    '-m', '2G',
    '-serial', 'stdio',
    '-vga', 'none',
    '-device', "$VgaDevice,xres=$Xres,yres=$Yres",
    '-device', 'rtl8139,netdev=net0',
    '-netdev', 'user,id=net0',
    '-device', 'virtio-tablet-pci',
    '-rtc', 'base=localtime',
    '-no-reboot'
)

# Appended rather than folded into the array above so the VGA device line stays
# the single place the display hardware is described -- headless is a property
# of the HOST window, not of the emulated adapter.
if ($Headless) { $QemuArgs += @('-display', 'none') }

# HMP monitor exposure -- scripts/qemu-screenshot.sh connects here to
# issue `screendump` commands. Only wired when -Monitor is explicitly
# passed so normal WHPX runs do not expose an unsolicited TCP listener.
# `server,nowait` means QEMU accepts one client without blocking boot.
#
# Transport: -MonitorSocket takes precedence (UNIX domain socket, only
# reachable by the invoking user -- preferred on shared Linux hosts) and
# falls through to TCP otherwise. Windows-native QEMU keeps TCP because
# WSL's AF_UNIX interop across the WSL/Windows boundary is flaky and
# Windows named-pipe syntax would need a different QEMU arg anyway.
if ($Monitor) {
    if ($MonitorSocket) {
        $QemuArgs += '-monitor', "unix:${MonitorSocket},server,nowait"
    } else {
        $QemuArgs += '-monitor', "telnet:127.0.0.1:${MonitorPort},server,nowait"
    }
}

# Validate user-controlled boot.conf values BEFORE they flow into the
# WSL `bash -c` bridge + patch-boot-conf.sh's sed REPLACEMENT. The
# current invocation joins $PatchArgs with spaces into a bash -c string
# argument, so a value containing `;`, backticks, `$(...)`, `&`, `|`,
# `\n`, etc would execute inside WSL. sed-side metacharacters (/ & \)
# corrupt the replacement instead.
#
# -TestSuite: enforced via ValidateSet on the param attribute above
# (exact match against the kernel TEST_CAT_* enum names). This catches
# typos like `boot*` or `fs,ob` at parse time instead of letting them
# silently fall through to the kernel's invalid-suite sentinel. Codex
# quality 2026-04-21 M2.
#
# -UtestFilter: must be (a) shell/sed-safe and (b) <= 63 chars so it
# fits in struct boot_config.utest_filter[64] without silent truncation
# in the bootloader copy. The pattern allows literal binary names
# ("test_libc.exe") or single-`*` globs ("test_smoke_*.exe") -- the
# launcher's `test_usermode_glob_match()` honours exactly this subset.
# Codex quality 2026-04-21 M1.
$UtestFilterPattern = '^[A-Za-z0-9_.*-]{1,63}$'

if ($UtestFilter -and $UtestFilter -notmatch $UtestFilterPattern) {
    Write-Host "Invalid -UtestFilter value: '$UtestFilter'" -ForegroundColor Red
    Write-Host "  Must be 1-63 chars, alphanumerics + . _ - * only (literal name or single-* glob)" -ForegroundColor Red
    pause; exit 1
}

# Patch boot.conf if debug/test mode requested
$PatchArgs = @()
if ($DebugTests) { $PatchArgs += @('debug', '1') }
if ($TestOnly -or $TestSuite -or $UtestFilter -or $NoKernelTests -or $NoUsermodeTests) { $PatchArgs += @('test', '1') }
if ($TestSuite) { $PatchArgs += @('test_suite', $TestSuite) }
if ($Quiet)     { $PatchArgs += @('test_quiet', '1') }
if ($UtestFilter) { $PatchArgs += @('utest_filter', $UtestFilter) }
if ($NoKernelTests) { $PatchArgs += @('test_kernel_skip', '1') }
if ($NoUsermodeTests) { $PatchArgs += @('test_usermode_skip', '1') }
if ($CrashTest) { $PatchArgs += @('crash_test', '1') }
if ($ErrorScreenTest) { $PatchArgs += @('error_screen_test', '1') }

if ($PatchArgs.Count -gt 0) {
    $PatchStr = $PatchArgs -join ' '
    $ModeLabel = ($PatchArgs | ForEach-Object -Begin { $i=0 } -Process {
        if ($i % 2 -eq 0) { "$_=" } else { "$_ " }; $i++
    }) -join ''
    Write-Host "  Mode:   $($ModeLabel.Trim())" -ForegroundColor Cyan
    & wsl.exe bash -c "cd '$WSL_PROJECT' && bash scripts/patch-boot-conf.sh $PatchStr"
} else {
    # No test flags -- reset boot.conf to defaults in case a previous
    # test run was interrupted before its finally{} block could restore it.
    & wsl.exe bash -c "cd '$WSL_PROJECT' && bash scripts/patch-boot-conf.sh reset" 2>$null
}

# Append extra arguments if provided (e.g., "-machine pc,i8042=on", or
# `-drive "id=test,file=C:\path with space\disk.img"` for quoted paths).
# Quote-aware tokenizer: splits on whitespace but preserves substrings
# enclosed in double quotes so -drive arguments with space-bearing paths
# survive -- a plain .Split(' ') would corrupt them.
if ($ExtraArgs) {
    $QemuArgs += [regex]::Matches($ExtraArgs, '[^\s"]+|"([^"]*)"') | ForEach-Object {
        if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Value }
    }
}

# Watchdog: bound the run when IOS_QEMU_TIMEOUT_SEC is set.
#
# `& $QEMU @QemuArgs` blocks until the guest exits, with no bound. That is right
# for an interactive session -- you close the window when you are done -- but it
# is what makes the aggregate sweep unattended-UNSAFE: at least one category
# boots through to the desktop and idles instead of shutting down, so
# run-all-tests.bat stops there forever. Headless makes that stall silent rather
# than visible, which is worse: the only symptom is a log that stops growing.
#
# Opt-IN, so no interactive behavior changes. The aggregate sets it; a developer
# running one category by hand still gets an unbounded session. On expiry the
# process is killed and a non-zero exit is returned, so the caller records a
# stalled category and moves on instead of hanging the whole chain.
$TimeoutSec = 0
if ($env:IOS_QEMU_TIMEOUT_SEC) { [int]::TryParse($env:IOS_QEMU_TIMEOUT_SEC, [ref]$TimeoutSec) | Out-Null }

# ---- Own our child process -------------------------------------------------
# The QEMU we start is OURS to clean up. Before this, the no-timeout branch used
# a bare `& $QEMU`, which keeps no handle: when the caller killed this script --
# `timeout 200 powershell.exe ...` from WSL is the normal shape -- PowerShell
# died and QEMU was orphaned. Three such orphans accumulated on 2026-07-28 and
# held the OVMF flash file, so the next launch failed with
#   cannot set up guest memory 'system.flash0': Invalid argument
#
# Deliberately NOT "kill anything named qemu-system-x86_64.exe": this script
# cannot tell its own leftovers from a VM the operator is using, and an
# unattended sweep that guesses wrong kills work in progress. Only PIDs we
# recorded ourselves are ever touched, and the name is re-checked first so a
# recycled PID cannot be mistaken for ours.
$QemuPidFile = Join-Path $env:TEMP "impossible-os-qemu.pid"

# Self-heal: a hard kill never runs `finally`, so reap the previous run's child
# here instead. This is what makes the cleanup survive `timeout` and Ctrl-C.
# The file holds a LIST, one PID per line, not a single value. Two hard-killed
# runs in a row would otherwise lose the older orphan when the second overwrote
# the record -- observed while testing this very change, which left two live
# QEMUs and only one of them tracked.
if (Test-Path $QemuPidFile) {
    $reaped = 0
    foreach ($line in (Get-Content $QemuPidFile -ErrorAction SilentlyContinue)) {
        $stalePid = 0
        if (-not [int]::TryParse($line.Trim(), [ref]$stalePid)) { continue }
        try {
            $stale = Get-Process -Id $stalePid -ErrorAction SilentlyContinue
            # Name re-check is the PID-reuse guard: a recycled PID belonging to
            # some other program must never be killed just because we once
            # recorded that number. Verified against a live explorer.exe decoy.
            if ($stale -and $stale.ProcessName -eq 'qemu-system-x86_64') {
                Write-Host "run-qemu: reaping orphaned QEMU from a previous run (PID $stalePid)" -ForegroundColor DarkYellow
                try { $stale.Kill($true) } catch { try { $stale.Kill() } catch {} }
                $reaped++
            }
        } catch { }
    }
    if ($reaped -gt 0) { Start-Sleep -Milliseconds 500 }
    Remove-Item $QemuPidFile -ErrorAction SilentlyContinue
}

$proc = $null
try {
    if ($true) {
        # -NoNewWindow keeps the parent console, so `-serial stdio` still
        # streams into whatever redirect the caller set up -- the log this
        # sweep exists to produce must not change shape just because it is
        # now bounded. Both branches now go through Start-Process so there is
        # always a handle to clean up with; the only difference is whether the
        # wait is bounded.
        $proc = Start-Process -FilePath $QEMU -ArgumentList $QemuArgs -NoNewWindow -PassThru
        Add-Content -Path $QemuPidFile -Value $proc.Id -ErrorAction SilentlyContinue
        if ($TimeoutSec -le 0) {
            $proc.WaitForExit()
            $global:LASTEXITCODE = $proc.ExitCode
        }
        elseif (-not $proc.WaitForExit($TimeoutSec * 1000)) {
            Write-Host ""
            Write-Host "run-qemu: STALLED -- no exit after $TimeoutSec s; killing." -ForegroundColor Red
            Write-Host "  A category that reaches the desktop and idles is a bug in THAT bat" -ForegroundColor DarkGray
            Write-Host "  (it should boot -TestOnly and shut down), not in the sweep." -ForegroundColor DarkGray
            try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
            $global:LASTEXITCODE = 124   # conventional timeout status
        }
        else {
            $global:LASTEXITCODE = $proc.ExitCode
        }
    }
} finally {
    # Reap our own child on every exit path we can still observe. A SIGKILL of
    # this script skips this block entirely -- that case is covered by the
    # reap-on-start above, which is why both exist.
    if ($proc -and -not $proc.HasExited) {
        try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
    }
    Remove-Item $QemuPidFile -ErrorAction SilentlyContinue

    # Always restore boot.conf to defaults
    if ($PatchArgs.Count -gt 0) {
        & wsl.exe bash -c "cd '$WSL_PROJECT' && bash scripts/patch-boot-conf.sh reset"
    }
}
