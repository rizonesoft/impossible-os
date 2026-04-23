@echo off
:: run-matrix-desktop-tests.bat -- multi-monitor + DPI test matrix entry
:: point for the desktop UI test framework (section 13).
::
:: HONESTY NOTE (Codex review 2026-04-22): the 6-cell matrix this
:: script will eventually iterate is mostly unbuildable today because
:: (a) the virtio-gpu multi-output driver has not landed (tracked as a
:: prerequisite), and (b) run-qemu.ps1 has no -ExtraBootConf
:: override path to inject per-cell `test_monitors=N` or DPI knobs
:: yet. An earlier draft of this file printed three [RUN] rows that
:: launched the same QEMU command each time; that was a
:: false-completeness anti-pattern. This version runs exactly one
:: honest baseline cell and labels the rest [SKIP]. The matrix shape
:: stays committed so the follow-up work below just flips SKIP lines
:: into real invocations.
::
:: Follow-up (do these together with the virtio-gpu prereq):
::   1. Add -ExtraBootConf "key=val;key=val" to run-qemu.ps1 so each
::      row can set `test_monitors=N` (and future `test_dpi=...`).
::   2. Swap the SKIP lines below for real invocations that pass per-
::      cell overrides.
::   3. Teach the compositor to honor a DPI boot-knob so the 144/192
::      rows exercise a distinct scale path.
::
:: Until then: the single-output baseline IS the entirety of the
:: honest coverage, and fb_snapshot_monitor OOB tests cover the API.

setlocal enabledelayedexpansion

set ROOT=%~dp0..\..\machines
set FAILED=0
set TOTAL=0
set SKIPPED=0

echo.
echo === Desktop UI Multi-Monitor Matrix (baseline + pending cells) ===
echo.

:: -- Baseline row: 1 monitor, single-output hardware path (what ships).
set /a TOTAL+=1
echo [RUN ] monitors=1 dpi=baseline  (single-output; existing TEST_CAT_DESKTOP suite)
powershell.exe -ExecutionPolicy Bypass -File "%ROOT%\run-qemu.ps1" -Accel whpx -TestOnly -TestSuite desktop -NoUsermodeTests
if errorlevel 1 set /a FAILED+=1

:: -- Pending cells: require the virtio-gpu multi-output driver + a
:: boot.conf override plumbing in run-qemu.ps1. Marked SKIP until both
:: prerequisites ship (see virtio-gpu item + follow-up above).
set /a TOTAL+=5
set /a SKIPPED+=5
echo [SKIP] monitors=1 dpi=144       -- needs DPI boot-knob + run-qemu override
echo [SKIP] monitors=1 dpi=192       -- needs DPI boot-knob + run-qemu override
echo [SKIP] monitors=2 dpi=96/144    -- needs virtio-gpu multi-output driver
echo [SKIP] monitors=2 dpi=96/192    -- needs virtio-gpu multi-output driver
echo [SKIP] monitors=3 dpi=96/144/192 -- needs virtio-gpu multi-output driver

echo.
echo === Matrix summary: %TOTAL% cells, %FAILED% failed, %SKIPPED% skipped (pending driver + runner work) ===
echo.
pause
