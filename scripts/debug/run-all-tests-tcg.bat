@echo off
:: run-all-tests-tcg.bat -- Run all tests under TCG (software emulation)
:: Use for NVMe/USB device emulation testing -- WHPX can't emulate these.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel tcg -TestOnly -SerialLog
pause
