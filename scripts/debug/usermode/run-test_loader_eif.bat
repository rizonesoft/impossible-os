@echo off
:: run-test_loader_eif.bat -- §15 binary-format loader coverage: EIF
::
:: Runs only test_loader_eif.exe and exits. Binary is produced by
:: nasm -f bin + scripts/build-eif.py (NO linker; the EIF header is
:: prepended by the Python builder). Launcher emits
:: `UTEST: test_loader_eif.exe: format=EIF` before the PASS verdict
:: so a regression that silently routes this through the ELF loader
:: surfaces as `format=ELF`.
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "test_loader_eif.exe"
pause
