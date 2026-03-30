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

# ── Toolchain auto-download ───────────────────────────────────────────────
# Downloads MinGW-w64 and WinFsp SDK into sdk/build/ on first run.
# No system-wide install, no environment variables, fully self-contained.

$MinGWUrl   = "https://github.com/niXman/mingw-builds-binaries/releases/download/14.2.0-rt_v12-rev1/x86_64-14.2.0-release-posix-seh-ucrt-rt_v12-rev1.7z"
$MinGWHash  = "x86_64-14.2.0-release-posix-seh-ucrt-rt_v12-rev1"
$WinFspUrl  = "https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25099.msi"

function Ensure-MinGW {
    $gcc = Join-Path $BuildDir "mingw64\bin\gcc.exe"
    if (Test-Path $gcc) { return }

    Write-Host "  SETUP" -ForegroundColor Cyan -NoNewline
    Write-Host " Downloading MinGW-w64 toolchain..."

    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }

    $archive = Join-Path $BuildDir "mingw64.7z"

    # Download
    Write-Host "       " -ForegroundColor DarkGray -NoNewline
    Write-Host "URL: $MinGWUrl"
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $MinGWUrl -OutFile $archive -UseBasicParsing
    $ProgressPreference = "Continue"

    # Extract — try 7z first, fall back to tar (Windows 10+ has tar with 7z support)
    Write-Host "       " -ForegroundColor DarkGray -NoNewline
    Write-Host "Extracting to sdk/build/mingw64/..."

    if (Get-Command 7z -ErrorAction SilentlyContinue) {
        & 7z x $archive -o"$BuildDir" -y | Out-Null
    } elseif (Get-Command tar -ErrorAction SilentlyContinue) {
        & tar -xf $archive -C $BuildDir
    } else {
        # .7z needs 7-Zip or tar; if neither available, try downloading as zip instead
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "Cannot extract .7z — install 7-Zip or use Windows 10+ (has tar)"
        Remove-Item $archive -Force -ErrorAction SilentlyContinue
        exit 1
    }

    Remove-Item $archive -Force -ErrorAction SilentlyContinue

    if (Test-Path $gcc) {
        $ver = & $gcc --version 2>&1 | Select-Object -First 1
        Write-Host "  OK   " -ForegroundColor Green -NoNewline
        Write-Host "MinGW installed: $ver"
    } else {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "MinGW extraction failed — gcc.exe not found"
        exit 1
    }
}

function Ensure-WinFsp {
    $header = Join-Path $BuildDir "winfsp\inc\winfsp\winfsp.h"
    if (Test-Path $header) { return }

    Write-Host "  SETUP" -ForegroundColor Cyan -NoNewline
    Write-Host " Downloading WinFsp SDK..."

    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }

    $msi = Join-Path $BuildDir "winfsp.msi"
    $extractDir = Join-Path $BuildDir "winfsp-extract"

    # Download
    Write-Host "       " -ForegroundColor DarkGray -NoNewline
    Write-Host "URL: $WinFspUrl"
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $WinFspUrl -OutFile $msi -UseBasicParsing
    $ProgressPreference = "Continue"

    # Extract headers + libs from MSI (admin-free extraction)
    Write-Host "       " -ForegroundColor DarkGray -NoNewline
    Write-Host "Extracting headers + libs from MSI..."

    if (-not (Test-Path $extractDir)) { New-Item -ItemType Directory -Path $extractDir -Force | Out-Null }
    & msiexec /a $msi /qn TARGETDIR="$extractDir" | Out-Null

    # Find and copy the SDK files (inc/ and lib/)
    $winfspDir = Join-Path $BuildDir "winfsp"
    if (-not (Test-Path $winfspDir)) { New-Item -ItemType Directory -Path $winfspDir -Force | Out-Null }

    # The MSI extracts to a nested path — find the inc/ directory
    $incSrc = Get-ChildItem -Path $extractDir -Recurse -Filter "winfsp.h" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($incSrc) {
        # Go up to the WinFsp root (parent of inc/winfsp/)
        $winfspRoot = $incSrc.Directory.Parent.Parent.FullName
        if (Test-Path (Join-Path $winfspRoot "inc")) {
            Copy-Item -Path (Join-Path $winfspRoot "inc") -Destination $winfspDir -Recurse -Force
        }
        if (Test-Path (Join-Path $winfspRoot "lib")) {
            Copy-Item -Path (Join-Path $winfspRoot "lib") -Destination $winfspDir -Recurse -Force
        }
    }

    # Cleanup
    Remove-Item $msi -Force -ErrorAction SilentlyContinue
    Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue

    if (Test-Path $header) {
        Write-Host "  OK   " -ForegroundColor Green -NoNewline
        Write-Host "WinFsp SDK installed to sdk/build/winfsp/"
    } else {
        Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
        Write-Host "WinFsp SDK extraction may have failed — winfsp.h not found"
        Write-Host "       " -ForegroundColor DarkGray -NoNewline
        Write-Host "Tools requiring WinFsp may fail to build"
    }
}

# Auto-download toolchains if needed (skip during clean)
if (-not $clean) {
    Ensure-MinGW
    Ensure-WinFsp
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
    Write-Host "No C compiler found — toolchain download may have failed."
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

    # Remove downloaded toolchains for a full reset
    if (Test-Path $BuildDir) {
        Write-Host "  Removing sdk/build/ (downloaded toolchains)..."
        Remove-Item $BuildDir -Recurse -Force
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
