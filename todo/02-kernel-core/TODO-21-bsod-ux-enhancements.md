# TODO-21 -- BSOD / Panic Screen & Crash Experience

> **Goal:** Transform the panic screen from a developer debug dump into a polished, informative crash experience that surpasses both Windows 11 (which dropped its QR code and sad face in 2025, replacing with a brief black screen) and Linux (which added DRM panic QR codes with compressed kmsg in 6.12 -- the current state of the art). Use TTF fonts, display actionable crash context, encode compressed crash data in a QR code, provide keyboard-driven recovery actions, track crash history locally, and signal crashes via audio for accessibility.
> When complete, Impossible OS has the best crash experience of any operating system -- smarter than Windows 11 (generic black screen, cloud-dependent recovery), richer than Linux DRM panic (data-bearing QR but no GUI polish, no recovery actions, no local crash history).

> [!IMPORTANT]
> **Current state:** `panic_screen()` in `src/kernel/panic.c` (617 lines) renders a full-screen blue screen with icon, stop code, register dump, RBP-chain stack trace, crash dump path, and 30-second auto-restart countdown. Uses `printk()` + `fb_set_color()` with the embedded boot_font (Selawik Regular, 18px bitmap atlas). Works from Phase 1 onward (requires framebuffer). Falls back to serial-only for Phase 0 panics.

> [!CAUTION]
> **Memory rule:** `panic_screen()` runs after a kernel fault. `kmalloc()` may be corrupted. Use only `printk()`, direct framebuffer writes, static buffers, and `pmm_alloc_contiguous()` (if PMM is intact). Never call VFS or scheduler functions from the panic screen -- the crash may have corrupted them.

---

## Inputs

- `src/kernel/panic.c` -- current BSOD implementation (617 lines)
- `src/kernel/bsod_icon.h` -- 128x128 alpha-blended emoticon icon
- `src/kernel/gfx/gfx_text.c` -- boot_font API + TTF font manager
- `include/font_mgr.h` -- `ttf_get()`, `ttf_draw_string()` declarations
- -> XREF: `TODO-16-crash-dump-generation.md` -- binary crash dump mechanics (scope boundary: TODO-16 owns dump format/analysis, this TODO owns on-screen UX)
- -> XREF: `TODO-02-system-logging.md section 9` -- crash-persistent klog capture (ring buffer persisted to physical memory; this TODO displays recovered entries on-screen)
- -> XREF: `TODO-01-kernel-init-sequencing.md section 9` -- degraded-boot recovery screen (related but separate: recovery screen is for non-fatal degraded boot, BSOD is for fatal panics)
- -> XREF: `TODO-10-exception-dispatch-seh.md` -- exception dispatch routes faults to panic_screen(); STOP code taxonomy depends on this TODO's KeBugCheckEx implementation
- -> XREF: `TODO-18-kernel-debugger-kd-protocol.md` -- debugger first-chance notification happens before panic; BSOD only shows if debugger is not attached
- -> XREF: `14-host-tools/TODO-04-crash-decode.md` -- host-side crash dump decoder; QR code data format must be compatible
- -> XREF: `10-services-security/TODO-04-restore-recovery.md` -- safe mode and recovery environment integration

---

## Outcome

- Panic screen uses Selawik Regular/Bold via TTF font manager when available (Phase 2+), falls back to boot bitmap font for Phase 0-1 panics -- text is crisp and properly sized at any resolution.
- Layout has proper margins, section spacing, and visual hierarchy -- stop code prominent, registers compact, stack trace scrollable.
- Three panic screen modes: `user` (clean, minimal info), `developer` (full registers + stack + klog), `qr` (data-bearing QR code) -- switchable via boot.conf `panic_screen=`.
- Last 10 klog entries displayed inline on developer/user modes providing crash context without requiring serial access.
- QR code encodes **compressed crash data** (stop code, registers, last 10 klog entries, faulting module) -- not a generic URL. Self-hosted decoder at configurable URL.
- "What failed" module identification -- identifies the faulting kernel module from RIP address using the symbol table.
- Crash statistics tracked in UEFI NVRAM: total crash count, last 5 stop codes with timestamps.
- After 3+ crashes in a row without successful boot, BSOD suggests safe mode and offers keyboard-driven recovery (F1=Safe Mode, F8=Recovery Shell).
- Auto-restart countdown improved with smooth progress bar, keyboard interrupt support (any key cancels restart).
- Audio crash notification: PC speaker beep pattern encodes crash category for accessibility (no screen reader possible during panic).

---

## Implementation Order

| Star | Order | Deliverable                                      | Depends On     | Status |
| --- | :---: | ------------------------------------------------ | -------------- | :----: |
| 💎  |   1   | TTF font rendering in panic screen               | --             |  [ ]   |
| 💎  |   2   | Improved layout and visual hierarchy             | §1             |  [ ]   |
| ⭐  |   3   | Crash context: last 10 klog entries inline       | --             |  [ ]   |
| ⭐  |   4   | Smart QR code with compressed crash data         | §2             |  [ ]   |
| 💎  |   5   | Auto-restart countdown improvements              | §2             |  [ ]   |
| ⭐  |   6   | Crash analysis hints on-screen                   | §2             |  [ ]   |
| ⭐  |   7   | Crash statistics counter in NVRAM                | --             |  [ ]   |
| ⭐  |   8   | Safe mode suggestion after repeated crashes      | §7             |  [ ]   |
| 💎  |   9   | "What failed" faulting module identification     | --             |  [ ]   |
| ⭐  |  10   | Panic screen modes (user / developer / QR)       | §1, §3, §4    |  [ ]   |
| ⭐  |  11   | Audio crash notification (PC speaker beep codes) | --             |  [ ]   |
| ⭐  |  12   | Keyboard-driven recovery actions at crash screen | §5, §8        |  [ ]   |
| 💎  |  13   | Dump collection progress percentage              | T16 §5        |  [ ]   |

> 💎 = parity -- Windows 11 and/or Linux have equivalent features.
> ⭐ = exclusive -- Impossible OS is superior or first.

---

## 1. TTF Font Rendering in Panic Screen

Use `ttf_draw_string()` when the font manager is initialized, fall back to `printk()` + boot_font when it isn't. The TTF path renders at the current screen resolution with proper antialiasing; the boot_font path is the existing behavior.

**Files:** `src/kernel/panic.c`, `include/font_mgr.h`

- [ ] Add `static int panic_has_ttf(void)` helper -- returns 1 if `ttf_get(FONT_UI, 16)` returns non-NULL
- [ ] Create `panic_print(int x, int y, uint32_t color, int size, const char *text)` wrapper -- uses `ttf_draw_string()` if available, otherwise `printk()` with `fb_set_color()`
- [ ] Replace all `printk()` calls in `panic_screen()` with `panic_print()` for consistent rendering
- [ ] Phase 0-1 panics: `panic_has_ttf()` returns 0, falls back to boot_font via `printk()` -- no change in behavior
- [ ] Phase 2+ panics: title rendered in FONT_UI_BOLD at 24px, body in FONT_UI at 14px, registers in FONT_MONO at 12px
- [ ] Commit: `"kernel: TTF font rendering in BSOD with boot_font fallback"`

> [!WARNING]
> **Regression risk:** If `ttf_draw_string()` crashes during panic (font data corrupted by the crash), the BSOD won't render. Mitigation: wrap TTF calls in a simple null-check guard; if `ttf_get()` returns NULL or the surface write faults, immediately fall back to `printk()`.

**Test checkpoint:** Force a panic after desktop init (crash_test=1) -- BSOD shows with TTF-rendered text (crisp antialiased). Force a panic during Phase 1 (before font manager) -- BSOD shows with boot_font (bitmap, same as today). Verify on QEMU WHPX, TCG, VirtualBox.

---

## 2. Improved Layout and Visual Hierarchy

Redesign the BSOD layout with proper margins, section dividers, and visual hierarchy. Windows 11 uses a sad face + two sentences + QR code; we go further with structured sections.

**Files:** `src/kernel/panic.c`

- [ ] Define layout constants: `PANIC_MARGIN_X = 80`, `PANIC_MARGIN_Y = 60`, `PANIC_SECTION_GAP = 24`
- [ ] Icon section: keep 128x128 emoticon, position at `(MARGIN_X, MARGIN_Y)`
- [ ] Title section: two-line narrative right of icon, vertically centered to icon height
- [ ] Stop code section: prominent large text (24px bold), separated by divider line
- [ ] Technical details section: collapsible-style layout -- registers in 2-column grid, stack trace in monospace
- [ ] Footer section: version string + crash dump path + restart countdown, anchored to bottom
- [ ] Support HiDPI: scale all layout constants by `g_boot_info.hidpi` flag (2x for >= 2560px width)
- [ ] Commit: `"kernel: BSOD layout redesign with visual hierarchy and HiDPI"`

**Test checkpoint:** BSOD renders with clean margins on 1280x720 (QEMU), 1920x1080 (VBox), and 2560x1440+ (if VBox VMSVGA custom). Register dump is in 2 columns. Stack trace is below registers. Footer is anchored to screen bottom. Verify no text truncation on any resolution.

---

## 3. Crash Context: Last 10 klog Entries Inline

Display the last 10 ring buffer entries on the BSOD, giving immediate crash context without needing serial access. This is the most useful information for first-pass triage.

**Files:** `src/kernel/panic.c`, `include/kernel/klog.h`

- [ ] Add `klog_get_recent(klog_entry_t *out, uint32_t max_count)` -- copies the last N ring entries into a caller-provided buffer (no allocation)
- [ ] In `panic_screen()`, after register dump, render "--- Last 10 log entries ---" section
- [ ] Each entry rendered as: `[timestamp] LEVEL subsystem: message` in FONT_MONO at 11px (or boot_font)
- [ ] Color-code by level: INFO=white, WARN=yellow, ERROR=red
- [ ] Limit to 10 entries to avoid scrolling past the screen bottom
- [ ] Commit: `"kernel: display last 10 klog entries on BSOD for crash context"`

**Test checkpoint:** Force crash_test=1 -- BSOD shows 10 recent log entries including the "crash_test=1 -- triggering deliberate BSOD" warning. Entries are color-coded. Verify on QEMU WHPX.

---

## 4. Smart QR Code with Compressed Crash Data

Render a QR code containing **actual compressed crash data** -- not a generic URL. Linux 6.12+ pioneered this with Zlib-compressed kmsg in QR codes (written in Rust). Impossible OS goes further: structured crash report with stop code, registers, faulting module, and last 10 klog entries, all Zlib-compressed into a data-bearing URL. Windows 11 **removed** its QR code entirely in 2025 (it only linked to a generic page).

**Files:** `src/kernel/panic.c`, new `src/kernel/qrcode.c`

- [ ] Implement QR code generator (Version 6-10, binary mode) -- static buffers, no allocation, no FPU. ~300 lines of C. Higher version than V2 needed for compressed binary payload.
- [ ] Build structured crash payload: `{ stop_code, rip, cr2, faulting_module[32], klog_entries[10][128] }`
- [ ] Compress payload with minimal Zlib deflate (or simpler LZ4 if TODO-20 delivers it first) -- static scratch buffer
- [ ] Encode as URL: `https://impossible.os/crash#?a=x86_64&v=VERSION&z=<base64_compressed_data>`
- [ ] Render QR code at 4x scale in bottom-right corner of BSOD, white modules on blue background
- [ ] Include text below QR: "Scan to decode crash data" + fallback text URL
- [ ] Configurable decoder URL via `HKLM\SYSTEM\Recovery\CrashQRUrl` (for self-hosted decoders)
- [ ] Commit: `"kernel: smart QR code with compressed crash data on BSOD"`

> [!TIP]
> Linux 6.12's QR code contains raw compressed kmsg. Impossible OS encodes a **structured report** with parsed fields (stop code, module name, recent events) -- the decoder can show a formatted diagnosis page, not just raw text. This is superior to both Windows (generic URL, now removed) and Linux (raw data requiring manual parsing).

**Test checkpoint:** Force crash_test=1 -- QR code visible in bottom-right. Scan with phone -- decoder page shows structured crash info (stop code, registers, last 10 log entries). Verify QR encodes correctly with `zbarimg` or phone scanner. Verify QR doesn't overlap other sections at 1280x720 and 1920x1080.

---

## 5. Auto-Restart Countdown Improvements

Improve the existing 30-second countdown with smoother animation, keyboard cancel support, and better visual feedback.

**Files:** `src/kernel/panic.c`

- [ ] Smooth progress bar: update every 100ms instead of every second (10x smoother animation)
- [ ] Add "Press any key to cancel restart" text below countdown
- [ ] Check keyboard buffer in countdown loop: if any key pressed, cancel restart and display "System halted. Press reset to restart."
- [ ] Move countdown display from absolute pixel position to footer-anchored position (works at any resolution)
- [ ] Registry-configurable: `HKLM\SYSTEM\Recovery\AutoRestart` (DWORD, seconds, 0=disabled) -- already partially implemented
- [ ] Commit: `"kernel: improved BSOD auto-restart countdown with key cancel"`

**Test checkpoint:** Force crash_test=1 -- countdown renders smoothly at bottom. Press a key during countdown -- restart cancels, "System halted" shown. Set AutoRestart=0 in Registry -- no countdown, immediate halt. Verify on QEMU WHPX (keyboard must work via i8042 probe).

---

## 6. Crash Analysis Hints On-Screen

Display helpful text based on the stop code, similar to Windows "search for STOP_CODE online". Go further with specific advice per exception type.

**Files:** `src/kernel/panic.c`

- [ ] Create `static const char *crash_hints[32]` table mapping exception vectors to user-friendly advice:
  - #DE: "A division by zero occurred. Check recent arithmetic operations."
  - #PF: "A memory access violation occurred at address shown in CR2."
  - #GP: "A general protection fault -- possible invalid memory access or privilege violation."
  - #DF: "A double fault -- stack overflow or corrupted interrupt handler."
  - #NM: "FPU/SSE instruction used without proper initialization."
  - Generic: "An unexpected error occurred. Check the stack trace for the faulting function."
- [ ] Display hint text below stop code in PANIC_DIM_COLOR
- [ ] Commit: `"kernel: per-exception crash analysis hints on BSOD"`

**Test checkpoint:** Force different crash types (crash_test for generic, NULL deref for #PF, div-by-zero for #DE) -- each shows appropriate hint text. Verify hints don't overlap other sections.

---

## 7. Crash Statistics & Loop Protection

Track crash history in UEFI NVRAM and Registry so the kernel knows how many times it has crashed, detects crash loops, and can disable auto-restart after repeated failures. Absorbed from `08-graphics-ui/TODO-03 §6` (crash loop protection).

**Files:** `src/kernel/panic.c`, `src/kernel/main/boot_hw.c`, `src/kernel/main/boot_desktop.c`

- [ ] Define `ImpossibleCrashStats` NVRAM variable: `{ uint32_t total_crashes, uint32_t consecutive_crashes, struct { uint32_t stop_code; uint64_t timestamp; } last_5[5] }`
- [ ] In `panic_screen()`: read current stats, increment `total_crashes` and `consecutive_crashes`, push stop code to circular `last_5[]`, write back to NVRAM
- [ ] Also write `HKLM\SYSTEM\Recovery\ConsecutiveCrashes` (DWORD) if Registry is accessible -- dual persistence (NVRAM survives corrupted disk, Registry survives NVRAM issues)
- [ ] Write `HKLM\SYSTEM\Recovery\LastBootTime` (DWORD, PIT ticks) after `registry_init()` in Phase 2
- [ ] In `boot_phase0()` (after UEFI runtime init): read NVRAM stats, reset `consecutive_crashes` to 0 (successful boot clears the counter)
- [ ] In `boot_desktop.c` after successful `desktop_init()`: `RegSetValueEx(HKLM, key, "ConsecutiveCrashes", REG_DWORD, &zero, 4)` -- confirms boot succeeded
- [ ] If `consecutive_crashes >= 3` in `panic_screen()`: skip auto-restart countdown, display halt message instead
- [ ] Display "Crash #N" on BSOD screen (total lifetime crash count)
- [ ] Log `[recovery] ConsecutiveCrashes=%u` at both increment and reset sites
- [ ] Commit: `"kernel: crash statistics + loop protection via NVRAM and Registry"`

> [!NOTE]
> UEFI NVRAM write endurance: one write per crash + one write per successful boot = 2 writes per boot cycle. Combined with ImpossiblePOST (2), ImpossibleBootPerf (1), and ImpossibleCrashLog (1) = 6 total NVRAM writes per boot. ~100K cycle endurance / 6 = ~16K boot cycles before flash wear concern.

**Test checkpoint:** Force crash_test=1, close QEMU, reboot normally -- `consecutive_crashes` should be 0. Force crash_test=1 twice in a row -- second crash shows "Crash #2" on BSOD. Verify stats persist across QEMU restarts (NVRAM preserved if OVMF_VARS not reset).

---

## 8. Safe Mode Suggestion After Repeated Crashes

If the kernel detects 3+ consecutive crashes (from NVRAM stats), display a prominent "Safe mode recommended" message on the BSOD and offer to boot into safe mode on next restart.

**Files:** `src/kernel/panic.c`, `src/kernel/main/boot_hw.c`

- [ ] In `panic_screen()`: if `consecutive_crashes >= 3`, display large yellow warning: "Multiple crashes detected. Safe mode boot recommended."
- [ ] Set `HKLM\SYSTEM\Recovery\BootToSafeMode = 1` in Registry (if Registry is accessible)
- [ ] In `boot_phase0()`: if `consecutive_crashes >= 3` from NVRAM, set `g_boot_info.config.boot_mode = 1` (safe mode) automatically
- [ ] Safe mode: skip non-essential drivers, reduce splash timeout, enable verbose logging
- [ ] Commit: `"kernel: safe mode suggestion after 3+ consecutive crashes"`

> [!WARNING]
> **Regression risk:** If safe mode itself crashes, the kernel enters an infinite crash loop. Mitigation: if `consecutive_crashes >= 5`, skip safe mode and boot minimal (serial-only, no desktop). If >= 10, halt with "recovery required" message.

**Test checkpoint:** Force crash_test=1 three times in a row -- third BSOD shows "Safe mode recommended" in yellow. Fourth boot (without crash_test) -- kernel boots in safe mode (boot_mode=1 in CONF line). Successful boot resets consecutive counter. Verify on QEMU WHPX.

---

## 9. "What Failed" Faulting Module Identification

Windows shows "What failed: ntfs.sys" when it can identify the faulting driver. Impossible OS can do the same using the symbol table -- resolve RIP to the nearest symbol and extract the module/source file name.

**Files:** `src/kernel/panic.c`, `src/kernel/symtab.c`

- [ ] Add `symtab_resolve_module(uint64_t addr, char *module_out, uint32_t max)` -- returns the source file or module name for a given kernel address (e.g., "ahci.c", "scheduler", "ntfs")
- [ ] In `panic_screen()`, after stop code, display "What failed: <module>" in PANIC_FG_COLOR if a faulting module can be identified from RIP
- [ ] If RIP is in user-mode range (0x800000+), show "What failed: <process_name>" from current task's name
- [ ] Commit: `"kernel: 'What failed' faulting module display on BSOD"`

**Test checkpoint:** Force crash_test=1 -- BSOD shows "What failed: boot_desktop.c" (since the crash is triggered in boot_desktop.c). Force a #PF in a driver -- BSOD shows the driver's source file name. Verify on QEMU WHPX.

---

## 10. Panic Screen Modes (User / Developer / QR)

Linux DRM panic (6.10+) offers three modes: `user` (simple message), `kmsg` (full log), `qr_code` (data QR). Impossible OS should offer similar flexibility -- end users see a clean screen, developers see full diagnostics, QR mode encodes crash data for mobile scanning.

**Files:** `src/kernel/panic.c`, `resources/boot/boot.conf`

- [ ] Add `panic_screen=user|dev|qr` boot.conf option (default: `user`)
- [ ] `user` mode: icon + stop code + "What failed" + hint + restart countdown. No registers, no stack trace, no klog. Clean and non-intimidating.
- [ ] `dev` mode: everything -- registers, stack trace, klog entries, symbols. Current behavior.
- [ ] `qr` mode: user mode + large QR code with compressed crash data. Minimal text, QR is prominent.
- [ ] Allow runtime switching via Registry `HKLM\SYSTEM\Recovery\PanicScreenMode` (string: "user", "dev", "qr")
- [ ] Commit: `"kernel: three panic screen modes -- user, developer, QR"`

> [!TIP]
> Neither Windows nor Linux lets the user choose crash screen verbosity at runtime. Windows shows one fixed layout. Linux requires a kernel parameter reboot. Impossible OS reads it from Registry -- changeable without reboot.

**Test checkpoint:** Boot with `panic_screen=user` -- BSOD shows clean minimal view. Boot with `panic_screen=dev` -- full diagnostic dump. Boot with `panic_screen=qr` -- large QR code with minimal text. Verify all three on QEMU WHPX.

---

## 11. Audio Crash Notification (PC Speaker Beep Codes)

Neither Windows nor Linux provides audio feedback during a kernel panic. Blind users or headless servers get zero information. Use the PC speaker (port 0x61/0x42/0x43) to emit a beep pattern encoding the crash category -- similar to POST beep codes but for crashes.

**Files:** `src/kernel/panic.c`

- [ ] Define crash beep patterns (short=100ms, long=300ms, gap=200ms):
  - 1 long: General kernel panic (KeBugCheck)
  - 2 short: Page fault (#PF) -- memory access violation
  - 3 short: General protection (#GP) -- privilege or alignment fault
  - 4 short: Double fault (#DF) -- stack overflow or nested exception
  - 1 long + 2 short: Device not available (#NM) -- FPU/SSE issue
  - Continuous short: Unrecoverable -- system halted permanently
- [ ] Implement `panic_beep(int pattern)` using PIT channel 2 + port 0x61 gate -- works on all x86 hardware, no driver needed
- [ ] Play beep pattern at start of `panic_screen()`, before any framebuffer access (works even if FB is corrupted)
- [ ] Repeat pattern every 10 seconds during halt/countdown
- [ ] Commit: `"kernel: audio crash notification via PC speaker beep codes"`

**Test checkpoint:** Force crash_test=1 -- 1 long beep audible from QEMU (requires `-audiodev` or bare metal speaker). Force different crash types -- different beep patterns. Verify beep plays even when framebuffer is not initialized (Phase 0 crash).

---

## 12. Keyboard-Driven Recovery Actions at Crash Screen

The current BSOD only offers auto-restart or halt. Neither Windows nor Linux offers interactive recovery choices at crash time. Impossible OS can offer F-key recovery actions: Safe Mode, disable last driver, recovery shell.

**Files:** `src/kernel/panic.c`, `src/kernel/main/boot_hw.c`

- [ ] During countdown or halt, poll i8042 keyboard for specific scancodes (F1-F8)
- [ ] F1 = Restart in Safe Mode: set `HKLM\SYSTEM\Recovery\BootToSafeMode = 1` + NVRAM flag, then restart
- [ ] F5 = Restart with verbose logging: set `boot.conf` debug=1 equivalent via NVRAM flag
- [ ] F8 = Recovery shell: if implemented, boot directly to recovery environment (-> XREF: `10-services-security/TODO-04-restore-recovery.md`)
- [ ] Esc = Cancel countdown and halt permanently
- [ ] Display F-key options as a footer menu: `F1 Safe Mode | F5 Verbose | F8 Recovery | Esc Halt`
- [ ] Commit: `"kernel: keyboard-driven recovery actions on BSOD"`

> [!WARNING]
> **Regression risk:** Keyboard polling in the panic handler must use raw i8042 port reads (inb 0x60/0x64), not the keyboard driver (which may be corrupted). If the i8042 doesn't respond (0xFF), skip the menu silently.

**Test checkpoint:** Force crash_test=1 -- F-key menu visible at bottom. Press F1 -- system restarts in safe mode (boot_mode=1 in next boot CONF line). Press Esc -- countdown stops, system halts. Verify on QEMU WHPX (i8042 probe must succeed).

---

## 13. Dump Collection Progress Percentage

Windows shows "37% complete" during crash dump collection. When TODO-16 implements binary crash dumps, the BSOD should show progress during the potentially slow dump write.

**Files:** `src/kernel/panic.c`

- [ ] Add `panic_set_progress(uint32_t percent)` callback for dump writers to report progress
- [ ] Display percentage text and progress bar during dump collection (before countdown starts)
- [ ] Text: "Collecting error information... 37% complete"
- [ ] Progress bar: same style as countdown bar, positioned above the countdown section
- [ ] If no dump writer is active (TODO-16 not yet implemented), skip progress and go straight to countdown
- [ ] Commit: `"kernel: dump collection progress percentage on BSOD"`

> [!NOTE]
> This section is a **FOUNDATION** dependency on TODO-16 §5 (minidump writer). The progress UI can be implemented now with a stub callback, but the actual percentage updates will come from TODO-16's dump writer.

**Test checkpoint:** With TODO-16 stub: crash_test=1 shows no progress (straight to countdown). After TODO-16 §5: crash shows "Collecting error information... N% complete" with advancing bar. Verify progress doesn't overlap other sections.

---

## OS Comparison

| ⭐ | Feature                | Win11 (2025)            | Linux (6.12+)            | Impossible OS               |
|----|------------------------|-------------------------|--------------------------|------------------------------|
| 💎 | GUI panic screen       | ✅ Black screen (2025)  | ✅ DRM panic (6.10+)    | ⬜ §1-§2 TTF + layout       |
| 💎 | Stop code display      | ✅ Name + hex           | ✅ Panic string          | ✅ Already done              |
| 💎 | Register dump          | ✅ Minidump context     | ✅ All GPRs              | ✅ Already done              |
| 💎 | Stack trace            | ✅ via debugger         | ✅ Inline backtrace      | ✅ Already done              |
| 💎 | "What failed" module   | ✅ Driver name shown    | ⚠️ In stack trace only   | ⬜ §9 symtab resolve        |
| ⭐ | Crash context (logs)   | ❌ Not shown            | ⚠️ kmsg mode only        | ⬜ §3 last 10 entries        |
| ⭐ | Smart QR (crash data)  | ❌ Removed in 2025      | ✅ Zlib-compressed kmsg  | ⬜ §4 structured + Zlib     |
| 💎 | Auto-restart           | ✅ Configurable         | ✅ panic_timeout         | ⬜ §5 improved countdown     |
| ⭐ | Per-exception hints    | ❌ Generic message      | ❌ Raw dump              | ⬜ §6 actionable advice      |
| ⭐ | Crash stats (NVRAM)    | ❌ WER server-side      | ❌ No local persistence  | ⬜ §7 local NVRAM counter    |
| ⭐ | Safe mode suggestion   | ⚠️ WinRE after 3 fails  | ❌ No auto-recovery      | ⬜ §8 consecutive detection  |
| ⭐ | Panic screen modes     | ❌ One fixed layout     | ✅ 3 modes (user/kmsg/qr)| ⬜ §10 user/dev/qr          |
| ⭐ | Audio crash signal     | ❌ Silent               | ❌ Silent                | ⬜ §11 PC speaker beeps     |
| ⭐ | Interactive recovery   | ❌ Cloud QMR only       | ❌ SysRq dead on panic   | ⬜ §12 F-key recovery menu  |
| 💎 | Dump progress %        | ✅ "37% complete"       | ❌ No visual feedback    | ⬜ §13 progress bar (T16)   |

> **Parity:** GUI crash screen, stop codes, register dump, stack trace, "what failed" module, QR code, auto-restart, dump progress.
> **Exclusive (Impossible OS first):** inline crash context logs, per-exception hints, local crash stats without cloud, audio beep codes for accessibility, keyboard-driven recovery actions at crash time, runtime-switchable panic screen modes.
> **Competitive edge:** Smart QR encodes structured crash data (better than Linux's raw kmsg). Interactive recovery actions at crash time (neither OS offers this). Audio crash signals for accessibility (no OS does this).

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bsod()`.

- [ ] Create `src/kernel/test/test_bsod.c` with:
  - `panic_has_ttf()` returns 0 before font manager init, 1 after
  - `klog_get_recent()` copies correct number of entries from ring
  - `crash_hints[7]` (DEVICE_NOT_AVAILABLE) returns non-NULL string
  - QR code generator produces valid grid for binary payload
  - Crash stats NVRAM header magic is correct
  - `symtab_resolve_module()` returns non-empty string for known kernel RIP
  - Beep pattern table has entries for all 32 x86 exception vectors
  - Panic screen mode enum has 3 values (USER, DEV, QR)
- [ ] Register in `test_runner_init()`: `test_register_bsod()`
- [ ] Commit: `"test: BSOD UX enhancement unit tests"`

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===`
- [ ] QEMU WHPX: crash_test=1 -> BSOD renders with TTF fonts (Phase 2+), all sections visible
- [ ] QEMU WHPX: Phase 1 panic (force via POST code injection) -> BSOD renders with boot_font fallback
- [ ] QEMU TCG: crash_test=1 -> BSOD renders correctly on single CPU
- [ ] VirtualBox: crash_test=1 -> BSOD renders at 1920x1080 VMSVGA
- [ ] Bare metal: verify BSOD renders on real hardware (if panic occurs)
- [ ] QR code scans correctly from phone camera -- decoder shows structured crash data
- [ ] Crash stats persist across QEMU restarts (NVRAM preserved if OVMF_VARS not reset)
- [ ] 3 consecutive crashes -> safe mode suggestion appears; auto-restart disabled
- [ ] Normal auto-restart: crash_test=1 -> countdown runs (default 30s) -> system reboots -> next boot is normal
- [ ] AutoRestart=0: crash_test=1 -> BSOD halts immediately (no countdown)
- [ ] Successful boot after crash -> `ConsecutiveCrashes` reset to 0 in Registry
- [ ] "What failed" shows correct module name for crash_test (boot_desktop.c)
- [ ] F1 on BSOD -> next boot in safe mode (boot_mode=1)
- [ ] PC speaker beep audible during crash (QEMU + bare metal)
