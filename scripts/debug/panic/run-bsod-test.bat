@echo off
:: run-bsod-test.bat -- Trigger deliberate BSOD to test panic screen
:: Patches crash_test=1 into boot.conf, boots QEMU. Kernel panics after
:: desktop init with "CRASH_TEST: Deliberate panic for testing".
:: After BSOD renders, close QEMU and run run-crash-recovery-test.bat.
echo Patching boot.conf: crash_test=1
wsl.exe bash -c "cd ~/impossible-os && bash scripts/patch-boot-conf.sh crash_test 1"
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx
echo Resetting boot.conf
wsl.exe bash -c "cd ~/impossible-os && bash scripts/patch-boot-conf.sh reset"
pause
