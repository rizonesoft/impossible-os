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

echo.
if %OVERALL% GTR 0 (
    echo [run-all-tests] one or more layers reported failures
    exit /b 1
) else (
    echo [run-all-tests] all chained layers passed
    exit /b 0
)
