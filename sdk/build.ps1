# sdk/build.ps1 -- Build all SDK tools on Windows with progress bar, timing, and error extraction.
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

# Use Continue -- "Stop" causes PowerShell to abort on ANY stderr output
# from external commands (gcc warnings, etc.), which kills the script silently.
$ErrorActionPreference = "Continue"

# -- Resolve paths ----------------------------------------------------------
$SdkRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcDir    = Join-Path $SdkRoot "src"
$ToolsDir  = Join-Path $SdkRoot "tools"
$BuildDir  = Join-Path $SdkRoot "build"

# -- UNC path workaround ---------------------------------------------------
# CMD.EXE and mingw32-make cannot use UNC paths (\\wsl.localhost\...) as CWD.
# If we're on a UNC path, map a temporary drive letter via subst.
$MappedDrive = $null
if ($SdkRoot -like "\\*") {
    # Find a free drive letter (Z: down to G:)
    $repoRoot = Split-Path -Parent $SdkRoot
    foreach ($letter in [char[]]('Z','Y','X','W','V','U','T','S','R','Q','P','O','N','M','L','K','J','I','H','G')) {
        $drive = "${letter}:"
        if (-not (Test-Path $drive)) {
            & subst $drive $repoRoot 2>$null
            if ($LASTEXITCODE -eq 0) {
                $MappedDrive = $drive
                # Rebase all paths to the mapped drive
                $SdkRoot  = Join-Path $drive "sdk"
                $SrcDir   = Join-Path $SdkRoot "src"
                $ToolsDir = Join-Path $SdkRoot "tools"
                $BuildDir = Join-Path $SdkRoot "build"
                Write-Host "  INFO " -ForegroundColor DarkGray -NoNewline
                Write-Host "Mapped UNC path to $drive"
                break
            }
        }
    }
    if (-not $MappedDrive) {
        Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
        Write-Host "Could not map UNC path to drive letter -- builds may fail"
    }
}

# Cleanup mapped drive on exit
function Cleanup-Drive {
    if ($script:MappedDrive) {
        & subst /d $script:MappedDrive 2>$null | Out-Null
    }
}
trap { Cleanup-Drive; break }

function Exit-Build($code) {
    Cleanup-Drive
    exit $code
}

if (-not (Test-Path $ToolsDir)) { New-Item -ItemType Directory -Path $ToolsDir -Force | Out-Null }

# -- Helpers ----------------------------------------------------------------
function Write-Divider  { Write-Host ("-" * 50) -ForegroundColor DarkGray }
function Write-Header   { Write-Host ("=" * 50) -ForegroundColor White }

function Format-Elapsed($sw) {
    return "{0:F1}" -f $sw.Elapsed.TotalSeconds
}

# -- Toolchain auto-download -----------------------------------------------
# Downloads MinGW-w64 and WinFsp SDK into sdk/build/ on first run.
# No system-wide install, no environment variables, fully self-contained.

# Pre-packaged build tools hosted on CDN -- no GitHub redirects, no MSI extraction
$CdnBase    = "https://impossible-storage.b-cdn.net/dev/sdk/build"
$SevenZipUrl = "$CdnBase/7z.zip"
$MinGWUrl    = "$CdnBase/mingw64.7z"
$WinFspUrl   = "$CdnBase/WinFsp.7z"

# Download a file with progress using curl.exe (ships with Windows 10+)
function Download-WithProgress {
    param([string]$Url, [string]$OutFile, [string]$Label)

    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        Write-Host "       Downloading $Label..." -ForegroundColor DarkGray
        & curl.exe -L --progress-bar -o $OutFile $Url
        if ($LASTEXITCODE -ne 0) {
            throw "curl failed with exit code $LASTEXITCODE"
        }
        return
    }

    # Fallback: Invoke-WebRequest (no progress bar, but works)
    Write-Host "       Downloading $Label (no progress -- install curl for progress bar)..." -ForegroundColor DarkGray
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
    $ProgressPreference = "Continue"
}

# Step 1: Ensure 7z.exe is available (needed to extract .7z archives)
function Ensure-7Zip {
    # Check local sdk/build/7z/7z.exe first
    $local7z = Join-Path $BuildDir "7z\7z.exe"
    if (Test-Path $local7z) { return $local7z }

    # Check system 7-Zip
    if (Get-Command 7z.exe -ErrorAction SilentlyContinue) { return "7z.exe" }
    if (Test-Path "C:\Program Files\7-Zip\7z.exe") { return "C:\Program Files\7-Zip\7z.exe" }
    if (Test-Path "C:\Program Files (x86)\7-Zip\7z.exe") { return "C:\Program Files (x86)\7-Zip\7z.exe" }

    # Download 7z.zip from CDN (small, extracts with Expand-Archive)
    Write-Host "  SETUP" -ForegroundColor Cyan -NoNewline
    Write-Host " Downloading 7-Zip..."

    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }

    $zipFile = Join-Path $BuildDir "7z.zip"
    try {
        Download-WithProgress -Url $SevenZipUrl -OutFile $zipFile -Label "7-Zip"
    } catch {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "7-Zip download failed: $_"
        Exit-Build 1
    }

    $destDir = Join-Path $BuildDir "7z"
    Expand-Archive -Path $zipFile -DestinationPath $destDir -Force
    Remove-Item $zipFile -Force -ErrorAction SilentlyContinue

    if (Test-Path $local7z) {
        Write-Host "  OK   " -ForegroundColor Green -NoNewline
        Write-Host "7-Zip installed to sdk/build/7z/"
        return $local7z
    } else {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "7-Zip extraction failed -- 7z.exe not found"
        Exit-Build 1
    }
}

# Step 2: Ensure MinGW-w64 is available
function Ensure-MinGW {
    param([string]$SevenZip)

    $gcc = Join-Path $BuildDir "mingw64\bin\gcc.exe"
    if (Test-Path $gcc) { return }

    Write-Host "  SETUP" -ForegroundColor Cyan -NoNewline
    Write-Host " Downloading MinGW-w64 toolchain..."

    $archive = Join-Path $BuildDir "mingw64.7z"

    try {
        Download-WithProgress -Url $MinGWUrl -OutFile $archive -Label "MinGW-w64"
    } catch {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "Download failed: $_"
        Exit-Build 1
    }

    Write-Host "       Extracting to sdk/build/mingw64/..." -ForegroundColor DarkGray
    & $SevenZip x $archive "-o$BuildDir" -y 2>&1 | Out-Null

    Remove-Item $archive -Force -ErrorAction SilentlyContinue

    if (Test-Path $gcc) {
        $ver = & $gcc --version 2>&1 | Select-Object -First 1
        Write-Host "  OK   " -ForegroundColor Green -NoNewline
        Write-Host "MinGW installed: $ver"
    } else {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "MinGW extraction failed -- gcc.exe not found"
        Exit-Build 1
    }
}

# Step 3: Ensure WinFsp SDK is available
function Ensure-WinFsp {
    param([string]$SevenZip)

    $header = Join-Path $BuildDir "WinFsp\inc\winfsp\winfsp.h"
    if (Test-Path $header) { return }

    Write-Host "  SETUP" -ForegroundColor Cyan -NoNewline
    Write-Host " Downloading WinFsp SDK..."

    $archive = Join-Path $BuildDir "WinFsp.7z"

    try {
        Download-WithProgress -Url $WinFspUrl -OutFile $archive -Label "WinFsp SDK"
    } catch {
        Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
        Write-Host "WinFsp download failed: $_"
        Write-Host "       Tools requiring WinFsp may fail to build" -ForegroundColor DarkGray
        return
    }

    Write-Host "       Extracting to sdk/build/WinFsp/..." -ForegroundColor DarkGray
    & $SevenZip x $archive "-o$BuildDir" -y 2>&1 | Out-Null

    Remove-Item $archive -Force -ErrorAction SilentlyContinue

    if (Test-Path $header) {
        Write-Host "  OK   " -ForegroundColor Green -NoNewline
        Write-Host "WinFsp SDK installed to sdk/build/WinFsp/"
    } else {
        Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
        Write-Host "WinFsp SDK extraction incomplete -- winfsp.h not found"
        Write-Host "       Install WinFsp manually from https://winfsp.dev/" -ForegroundColor DarkGray
    }
}

# Auto-download toolchains if needed (skip during clean)
if (-not $clean) {
    $SevenZip = Ensure-7Zip
    Ensure-MinGW -SevenZip $SevenZip
    Ensure-WinFsp -SevenZip $SevenZip
}

# -- Detect compiler -------------------------------------------------------
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
    Write-Host "No C compiler found -- toolchain download may have failed."
    Write-Host "=== SDK BUILD FAILED ==="
    Exit-Build 1
}

if (-not $MAKE) {
    Write-Host "  FAIL " -ForegroundColor Red -NoNewline
    Write-Host "No make found (need mingw32-make or make)."
    Write-Host "=== SDK BUILD FAILED ==="
    Exit-Build 1
}

# -- Check optional dependencies -------------------------------------------
function Check-Dependencies {
    $WinFspLocal = Join-Path $BuildDir "WinFsp\inc\winfsp\winfsp.h"
    $WinFspSystem = "C:\Program Files (x86)\WinFsp\inc\winfsp\winfsp.h"

    if (Test-Path $WinFspLocal) {
        Write-Host "  INFO " -ForegroundColor DarkGray -NoNewline
        Write-Host "WinFsp SDK found (sdk/build/winfsp)"
    } elseif (Test-Path $WinFspSystem) {
        Write-Host "  INFO " -ForegroundColor DarkGray -NoNewline
        Write-Host "WinFsp found (system install)"
    } else {
        Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
        Write-Host "MISSING: WinFsp SDK -- run build.bat to auto-download (see TODO-01 S5), or install from: https://winfsp.dev/"
        Write-Host "       " -ForegroundColor DarkGray -NoNewline
        Write-Host "Tools requiring WinFsp may fail to build"
    }
}

# -- Discover SDK tool directories -----------------------------------------
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

# -- Clean ------------------------------------------------------------------
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
    Exit-Build 0
}

# -- Build ------------------------------------------------------------------
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
    Exit-Build 0
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

# -- Summary ----------------------------------------------------------------
Write-Divider
$buildTimer.Stop()
$totalElapsed = Format-Elapsed $buildTimer

if ($failCount -eq 0) {
    Write-Host "  " -NoNewline
    Write-Host "SDK BUILD OK" -ForegroundColor Green -NoNewline
    Write-Host " -- $toolCount tools built in ${totalElapsed}s"
    Write-Header
    Write-Host "=== SDK BUILD OK ==="
    Exit-Build 0
} else {
    Write-Host "  " -NoNewline
    Write-Host "SDK BUILD FAILED" -ForegroundColor Red -NoNewline
    Write-Host " -- $failCount/$toolCount tools failed (${totalElapsed}s)"
    Write-Header
    Write-Host "=== SDK BUILD FAILED ==="
    Exit-Build 1
}
