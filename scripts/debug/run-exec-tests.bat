@echo off
:: run-exec-tests.bat -- Binary/exec and related kernel library tests
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite exec
pause
