@echo off
:: run-all-tests.bat -- Run every Impossible OS test category in sequence
::
:: Boots QEMU once for each category that has a runner directory:
::   1. kernel\run-all-kernel-tests.bat   -- every TEST_CAT_* suite
::   2. usermode\run-all-usermode-tests.bat -- every test_*.exe binary
::      (no-op until the user-mode test framework's launcher ships)
::   3. desktop\run-all-desktop-tests.bat   -- every desktop UI test
::      (no-op until the desktop UI test framework ships)
::
:: For development iteration on a single subsystem, prefer the
:: per-category bat files in the matching subdirectory -- they finish
:: in seconds where this aggregate runner takes minutes.
::
:: To run the historical "all kernel tests in one boot" behaviour
:: (what this file used to do before the kernel\ + usermode\ + desktop\
:: split), call kernel\run-all-kernel-tests.bat directly.
::
:: Each category aggregate is invoked unconditionally; if a category
:: directory has no aggregate yet, the corresponding line is a no-op.

setlocal
set DEBUG=%~dp0

echo.
echo === [1/3] Kernel test suites ===
echo.
if exist "%DEBUG%kernel\run-all-kernel-tests.bat" (
    call "%DEBUG%kernel\run-all-kernel-tests.bat"
) else (
    echo [SKIP] %DEBUG%kernel\run-all-kernel-tests.bat not found
)

echo.
echo === [2/3] User-mode test binaries ===
echo.
if exist "%DEBUG%usermode\run-all-usermode-tests.bat" (
    call "%DEBUG%usermode\run-all-usermode-tests.bat"
) else (
    echo [SKIP] No usermode-only aggregate -- the kernel run above already triggered
    echo        the user-mode test launcher because boot.conf test=1 fires both
    echo        test_runner_run^(^) and test_usermode_run^(^). Per-binary iteration
    echo        runners live in usermode\run-^<binary^>.bat ^(each one also re-runs
    echo        the kernel suite -- a future test_kernel=0 knob could change this^).
)

echo.
echo === [3/3] Desktop UI tests ===
echo.
if exist "%DEBUG%desktop\run-all-desktop-tests.bat" (
    call "%DEBUG%desktop\run-all-desktop-tests.bat"
) else (
    echo [SKIP] %DEBUG%desktop\run-all-desktop-tests.bat not found ^(desktop UI test framework not yet implemented^)
)

echo.
echo === All test categories invoked ===
endlocal
pause
