@echo off
:: run-windows-1080p.bat — QEMU HiDPI test at 1920x1080 (scale=1x, larger canvas)
:: Serial output: [??] SPLASH  1920x1080  scale=1x  font=16px  icon=135px

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-windows.ps1" -Xres 1920 -Yres 1080
pause
