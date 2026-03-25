---
description: How to test Impossible OS on real hardware via USB boot
---

> **Reference document.** `.cursor/rules/` and `.cursor/skills/` are the single source of truth for AI guidance.
> This file is kept as a supplementary reference for real-hardware testing specifically.
> For build and safety rules, see [`.cursor/rules/safety-build.mdc`](../../.cursor/rules/safety-build.mdc).

# Test on Real Hardware

## Prerequisites
- A clean build that passes QEMU testing first (use `/implement-todo` workflow)
- A USB drive formatted for EFI boot
- Physical access to the target machine

## Steps

1. **Clean build:**
   ```bash
   bash scripts/build.sh clean
   ```
   // turbo
   Verify: `tail -1 build/build.log` shows `=== BUILD OK ===`

2. **Write USB** *(manual — user does this)*
   Prompt the user:
   > Please write `build/system-disk.img` to USB using:
   > - **Windows:** `scripts/deploy/write-usb.ps1`
   > - **Linux:** `scripts/deploy/write-usb.sh`
   
   Wait for user confirmation before continuing.

3. **Boot target machine** *(manual — user does this)*
   Prompt the user:
   > Please boot the target machine from USB (UEFI boot, disable Secure Boot).
   > Let it run until the desktop appears or it stops/crashes.
   > Then remove the USB and plug it back into the dev machine.

4. **Read boot log from USB** *(agent does this when user confirms)*
   ```bash
   # Mount USB and find the log
   ls /media/*/BOOT_*.LOG 2>/dev/null || ls /mnt/*/BOOT_*.LOG 2>/dev/null
   ```
   Read the most recent `BOOT_NNN.LOG` file.

5. **Analyze the log.** Search for:
   - `[!!]` — warnings (non-fatal but noteworthy)
   - `[FAIL]` — failed initialization (component didn't start)
   - `PANIC` — kernel panic (fatal, system halted)
   - `FAULT` — CPU exception (page fault, GP fault, etc.)
   - `HARDWARE.TXT` — if present, read for hardware detection results

6. **Update the hardware compatibility log:**
   - Record the tested machine and result in the current task notes or other active tracking document
   - Include: machine name, CPU, RAM, GPU, result (PASS/FAIL), date, and notes

## Output

Report to the user:
- ✅ PASS or ❌ FAIL
- Boot log summary (key messages)
- Issues found with line references
- Updated real-hardware test notes
