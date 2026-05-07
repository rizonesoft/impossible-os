@echo off
:: run-all-desktop-tests.bat -- Desktop layer aggregate.
::
:: Chains the desktop test bats so the cross-layer
:: scripts/debug/run-all-tests.bat aggregate has a canonical entry
:: point for this layer (matches the kernel/usermode/release pattern).
::
:: Each step preserves its PowerShell exit code via `exit /b %RC%`;
:: this aggregate accumulates failures into OVERALL and exits non-zero
:: if any step failed.

setlocal
set OVERALL=0
set NO_PAUSE=1

echo === run-desktop-tests ===
call "%~dp0run-desktop-tests.bat"
if errorlevel 1 set OVERALL=1

:: NOT chained: run-matrix-desktop-tests.bat. Its only active [RUN]
:: cell currently invokes the same `-TestSuite desktop -NoUsermodeTests`
:: command as run-desktop-tests.bat (the remaining matrix cells are
:: SKIP pending virtio-gpu multi-output + run-qemu.ps1 -ExtraBootConf
:: support). Chaining both adds a duplicate QEMU boot with zero
:: additional coverage. Re-include here once matrix cells are
:: genuinely distinct.

echo.
if %OVERALL% GTR 0 (
    echo [desktop] one or more steps failed
    set RC=1
) else (
    echo [desktop] all chained steps passed
    set RC=0
)
if not defined NO_PAUSE pause
exit /b %RC%
