# P0005 — Blue Screen of Death (BSOD)

> **Goal:** A complete kernel panic experience — from crash detection through
> display, diagnostics, crash dump, auto-restart, and crash recovery UI.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. Panic Screen ✅

> **Status:** Implemented in `src/kernel/panic.c`.

- [x] Blue background (`#003380`) with direct framebuffer rendering
- [x] Embedded BSOD icon (white, alpha-blended via `bsod_icon.h`)
- [x] Stop boot animation dots (`boot_splash_finish()`)
- [x] Display: stop code, description, source file:line
- [x] Register dump (RAX–R15, RSP, RFLAGS, CR2, CR3, CS, SS)
- [x] Stack trace via RBP chain walk (max 16 frames)
- [x] Commit: `"panic: embed bsod.png icon, stop boot animation, add BSOD_TEST trigger"`

---

## 2. Crash Dump ✅

> **Status:** Implemented in `src/kernel/panic.c`.

- [x] Write crash dump to `C:\Impossible\System\crashdump.log`
- [x] Includes: timestamp, exception, description, registers, stack trace

---

## 3. Auto-Restart ✅

> **Status:** Implemented in `src/kernel/panic.c`.

- [x] Registry setting: `HKLM\SYSTEM\Recovery\AutoRestart` (DWORD, default 30 seconds)
- [x] PIT-based countdown with progress bar at bottom of BSOD screen
- [x] Reboot via port `0xCF9` (system reset), triple-fault fallback
- [x] If `AutoRestart = 0`, display "System halted. Press reset to restart."
- [x] Auto-creates Registry key with defaults if not present

---

## 4. BSOD Test Trigger ✅

> **Status:** Available via compile-time flag.

- [x] `#ifdef BSOD_TEST` in `main.c` triggers deliberate panic before desktop
- [x] Enable with `-DBSOD_TEST` in Makefile `CFLAGS`
- [x] Commit: `"panic: add spacing between BSOD icon and text"`

---

## 5. Auto-Restart Validation

> **Depends on:** §3 (auto-restart), §4 (test trigger)

**Prompt:** Validate the BSOD auto-restart end-to-end: enable `BSOD_TEST`, build, boot in QEMU. Verify the BSOD screen appears, countdown runs for the configured number of seconds, progress bar advances, and the system reboots automatically. After reboot, verify the system boots normally (no crash loop). Also test with `AutoRestart = 0` to confirm the system halts instead. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit test results.


- [ ] **Test:** Enable `BSOD_TEST` → boot → BSOD appears → countdown runs → system reboots
- [ ] **Test:** After auto-reboot, system boots normally (no infinite crash loop)
- [ ] **Test:** Set `AutoRestart = 0` → BSOD shows "System halted" and does NOT reboot
- [ ] **Test:** Change `AutoRestart = 10` → verify 10-second countdown
- [ ] **Test:** Verify progress bar renders correctly during countdown
- [ ] **Test:** Verify crash dump is written before restart

---

## 6. Crash Loop Protection (Future)

> **Depends on:** §3 (auto-restart)

**Prompt:** Prevent infinite crash loops: if the system crashes within 60 seconds of a BSOD auto-restart, disable auto-restart and halt instead. Track consecutive crash count in a Registry key `HKLM\SYSTEM\Recovery\ConsecutiveCrashes`. Reset to 0 on successful boot (e.g., once the desktop is loaded). If count ≥ 3, skip auto-restart and display: "Your PC has crashed multiple times. Automatic restart has been disabled." After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"panic: crash loop protection"`.


- [ ] Track `HKLM\SYSTEM\Recovery\ConsecutiveCrashes` (DWORD)
- [ ] Increment on each BSOD, reset to 0 on successful desktop load
- [ ] If `ConsecutiveCrashes >= 3`, skip auto-restart → halt with message
- [ ] Commit: `"panic: crash loop protection"`

---

## 7. Recovery Settings in `power.spl` (Future)

> **Depends on:** §3, Settings Panel (TODO-310-Terminal.md §4)

- [ ] Add "Recovery" section to `power.spl` settings applet:
  - [ ] Toggle: "Automatically restart after a system failure" (on/off)
  - [ ] Auto-restart delay slider (10, 15, 30, 60 seconds)
  - [ ] "Write crash dump" checkbox
- [ ] Registry: `HKLM\SYSTEM\Recovery\AutoRestart`, `WriteCrashDump`
- [ ] Commit: `"apps: recovery settings in power.spl"`

---

## 8. Crash Analysis on Boot (Future)

> **Depends on:** §2 (crash dump)

- [ ] On boot, check for `C:\Impossible\System\crashdump.log`
- [ ] If present, show notification toast: "Your PC restarted after a problem"
- [ ] *(Stretch)* Crash viewer app: display crash dump with formatted register table
- [ ] Commit: `"kernel: post-crash notification"`

---

## Priority Order

| Priority | Section                    | Reason                                   |
|----------|----------------------------|------------------------------------------|
| ✅ Done   | 1. Panic Screen            | Core BSOD display                        |
| ✅ Done   | 2. Crash Dump              | Diagnostics written to disk              |
| ✅ Done   | 3. Auto-Restart            | Registry-configurable countdown + reboot |
| ✅ Done   | 4. BSOD Test Trigger       | Compile-time panic for testing           |
| 🔴 P0     | 5. Auto-Restart Validation | Verify end-to-end BSOD → reboot flow     |
| 🟠 P1     | 6. Crash Loop Protection   | Prevent infinite reboot cycles           |
| 🟡 P2     | 7. Recovery Settings       | UI for auto-restart configuration        |
| 🟢 P3     | 8. Crash Analysis on Boot  | Post-crash notification                  |

---

## OS Comparison

| Feature                           | Windows 11 (BSOD/WinRE)              | Linux (kernel oops / kdump)           | Impossible OS                          |
|-----------------------------------|---------------------------------------|---------------------------------------|----------------------------------------|
| Graphical panic screen            | ✅ BSOD with QR code + stop code      | ⚠️ Plain text (unless plymouth)       | ✅ Done §1 — gradient + logo + icon    |
| Stop code display                 | ✅ `STOP 0x0000xxxx`                  | ✅ Oops: `BUG:` + backtrace            | ✅ Done §1 — stop code + description  |
| Register dump                     | ✅ Minidump + live view               | ✅ Oops register dump                  | ✅ Done §1 — RAX–R15, RFLAGS, CR2/3  |
| Stack trace                       | ✅ !analyze -v (WinDbg)               | ✅ Oops call trace                     | ✅ Done §1 — RBP-chain, 16 frames     |
| Crash dump to disk                | ✅ MEMORY.DMP / minidump              | ✅ kdump → /var/crash                  | ✅ Done §2 — `crashdump.log`           |
| Auto-restart countdown            | ✅ Registry `AutoReboot`              | ✅ `kernel.panic =` sysctl             | ✅ Done §3 — PIT countdown + CF9 reboot |
| Progress bar during countdown     | ❌ No progress bar                    | ❌ No progress bar                    | ✅ Done §3 — **unique feature**       |
| QR code for troubleshooting       | ✅ Windows BSOD QR                    | ❌                                    | 🔵 Future                             |
| Crash loop protection             | ✅ Automatic Repair / WinRE           | ✅ `kernel.panic_on_oops`              | ⬜ §6 P1 — consecutive crash counter  |
| Recovery UI (F8/WinRE)           | ✅ WinRE boot                        | ✅ GRUB rescue                         | ⬜ §7 P2 (see TODO-160 F8 menu)       |
| Post-boot crash notification      | ✅ "Your PC didn't restart correctly"| ✅ coredumpctl / journald              | ⬜ §8 P3                              |
| **Progress bar (unique)**        | ❌ No countdown bar                   | ❌ No countdown bar                   | ✅ **Done — only OS with BSOD progress bar** |
| **Inline kernel (no WinRE)**     | ❌ Recovery requires WinRE            | ❌ Recovery needs rescue kernel        | ⬜ **TODO-160 §4 — F8 in main kernel** |
