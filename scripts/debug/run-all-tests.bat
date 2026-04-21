@echo off
:: run-all-tests.bat -- Run every Impossible OS test category in sequence
::
:: Boots QEMU once for each layer:
::   1. Kernel TEST_CAT_* sweep (test=1 + test_usermode_skip=1)
::   2. User-mode test_*.exe binaries (test=1 + test_kernel_skip=1)
::   3. Desktop UI tests (placeholder -- framework not yet implemented)
::
:: IMPORTANT: this bat invokes scripts/machines/run-qemu.ps1 DIRECTLY
:: rather than call'ing the per-layer aggregate bats, and passes
:: -TestAutoShutdown so each QEMU session EXITS automatically after
:: its tests finish. The child bats each end with `pause` for
:: interactive UX (operator sees results before the QEMU window
:: closes), which BLOCKS the chain; and without auto-shutdown QEMU
:: stayed open forever after tests (boot continued to desktop and
:: idled there), which ALSO blocked the chain.
::
:: With -TestAutoShutdown and direct PowerShell invocation, the
:: kernel calls acpi_shutdown() right after the test runner
:: completes; QEMU exits; PowerShell returns; the next layer starts
:: automatically. A single pause at the bottom preserves the "see
:: the summary before the window closes" UX at the chain level.
::
:: For development iteration on a single category, prefer the
:: per-category bats in kernel\, usermode\, or desktop\ subdirs --
:: they finish in seconds where this aggregate takes minutes.

setlocal
set MACHINES=%~dp0..\machines

echo.
echo === [1/3] Kernel test suites ===
echo.
powershell.exe -ExecutionPolicy Bypass -File "%MACHINES%\run-qemu.ps1" -Accel whpx -TestOnly -NoUsermodeTests -TestAutoShutdown

echo.
echo === [2/3] User-mode test binaries ===
echo.
powershell.exe -ExecutionPolicy Bypass -File "%MACHINES%\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -TestAutoShutdown

echo.
echo === [3/3] Desktop UI tests ===
echo.
if exist "%~dp0desktop\run-all-desktop-tests.bat" (
    call "%~dp0desktop\run-all-desktop-tests.bat"
) else (
    echo [SKIP] %~dp0desktop\run-all-desktop-tests.bat not found ^(desktop UI test framework not yet implemented^)
)

echo.
echo === All test categories invoked ===
endlocal
pause
