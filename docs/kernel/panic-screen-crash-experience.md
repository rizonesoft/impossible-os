<!-- docs: covers=todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md sources=src/kernel/panic.c,include/kernel/panic.h,src/kernel/bsod_icon.h,src/kernel/symtab.c,src/kernel/exec.c reviewed=2026-09-28 order=28 -->
# Panic Screen and Crash Experience

## What is it?

The panic screen is the full-screen blue crash display the kernel draws when a fault or an explicit bugcheck cannot be recovered. `panic_screen()` in [`panic.c`](../../src/kernel/panic.c) renders the icon, exception name, fault registers, an RBP-chain stack trace and the crash log path straight to the framebuffer, with no dependency on the compositor. The richer crash experience in the roadmap (TrueType text, inline log context, a data-bearing QR code, crash statistics, panic modes, beep codes and keyboard recovery actions) is not built yet.

## How does it work?

`panic_screen()` is a thin public wrapper around the static `panic_screen_impl()`, which does the work: it masks interrupts, decides through a compare-exchange token which CPU owns the crash record when several fault at once, and copies the caller's description and file strings into fixed local buffers so a corrupted pointer is never read twice. It then calls `panic_collect_evidence()` to write a cross-boot forensic record (`struct panic_evidence`, [`panic.h`](../../include/kernel/panic.h)) to a fixed physical page before it touches the screen or the disk. That record carries its own CRC-32 and a magic and epoch publication word, so the next boot's `panic_evidence_restore_early()` can tell whether a prior crash record is valid, and `panic_had_previous_crash()` lets the desktop surface an "unexpected shutdown" notice.

Only after the evidence is captured does the screen change: the 128x128 icon ([`bsod_icon.h`](../../src/kernel/bsod_icon.h)), the exception name from the 32-entry `panic_exception_names` table, the register dump, and a stack trace built by walking the RBP chain and resolving each return address with `symtab_resolve()` ([`symtab.c`](../../src/kernel/symtab.c)). All text goes through `printk()` and `fb_set_color()` in the console bitmap font; nothing in `panic.c` calls the TrueType renderer. When the VFS is up, the crash reason and stack trace are also written to `C:\Impossible\System\crashdump.log`.

The auto-restart countdown reads the `AutoRestart` DWORD under `HKLM\SYSTEM\Recovery` and falls back to `DEFAULT_RESTART_SECS` (30) when the value is missing or invalid. The runtime tunable `panic.timeout` (0 to 3600 seconds, 0 disables the restart) overrides the Registry value when it is set. There is no keyboard-cancel path, no crash-loop counter, and no NVRAM crash statistics.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `panic_screen(frame, error_code, description, file, line)` | Public entry point for a raw-fault panic ([`panic.c`](../../src/kernel/panic.c)) |
| `KeBugCheckEx()`, `KeBugCheckExFrame()` | Bugcheck entry points that share the same renderer |
| `struct panic_evidence`, `panic_evidence_populate()`, `panic_evidence_restore()` | Cross-boot crash record with CRC-32 and publication word ([`panic.h`](../../include/kernel/panic.h)) |
| `panic_evidence_restore_early()`, `panic_had_previous_crash()` | Early-boot recovery of the prior boot's record, and the flag the desktop reads |
| `symtab_resolve()` | Symbol resolution for the stack trace ([`symtab.c`](../../src/kernel/symtab.c)) |
| `bugcheck_name()` | STOP-code to name lookup over the `s_bugcheck_names` table |
| `HKLM\SYSTEM\Recovery\AutoRestart`, `panic.timeout` | Registry DWORD and runtime tunable for the countdown length in seconds; the tunable wins |

## How do I use it?

The panic screen needs no configuration; it renders whenever a fault reaches `panic_screen()` or a bugcheck calls into the shared renderer, provided the framebuffer is up (Phase 1 onward). A Phase 0 panic, before the framebuffer exists, is serial-only. The countdown is set by the `AutoRestart` Registry value or, taking precedence, the `panic.timeout` tunable; `boot.conf` has no panic-screen key. To see it on a debug build, press `Ctrl+ScrollLock` twice within two seconds, which calls `KeBugCheckEx()` with `BUGCHECK_MANUALLY_INITIATED_CRASH` when `HKLM\SYSTEM\CrashControl\CrashOnCtrlScroll` is set (see [Crash Dump Generation](crash-dump-generation.md)).

```bash
bash scripts/test.sh SUITE=boot   # bugcheck names and crash capture suites
```

## What is not implemented yet?

- **TrueType text.** The screen renders in the console bitmap font through `printk()`; `panic.c` never calls the TrueType renderer ([TTF Font Rendering in Panic Screen](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#1-ttf-font-rendering-in-panic-screen)).
- **The redesigned layout.** No margin, section or HiDPI scaling constants exist ([Improved Layout and Visual Hierarchy](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#2-improved-layout-and-visual-hierarchy)).
- **Inline klog context.** There is no `klog_get_recent()`, so the last log lines are not shown ([Crash Context: Last 10 klog Entries Inline](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#3-crash-context-last-10-klog-entries-inline)).
- **A data-bearing QR code.** No QR encoder exists ([Smart QR Code with Compressed Crash Data](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#4-smart-qr-code-with-compressed-crash-data)).
- **A cancellable countdown.** The countdown has no keyboard-cancel check ([Auto-Restart Countdown Improvements](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#5-auto-restart-countdown-improvements)).
- **Per-exception hints, crash statistics and loop protection.** No hints table, NVRAM statistics or consecutive-crash tracking exist ([Crash Analysis Hints On-Screen](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#6-crash-analysis-hints-on-screen), [Crash Statistics & Loop Protection](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#7-crash-statistics--loop-protection), [Safe Mode Suggestion After Repeated Crashes](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#8-safe-mode-suggestion-after-repeated-crashes)).
- **A "What failed" module line.** `exec_find_module_by_pc()` exists in [`exec.c`](../../src/kernel/exec.c) but the panic path does not call it ([What Failed Faulting Module Identification](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#9-what-failed-faulting-module-identification)).
- **Panic modes, beep codes and keyboard recovery actions.** None of the user, developer or QR modes, PC speaker codes or F-key actions exist ([Panic Screen Modes (User / Developer / QR)](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#10-panic-screen-modes-user--developer--qr), [Audio Crash Notification (PC Speaker Beep Codes)](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#11-audio-crash-notification-pc-speaker-beep-codes), [Keyboard-Driven Recovery Actions at Crash Screen](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#12-keyboard-driven-recovery-actions-at-crash-screen)).
- **Dump progress.** There is no progress callback, and the minidump writer it would report on is not built ([Dump Collection Progress Percentage](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md#13-dump-collection-progress-percentage)).

## How does it compare with Windows 11 and Linux?

Today's screen matches the basics both operating systems ship: a full-screen stop display, a register dump and a stack trace. Windows 11 24H2 moved to a sparse black screen without a QR code, and the Linux DRM panic handler added kmsg QR codes and several verbosity modes in 6.12. The roadmap aims past both with a structured QR payload, local crash statistics and audio feedback, but none of that exists yet, so the current screen is closer to a pre-24H2 Windows stop screen than to either target.

## See also

- [BSOD / Panic Screen & Crash Experience roadmap](../../todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md)
- [Crash Dump Generation](crash-dump-generation.md)
- [Kernel Init Sequencing](kernel-init-sequencing.md)
- [System Logging (klog)](system-logging.md)
- [Kernel Debugger (KD Protocol)](kernel-debugger-kd.md)
