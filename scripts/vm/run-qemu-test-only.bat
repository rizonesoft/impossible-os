@echo off
:: run-qemu-test-only.bat — Boot with test=1 (unit tests only, then shutdown)
::
:: Runs kernel unit tests and immediately shuts down via ACPI.
:: Used for automated CI-style testing. Results in serial output.
:: QEMU exits cleanly after tests complete.

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1" -Accel whpx -TestOnly
pause
