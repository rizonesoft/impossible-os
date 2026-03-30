# sdk/build.ps1 — Build all SDK tools on Windows with progress bar, timing, and error extraction.
#
# Usage:
#   sdk\build.bat          Build all SDK tools
#   sdk\build.bat clean    Clean all SDK tool build artifacts
#
# Output last line is always one of:
#   === SDK BUILD OK ===
#   === SDK BUILD FAILED ===

param(
    [switch]$clean
)

# Also accept "clean" as a positional argument
if ($args -contains "clean") { $clean = $true }

$ErrorActionPreference = "Stop"

# ── Resolve paths ──────────────────────────────────────────────────────────
$SdkRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir    = Join-Path $SdkRoot "src"
$ToolsDir  = Join-Path $SdkRoot "tools"
$BuildDir  = Join-Path $SdkRoot "build"

if (-not (Test-Path $ToolsDir)) { New-Item -ItemType Directory -Path $ToolsDir -Force | Out-Null }

# ── Helpers ────────────────────────────────────────────────────────────────
function Write-Divider  { Write-Host ("─" * 50) -ForegroundColor DarkGray }
function Write-Header   { Write-Host ("═" * 50) -ForegroundColor White }

function Format-Elapsed($sw) {
    return "{0:F1}" -f $sw.Elapsed.TotalSeconds
}

# ── Detect compiler ───────────────────────────────────────────────────────
# Prefer local MinGW from sdk/build/, fall back to system PATH
$LocalGcc   = Join-Path $BuildDir "mingw64\bin\gcc.exe"
$LocalMake  = Join-Path $BuildDir "mingw64\bin\mingw32-make.exe"

$CC   = $null
$MAKE = $null

if (Test-Path $LocalGcc) {
    $CC   = $LocalGcc
    $MAKE = $LocalMake
    $CompilerSource = "sdk/build/mingw64"
} elseif (Get-Command gcc -ErrorAction SilentlyContinue) {
    $CC   = "gcc"
    $MAKE = if (Get-Command mingw32-make -ErrorAction SilentlyContinue) { "mingw32-make" }
            elseif (Get-Command make -ErrorAction SilentlyContinue) { "make" }
            else { $null }
    $CompilerSource = "system PATH"
} elseif (Get-Command clang -ErrorAction SilentlyContinue) {
    $CC   = "clang"
    $MAKE = if (Get-Command make -ErrorAction SilentlyContinue) { "make" } else { $null }
    $CompilerSource = "system PATH"
}

if (-not $CC) {
    Write-Host "  FAIL " -ForegroundColor Red -NoNewline
    Write-Host "No C compiler found. Run build.bat to auto-download MinGW (see TODO-01 §5)."
    Write-Host "=== SDK BUILD FAILED ==="
    exit 1
}

if (-not $MAKE) {
    Write-Host "  FAIL " -ForegroundColor Red -NoNewline
    Write-Host "No make found (need mingw32-make or make)."
    Write-Host "=== SDK BUILD FAILED ==="
    exit 1
}

# ── Check optional dependencies ───────────────────────────────────────────
function Check-Dependencies {
    $WinFspLocal = Join-Path $BuildDir "winfsp\inc\winfsp\winfsp.h"
    $WinFspSystem = "C:\Program Files (x86)\WinFsp\inc\winfsp\winfsp.h"

    if (Test-Path $WinFspLocal) {
        Write-Host "  INFO " -ForegroundColor DarkGray -NoNewline
        Write-Host "WinFsp SDK found (sdk/build/winfsp)"
    } elseif (Test-Path $WinFspSystem) {
        Write-Host "  INFO " -ForegroundColor DarkGray -NoNewline
        Write-Host "WinFsp found (system install)"
    } else {
        Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
        Write-Host "MISSING: WinFsp SDK — run build.bat to auto-download (see TODO-01 §5), or install from: https://winfsp.dev/"
        Write-Host "       " -ForegroundColor DarkGray -NoNewline
        Write-Host "Tools requiring WinFsp may fail to build"
    }
}

# ── Discover SDK tool directories ─────────────────────────────────────────
function Get-ToolDirs {
    $tools = @()
    if (Test-Path $SrcDir) {
        Get-ChildItem -Path $SrcDir -Directory | Sort-Object Name | ForEach-Object {
            $makefile = Join-Path $_.FullName "Makefile"
            if (Test-Path $makefile) {
                $tools += $_.FullName
            }
        }
    }
    return $tools
}

# ── Clean ──────────────────────────────────────────────────────────────────
if ($clean) {
    Write-Header
    Write-Host "  SDK CLEAN" -ForegroundColor White
    Write-Header

    $toolDirs = Get-ToolDirs
    foreach ($dir in $toolDirs) {
        $name = Split-Path -Leaf $dir
        Write-Host "  Cleaning $name..."
        & $MAKE -C $dir clean OUTDIR="$ToolsDir" CC="$CC" --no-print-directory 2>$null
    }

    Write-Host "  OK   " -ForegroundColor Green -NoNewline
    Write-Host "Clean complete"
    Write-Host "=== SDK BUILD OK ==="
    exit 0
}

# ── Build ──────────────────────────────────────────────────────────────────
$buildTimer = [System.Diagnostics.Stopwatch]::StartNew()

Write-Header
Write-Host "  SDK BUILD" -ForegroundColor White
Write-Header

Write-Host "  Compiler: " -NoNewline
Write-Host "$CC" -ForegroundColor Cyan -NoNewline
Write-Host " ($CompilerSource)"
Write-Divider

Check-Dependencies
Write-Divider

# Discover tools
$toolDirs = Get-ToolDirs
$toolCount = $toolDirs.Count

if ($toolCount -eq 0) {
    Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
    Write-Host "No SDK tools found in $SrcDir"
    Write-Host "       Each tool needs a Makefile in its directory"
    Write-Divider
    Write-Host "  OK   " -ForegroundColor Green -NoNewline
    Write-Host "Nothing to build"
    Write-Host "=== SDK BUILD OK ==="
    exit 0
}

# Report discovered tools
$toolNames = ($toolDirs | ForEach-Object { Split-Path -Leaf $_ }) -join ", "
Write-Host "  Found " -NoNewline
Write-Host "$toolCount" -ForegroundColor Cyan -NoNewline
Write-Host " SDK tools: $toolNames"
Write-Divider

# Build each tool
$failCount = 0
$idx = 0

foreach ($dir in $toolDirs) {
    $idx++
    $name = Split-Path -Leaf $dir
    $toolTimer = [System.Diagnostics.Stopwatch]::StartNew()

    Write-Host "  " -NoNewline
    Write-Host "[$idx/$toolCount]" -ForegroundColor White -NoNewline
    Write-Host " Building " -NoNewline
    Write-Host "$name" -ForegroundColor Cyan -NoNewline
    Write-Host "..." -NoNewline

    # Capture make output for error extraction
    $buildOutput = & $MAKE -C $dir CC="$CC" OUTDIR="$ToolsDir" --no-print-directory 2>&1
    $buildRC = $LASTEXITCODE

    $toolTimer.Stop()
    $elapsed = Format-Elapsed $toolTimer

    if ($buildRC -eq 0) {
        Write-Host " OK " -ForegroundColor Green -NoNewline
        Write-Host "(${elapsed}s)"
    } else {
        Write-Host " FAILED " -ForegroundColor Red -NoNewline
        Write-Host "(${elapsed}s)"
        $failCount++

        # Extract relevant compiler errors
        Write-Divider
        $buildOutput | Select-String -Pattern "(error:|undefined reference|fatal error|cannot find)" | Select-Object -First 20 | ForEach-Object {
            Write-Host $_.Line
        }
        Write-Divider
    }
}

# ── Summary ────────────────────────────────────────────────────────────────
Write-Divider
$buildTimer.Stop()
$totalElapsed = Format-Elapsed $buildTimer

if ($failCount -eq 0) {
    Write-Host "  " -NoNewline
    Write-Host "SDK BUILD OK" -ForegroundColor Green -NoNewline
    Write-Host " — $toolCount tools built in ${totalElapsed}s"
    Write-Header
    Write-Host "=== SDK BUILD OK ==="
    exit 0
} else {
    Write-Host "  " -NoNewline
    Write-Host "SDK BUILD FAILED" -ForegroundColor Red -NoNewline
    Write-Host " — $failCount/$toolCount tools failed (${totalElapsed}s)"
    Write-Header
    Write-Host "=== SDK BUILD FAILED ==="
    exit 1
}
