@echo off
:: run-desktop-tests.bat -- Desktop UI tests (framebuffer snapshot, input injection, WM introspection, visual regression)
:: -NoUsermodeTests: the kernel TEST_CAT_* filter gates only kernel suites; the user-mode launcher runs
::                   independently unless explicitly skipped. Skip it so this bat stays desktop-only.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite desktop -NoUsermodeTests
pause
