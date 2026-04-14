@echo off
:: run-klog-v2-tests.bat -- klog v2 lockless tests (per-CPU rings, lanes, drain worker)
powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\machines\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite klog_v2
