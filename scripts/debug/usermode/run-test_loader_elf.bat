@echo off
:: run-test_loader_elf.bat -- §15 binary-format loader coverage: ELF
::
:: Runs only test_loader_elf.exe and exits. The launcher emits
:: `UTEST: test_loader_elf.exe: format=ELF` before the PASS verdict
:: so a regression in the exec dispatcher's magic-byte match surfaces
:: as `format=<wrong>` on the next run.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_loader_elf.exe"
pause
