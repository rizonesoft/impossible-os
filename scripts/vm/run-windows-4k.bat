@echo off
:: run-windows-4k.bat — QEMU HiDPI test at 3840x2160 (scale=2x, 4K canvas)
:: Serial output: [??] SPLASH  3840x2160  scale=2x  font=32px  icon=270px

powershell.exe -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1" -Xres 3840 -Yres 2160
pause
