# TODO-05 — Visual POST Display (VPD)

> **Goal:** Replace the crude `HV_BAR` colored pixel bars with a production-grade Visual POST Display — a two-tier boot progress visualization with embedded micro-font, TSC timing, status indicators, and UEFI NVRAM crash persistence. The VPD is the single visual diagnostic system for every boot: it owns the pre-splash black-screen phase, integrates seamlessly into the splash, and on crash-restart shows exactly where the previous boot failed — all without serial, all configurable, all platforms.

> [!NOTE]
> **Origin:** The `HV_BAR` bars were added during bare-metal debugging (2026-03-28) — four colored lines on a black screen that pinpointed SMEP and HPET crashes in minutes. This TODO turns that hack into the coolest boot diagnostic feature in any OS.

> [!IMPORTANT]
> **Two-tier architecture:** Tier 1 (pre-splash) runs before `fb_init()` with zero kernel dependencies — no heap, no PMM, no klog, no fonts. Tier 2 (post-splash) runs inside the boot splash with full font rendering. The transition is seamless: when the splash starts, it composites over the Tier 1 bars.

## Inputs

- [`include/kernel/hv_bar.h`](../../include/kernel/hv_bar.h) — current prototype (to be replaced)
- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) — Phase 0 HV_BAR call sites
- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c) — Phase 1 HV_BAR call sites
- [`src/kernel/main/boot_storage.c`](../../src/kernel/main/boot_storage.c) — Phase 2 HV_BAR call sites
- [`src/kernel/main/boot_desktop.c`](../../src/kernel/main/boot_desktop.c) — Phase 3 HV_BAR call sites
- [`src/kernel/boot_splash.c`](../../src/kernel/boot_splash.c) — splash system to integrate with
- [`src/kernel/main/boot_progress.c`](../../src/kernel/main/boot_progress.c) — `boot_stage_report()`, POST hex display, stage metadata
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) — `boot_config` struct, framebuffer info
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) — `boot.conf` parsing, NVRAM POST read/write
- → XREF: `TODO-02-boot-diagnostics.md §5` — debug color bar waterfall (superseded by this TODO)
- → XREF: `TODO-02-boot-diagnostics.md §2` — `boot_stage_report()` named-stage API (VPD consumes this)
- → XREF: `TODO-02-boot-diagnostics.md §6` — panic forensic evidence (VPD displays crash history from NVRAM)
- → XREF: `TODO-03-interrupt-timer-arch.md §6` — boot timing and TSC frequency (VPD reads elapsed ms)
- → XREF: `TODO-06-bare-metal-hardening.md §1-§2` — 4-digit POST code system feeds VPD stage names and codes
- → XREF: `TODO-06-bare-metal-hardening.md §5` — hw interrupt investigation; VPD must work on bare metal where HV_BAR failed due to page flips

## Outcome

- `postbars=0` (default): VPD hidden entirely; normal splash only.
- `postbars=1`: Tier 1 bars visible during pre-splash Phase 0 + early Phase 1; post-splash progress integrates into the splash status area with named stages and timing.
- `postbars=2`: Full diagnostic mode — raw VPD visible throughout boot, no splash art, all stages with names/timing/status on screen.
- On crash-restart: next boot shows "Last boot failed at: STAGE_NAME (0xNN)" regardless of `postbars` setting — safety feature that's always active.
- HV_BAR prototype fully replaced; `include/kernel/hv_bar.h` removed.
- Works on all platforms: bare metal, QEMU WHPX, VirtualBox, QEMU TCG.

## Implementation Order

| ⭐  | Order | Deliverable                                             | Depends On | Status |
| --- | :---: | ------------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | 4-digit POST code system (0x0000–0xFFFF)                | —          |  [x]   |
| ⭐  |   2   | POST codes in UEFI bootloader + every kernel function   | §1         |  [x]   |
| ⭐  |   3   | Embedded 5×7 bitmap micro-font                          | —          |  [x]   |
| 💎  |   4   | Tier 1: Pre-splash VPD renderer                         | §1, §3     |  [ ]   |
| 💎  |   5   | `boot.conf` `postbars` configuration                    | §4         |  [ ]   |
| 💎  |   6   | Named stages with TSC timing                            | §4         |  [ ]   |
| 💎  |   7   | Status indicators and progress bar                      | §6         |  [ ]   |
| ⭐  |   8   | NVRAM crash persistence and "last boot failed" display  | §6         |  [ ]   |
| 💎  |   9   | Tier 2: Splash-integrated progress                      | §6, §7     |  [ ]   |
| 💎  |  10   | Seamless tier transition                                | §4, §9     |  [ ]   |
| ⭐  |  11   | Phase grouping and diagnostic layout                    | §7         |  [ ]   |
| 💎  |  12   | HV_BAR removal and migration                            | §4, §6     |  [ ]   |
| ⭐  |  13   | Panic integration and failure highlighting              | §8, §11    |  [ ]   |

> 💎 = parity — Windows has boot progress display (logo + dots); Linux has `plymouth` splash and `systemd-analyze blame`.
> ⭐ = exclusive — embedded micro-font pre-splash diagnostics, NVRAM crash persistence display, and full timing waterfall are not available in Windows or Linux at the kernel level without external tools.

---

## 1. 4-Digit POST Code System (0x0000–0xFFFF)

Replace the current 2-digit POST codes (28 values in 0x10–0x63) with a 4-digit system that gives every subsystem — including the UEFI bootloader — its own range. POST codes are the first diagnostic signal, active before any display, font, or kernel subsystem exists.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_init.h`, `src/kernel/main/boot_init.c`, `src/kernel/main/boot_progress.c`

> [!IMPORTANT]
> POST codes must start in the **UEFI bootloader** before `ExitBootServices` — this is the earliest possible diagnostic point. I/O port 0x80 is 8-bit on most hardware POST cards; write high byte to port 0x80. UEFI NVRAM stores the full 16-bit value. On-screen display renders all 4 hex digits once the framebuffer is available.

- [x] Define `POST16(code)` macro: writes high byte to I/O 0x80, stores full 16-bit in UEFI NVRAM, updates on-screen display when available
- [x] Define POST code ranges:

| Range | Phase | Example codes |
|-------|-------|---------------|
| 0xB000–0xBFFF | UEFI Bootloader | 0xB001=efi_main, 0xB010=GOP, 0xB020=kernel_load, 0xB030=ExitBS, 0xB040=page_tables, 0xB050=kernel_jump |
| 0x0000–0x0FFF | Phase 0 — Critical Init | 0x0010=serial, 0x0020=PMM_enter, 0x0021=PMM_exit, 0x0030=VMM |
| 0x1000–0x1FFF | Phase 1 — Platform | 0x1000=GDT, 0x1010=IDT, 0x1020=ACPI, 0x1030=LAPIC, 0x1040=timer |
| 0x2000–0x2FFF | Phase 2 — System | 0x2000=PCI, 0x2010=NIC, 0x2020=AHCI, 0x2060=VFS |
| 0x3000–0x3FFF | Phase 3 — Desktop | 0x3000=sched, 0x3010=fonts, 0x3020=compositor |
| 0xF000–0xFFFE | Reserved | 0xFF00=BOOT_OK, 0xFFFE=BOOT_FAILED |

- [x] `boot_post_write16(uint16_t code)` — stores 2 bytes in UEFI NVRAM (backward-compatible)
- [x] `boot_post_read16()` — reads 2 bytes if available, 1 byte otherwise
- [x] On-screen POST display: render 4 hex digits at 2× scale (16×16 px per glyph, top-right corner, existing 8×8 hex font doubled). Total display: 76×20 px
- [x] Bootloader: replaced `post_code(uint8_t)` with `post_code16(uint16_t)` using 0xB000 range + serial output
- [x] Commit: `"boot: 4-digit POST code system with UEFI bootloader coverage"`

**Test checkpoint:** QEMU: serial shows `[BOOT] POST 0xB001` before kernel entry. After kernel: `[PHASE0] PMM (0x0020)`. UEFI NVRAM stores 16-bit value. Bare metal: POST code reader shows high byte on port 0x80.

## 2. POST Codes in Every Boot Function

Instrument every function — from UEFI `efi_main` through kernel `compositor_run` — with entry/exit POST codes. A crash between entry (even) and exit (odd) pinpoints the exact function.

**Files:** `src/boot/uefi/bootx64.c`, all `boot_*.c` files, all driver `*_init()` functions

- [x] UEFI Bootloader: `efi_main` (0xB001), `init_gop` (0xB010), `load_kernel` (0xB020/21), `ExitBS` (0xB050), `page_tables` (0xB060), `kernel_jump` (0xB070)
- [x] Phase 0: `serial` (0x0010/11), `pmm` (0x0020/21), `vmm` (0x0030/31), `heap` (0x0040/41), `klog` (0x0050/51), `cpuid` (0x0060/61), `cpu_harden` (0x0070/71), `nx_policy` (0x0080/81), `simd` (0x0090/91)
- [x] Phase 1: `gdt` (0x1000/01), `idt` (0x1010/11), `acpi` (0x1020/21), `lapic` (0x1030), `timer` (0x1040/41), `rtc` (0x1050/51), `kbd` (0x1060/61), `fb` (0x1080/81), `splash` (0x1090/91)
- [x] Phase 2: `pci` (0x2000/01), `xhci` (0x2010/11), `nic` (0x2020/21), `net` (0x2030/31), `ata` (0x2040/41), `ahci` (0x2050/51), `vfs` (0x2060/61), `registry` (0x2080/81), `smp` (0x2090/91)
- [x] Phase 3: `sched` (0x3000/01), `desktop` (0x3030/31), `compositor` (0x3040)
- [x] Commit: `"boot: POST16 in every function from UEFI efi_main through compositor"`

**Test checkpoint:** Force crash in `mouse_init`. Reboot. Serial shows "Last boot failed at: 0x1070 (mouse_init)". Bare metal: NVRAM contains 0x1070, next boot displays it. Pinpointed in seconds, not hours.

## 3. Embedded 5×7 Bitmap Micro-Font
A zero-dependency pixel font baked into a single header — renders ASCII text directly to VRAM before any kernel subsystem exists. This is the foundation for all VPD text rendering in Tier 1.

**Files:** `include/kernel/vpd_font.h` (new)

> [!IMPORTANT]
> This font must have absolutely zero dependencies: no heap, no PMM, no klog, no framebuffer driver. It writes directly to the physical framebuffer address from `g_boot_info.fb`. The font data is `static const` embedded in the header.

- [x] Design 5×7 pixel bitmap glyphs for printable ASCII (0x20–0x7E, 95 chars)
- [x] Pack each glyph as 7 bytes (5-bit-wide rows, MSB-aligned), total 665 bytes
- [x] `vpd_putchar(fb, pitch_px, x, y, c, color)` — render one character
- [x] `vpd_puts(fb, pitch_px, x, y, s, color)` — render string, 6px per char
- [x] `vpd_putu32(fb, pitch_px, x, y, val, color)` — unsigned decimal text
- [x] `vpd_puthex16(fb, pitch_px, x, y, val, color)` — 4-digit hex
- [x] `vpd_puthex8(fb, pitch_px, x, y, val, color)` — 2-digit hex
- [x] All functions static inline in header — no .c file, no linker dependency
- [ ] Commit: `"boot: embedded 5x7 bitmap micro-font for pre-splash VPD"`

## 4. Tier 1: Pre-Splash VPD Renderer
Replace `HV_BAR` with a structured pre-splash renderer that draws named stage bars with text labels directly to VRAM. Active from the first instruction after `g_boot_info` is parsed until the splash takes over.

**Files:** `include/kernel/vpd.h` (new), `src/kernel/vpd.c` (new)

> [!IMPORTANT]
> Tier 1 writes directly to the hardware framebuffer (`g_boot_info.fb.addr`). No back buffer, no `fb_swap()`. This is intentional — the framebuffer driver doesn't exist yet.

> [!CAUTION]
> **Page flip gotcha (discovered 2026-03-28):** After `boot_splash_init()` performs fade-in page flips, the visible VRAM page changes. Tier 1 bars written to page 0 become invisible. On real hardware (non-Bochs VGA), writing to page 1 offset (`fb.addr + height * pitch`) may go past VRAM bounds and crash. Tier 1 must either: (a) only render before `fb_init()`, or (b) detect the current visible page and write to it. §8 (tier transition) must handle this cleanly.

- [ ] `vpd_init()` — called immediately after `g_boot_info` is parsed; stores fb pointer, pitch, dimensions; clears the VPD area (top 80–100 px) to black
- [ ] `vpd_stage_begin(uint8_t phase, const char *name, uint8_t postcode)` — draw stage row: colored status square + name text + POST hex code; mark as in-progress (yellow)
- [ ] `vpd_stage_done(void)` — update current stage's status square to green (done); record TSC elapsed and render timing text
- [ ] `vpd_stage_fail(void)` — update current stage's status square to red (failed)
- [ ] Layout: each stage occupies 10px height (7px text + 3px gap); left margin 4px; status square 6×6px; name at x=14px; timing right-aligned; POST code after timing
- [ ] Phase separator: 2px horizontal line in dark gray between Phase 0/1/2/3 groups
- [ ] Color palette: background black, text white (0xC0C0C0), done green (0x00CC00), in-progress yellow (0xCCCC00), failed red (0xFF2222), pending gray (0x404040)
- [ ] `vpd_is_active()` — returns 1 if VPD is enabled and rendering
- [ ] Replace all `HV_BAR(n)` calls with `vpd_stage_begin`/`vpd_stage_done` pairs
- [ ] Commit: `"boot: Tier 1 VPD renderer — pre-splash named stages with text"`

## 5. `boot.conf` `postbars` Configuration
Add the `postbars` key to `boot.conf` parsing so the VPD can be configured without recompilation.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [ ] Add `uint8_t postbars` field to `struct boot_config` (0=off, 1=integrated, 2=diagnostic)
- [ ] Default: `postbars = 0` (VPD hidden; normal splash)
- [ ] Parse in `parse_conf_kv()`: `postbars=off` → 0, `postbars=on` → 1, `postbars=diag` → 2, numeric accepted
- [ ] `vpd_init()` reads `g_boot_info.config.postbars`; returns early if 0 (except crash-restart display, which is always active — see §6)
- [ ] Add `postbars=off` to default `boot.conf` with comment
- [ ] Commit: `"boot: postbars boot.conf key for VPD configuration"`

## 6. Named Stages with TSC Timing
Wire the VPD into `boot_stage_report()` so every named stage automatically appears in the VPD with millisecond timing from the TSC.

**Files:** `src/kernel/main/boot_progress.c`, `src/kernel/vpd.c`

> [!IMPORTANT]
> TSC frequency may not be known in Phase 0 (before `boot_timing_init()`). Use the bootloader-provided `g_boot_info.timing.tsc_freq` if available; otherwise display raw TSC deltas. The timing text updates retroactively once frequency is known.

- [ ] `boot_stage_report()` calls `vpd_stage_begin(meta->phase, meta->name, meta->postcode)` before the stage body and `vpd_stage_done()` at the next `boot_stage_report()` call
- [ ] `vpd_stage_done()` reads TSC, computes elapsed ms, renders `+NNNms` right-aligned on the stage row
- [ ] If TSC freq unknown: render `+???ms` placeholder; retroactively fill in on `boot_timing_init()` if VPD rows are still visible
- [ ] Stage name rendering uses the `s_meta[].name` strings already defined in `boot_progress.c`
- [ ] Commit: `"boot: VPD named stages with TSC-derived millisecond timing"`

## 7. Status Indicators and Progress Bar
Add visual status icons and a proportional progress bar below the stage list.

**Files:** `src/kernel/vpd.c`, `include/kernel/vpd.h`

- [ ] Status indicators: 6×6 px filled squares — green (done), yellow (in-progress), red (failed), dark gray (pending)
- [ ] `vpd_update_progress(uint8_t percent)` — draw a 2px-tall progress bar at the bottom of the VPD area; filled portion in accent blue (0x0078D4), unfilled in dark gray
- [ ] Progress percentage sourced from `s_meta[stage].percent` (0–100 already defined)
- [ ] Each `vpd_stage_begin()` updates pending stages below to gray-square + gray-text (future preview)
- [ ] Commit: `"boot: VPD status indicators + progress bar"`

## 8. NVRAM Crash Persistence and "Last Boot Failed" Display
On crash-restart, display exactly where the previous boot failed — always active regardless of `postbars` setting. This is the killer feature: you crash, reboot, and the screen tells you what happened.

**Files:** `src/kernel/vpd.c`, `src/kernel/main/boot_hw.c`, `src/kernel/main/boot_progress.c`

> [!IMPORTANT]
> The existing `boot_post_write()` / `boot_post_read()` NVRAM persistence already saves the last POST code. This section extends it to also save the stage name index, so the next boot can display "Last boot failed at: TIMER (0x35)" instead of just "POST: 0x35".

- [ ] Extend NVRAM POST variable: save 2 bytes — `{ uint8_t postcode, uint8_t stage_index }` (backward-compatible: reader ignores extra byte if only 1 byte present)
- [ ] `boot_post_write_stage(uint8_t postcode, uint8_t stage_idx)` — called from `boot_stage_report()`
- [ ] `boot_post_read_stage(uint8_t *postcode, uint8_t *stage_idx)` — called in early `boot_phase0()`
- [ ] If last boot was incomplete (POST != 0xFF): render a 1-line crash banner at the very top of the screen: `"Last boot failed at: STAGE_NAME (0xNN)"` in red text on black, using the Tier 1 micro-font
- [ ] Crash banner is **always visible** regardless of `postbars` setting — it's a safety feature
- [ ] Banner auto-clears when `BOOT_STAGE_DESKTOP_READY` is reached (splash composites over it)
- [ ] Serial log: `[POST] Last boot failed at: STAGE_NAME (0xNN)` (enhances existing serial message)
- [ ] Commit: `"boot: NVRAM crash persistence — 'Last boot failed at' display"`

## 9. Tier 2: Splash-Integrated Progress
When `postbars=1`, the boot splash status text area shows VPD-style named stages with timing instead of generic "Setting up interrupts..." text.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/vpd.c`

- [ ] `boot_splash_status()` in `postbars=1` mode: instead of rendering bare `msg` text, render `"STAGE_NAME +NNNms ✓"` using the existing TTF font renderer
- [ ] Stage history scroll: show the last 3–4 completed stages above the current one, with decreasing opacity (100%, 60%, 30%)
- [ ] Progress bar integration: use the splash's existing bottom area to render the VPD progress bar, matching the splash accent color
- [ ] `postbars=2` (diagnostic mode): skip splash art entirely; render the full Tier 1 VPD layout using the TTF font at larger scale (12px instead of 7px) with phase grouping
- [ ] Commit: `"boot: Tier 2 splash-integrated VPD progress display"`

## 10. Seamless Tier Transition
When the splash starts, smoothly replace the Tier 1 raw VRAM bars with the Tier 2 splash-rendered progress — no visual glitch, no lost state.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/vpd.c`

- [ ] `vpd_transition_to_splash()` — called from `boot_splash_init()` after the splash background is drawn; signals Tier 1 to stop writing raw VRAM
- [ ] The splash fade-in naturally covers the Tier 1 bars (they're in the top ~100px; the splash background overwrites them)
- [ ] Transfer VPD state (completed stages, timings, current stage) from Tier 1 static data to the splash renderer so Tier 2 can show complete history
- [ ] `vpd_is_tier1()` / `vpd_is_tier2()` — query which tier is active; used by `boot_stage_report()` to route rendering
- [ ] In `postbars=2` mode: no transition — Tier 1 layout persists throughout boot, splash background is never drawn
- [ ] Commit: `"boot: seamless VPD tier transition from raw VRAM to splash"`

## 11. Phase Grouping and Diagnostic Layout
Full diagnostic layout with phase headers, visual separators, and structured stage grouping — the "server POST screen" aesthetic.

**Files:** `src/kernel/vpd.c`

- [ ] Phase headers: `"PHASE 0 — Critical Init"`, `"PHASE 1 — Platform Services"`, `"PHASE 2 — System Services"`, `"PHASE 3 — User Platform"` rendered in brighter white, 2px underline
- [ ] Phase separators: 1px dark gray horizontal line between phase groups
- [ ] Stage indentation: 2-space indent under phase header
- [ ] Column alignment: status icon at x=8, name at x=18, dots/fill at x=200, timing at x=280, POST hex at x=330
- [ ] Dot leaders: `"PMM .................. +87ms  ✓  0x20"` — fill between name and timing with dots for readability
- [ ] Visible in `postbars=2` mode and during crash-restart diagnostic display
- [ ] Commit: `"boot: VPD phase grouping with diagnostic layout"`

## 12. HV_BAR Removal and Migration
Remove the `HV_BAR` prototype and migrate all call sites to the VPD API.

**Files:** all files that `#include "kernel/hv_bar.h"`

- [ ] Replace every `HV_BAR(n)` call with the appropriate `vpd_stage_begin()` / `vpd_stage_done()` pair
- [ ] Remove `include/kernel/hv_bar.h`
- [ ] Remove `#include "kernel/hv_bar.h"` from all source files
- [ ] Update TODO-02 §5 to reference this TODO: mark §5 as superseded by TODO-05
- [ ] Verify: `postbars=0` — no visual artifacts; `postbars=1` — clean splash with progress; `postbars=2` — full diagnostic screen
- [ ] Commit: `"boot: remove HV_BAR prototype — fully replaced by VPD"`

## 13. Panic Integration and Failure Highlighting
On crash, the VPD marks the active stage as failed. On next boot, the failure is highlighted in the diagnostic display.

**Files:** `src/kernel/vpd.c`, `src/kernel/panic.c`

- [ ] `vpd_stage_fail()` called from `kernel_panic()` before the BSOD screen is drawn — marks current stage red
- [ ] If VPD is in Tier 1 (pre-splash panic): the failed stage is visible as a red bar with name on the black screen; system halts with this visible
- [ ] If VPD is in Tier 2 (post-splash panic): the panic screen includes a "Boot Progress" section showing all completed stages + the failed stage highlighted in red
- [ ] On crash-restart with `postbars=2`: full diagnostic screen shows the failed stage from last boot in red with `"✗ FAILED"` label, sourced from NVRAM data (§6)
- [ ] Commit: `"boot: VPD panic integration — failure highlighting and crash-restart display"`

---

## OS Comparison

| ⭐ | Feature                 | Win11                        | Linux                        | Impossible OS                  |
|----|-------------------------|------------------------------|------------------------------|--------------------------------|
| 💎 | Boot progress visual    | ✅ Spinning dots            | ✅ Plymouth splash          | ⬜ §2+§7 — two-tier VPD        |
| ⭐ | Pre-splash diagnostics  | ❌ Black screen             | ⚠️ fbcon (if compiled in)   | ⬜ §1+§2 — micro-font stages   |
| 💎 | Boot stage timing       | ⚠️ ETW (not visible)        | ✅ systemd-analyze (post)   | ⬜ §4 — live TSC ms per stage  |
| ⭐ | NVRAM crash display     | ⚠️ Generic error message    | ❌ No NVRAM persistence     | ⬜ §6 — "Last boot failed at"  |
| 💎 | POST code display       | ✅ Motherboard LED          | ❌ Not an OS feature        | ✅ Done — TODO-02 §3           |
| 💎 | Configurable diag       | ✅ bcdedit bootlog          | ✅ systemd.log_level        | ⬜ §3 — postbars=off/on/diag   |
| ⭐ | Panic-aware progress    | ❌ No boot context in BSOD  | ❌ No boot context in oops  | ⬜ §11 — failed stage in red   |

> **After §1–§11:** The most informative boot diagnostic in any OS — named stages with ms timing from first instruction, NVRAM crash forensics on restart, seamless splash integration. No serial. No tools. Just boot and see.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `postbars=0`: boot shows normal splash; no VPD bars or text visible at any point
- [ ] `postbars=1`: Phase 0 shows Tier 1 bars with names; splash takes over seamlessly in Phase 1; splash status shows named stages with timing
- [ ] `postbars=2`: full diagnostic screen throughout boot; no splash art; all stages with phase headers, timing, status icons, and POST codes
- [ ] Crash test: force panic in Phase 1 → reboot → next boot shows "Last boot failed at: STAGE (0xNN)" banner at top regardless of `postbars` setting
- [ ] Bare metal: VPD renders correctly on real hardware (i5-11600K confirmed platform)
- [ ] QEMU WHPX, VBox, TCG: VPD renders correctly on all VM platforms
- [ ] `include/kernel/hv_bar.h` deleted; no references to `HV_BAR` remain in codebase
- [ ] Commit: `"boot: Visual POST Display complete — two-tier diagnostics with NVRAM crash persistence"`
