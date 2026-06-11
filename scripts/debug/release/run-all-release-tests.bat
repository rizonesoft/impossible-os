@echo off
:: run-all-release-tests.bat -- Windows-host release tooling aggregate.
::
:: Chains the NON-DESTRUCTIVE release-side tests for a Windows host
:: (PowerShell 5.1+). Excludes write-usb.ps1 because it is destructive
:: and interactive (requires a real USB stick + double confirmation);
:: launch run-write-usb.bat by hand for that.
::
:: Each step exits non-zero on failure; the bat aborts and reports
:: which step failed. Mirrors run-all-kernel-tests.bat shape.

setlocal EnableDelayedExpansion
set FAILS=0
:: pushd auto-maps UNC working dirs to a drive letter so the spawned
:: PS1s see a real local CWD (\\wsl.localhost\... is unsupported as a
:: CMD CWD). Without this every relative path in the chained PS1s
:: fails because CMD silently falls back to C:\Windows.
pushd "%~dp0"
set ROOT=%~dp0..\..\..

:: Exit-code contract for the chained PS1 peers (matches their docstrings):
::   0  test passed
::   1  test failed (real regression)
::   2  usage / missing tool / unmet prereq -- environmental, not a regression
::
:: rc=2 is treated as [SKIP] (not [FAIL]) so the aggregator stays useful on
:: hosts that don't have the full Windows-side toolchain installed. The
:: PS1 already prints `[ERROR] missing tool: ...` with install hints to
:: stderr, so the [SKIP] line is just the aggregator's interpretation.

echo === build-manifest.ps1 (26 assertions) ===
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\test-build-manifest.ps1"
set RC=!ERRORLEVEL!
if !RC! NEQ 0 (
    echo [FAIL] test-build-manifest.ps1
    set /A FAILS+=1
)

if exist "%ROOT%\build\release\disk.img" (
    echo === to-vhdx.ps1 ^(qemu-img convert + verify^) ===
    powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\to-vhdx.ps1"
    set RC=!ERRORLEVEL!
    if !RC! EQU 2 (
        echo [SKIP] to-vhdx.ps1 -- tool/prereq missing ^(see [ERROR] line above^)
    ) else if !RC! NEQ 0 (
        echo [FAIL] to-vhdx.ps1
        set /A FAILS+=1
    )

    echo === to-iso.ps1 ^(xorriso^) ===
    powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\to-iso.ps1"
    set RC=!ERRORLEVEL!
    if !RC! EQU 2 (
        echo [SKIP] to-iso.ps1 -- tool/prereq missing ^(see [ERROR] line above^)
    ) else if !RC! NEQ 0 (
        echo [FAIL] to-iso.ps1
        set /A FAILS+=1
    )
) else (
    if defined RELEASE_AGG_PERMIT_MISSING_IMG (
        echo === to-vhdx.ps1 / to-iso.ps1 SKIPPED ^(missing build/release/disk.img; opt-in permissive mode^) ===
        echo Run scripts/release/build-image.sh in WSL first to produce the input image.
    ) else (
        echo [FAIL] missing prereq build/release/disk.img -- run scripts/release/build-image.sh in WSL
        echo To skip the converter checks instead of failing, set RELEASE_AGG_PERMIT_MISSING_IMG=1
        set /A FAILS+=1
    )
)

echo.
if %FAILS% GTR 0 (
    echo [release] %FAILS% step^(s^) failed
    set RC=1
) else (
    echo [release] all chained steps passed
    set RC=0
)
popd
if not defined NO_PAUSE pause
exit /b %RC%
