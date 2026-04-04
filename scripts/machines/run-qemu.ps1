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
#   -SerialLog                             -> tee serial to scripts/debug/serial.log
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
    [string]$TestSuite = '',  # category filter: mm, fs, ob, security, ipc, sched, boot, abi, storage
    [switch]$Quiet,       # suppress PASS lines, show FAIL + summary only
    [string]$ExtraArgs = '',  # additional QEMU arguments (e.g., "-machine pc,i8042=on")
    [switch]$CrashTest,   # boot with crash_test=1 (deliberate BSOD after desktop)
    [switch]$SerialLog    # tee serial output to scripts/debug/serial.log
)

$ErrorActionPreference = "Stop"

# Resolve project root from this script's location (scripts/machines -> parent -> parent)
$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Path
$PROJECT    = Split-Path -Parent (Split-Path -Parent $SCRIPT_DIR)
$BUILD      = Join-Path $PROJECT "build"

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
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_CODE_4M.fd ~/impossible-os/build/'
    & wsl.exe bash -c 'cp /usr/share/OVMF/OVMF_VARS_4M.fd ~/impossible-os/build/'
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

# Patch boot.conf if debug/test mode requested
$PatchArgs = @()
if ($DebugTests) { $PatchArgs += @('debug', '1') }
if ($TestOnly -or $TestSuite) { $PatchArgs += @('test', '1') }
if ($TestSuite) { $PatchArgs += @('test_suite', $TestSuite) }
if ($Quiet)     { $PatchArgs += @('test_quiet', '1') }
if ($CrashTest) { $PatchArgs += @('crash_test', '1') }

if ($PatchArgs.Count -gt 0) {
    $PatchStr = $PatchArgs -join ' '
    $ModeLabel = ($PatchArgs | ForEach-Object -Begin { $i=0 } -Process {
        if ($i % 2 -eq 0) { "$_=" } else { "$_ " }; $i++
    }) -join ''
    Write-Host "  Mode:   $($ModeLabel.Trim())" -ForegroundColor Cyan
    & wsl.exe bash -c "cd ~/impossible-os && bash scripts/patch-boot-conf.sh $PatchStr"
} else {
    # No test flags -- reset boot.conf to defaults in case a previous
    # test run was interrupted before its finally{} block could restore it.
    & wsl.exe bash -c "cd ~/impossible-os && bash scripts/patch-boot-conf.sh reset" 2>$null
}

# Append extra arguments if provided (e.g., "-machine pc,i8042=on")
if ($ExtraArgs) {
    $QemuArgs += $ExtraArgs.Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)
}

$SerialLogPath = Join-Path $PROJECT "scripts\debug\serial.log"

try {
    # Temporarily allow native command stderr (QEMU emits harmless warnings
    # like "Ignoring request for interrupt vector 0" that PowerShell treats
    # as terminating errors when $ErrorActionPreference is Stop).
    $ErrorActionPreference = "SilentlyContinue"
    if ($SerialLog) {
        Write-Host "  Serial: -> $SerialLogPath" -ForegroundColor DarkGray
        & $QEMU @QemuArgs 2>$null | Tee-Object -FilePath $SerialLogPath
    } else {
        & $QEMU @QemuArgs
    }
    $ErrorActionPreference = "Stop"
} finally {
    # Always restore boot.conf to defaults
    if ($PatchArgs.Count -gt 0) {
        & wsl.exe bash -c "cd ~/impossible-os && bash scripts/patch-boot-conf.sh reset"
    }
}
