@echo off
:: run-all-kernel-tests-tcg.bat -- Run all tests under TCG (software emulation)
:: Use for NVMe/USB device emulation testing -- WHPX can't emulate these.
:: -NoUsermodeTests keeps this kernel-only (matches run-all-kernel-tests.bat).
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel tcg -TestOnly -NoUsermodeTests
pause
