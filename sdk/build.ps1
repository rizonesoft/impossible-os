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
    $repoRoot = Split-Path -Parent $SdkRoot
    foreach ($letter in [char[]]('Z','Y','X','W','V','U','T','S','R','Q','P','O','N','M','L','K','J','I','H','G')) {
        $drive = "${letter}:"
        if (-not (Test-Path $drive)) {
            & subst $drive $repoRoot 2>$null
            if ($LASTEXITCODE -eq 0) {
                $MappedDrive = $drive
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
function Write-Divider  { Write-Host ("-" * 60) -ForegroundColor DarkGray }
function Write-Header   { Write-Host ("=" * 60) -ForegroundColor White }

function Format-Elapsed($sw) {
    return "{0:F1}" -f $sw.Elapsed.TotalSeconds
}

function Write-ProgressBar {
    param([int]$Percent, [string]$Label)
    $barWidth = 30
    $filled = [math]::Floor($barWidth * $Percent / 100)
    $empty = $barWidth - $filled
    $bar = ("#" * $filled) + ("-" * $empty)
    Write-Host "`r       [$bar] ${Percent}% $Label      " -ForegroundColor DarkGray -NoNewline
}

# -- Toolchain auto-download -----------------------------------------------
# Downloads MinGW-w64 into sdk/build/ on first run from official sources.
# WinFsp headers + lib are vendored in sdk/include/ and sdk/lib/.

# Official MinGW-w64 release (github.com/niXman/mingw-builds-binaries)
$MinGWVersion = "14.2.0-rt_v12-rev1"
$MinGWFile    = "x86_64-14.2.0-release-posix-seh-ucrt-rt_v12-rev1.7z"
$MinGWUrl     = "https://github.com/niXman/mingw-builds-binaries/releases/download/$MinGWVersion/$MinGWFile"

# Official 7-Zip console version (7-zip.org)
$SevenZipUrl  = "https://7-zip.org/a/7zr.exe"

# Download with progress (ships with Windows 10+)
function Download-WithProgress {
    param([string]$Url, [string]$OutFile, [string]$Label)

    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        Write-Host "       Downloading $Label..." -ForegroundColor DarkGray
        # Use default curl progress (not --progress-bar which can show garbled chars)
        & curl.exe -L -# -o $OutFile $Url 2>&1 | ForEach-Object {
            # curl outputs progress to stderr; just let it flow
        }
        # Check if download succeeded
        if (-not (Test-Path $OutFile) -or (Get-Item $OutFile).Length -eq 0) {
            throw "Download produced empty or missing file"
        }
    } else {
        # Fallback: PowerShell native with Write-Progress
        Write-Host "       Downloading $Label..." -ForegroundColor DarkGray
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $wc = New-Object System.Net.WebClient
        $dlDone = $false
        Register-ObjectEvent -InputObject $wc -EventName DownloadProgressChanged -Action {
            Write-Progress -Activity "Downloading" -Status "$($EventArgs.ProgressPercentage)%" -PercentComplete $EventArgs.ProgressPercentage
        } | Out-Null
        Register-ObjectEvent -InputObject $wc -EventName DownloadFileCompleted -Action {
            $script:dlDone = $true
        } | Out-Null
        $wc.DownloadFileAsync([Uri]$Url, $OutFile)
        while (-not $dlDone) { Start-Sleep -Milliseconds 200 }
        Write-Progress -Activity "Downloading" -Completed
        $wc.Dispose()
    }

    if (Test-Path $OutFile) {
        $sizeMB = [math]::Round((Get-Item $OutFile).Length / 1MB, 1)
        Write-Host "       Downloaded ${sizeMB} MB" -ForegroundColor DarkGray
    }
}

# Step 1: Ensure 7-Zip extractor is available
function Ensure-7Zip {
    # Check local sdk/build/7zr.exe first (standalone console extractor)
    $local7z = Join-Path $BuildDir "7zr.exe"
    if (Test-Path $local7z) { return $local7z }

    # Check system 7-Zip
    if (Get-Command 7z.exe -ErrorAction SilentlyContinue) { return "7z.exe" }
    if (Test-Path "C:\Program Files\7-Zip\7z.exe") { return "C:\Program Files\7-Zip\7z.exe" }
    if (Test-Path "C:\Program Files (x86)\7-Zip\7z.exe") { return "C:\Program Files (x86)\7-Zip\7z.exe" }

    # Download 7zr.exe from official 7-zip.org (standalone ~1MB, no installation needed)
    Write-Host "  SETUP" -ForegroundColor Cyan -NoNewline
    Write-Host " Downloading 7-Zip extractor (7-zip.org)..."

    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }

    try {
        Download-WithProgress -Url $SevenZipUrl -OutFile $local7z -Label "7zr.exe"
    } catch {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "7-Zip download failed: $_"
        Exit-Build 1
    }

    if (Test-Path $local7z) {
        Write-Host "  OK   " -ForegroundColor Green -NoNewline
        Write-Host "7-Zip extractor ready"
        return $local7z
    } else {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "7zr.exe download failed"
        Exit-Build 1
    }
}

# Step 2: Ensure MinGW-w64 is available
function Ensure-MinGW {
    param([string]$SevenZip)

    $gcc = Join-Path $BuildDir "mingw64\bin\gcc.exe"
    if (Test-Path $gcc) { return }

    Write-Host "  SETUP" -ForegroundColor Cyan -NoNewline
    Write-Host " Installing MinGW-w64 GCC $MinGWVersion..."
    Write-Host "       Source: github.com/niXman/mingw-builds-binaries" -ForegroundColor DarkGray

    if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null }

    $archive = Join-Path $BuildDir "mingw64.7z"

    try {
        Download-WithProgress -Url $MinGWUrl -OutFile $archive -Label "MinGW-w64 ($MinGWFile)"
    } catch {
        Write-Host "  FAIL " -ForegroundColor Red -NoNewline
        Write-Host "Download failed: $_"
        Exit-Build 1
    }

    # Extract with progress
    Write-Host "       Extracting MinGW-w64..." -ForegroundColor DarkGray
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    # 7zr.exe outputs percentage lines like " 12% - mingw64/bin/gcc.exe"
    # Parse these for progress updates
    $lastPct = -1
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo.FileName = $SevenZip
    $proc.StartInfo.Arguments = "x `"$archive`" -o`"$BuildDir`" -y"
    $proc.StartInfo.UseShellExecute = $false
    $proc.StartInfo.RedirectStandardOutput = $true
    $proc.StartInfo.RedirectStandardError = $true
    $proc.StartInfo.CreateNoWindow = $true
    $proc.Start() | Out-Null

    while (-not $proc.HasExited) {
        $line = $proc.StandardOutput.ReadLine()
        if ($line -match "^\s*(\d+)%") {
            $pct = [int]$Matches[1]
            if ($pct -ne $lastPct) {
                $lastPct = $pct
                Write-ProgressBar -Percent $pct -Label "extracting"
            }
        }
    }
    # Drain remaining output
    $proc.StandardOutput.ReadToEnd() | Out-Null
    $proc.WaitForExit()
    Write-ProgressBar -Percent 100 -Label "done"
    Write-Host ""

    $sw.Stop()
    Write-Host "       Extracted in $("{0:F1}" -f $sw.Elapsed.TotalSeconds)s" -ForegroundColor DarkGray

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

# Auto-download MinGW if needed (skip during clean)
# WinFsp headers + lib are vendored in sdk/include/ and sdk/lib/
if (-not $clean) {
    $SevenZip = Ensure-7Zip
    Ensure-MinGW -SevenZip $SevenZip
}

# -- Detect compiler -------------------------------------------------------
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
    $vendoredHeader = Join-Path $SdkRoot "include\winfsp\winfsp.h"
    $vendoredLib = Join-Path $SdkRoot "lib\winfsp-x64.lib"

    if ((Test-Path $vendoredHeader) -and (Test-Path $vendoredLib)) {
        Write-Host "  INFO " -ForegroundColor DarkGray -NoNewline
        Write-Host "WinFsp SDK: headers + lib vendored in sdk/"
    } else {
        Write-Host "  WARN " -ForegroundColor Yellow -NoNewline
        Write-Host "MISSING: WinFsp SDK files in sdk/include/ or sdk/lib/"
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

$toolNames = ($toolDirs | ForEach-Object { Split-Path -Leaf $_ }) -join ", "
Write-Host "  Found " -NoNewline
Write-Host "$toolCount" -ForegroundColor Cyan -NoNewline
Write-Host " SDK tools: $toolNames"
Write-Divider

# Build each tool with progress
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

    # SHELL=cmd.exe prevents mingw32-make from using /usr/bin/sh (WSL)
    $buildOutput = & $MAKE -C $dir CC="$CC" OUTDIR="$ToolsDir" SHELL=cmd.exe --no-print-directory 2>&1
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

        Write-Divider
        $errors = $buildOutput | Select-String -Pattern "(error:|undefined reference|fatal error|cannot find)" | Select-Object -First 20
        if ($errors) {
            $errors | ForEach-Object { Write-Host $_.Line }
        } else {
            $buildOutput | ForEach-Object { Write-Host $_ }
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
