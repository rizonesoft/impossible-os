@echo off
:: run-write-usb.bat -- launches the Windows-host USB writer.
:: Owns: scripts/deploy/write-usb.ps1 (fail-closed; strict BusType=USB,
:: TOCTOU re-check, pre-write source-ESP sha256 capture, fail-closed
:: post-write verify). The PS1 has #Requires -RunAsAdministrator and
:: prompts twice (YES + disk-number) before writing.
::
:: NOT chained by run-all-release-tests.bat (destructive + interactive).
:: Use this bat for hand-driven USB imaging from File Explorer; the
:: -Verb RunAs relaunch elevates without requiring a separate admin
:: PowerShell window.
powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "Start-Process pwsh -ArgumentList '-NoProfile','-File','%~dp0..\..\deploy\write-usb.ps1' -Verb RunAs"
