@echo off
:: run-all-kernel-tests.bat -- Run every kernel-side TEST_CAT_* suite
::
:: Boots QEMU once with `test=1` and no SUITE filter, so the kernel
:: test runner walks all categories (mm/fs/sched/ob/security/ipc/
:: boot/abi/storage/exec/x86 + any future cats). Equivalent to the
:: per-category bat files in this directory but in one boot.
::
:: Use one of the per-category bats (run-mm-tests.bat,
:: run-boot-tests.bat, etc.) when iterating on a single subsystem
:: during development -- they are faster because the test runner
:: short-circuits on SUITE mismatch.
::
:: -NoUsermodeTests sets boot.conf test_usermode_skip=1 so this aggregate
:: is genuinely kernel-only -- run usermode\run-all-usermode-tests.bat
:: separately for the user-mode layer.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoUsermodeTests
set RC=%errorlevel%
if not defined NO_PAUSE pause
exit /b %RC%
