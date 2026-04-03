# TODO-21 -- BSOD / Panic Screen UX Enhancements

> **Goal:** Transform the panic screen from a developer debug dump into a polished, informative, Windows 11-quality crash experience. Use the TTF font manager when available, display actionable crash context, persist crash history in NVRAM, and guide users toward recovery. The current BSOD works but uses the boot bitmap font at a fixed size, has no crash context summary, and provides no guidance beyond "we'll restart for you."
> When complete, Impossible OS has the best panic screen of any operating system -- better than Windows 11 (no QR code, no context), better than Linux (text-only panic, no GUI).

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

---

## Outcome

- Panic screen uses Selawik Regular/Bold via TTF font manager when available (Phase 2+), falls back to boot bitmap font for Phase 0-1 panics -- text is crisp and properly sized at any resolution.
- Layout has proper margins, section spacing, and visual hierarchy -- stop code prominent, registers compact, stack trace scrollable.
- Last 10 klog entries displayed inline on BSOD providing crash context without requiring serial access.
- QR code rendered on-screen linking to `https://impossible.os/crash/<STOP_CODE>` for quick mobile lookup.
- Crash statistics tracked in UEFI NVRAM: total crash count, last 5 stop codes with timestamps.
- After 3+ crashes in a row without successful boot, BSOD suggests safe mode boot.
- Auto-restart countdown improved with smooth progress bar, keyboard interrupt support (any key cancels restart).

---

## Implementation Order

| Star | Order | Deliverable                                 | Depends On     | Status |
| --- | :---: | ------------------------------------------- | -------------- | :----: |
| 💎  |   1   | TTF font rendering in panic screen          | --             |  [ ]   |
| 💎  |   2   | Improved layout and visual hierarchy        | section 1              |  [ ]   |
| ⭐  |   3   | Crash context: last 10 klog entries inline  | --             |  [ ]   |
| ⭐  |   4   | QR code crash URL on-screen                 | section 2              |  [ ]   |
| 💎  |   5   | Auto-restart countdown improvements         | section 2              |  [ ]   |
| ⭐  |   6   | Crash analysis hints on-screen              | section 2              |  [ ]   |
| ⭐  |   7   | Crash statistics counter in NVRAM           | --             |  [ ]   |
| ⭐  |   8   | Safe mode suggestion after repeated crashes | section 7              |  [ ]   |

> 💎 = parity -- Windows 11 and Linux both have these features.
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

## 4. QR Code Crash URL On-Screen

Render a QR code on the BSOD linking to a crash help page. Windows 11 shows a QR code linking to `windows.com/stopcode`; Impossible OS links to `https://impossible.os/crash/<STOP_CODE>`.

**Files:** `src/kernel/panic.c`, new `src/kernel/qrcode.c`

- [ ] Implement minimal QR code generator (Version 2, 25x25 modules, alphanumeric mode) -- ~200 lines of C, no allocation needed (static 25x25 grid)
- [ ] Generate QR for URL: `IMPOSSIBLE.OS/CRASH/<STOP_CODE>` (uppercase for alphanumeric efficiency)
- [ ] Render QR code at 4x scale (100x100 pixels) in bottom-right corner of BSOD, white modules on blue background
- [ ] Include text below QR: "Scan for help with this error"
- [ ] Commit: `"kernel: QR code crash URL on BSOD screen"`

> [!NOTE]
> QR Version 2 supports 25 alphanumeric characters -- enough for `IMPOSSIBLE.OS/CRASH/GENERAL_PROTECTION_FAULT`. The generator is pure integer math with a static buffer -- no heap, no FPU, safe in panic context.

**Test checkpoint:** Force crash_test=1 -- QR code visible in bottom-right. Scan with phone camera -- URL resolves (even if the domain doesn't exist yet, the QR encoding should be valid). Verify QR doesn't overlap register dump or stack trace.

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

## 7. Crash Statistics Counter in NVRAM

Track crash history in UEFI NVRAM so the kernel knows how many times it has crashed and what the recent stop codes were.

**Files:** `src/kernel/panic.c`, `src/kernel/main/boot_hw.c`

- [ ] Define `ImpossibleCrashStats` NVRAM variable: `{ uint32_t total_crashes, uint32_t consecutive_crashes, struct { uint32_t stop_code; uint64_t timestamp; } last_5[5] }`
- [ ] In `panic_screen()`: read current stats, increment `total_crashes` and `consecutive_crashes`, push stop code to circular `last_5[]`, write back to NVRAM
- [ ] In `boot_phase0()` (after UEFI runtime init): read stats, reset `consecutive_crashes` to 0 (successful boot clears the counter)
- [ ] Display "Crash #N" on BSOD screen (total lifetime crash count)
- [ ] Commit: `"kernel: crash statistics counter in UEFI NVRAM"`

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

## OS Comparison

| ⭐ | Feature               | Win11                 | Linux                  | Impossible OS              |
|----|-----------------------|-----------------------|------------------------|----------------------------|
| 💎 | GUI panic screen      | ✅ Blue screen + icon | ❌ Text-only panic    | ⬜ §1-§2 TTF + layout      |
| 💎 | Stop code display     | ✅ Named stop code    | ✅ Oops/panic text    | ✅ Already done             |
| 💎 | Register dump         | ✅ Minidump context   | ✅ All GPRs           | ✅ Already done             |
| 💎 | Stack trace           | ✅ via debugger       | ✅ Inline backtrace   | ✅ Already done             |
| ⭐ | Crash context (logs)  | ❌ Not shown          | ❌ Dmesg separate     | ⬜ §3 last 10 entries       |
| 💎 | QR code               | ✅ windows.com/stop   | ❌ None               | ⬜ §4 crash URL             |
| 💎 | Auto-restart          | ✅ Configurable       | ✅ panic_timeout      | ⬜ §5 improved countdown    |
| ⭐ | Per-exception hints   | ❌ Generic message    | ❌ Raw dump           | ⬜ §6 actionable advice     |
| ⭐ | Crash stats (NVRAM)   | ❌ WER server-side    | ❌ No persistence     | ⬜ §7 local NVRAM counter   |
| ⭐ | Safe mode suggestion  | ⚠️ Only after 3 fails | ❌ No auto-recovery   | ⬜ §8 consecutive detection |

> After parity: Impossible OS matches Windows 11 on GUI BSOD, stop codes, restart countdown, and QR code.
> Exclusive: inline crash context logs, per-exception hints, local NVRAM crash statistics, and automatic safe mode suggestion -- none of which Windows 11 or Linux provide.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bsod()`.

- [ ] Create `src/kernel/test/test_bsod.c` with:
  - `panic_has_ttf()` returns 0 before font manager init, 1 after
  - `klog_get_recent()` copies correct number of entries from ring
  - `crash_hints[7]` (DEVICE_NOT_AVAILABLE) returns non-NULL string
  - QR code generator produces valid 25x25 grid for short alphanumeric input
  - Crash stats NVRAM header magic is correct
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
- [ ] QR code scans correctly from phone camera
- [ ] Crash stats persist across QEMU restarts (NVRAM)
- [ ] 3 consecutive crashes -> safe mode suggestion appears
