---
schema_version: 1
id: bsod-ux-enhancements
domain: 02-kernel-core
status: active
title: "TODO-28: BSOD / Panic Screen & Crash Experience"
---

# TODO-28: BSOD / Panic Screen & Crash Experience

> **Goal:** Transform the panic screen from a developer debug dump into a polished, informative crash experience that surpasses both Windows 11 (which dropped its QR code and sad face in 2025, replacing with a brief black screen) and Linux (which added DRM panic QR codes with compressed kmsg in 6.12: the current state of the art). Use TTF fonts, display actionable crash context, encode compressed crash data in a QR code, provide keyboard-driven recovery actions, track crash history locally, and signal crashes via audio for accessibility. When complete, Impossible OS has the best crash experience of any operating system: smarter than Windows 11 (generic black screen, cloud-dependent recovery), richer than Linux DRM panic (data-bearing QR but no GUI polish, no recovery actions, no local crash history).

> [!IMPORTANT]
> **Current state:** `panic_screen()` in `src/kernel/panic.c` (975 lines) renders a full-screen blue screen with icon, stop code, register dump, RBP-chain stack trace, crash dump path, and 30-second auto-restart countdown. Uses `printk()` + `fb_set_color()` with the embedded boot_font (Selawik Regular, 18px bitmap atlas). Works from Phase 1 onward (requires framebuffer). Falls back to serial-only for Phase 0 panics.

> [!CAUTION]
> **Memory rule:** `panic_screen()` runs after a kernel fault. `kmalloc()` may be corrupted. Use only `printk()`, direct framebuffer writes, static buffers, and `pmm_alloc_contiguous()` (if PMM is intact). Never call VFS or scheduler functions from the panic screen; the crash may have corrupted them.

---

## Inputs

- `src/kernel/panic.c`: current BSOD implementation (975 lines)
- `src/kernel/bsod_icon.h`: 128x128 alpha-blended emoticon icon
- `src/kernel/gfx/gfx_text.c`: boot_font API + TTF font manager
- `include/font_mgr.h`: `ttf_get()`, `ttf_draw_string()` declarations
- `include/kernel/klog.h`: ring buffer API for §3 inline klog
- `src/kernel/symtab.c`: `symtab_resolve` / §9 module name from RIP
- `resources/boot/boot.conf`: `panic_screen=` mode switch (§10)
- `src/kernel/main/boot_hw.c`: Phase 0 hooks, NVRAM, safe mode flags (§7, §8, §12)
- `src/kernel/main/boot_desktop.c`: successful boot clears crash stats (§7)
- → XREF: `TODO-02-kernel-configuration-policy.md §5, §10` -- Safe Mode policy, repeated-failure thresholds, and boot-acceptance state are owned there; this TODO only owns crash-screen UX and recovery request plumbing
- → XREF: `TODO-27-crash-dump-generation.md` §1 through §9: binary crash dump mechanics (scope boundary: T27 owns dump format/analysis, this TODO owns on-screen UX)
- → XREF: `TODO-03-kernel-libraries.md` §5, §6: LZ4 (T03 §5) or miniz zlib deflate (T03 §6) for §6 QR payload compression once those ports land (panic path must stay kmalloc-free; use static scratch + `pmm_alloc_contiguous` for output per T03 memory rules)
- → XREF: `TODO-17-binary-system.md` §7: `exec_find_module_by_pc()` / module registry for §12 optional F2 "last module" safe boot and for richer §9 "What failed" than symbol file names alone
- → XREF: `TODO-04-system-logging.md` §7: crash-persistent klog capture (ring buffer persisted to physical memory; this TODO displays recovered entries on-screen)
- → XREF: `TODO-01-kernel-init-sequencing.md` §9: degraded-boot recovery screen (related but separate: recovery screen is for non-fatal degraded boot, BSOD is for fatal panics)
- → XREF: `TODO-23-exception-dispatch-seh.md` §3: fault-to-exception mapping routes to `panic_screen()`; STOP codes align with `KeBugCheckEx` (see also `TODO-27-crash-dump-generation.md` §1)
- → XREF: `TODO-29-kernel-debugger-kd-protocol.md` §5: debugger first-chance notification happens before panic; BSOD only shows if debugger is not attached
- → XREF: `TODO-29-kernel-debugger-kd-protocol.md` §15: when `kd_active`, defer `panic_screen()` until debugger continue or timeout (mirrored checklist in TODO-29)
- → XREF: `14-host-tools/TODO-04-crash-decode.md` (`D14 T11`): host-side crash dump decoder; QR code data format must be compatible
- → XREF: `10-platform-services/TODO-04-restore-recovery.md` (`D10 T11`): safe mode and recovery environment integration

---

## Outcome

- Panic screen uses Selawik Regular/Bold via TTF font manager when available (Phase 2+), falls back to boot bitmap font for Phase 0-1 panics; text is crisp and properly sized at any resolution.
- Layout has proper margins, section spacing, and visual hierarchy: stop code prominent, registers compact, stack trace scrollable.
- Three panic screen modes: `user` (clean, minimal info), `developer` (full registers + stack + klog), `qr` (data-bearing QR code), switchable via boot.conf `panic_screen=`.
- Last 10 klog entries displayed inline on developer/user modes providing crash context without requiring serial access.
- QR code encodes **compressed crash data** (stop code, registers, last 10 klog entries, faulting module): not a generic URL. Self-hosted decoder at configurable URL.
- "What failed" module identification: identifies the faulting kernel module from RIP address using the symbol table.
- Crash statistics tracked in UEFI NVRAM: total crash count, last 5 stop codes with timestamps.
- After 3+ crashes in a row without successful boot, BSOD suggests safe mode and offers keyboard-driven recovery (F1=Safe Mode, F8=Recovery Shell).
- Auto-restart countdown improved with smooth progress bar, keyboard interrupt support (any key cancels restart).
- Audio crash notification: PC speaker beep pattern encodes crash category for accessibility (no screen reader possible during panic).

---

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎  |   1   | TTF font rendering in panic screen       | none       |  [ ]   |
| 💎  |   2   | Improved layout and visual hierarchy     | §1         |  [ ]   |
| ⭐  |   3   | Crash context: last 10 klog entries inline | none       |  [ ]   |
| ⭐  |   4   | Smart QR code with compressed crash data | §2         |  [ ]   |
| 💎  |   5   | Auto-restart countdown improvements      | §2         |  [ ]   |
| ⭐  |   6   | Crash analysis hints on-screen           | §2         |  [ ]   |
| ⭐  |   7   | Crash statistics counter in NVRAM        | none       |  [ ]   |
| ⭐  |   8   | Safe mode suggestion after repeated crashes | §7         |  [ ]   |
| 💎  |   9   | "What failed" faulting module identification | none       |  [ ]   |
| ⭐  |  10   | Panic screen modes (user / developer / QR) | §1, §3, §4 |  [ ]   |
| ⭐  |  11   | Audio crash notification (PC speaker beep codes) | none       |  [ ]   |
| ⭐  |  12   | Keyboard-driven recovery actions at crash screen | §5, §8     |  [ ]   |
| 💎  |  13   | Dump collection progress percentage      | T27 §5     |  [ ]   |

> 💎 = parity: Windows 11 and/or Linux have equivalent features.
> ⭐ = exclusive: Impossible OS is superior or first.

---

## 1. TTF Font Rendering in Panic Screen

Use `ttf_draw_string()` when the font manager is initialized, fall back to `printk()` + boot_font when it isn't. The TTF path renders at the current screen resolution with proper antialiasing; the boot_font path is the existing behavior.

**Files:** `src/kernel/panic.c`, `include/font_mgr.h`

> [!WARNING]
> **Regression risk:** If `ttf_draw_string()` crashes during panic (font data corrupted by the crash), the BSOD won't render. Mitigation: wrap TTF calls in a simple null-check guard; if `ttf_get()` returns NULL or the surface write faults, immediately fall back to `printk()`.

- [ ] Add `static int panic_has_ttf(void)` helper: returns 1 if `ttf_get(FONT_UI, 16)` returns non-NULL
- [ ] Create `panic_print(int x, int y, uint32_t color, int size, const char *text)` wrapper: uses `ttf_draw_string()` if available, otherwise `printk()` with `fb_set_color()`
- [ ] Replace all `printk()` calls in `panic_screen()` with `panic_print()` for consistent rendering
- [ ] Phase 0-1 panics: `panic_has_ttf()` returns 0, falls back to boot_font via `printk()` (no change in behavior)
- [ ] Phase 2+ panics: title rendered in FONT_UI_BOLD at 24px, body in FONT_UI at 14px, registers in FONT_MONO at 12px
- [ ] When `kd_active` (see `T29 §5`), skip `panic_screen()` until the debugger continues or a fixed timeout elapses; log the defer path over serial only (-> XREF `TODO-29-kernel-debugger-kd-protocol.md` §15)
- [ ] Commit: `"kernel: TTF font rendering in BSOD with boot_font fallback"`

**Test checkpoint:** Force a panic after desktop init (crash_test=1): BSOD shows with TTF-rendered text (crisp antialiased). Force a panic during Phase 1 (before font manager): BSOD shows with boot_font (bitmap, same as today). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 2. Improved Layout and Visual Hierarchy

Redesign the BSOD layout with proper margins, section dividers, and visual hierarchy. Pre-24H2 Windows 11 showed a sad face, short copy, and a troubleshooting QR; 24H2+ moves to a minimal black screen without that QR (OS Comparison row). Impossible OS still targets a richer structured layout than either snapshot.

**Files:** `src/kernel/panic.c`

- [ ] Define layout constants: `PANIC_MARGIN_X = 80`, `PANIC_MARGIN_Y = 60`, `PANIC_SECTION_GAP = 24`
- [ ] Icon section: keep 128x128 emoticon, position at `(MARGIN_X, MARGIN_Y)`
- [ ] Title section: two-line narrative right of icon, vertically centered to icon height
- [ ] Stop code section: prominent large text (24px bold), separated by divider line
- [ ] Technical details section: collapsible-style layout with registers in 2-column grid, stack trace in monospace
- [ ] Footer section: version string + crash dump path + restart countdown, anchored to bottom
- [ ] Support HiDPI: scale all layout constants by `g_boot_info.hidpi` flag (2x for >= 2560px width)
- [ ] Commit: `"kernel: BSOD layout redesign with visual hierarchy and HiDPI"`

**Test checkpoint:** BSOD renders with clean margins on 1280x720 (QEMU), 1920x1080 (VBox), and 2560x1440+ (if VBox VMSVGA custom). Register dump is in 2 columns. Stack trace is below registers. Footer is anchored to screen bottom. Verify no text truncation on any resolution. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 3. Crash Context: Last 10 klog Entries Inline

Display the last 10 ring buffer entries on the BSOD, giving immediate crash context without needing serial access. This is the most useful information for first-pass triage.

**Files:** `src/kernel/panic.c`, `include/kernel/klog.h`

- [ ] Add `klog_get_recent(klog_entry_t *out, uint32_t max_count)`: copies the last N ring entries into a caller-provided buffer (no allocation)
- [ ] In `panic_screen()`, after register dump, render a visible `Last 10 log entries` section header (plain text, not markdown rule lines)
- [ ] Each entry rendered as: `[timestamp] LEVEL subsystem: message` in FONT_MONO at 11px (or boot_font)
- [ ] Color-code by level: INFO=white, WARN=yellow, ERROR=red
- [ ] Limit to 10 entries to avoid scrolling past the screen bottom
- [ ] Commit: `"kernel: display last 10 klog entries on BSOD for crash context"`

**Test checkpoint:** Force crash_test=1: BSOD shows 10 recent log entries including the `crash_test=1: triggering deliberate BSOD` warning. Entries are color-coded. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 4. Smart QR Code with Compressed Crash Data

Render a QR code containing **actual compressed crash data**: not a generic URL. Linux 6.12+ pioneered this with Zlib-compressed kmsg in QR codes (written in Rust). Impossible OS goes further: structured crash report with stop code, registers, faulting module, and last 10 klog entries, all Zlib-compressed into a data-bearing URL. Windows 11 **removed** its QR code entirely in 2025 (it only linked to a generic page).

**Files:** `src/kernel/panic.c`, new `src/kernel/qrcode.c`

> [!TIP]
> Linux 6.12's QR code contains raw compressed kmsg. Impossible OS encodes a **structured report** with parsed fields (stop code, module name, recent events); the decoder can show a formatted diagnosis page, not just raw text. This is superior to both Windows (generic URL, now removed) and Linux (raw data requiring manual parsing).

- [ ] Implement QR code generator (Version 6-10, binary mode): static buffers, no allocation, no FPU. ~300 lines of C. Higher version than V2 needed for compressed binary payload.
- [ ] Build structured crash payload: `{ stop_code, rip, cr2, faulting_module[32], klog_entries[10][128] }`
- [ ] Compress payload with minimal Zlib deflate (or simpler LZ4 if TODO-03 §5 delivers it first): static scratch buffer
- [ ] Encode as URL: `https://impossible.os/crash#?a=x86_64&v=VERSION&z=<base64_compressed_data>`
- [ ] Render QR code at 4x scale in bottom-right corner of BSOD, white modules on blue background
- [ ] Include text below QR: "Scan to decode crash data" + fallback text URL
- [ ] Configurable decoder URL via `HKLM\SYSTEM\Recovery\CrashQRUrl` (for self-hosted decoders)
- [ ] Skip QR rendering when framebuffer is unavailable (Phase 0 panic or headless); keep serial diagnostics and rely on §11 beep where enabled (Linux QR is similarly DRM-bound; no Rust or `DRM_PANIC_SCREEN_QR_CODE` dependency in Impossible OS)
- [ ] Commit: `"kernel: smart QR code with compressed crash data on BSOD"`

**Test checkpoint:** Force crash_test=1: QR code visible in bottom-right. Scan with phone: decoder page shows structured crash info (stop code, registers, last 10 log entries). Verify QR encodes correctly with `zbarimg` or phone scanner. Verify QR doesn't overlap other sections at 1280x720 and 1920x1080. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. Auto-Restart Countdown Improvements

Improve the existing 30-second countdown with smoother animation, keyboard cancel support, and better visual feedback.

**Files:** `src/kernel/panic.c`

- [ ] Smooth progress bar: update every 100ms instead of every second (10x smoother animation)
- [ ] Add "Press any key to cancel restart" text below countdown
- [ ] Check keyboard buffer in countdown loop: if any key pressed, cancel restart and display "System halted. Press reset to restart."
- [ ] Move countdown display from absolute pixel position to footer-anchored position (works at any resolution)
- [ ] Registry-configurable: `HKLM\SYSTEM\Recovery\AutoRestart` (DWORD, seconds, 0=disabled): already partially implemented
- [ ] Commit: `"kernel: improved BSOD auto-restart countdown with key cancel"`

**Test checkpoint:** Force crash_test=1: countdown renders smoothly at bottom. Press a key during countdown: restart cancels, "System halted" shown. Set AutoRestart=0 in Registry: no countdown, immediate halt. Keyboard path uses raw i8042 when driver path is unsafe. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. Crash Analysis Hints On-Screen

Display helpful text based on the stop code, similar to Windows "search for STOP_CODE online". Go further with specific advice per exception type.

**Files:** `src/kernel/panic.c`

- [ ] Create `static const char *crash_hints[32]` table mapping exception vectors to user-friendly advice:
  - #DE: "A division by zero occurred. Check recent arithmetic operations."
  - #PF: "A memory access violation occurred at address shown in CR2."
  - #GP: "A general protection fault: possible invalid memory access or privilege violation."
  - #DF: "A double fault: stack overflow or corrupted interrupt handler."
  - #NM: "FPU/SSE instruction used without proper initialization."
  - Generic: "An unexpected error occurred. Check the stack trace for the faulting function."
- [ ] Display hint text below stop code in PANIC_DIM_COLOR
- [ ] Commit: `"kernel: per-exception crash analysis hints on BSOD"`

**Test checkpoint:** Force different crash types (crash_test for generic, NULL deref for #PF, div-by-zero for #DE): each shows appropriate hint text. Verify hints don't overlap other sections. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. Crash Statistics & Loop Protection

Track crash history in UEFI NVRAM and Registry so the kernel knows how many times it has crashed, detects crash loops, and can disable auto-restart after repeated failures. Absorbed from `08-graphics-ui/TODO-13-boot-splash-recovery.md` (crash loop protection moved to this file §8 through §10).

**Files:** `src/kernel/panic.c`, `src/kernel/main/boot_hw.c`, `src/kernel/main/boot_desktop.c`

> [!NOTE]
> UEFI NVRAM write endurance: one write per crash + one write per successful boot = 2 writes per boot cycle. Combined with ImpossiblePOST (2), ImpossibleBootPerf (1), and ImpossibleCrashLog (1) = 6 total NVRAM writes per boot. ~100K cycle endurance / 6 = ~16K boot cycles before flash wear concern.

- [ ] Define `ImpossibleCrashStats` NVRAM variable: `{ uint32_t total_crashes, uint32_t consecutive_crashes, struct { uint32_t stop_code; uint64_t timestamp; } last_5[5] }`
- [ ] In `panic_screen()`: read current stats, increment `total_crashes` and `consecutive_crashes`, push stop code to circular `last_5[]`, write back to NVRAM
- [ ] Also write `HKLM\SYSTEM\Recovery\ConsecutiveCrashes` (DWORD) if Registry is accessible: dual persistence (NVRAM survives corrupted disk, Registry survives NVRAM issues)
- [ ] Write `HKLM\SYSTEM\Recovery\LastBootTime` (DWORD, PIT ticks) after `registry_init()` in Phase 2
- [ ] In `boot_phase0()` (after UEFI runtime init): read NVRAM stats, reset `consecutive_crashes` to 0 (successful boot clears the counter)
- [ ] In `boot_desktop.c` after successful `desktop_init()`, call `RegSetValueEx(HKLM, key, "ConsecutiveCrashes", REG_DWORD, &zero, 4)` to confirm boot succeeded
- [ ] If `consecutive_crashes >= 3` in `panic_screen()`: skip auto-restart countdown, display halt message instead
- [ ] Display "Crash #N" on BSOD screen (total lifetime crash count)
- [ ] Log `[recovery] ConsecutiveCrashes=%u` at both increment and reset sites
- [ ] Commit: `"kernel: crash statistics + loop protection via NVRAM and Registry"`

**Test checkpoint:** Force crash_test=1, close QEMU, reboot normally: `consecutive_crashes` should be 0. Force crash_test=1 twice in a row: second crash shows "Crash #2" on BSOD. Verify stats persist across QEMU restarts (NVRAM preserved if OVMF_VARS not reset). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Safe Mode Suggestion After Repeated Crashes

If the kernel detects 3+ consecutive crashes (from NVRAM stats), display a prominent "Safe mode recommended" message on the BSOD and offer to boot into safe mode on next restart.

**Files:** `src/kernel/panic.c`, `src/kernel/main/boot_hw.c`

> [!WARNING]
> **Regression risk:** If safe mode itself crashes, the kernel enters an infinite crash loop. Mitigation: if `consecutive_crashes >= 5`, skip safe mode and boot minimal (serial-only, no desktop). If >= 10, halt with "recovery required" message.
>
> **Scope boundary:** TODO-02 owns the repeated-failure policy, Safe Mode reason codes, and whether the next boot really enters Safe Mode. This section owns only the BSOD messaging, operator choice, and the request flag handed to that policy.

- [ ] In `panic_screen()`: if `consecutive_crashes >= 3`, display large yellow warning: "Multiple crashes detected. Safe mode boot recommended."
- [ ] Set `HKLM\SYSTEM\Recovery\BootToSafeMode = 1` or the equivalent recovery-request flag consumed by TODO-02 §5 and §10 (if Registry is accessible)
- [ ] In `boot_phase0()`: surface the repeated-crash recovery request to TODO-02 policy resolution instead of directly forcing `g_boot_info.config.boot_mode`
- [ ] Safe-mode behavior itself (skip non-essential drivers, reduce splash timeout, enable verbose logging) is implemented under TODO-02 §5; this section only verifies the request reaches that policy path
- [ ] Commit: `"kernel: safe mode suggestion after 3+ consecutive crashes"`

**Test checkpoint:** Force crash_test=1 three times in a row: third BSOD shows "Safe mode recommended" in yellow. Fourth boot (without crash_test): kernel boots in safe mode (boot_mode=1 in CONF line). Successful boot resets consecutive counter. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. "What Failed" Faulting Module Identification

Windows shows "What failed: ntfs.sys" when it can identify the faulting driver. Impossible OS can do the same using the symbol table: resolve RIP to the nearest symbol and extract the module/source file name.

**Files:** `src/kernel/panic.c`, `src/kernel/symtab.c`

- [ ] Add `symtab_resolve_module(uint64_t addr, char *module_out, uint32_t max)`: returns the source file or module name for a given kernel address (e.g., "ahci.c", "scheduler", "ntfs"); build on existing `symtab_resolve()` in `symtab.c` (already used by `panic_screen` stack walk)
- [ ] In `panic_screen()`, after stop code, display "What failed: <module>" in PANIC_FG_COLOR if a faulting module can be identified from RIP
- [ ] If RIP is in user-mode range (0x800000+), show "What failed: <process_name>" from current task's name
- [ ] Commit: `"kernel: 'What failed' faulting module display on BSOD"`

**Test checkpoint:** Force crash_test=1: BSOD shows "What failed: boot_desktop.c" (since the crash is triggered in boot_desktop.c). Force a #PF in a driver: BSOD shows the driver's source file name. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. Panic Screen Modes (User / Developer / QR)

Linux DRM panic (6.10+) offers three modes: `user` (simple message), `kmsg` (full log), `qr_code` (data QR). Impossible OS should offer similar flexibility: end users see a clean screen, developers see full diagnostics, QR mode encodes crash data for mobile scanning.

**Files:** `src/kernel/panic.c`, `resources/boot/boot.conf`

> [!TIP]
> Neither Windows nor Linux lets the user choose crash screen verbosity at runtime. Windows shows one fixed layout. Linux requires a kernel parameter reboot. Impossible OS reads it from Registry (changeable without reboot).

- [ ] Add `panic_screen=user|dev|qr` boot.conf option (default: `user`)
- [ ] `user` mode: icon + stop code + "What failed" + hint + restart countdown. No registers, no stack trace, no klog. Clean and non-intimidating.
- [ ] `dev` mode: full diagnostic layout (registers, stack trace, klog entries, symbols). Current behavior.
- [ ] `qr` mode: user mode + large QR code with compressed crash data. Minimal text, QR is prominent.
- [ ] Allow runtime switching via Registry `HKLM\SYSTEM\Recovery\PanicScreenMode` (string: "user", "dev", "qr")
- [ ] Commit: `"kernel: three panic screen modes (user, developer, QR)"`

**Test checkpoint:** Boot with `panic_screen=user`: BSOD shows clean minimal view. Boot with `panic_screen=dev`: full diagnostic dump. Boot with `panic_screen=qr`: large QR code with minimal text. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 11. Audio Crash Notification (PC Speaker Beep Codes)

Neither Windows nor Linux provides audio feedback during a kernel panic. Blind users or headless servers get zero information. Use the PC speaker (port 0x61/0x42/0x43) to emit a beep pattern encoding the crash category: similar to POST beep codes but for crashes.

**Files:** `src/kernel/panic.c`

- [ ] Define crash beep patterns (short=100ms, long=300ms, gap=200ms):
  - 1 long: General kernel panic (KeBugCheck)
  - 2 short: Page fault (#PF): memory access violation
  - 3 short: General protection (#GP): privilege or alignment fault
  - 4 short: Double fault (#DF): stack overflow or nested exception
  - 1 long + 2 short: Device not available (#NM): FPU/SSE issue
  - Continuous short: Unrecoverable: system halted permanently
- [ ] Implement `panic_beep(int pattern)` using PIT channel 2 + port 0x61 gate: works on all x86 hardware, no driver needed
- [ ] Play beep pattern at start of `panic_screen()`, before any framebuffer access (works even if FB is corrupted)
- [ ] Repeat pattern every 10 seconds during halt/countdown
- [ ] Commit: `"kernel: audio crash notification via PC speaker beep codes"`

**Test checkpoint:** Force crash_test=1: 1 long beep audible from QEMU (requires `-audiodev` or bare metal speaker). Force different crash types: different beep patterns. Verify beep plays even when framebuffer is not initialized (Phase 0 crash). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 12. Keyboard-Driven Recovery Actions at Crash Screen

The current BSOD only offers auto-restart or halt. Neither Windows nor Linux offers interactive recovery choices at crash time. Impossible OS can offer F-key recovery actions: Safe Mode, disable last driver, recovery shell.

**Files:** `src/kernel/panic.c`, `src/kernel/main/boot_hw.c`

> [!WARNING]
> **Regression risk:** Keyboard polling in the panic handler must use raw i8042 port reads (inb 0x60/0x64), not the keyboard driver (which may be corrupted). If the i8042 doesn't respond (0xFF), skip the menu silently.

- [ ] During countdown or halt, poll i8042 keyboard for specific scancodes (F1-F8)
- [ ] F2 = Defer next boot with last-loaded kernel module disabled: if `exec_find_module_by_pc(RIP)` (T17 §6) returns a module record, set `HKLM\SYSTEM\Recovery\BootBlacklistDriver` (REG_SZ) or NVRAM flag for loader to skip that image on next boot; if registry or module walk is unsafe, show "F2 unavailable" and log once to serial
- [ ] F1 = Restart in Safe Mode: set `HKLM\SYSTEM\Recovery\BootToSafeMode = 1` + NVRAM flag, then restart
- [ ] F5 = Restart with verbose logging: set `boot.conf` debug=1 equivalent via NVRAM flag
- [ ] F8 = Recovery shell: if implemented, boot directly to recovery environment (-> XREF: `10-platform-services/TODO-04-restore-recovery.md` (`D10 T11`))
- [ ] Esc = Cancel countdown and halt permanently
- [ ] Display F-key options as a footer menu: `F1 Safe Mode | F2 Last driver | F5 Verbose | F8 Recovery | Esc Halt`
- [ ] Commit: `"kernel: keyboard-driven recovery actions on BSOD"`

**Test checkpoint:** Force crash_test=1: F-key menu visible at bottom. Press F1: system restarts in safe mode (boot_mode=1 in next boot CONF line). Press F2: next boot defers or skips last faulting module when T17 module registry is available; otherwise serial shows unavailable once. Press Esc: countdown stops, system halts. i8042 raw poll must succeed or menu skips silently. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 13. Dump Collection Progress Percentage

Windows shows "37% complete" during crash dump collection. When T27 implements binary crash dumps, the BSOD should show progress during the potentially slow dump write.

**Files:** `src/kernel/panic.c`

> [!NOTE]
> This section is a **FOUNDATION** dependency on T27 §5 (minidump writer). The progress UI can be implemented now with a stub callback, but the actual percentage updates will come from T27's dump writer.

- [ ] Add `panic_set_progress(uint32_t percent)` callback for dump writers to report progress
- [ ] Display percentage text and progress bar during dump collection (before countdown starts)
- [ ] Text: "Collecting error information... 37% complete"
- [ ] Progress bar: same style as countdown bar, positioned above the countdown section
- [ ] When T27 reports an indeterminate phase (no byte-backed percent yet), show non-numeric status (for example "Still collecting error information") instead of a stuck 0% bar; align the `panic_set_progress` contract with T27 §5 callbacks
- [ ] If no dump writer is active (T27 §5 not yet implemented), skip progress and go straight to countdown
- [ ] Commit: `"kernel: dump collection progress percentage on BSOD"`

**Test checkpoint:** With T27 stub: crash_test=1 shows no progress (straight to countdown). After T27 §5: crash shows "Collecting error information... N% complete" with advancing bar; when the writer is in an indeterminate phase, BSOD shows non-numeric status (no frozen 0%). Verify progress doesn't overlap other sections. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐  | Feature                             | 🪟 Win11      | 🐧 Linux     | 🚀 Impossible OS            |
| --- | ----------------------------------- | ------------- | ------------ | --------------------------- |
| 💎  | GUI panic UI                        | ✅ Black 2025 | ✅ DRM panic | ⬜ §1 through §2 TTF        |
| 💎  | Stop code text                      | ✅ Yes        | ✅ String    | ✅ today                    |
| 💎  | Register dump                       | ✅ Minidump   | ✅ GPRs      | ✅ today                    |
| 💎  | Stack trace                         | ✅ dbg        | ✅ trace     | ✅ today                    |
| 💎  | What failed mod                     | ✅ Driver     | ⚠️ in trace   | ⬜ §9                       |
| ⭐  | Inline klog ctx                     | ❌ none       | ⚠️ kmsg       | ⬜ §3                       |
| ⭐  | Smart data QR                       | ❌ gone       | ✅ zlib kmsg | ⬜ §4                       |
| 💎  | Auto restart                        | ✅ cfg        | ✅ timeout   | ⬜ §5                       |
| ⭐  | Crash hints                         | ❌ generic    | ❌ raw       | ⬜ §6                       |
| ⭐  | Crash stats NVRAM                   | ❌ cloud      | ❌ none      | ⬜ §7                       |
| ⭐  | Safe mode nudge                     | ⚠️ WinRE       | ❌ none      | ⬜ §8                       |
| ⭐  | Panic modes                         | ❌ fixed      | ✅ 3 modes   | ⬜ §10                      |
| ⭐  | Audio beep                          | ❌ silent     | ❌ silent    | ⬜ §11                      |
| ⭐  | F-key recovery                      | ❌ cloud      | ❌ SysRq     | ⬜ §12                      |
| 💎  | Dump progress %                     | ✅ text       | ❌ none      | ⬜ §13 + T27 §5             |
| ⭐  | Runtime panic verbosity (no reboot) | ❌ fixed UI   | ⚠️ boot param | ⬜ §10 Registry + boot.conf |

**Parity:** GUI, codes, regs, stack, module hint, QR or restart or progress where Win or Linux ship equivalents. **Exclusive:** inline klog, hints, NVRAM stats, safe-mode path, user or dev or QR modes, PC speaker, F-key menu, structured QR, Registry-driven verbosity without a full kernel rebuild.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bsod()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`). Use `test_suite_register_cat(..., TEST_CAT_BOOT)` for each case.

- [ ] Create `src/kernel/test/test_bsod.c` with:
  - `panic_has_ttf()` returns 0 before font manager init, 1 after
  - `klog_get_recent()` copies correct number of entries from ring
  - `crash_hints` entry for vector 7 (#NM) returns non-NULL advice string
  - QR code generator produces valid grid for binary payload
  - Crash stats NVRAM header magic is correct
  - `symtab_resolve_module()` returns non-empty string for known kernel RIP
  - Beep pattern table has entries for all 32 x86 exception vectors
  - Panic screen mode enum has 3 values (USER, DEV, QR)
- [ ] Add `extern void test_register_bsod(void);` in `test_runner.c`, call `test_register_bsod()` from `test_runner_init()`
- [ ] Commit: `"test: BSOD UX enhancement unit tests"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot`; every `test_bsod_*` case PASS; `tail -1 build/build.log` is `=== BUILD OK ===`.

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===`
- [ ] QEMU WHPX: crash_test=1 -> BSOD renders with TTF fonts (Phase 2+), all sections visible
- [ ] QEMU WHPX: Phase 1 panic (force via POST code injection) -> BSOD renders with boot_font fallback
- [ ] QEMU TCG: crash_test=1 -> BSOD renders correctly on single CPU
- [ ] VirtualBox: crash_test=1 -> BSOD renders at 1920x1080 VMSVGA
- [ ] Bare metal: verify BSOD renders on real hardware (if panic occurs)
- [ ] QR code scans correctly from phone camera: decoder shows structured crash data
- [ ] Crash stats persist across QEMU restarts (NVRAM preserved if OVMF_VARS not reset)
- [ ] 3 consecutive crashes -> safe mode suggestion appears; auto-restart disabled
- [ ] Normal auto-restart: crash_test=1 -> countdown runs (default 30s) -> system reboots -> next boot is normal
- [ ] AutoRestart=0: crash_test=1 -> BSOD halts immediately (no countdown)
- [ ] Successful boot after crash -> `ConsecutiveCrashes` reset to 0 in Registry
- [ ] "What failed" shows correct module name for crash_test (boot_desktop.c)
- [ ] F1 on BSOD -> next boot in safe mode (boot_mode=1)
- [ ] F2 on BSOD: next boot defers last faulting module when T17 §6 module walk is safe; otherwise one serial line `F2 unavailable`
- [ ] PC speaker beep audible during crash (QEMU + bare metal)

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)

---

## History

| Date       | Action   | Summary |
| ---------- | -------- | ------- |
| 2026-04-13 | validate | Full-file validate: `→ XREF` Inputs; added klog/symtab/boot.conf/boot_hw/boot_desktop anchors; OS table re-padded + header; §1 kd bullet after WARNING; prose `--` fixes; D10/D14 shorthand on cross-domain XREFs; History created. |
| 2026-04-13 | validate | Colon joiners for prose (CLAUDE.md); Depends On `none` for independent rows; §10 dev-mode bullet clarified; Unit Tests checkpoint uses `;` after shell command; §7 `RegSetValueEx` bullet restored (call ... to confirm); XREF § targets checked (T29 §15, T27 §5); `run-boot-tests.bat` present. |
| 2026-04-13 | gap-analysis | Win11 24H2 black BSOD + Linux 6.12 DRM QR researched; code-truth: T28 UX all `[ ]`, `symtab_resolve` exists, no `klog_get_recent`/TTF panic path; §13 indeterminate dump UX bullet + checkpoint; reciprocal XREF in T04 Inputs. |
| 2026-04-13 | validate | Structural pass: Inputs T03 § disambiguation; §2 Win11 24H2 wording; §12 F8 `D10 T11`; continuation grep clean; XREF §1-§15 and T27 §1-§9 anchors OK; IO row 13 blocked on T27 §5 `[ ]`; `exec_find_module_by_pc` exists (`exec.c`); `run-boot-tests.bat` OK. |
