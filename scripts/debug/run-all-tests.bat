@echo off
:: run-all-tests.bat -- cross-layer aggregate for Windows hosts.
::
:: Chains the four per-layer aggregates (kernel + usermode + desktop +
:: release) under their respective subdirs. Missing aggregates are
:: silent no-ops via `if exist`, so a layer that hasn't shipped its
:: aggregate yet doesn't fail the whole sweep.
::
:: Layer aggregates owned by:
::   kernel/    -- TODO-08 (test infrastructure) + every TEST_CAT_* TODO
::   usermode/  -- user-mode test framework (under 16-sdk-tooling)
::   desktop/   -- desktop compositor + UI tests
::   release/   -- Windows host parity for release tooling
::
:: This is the SOLE bat permitted at scripts/debug/ root; per-category
:: bats live in their layer subdir (enforced by validate.py bat-alignment
:: check + scripts/lint.sh). Adding a new layer:
::   1. create scripts/debug/<layer>/ with the per-artifact bats
::   2. add scripts/debug/<layer>/run-all-<layer>-tests.bat
::   3. add the layer to VALID_TEST_LAYERS in scripts/todo-graph/validate.py
::   4. add an `if exist` block here

setlocal
set OVERALL=0
:: Suppress per-layer `pause` prompts so callees propagate the real
:: PowerShell exit code via `exit /b %RC%` instead of the pause result.
:: Each layer aggregate honors `if not defined NO_PAUSE pause`.
set NO_PAUSE=1
:: Suppress the per-launch QEMU window for the whole sweep. This aggregate
:: chains ~20 per-category launches; each one opening its own window gives the
:: operator a storm they cannot dismiss, because closing one only lets the next
:: open. run-qemu.ps1 honors IOS_HEADLESS=1 as the env form of -Headless, and
:: setlocal above keeps it scoped to this sweep, so an operator running a SINGLE
:: category bat directly still gets a window. Only the display is suppressed:
:: the VGA device is still emulated, screendump over -Monitor still works, and
:: serial (the log this sweep exists to produce) is untouched.
:: Set IOS_HEADLESS=0 before calling if you want the windows back.
if not defined IOS_HEADLESS set IOS_HEADLESS=1
:: Bound every per-category launch so one stalled guest cannot wedge the sweep.
:: run-qemu.ps1 blocks until the guest exits, and at least one category boots
:: through to the desktop and idles instead of shutting down -- unattended, that
:: stops the chain forever, and headless makes it silent (the only symptom is a
:: log that stops growing). 600s is far above a normal category (a full suite is
:: ~25s) so this only ever fires on a genuine stall. A timed-out category exits
:: 124, is recorded as a failure, and the sweep MOVES ON.
if not defined IOS_QEMU_TIMEOUT_SEC set IOS_QEMU_TIMEOUT_SEC=600
:: pushd auto-maps a UNC working dir (\\wsl.localhost\...) to a temp
:: drive letter so chained bats see a real local CWD; without this
:: CMD falls back to C:\Windows when launched from a UNC path and
:: every relative path resolution downstream fails.
pushd "%~dp0"

if exist "%~dp0kernel\run-all-kernel-tests.bat" (
    echo === KERNEL LAYER ===
    call "%~dp0kernel\run-all-kernel-tests.bat"
    if errorlevel 1 set OVERALL=1
)

if exist "%~dp0usermode\run-all-usermode-tests.bat" (
    echo.
    echo === USERMODE LAYER ===
    call "%~dp0usermode\run-all-usermode-tests.bat"
    if errorlevel 1 set OVERALL=1
)

if exist "%~dp0desktop\run-all-desktop-tests.bat" (
    echo.
    echo === DESKTOP LAYER ===
    call "%~dp0desktop\run-all-desktop-tests.bat"
    if errorlevel 1 set OVERALL=1
)

if exist "%~dp0release\run-all-release-tests.bat" (
    echo.
    echo === RELEASE LAYER ===
    call "%~dp0release\run-all-release-tests.bat"
    if errorlevel 1 set OVERALL=1
)

popd
echo.
if %OVERALL% GTR 0 (
    echo [run-all-tests] one or more layers reported failures
    exit /b 1
) else (
    echo [run-all-tests] all chained layers passed
    exit /b 0
)
