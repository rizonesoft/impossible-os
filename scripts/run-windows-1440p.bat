@echo off
:: run-windows-1440p.bat — QEMU HiDPI test at 2560x1440 (scale=2x)
:: Serial output: [??] SPLASH  2560x1440  scale=2x  font=32px  icon=180px

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-windows-1440p.ps1"
pause
