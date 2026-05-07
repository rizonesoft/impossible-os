@echo off
:: run-to-vhdx.bat -- VHDX converter (qemu-img.exe convert, byte-content
:: parity with the Linux peer). Owns: scripts/release/to-vhdx.ps1.
:: Reads build/release/disk.img, writes build/release/disk.vhdx; runs
:: qemu-img info + qemu-img compare for self-verification.
powershell.exe -ExecutionPolicy Bypass -NoProfile -File "%~dp0..\..\release\to-vhdx.ps1" %*
pause
