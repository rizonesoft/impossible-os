---
schema_version: 1
id: boot-diagnostics
domain: 01-boot-platform
status: active
title: "TODO-14 -- Boot Diagnostics, Heartbeat & Spinner"
---

# TODO-14 -- Boot Diagnostics, Heartbeat & Spinner

> **Goal:** The arc spinner and boot splash are done. This TODO builds the production diagnostics layer: a named-stage boot progress API that feeds the splash, POST-style hex codes visible on hardware debug cards, cross-boot panic forensics, a panic QR code, runtime vital-signs overlay, alive-blink hang detection, and a multi-instance compositor-integrated spinner -- turning the ad-hoc debug tooling into production-grade features.

> [!IMPORTANT]
> **Current state:** §1--§3 are shipped and verified (UEFI `post_code16` milestones incl. boot-device `0xB090`/`0xB091`, kernel `boot_stage_report` + `post_display16`, port `0x80` high byte, `boot_timeline_dump_json()` wired from `boot_desktop.c` after desktop-ready). `boot_progress_poll()` has **no in-tree callers** yet. §4--§8, `test_boot_diag`, and §9 below remain open. `panic_collect_evidence` / `spinner_create` / `vital_signs` sources are not in the tree.

> [!NOTE]
> **Origin:** The HV_BAR colored pixel bars were added during Hyper-V Gen 2 debugging -- crude but instantly effective. This TODO formalises that approach as an opt-in production debug feature while replacing the unconditional hack with proper structured output.

> [!IMPORTANT]
> **`boot_progress(phase, step, postcode)`** already exists in `boot_init.h` (implemented by `02-kernel-core/TODO-01-kernel-init-sequencing.md` §1). §2 of this TODO extends it with a named-stage API, a 32-entry stage history (capped, no wrap), elapsed-ms tracking, and splash status integration -- do not rewrite the lower-level call, build on it.

## Inputs

- [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h)
- [`include/kernel/boot_splash.h`](../../include/kernel/boot_splash.h)
- [`include/kernel/spinner.h`](../../include/kernel/spinner.h)
- [`include/kernel/panic.h`](../../include/kernel/panic.h)
- [`src/kernel/panic.c`](../../src/kernel/panic.c)
- [`src/kernel/boot_timing.c`](../../src/kernel/boot_timing.c)
- [`src/kernel/main/boot_desktop.c`](../../src/kernel/main/boot_desktop.c) -- successful desktop-ready path calls `boot_timeline_dump_json()`
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §1` -- `boot_progress(phase, step, postcode)` in `boot_init.h`; §2 of this TODO wraps it with a named-stage layer
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §7` -- `boot_halt()` is the pre-FB panic anchor that §5 extends with forensic evidence
- → XREF: `02-kernel-core/TODO-27-crash-dump-generation.md` -- crash dumps complement §5 panic forensics; coordinate PMM page reservation at `0x80000` to avoid collision with minidump workspace
- → XREF: `02-kernel-core/TODO-14-registry-completion.md` -- `HKLM\SYSTEM\Boot\AliveBlink`, `VitalSigns` registry keys (visual POST / debug bar: [TODO-15 -- Visual POST Display](TODO-15-visual-post-display.md))
- → XREF: `TODO-02-uefi-hardening-secureboot.md §7` -- boot UX polish calls `boot_splash_status()` via the §2 API
- → XREF: `08-graphics-ui/TODO-08-window-manager.md` §8 -- compositor frame loop must call `spinner_tick()` on every active `g_active_spinners[]` entry per frame; that file §8 (Compositor Performance) owns per-frame integration
- → XREF: `TODO-03-bootloader-error-recovery.md §13` -- bootloader-stage NVRAM error codes; §3 here persists kernel-stage panic evidence, §13 there persists UEFI-stage boot error codes -- complementary
- → XREF: `TODO-03-bootloader-error-recovery.md §14` -- bootloader QR code on UEFI error screen; §4 here does the same for kernel-stage panic BSOD -- share QR encoder if both are implemented
- → XREF: `TODO-15-visual-post-display.md` -- full-screen VPD (tiered bars, splash integration, NVRAM last-boot) complements this file; TODO-14 owns POST16 I/O port `0x80`, framebuffer corner digits, named-stage serial, and panic evidence specs

## Outcome

- Serial log shows `[+NNNms] BOOT_PMM: Physical memory manager ready` style entries for every major stage.
- `boot-timeline.json` written under BlackBox `X:\Boot\` (or klog fallback) after successful boot summarizing timed steps (`boot_timeline_dump_json()`).
- Four-digit hex POST code (native 8 px glyphs) visible in the top-right corner of the framebuffer from kernel entry until `BOOT_DESKTOP_READY`; high byte of each 16-bit code is output to I/O port 0x80.
- On panic: `struct panic_evidence` captured at `0x80000`; next boot saves `last-panic.txt` and shows "System shut down unexpectedly" toast.
- Panic BSOD shows a QR code in the bottom-right corner linking to the troubleshooting page.
- Alive blink returns after **§4** ships (safe framebuffer path; ISR `fb_swap_rect` is not viable).
- **§7** multi-instance `spinner_create()` / `spinner_tick()` and per-frame compositor ticks ship with that section (→ XREF: `08-graphics-ui/TODO-08-window-manager.md` §8).
- `VitalSigns=1` activates a 20 px bottom-strip showing CPU %, RAM, IRQ rate, uptime, and FPS.

## Implementation Order

| ⭐  | Order | Deliverable                        | Depends On                    | Status |
| --- | :---: | ---------------------------------- | ----------------------------- | :----: |
| 💎  |   1   | UEFI pre-kernel POST codes         | --                            |  [x]   |
| 💎  |   2   | Boot progress named-stage API      | §1                            |  [x]   |
| 💎  |   3   | POST-style hex code display        | §2                            |  [x]   |
| 💎  |   4   | Alive blink / hang detection       | §2                            |  [ ]   |
| 💎  |   5   | Panic forensic evidence            | §2                            |  [ ]   |
| ⭐  |   6   | Panic QR code                      | §5                            |  [ ]   |
| 💎  |   7   | System-wide multi-instance spinner | 08-graphics-ui/TODO-06 §8     |  [ ]   |
| ⭐  |   8   | Runtime vital signs strip          | §7                            |  [ ]   |
| 💎  |   9   | Boot timeline visualization/import | §2                            |  [ ]   |

> 💎 = parity -- Windows and Linux both have equivalent diagnostics; Impossible OS must match them.
> ⭐ = exclusive -- the QR code on BSOD and always-visible vital-signs strip are not present in either competitor at the kernel level.

---

## 1. UEFI Pre-Kernel POST Codes
Write I/O port 0x80 POST codes from the bootloader so hardware POST-code reader cards decode boot progress before the kernel even starts.

**Files:** `src/boot/uefi/bootx64.c`

- [x] Add `post_code(uint8_t code)` inline in `bootx64.c`: `outb(0x80, code)`; add `post_code16(uint16_t code)` (writes high byte to port `0x80`, logs full value on serial) plus legacy 8-bit names `POST_ENTRY` through `POST_KERNEL_JUMP` for reference
- [x] Insert `post_code16()` at bootloader milestones: `0xB001` entry, `0xB090`/`0xB091` boot device (LoadedImage) before GOP, `0xB010` GOP init, `0xB020`/`0xB021` ELF open/load, `0xB030` RSDP, `0xB080`-`0xB085` USB discovery and xHCI DMA/takeover substeps, `0xB040` memory map, `0xB050` ExitBootServices, `0xB060` page tables, `0xB070` kernel jump
- [x] QEMU ignores port 0x80 writes silently -- no fault, verified by clean build
- [x] Commit: `"boot: UEFI pre-kernel POST codes to I/O port 0x80"`

**Test checkpoint:** UEFI build; milestones emit `outb(0x80, code)` in order. QEMU WHPX, QEMU TCG, VirtualBox: no fault on port writes. Bare metal: POST card matches sequence if present.
> **Verified:** 2026-04-12 -- Milestone table reconciled to `post_code16`; Codex: disarm watchdog on `init_gop` error return before firmware UI. Accepted: none.

## 2. Boot Progress Named-Stage API
High-level named-stage wrapper over the existing `boot_progress()` that adds a 32-entry stage history (capped, no wrap), elapsed-ms tracking, and `boot_splash_status()` forwarding.

**Files:** `include/kernel/boot_progress.h`, `src/kernel/main/boot_progress.c`

- [x] Define `boot_stage_t` enum with 15 stages from `UEFI_INIT` through `DESKTOP_READY`
- [x] Map each stage to phase, postcode, progress %, and name in `s_meta[]` table
- [x] `boot_stage_report()`: calls `boot_progress()`, records up to 32 stage entries with TSC + elapsed_ms (cap at 32; no wrap), serial logs `[+NNNms] STAGE_NAME: msg`
- [x] `boot_get_elapsed_ms()`: returns ms since `KERNEL_ENTRY` via `boot_timing_tsc_freq()`
- [x] Serial log format: `[+NNNms] STAGE_NAME: msg`
- [x] `boot_stage_history_get()` accessor for panic forensics
- [x] `boot_progress_poll()`: re-sends last stage to `boot_splash_status()` for timer-driven visual refresh (no in-tree caller yet -- wire with splash/timer when UI needs refresh without new history rows)
- [x] `boot_timeline_dump_json()`: writes TSC-based step timeline as JSON to `boot-timeline.json` (BlackBox `X:\Boot\` or klog dir fallback); uses `boot_timing_get_steps()` + `boot_prog_tsc_delta_ms()` (`src/kernel/main/boot_progress.c`)
- [x] `boot_timeline_dump_json()` invoked after successful desktop-ready init (`src/kernel/main/boot_desktop.c` ~229, after NVRAM POST success + timing reports)
- [x] Commit: `"kernel: boot progress named-stage API + stage history + elapsed-ms tracking"` (exact subject varies across bring-up commits)

**Test checkpoint:** Serial shows `[+NNNms]` lines with stage names for at least 8 transitions; `boot_stage_history_get()` does not fault when called from panic paths. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Verified:** 2026-04-12 -- Named-stage API + `boot_timeline_dump_json` wiring; Codex: safe TSC-to-ms when `freq < 1000`, close timeline parent dir after `create`. Accepted: none.

## 3. POST-Style Hex Code Display
Render a 4-digit hex POST code in the top-right framebuffer corner visible on every boot, cleared when the desktop is ready.

**Files:** `src/kernel/main/boot_progress.c`, `include/kernel/boot_progress.h`

- [x] 8x8 hex font (16 glyphs, 128 bytes) embedded in `boot_progress.c`
- [x] `post_display16(code)`: renders 4 hex digits at native 8 px scale (gray on black) at top-right via direct VRAM before `SUBSYS_FB` or `fb_put_pixel()` + `fb_swap_rect()` after framebuffer init
- [x] Port `0x80` is written from `boot_post_write16()` inside `boot_progress()` (also invoked by `boot_stage_report()`); `post_display16()` skips pixel writes when FB unavailable or `postcode=0` after config parse
- [x] Called from `boot_stage_report()` after serial write (and again from `boot_progress()` / `boot_post_write16()` for the same stage)
- [x] Cleared on `BOOT_STAGE_DESKTOP_READY` via `fb_fill_rect()` + `fb_swap_rect()` over the same width/height as `post_display16()` (`POST16_TOTAL_W` x `POST16_TOTAL_H`)
- [x] Commit: `"kernel: POST-style hex code display in framebuffer corner + I/O port 0x80"` (exact subject varies; see `git log -- src/kernel/main/boot_progress.c`)

**Test checkpoint:** From kernel entry through desktop ready: four hex digits top-right; I/O port `0x80` high byte tracks the current 16-bit POST; digits clear on desktop ready. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Verified:** 2026-04-12 -- Desktop-ready clear uses `POST16_*` geometry; same TSC-ms guard as §2. Accepted: none.

## 4. Alive Blink / Hang Detection *(deferred -- ISR `fb_swap_rect` caused recursive interrupts on bare metal; redesign before ship)*
A 4x4 px hang indicator once a timer-driven path can update the framebuffer without swapping from ISR context.

**Files:** `src/kernel/drivers/pit.c` (or `timer.c`), `src/kernel/main/boot_progress.c`

- [ ] Remove or gate any path that calls `fb_swap_rect()` from `timer_tick_callback_fire()` / ISR (`alive_blink_tick` regression)
- [ ] Design safe commit: compositor tick, DPC, or flag set in ISR plus deferred FB flush
- [ ] Re-enable `heartbeat=` from `g_boot_info.config` (0=off, 1=on); optional `HKLM\SYSTEM\Boot\AliveBlink` after → XREF: `02-kernel-core/TODO-14-registry-completion.md`
- [ ] 4x4 px green square at top-left (4,4); toggles ~0.5 s using the safe path only
- [ ] CPU usage threshold deferred: needs scheduler per-second CPU accounting (future)
- [ ] Commit: `"kernel: alive blink hang indicator -- safe FB path, no ISR swap"`

**Test checkpoint:** With §4 shipped: QEMU WHPX, QEMU TCG, VirtualBox: `heartbeat=1` shows stable blink, no lockup or FB corruption. Bare metal: same; confirm no recursive interrupt storm.

## 5. Panic Forensic Evidence *(deferred -- needs stable boot first; crash evidence is useless if boot itself crashes)*
Capture a `panic_evidence` struct at fault time, survive across soft reboot via a dedicated PMM page, and restore on next boot.

**Files:** `src/kernel/panic.c`, `include/kernel/panic.h`, `src/kernel/main/boot_hw.c`

> [!IMPORTANT]
> The PMM page at `0x80000` must be reserved in `pmm_init()` before it can be used as the cross-boot evidence page. Coordinate with `02-kernel-core/TODO-27-crash-dump-generation.md` to avoid using the same fixed address for the minidump workspace.

- [ ] Define `struct panic_evidence`: `uint32_t magic` (`0xDEADBEEF`), last 16 `boot_stage_history[]` entries, last 8 klog ring entries, last POST code byte, `uint64_t cr0/cr3/cr4` at fault, `uint32_t irq_mask`, `uint32_t pmm_free_pages`, `uint64_t fault_rip`, `char message[256]`
- [ ] `panic_collect_evidence(rip, msg)`: copy data into `struct panic_evidence` at physical `0x80000`; called at the very start of `kernel_panic()` before any screen output or VFS access
- [ ] Reserve physical page `0x80000` in `pmm_init()`: mark as `PMEM_RESERVED` so it is never handed out as a free page
- [ ] In boot Phase 0 (`boot_hw_init`): check `*(uint32_t*)0x80000 == 0xDEADBEEF`; if so, copy evidence to kernel heap buffer, clear the magic, log `[PANIC] Previous crash evidence found`; after VFS is up, write to `X:\Crash\last-panic.txt` (BlackBox) or `C:\Impossible\System\Logs\` (fallback) -- → XREF: `01-boot-platform/TODO-24-blackbox-service-partition.md` §7 (`X:\Crash\` path owner)
- [ ] After VFS write: show "System shut down unexpectedly" toast at desktop-ready (set `g_boot_info.had_panic = 1` flag; desktop init reads it)
- [ ] Commit: `"kernel: panic forensic evidence -- cross-boot PMM page + last-panic.txt"`

**Test checkpoint:** Force `kernel_panic("test")`, reboot: serial shows `[PANIC] Previous crash evidence found`; `X:\Crash\last-panic.txt` contains fault RIP + POST code. QEMU WHPX, QEMU TCG, VirtualBox, bare metal: evidence survives warm reboot.

## 6. Panic QR Code *(deferred -- depends on §5)*
Embed a minimal QR code encoder and render a phone-scannable URL in the BSOD corner.

**Files:** `src/kernel/qr_encode.c`, `include/kernel/qr_encode.h`, `src/kernel/panic.c`

- [ ] Implement or embed a minimal MIT-licensed QR code encoder (~500 lines): `qr_encode(const char *text, uint8_t *matrix, int *size)` where `matrix` is a `size` by `size` bit grid (1=dark, 0=light); support QR version 3-6 (covers URLs up to ~150 chars); error correction level M
- [ ] `panic_qr_url(buf, bufsize, message, post_code, os_version)`: format `https://docs.impossible-os.dev/panic?msg=<short>&post=0x{post}&v={ver}` (truncate message to 40 chars to keep URL under 120 chars)
- [ ] In `kernel_panic()` BSOD renderer: after drawing the main panic screen, call `qr_encode(url, matrix, &qr_size)`; render QR module grid at bottom-right (12 px from corner), module size = 4 px, white modules on black background; quiet zone = 4 modules
- [ ] Ensure `qr_encode.c` is freestanding: no libc, no floating point; uses only `kernel/types.h` and `kernel/libc/string.h`
- [ ] Commit: `"kernel: minimal QR encoder + panic BSOD QR code for phone-scannable troubleshooting"`

**Test checkpoint:** With §5+§6 shipped: forced panic shows scannable QR bottom-right; URL resolves to docs panic page. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 7. System-Wide Multi-Instance Spinner *(deferred -- desktop polish, single spinner works)*
Extend the existing single-instance `spinner.h` to support up to 8 simultaneous named spinner instances for use across the desktop.

**Files:** `include/kernel/spinner.h`, `src/kernel/spinner.c`

> [!NOTE]
> The existing `spinner_init/start/advance/stop` API covers the boot splash single spinner (shipped with §1--§3 above). This section adds a multi-instance layer without breaking boot splash.

- [ ] Define `spinner_t` struct: `int32_t cx, cy, radius, stroke; uint32_t color; int32_t angle, sweep; uint8_t active; uint8_t size_class`
- [ ] `spinner_create(uint8_t size_class, uint32_t color)`: allocate from a static pool of 8 `spinner_t` slots; size classes: `SPINNER_SMALL=16`, `SPINNER_MEDIUM=32`, `SPINNER_LARGE=48`, `SPINNER_XLARGE=64` (px radius); returns `spinner_t*` or NULL if pool full
- [ ] `spinner_destroy(spinner_t *s)`: mark slot as inactive; stop animation
- [ ] `spinner_set_position(spinner_t *s, int32_t cx, int32_t cy)`: update position without restarting
- [ ] `spinner_tick(spinner_t *s)`: advance angle + breathing (port animation logic from existing `spinner_advance()` -- single instance → multi); call `spinner_render(s, cx, cy)`
- [ ] `spinner_render(spinner_t *s, struct fb_surface *surface, int32_t x, int32_t y)`: draw arc ring onto `surface` (compositor surface or direct framebuffer)
- [ ] Compositor integration: WM maintains a `spinner_t *g_active_spinners[8]` list; compositor loop calls `spinner_tick()` on each non-NULL entry per frame; used by loading dialogs, Start Menu search, download progress, Service Manager
- [ ] Backward compatibility: existing `spinner_init/start/advance/stop` calls remain valid; they operate on `g_active_spinners[0]` (the boot splash slot)
- [ ] Commit: `"kernel: multi-instance spinner_t pool for compositor-integrated loading indicators"`

**Test checkpoint:** With §7 shipped: pool of 8; compositor ticks each active `spinner_t` per frame per → XREF: `08-graphics-ui/TODO-08-window-manager.md` §8; boot splash still uses slot 0. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 8. Runtime Vital Signs Strip *(deferred -- developer tool, needs scheduler stats first)*
An always-visible 20 px overlay strip at the bottom of the desktop showing live system metrics for developers.

**Files:** `src/desktop/vital_signs.c`, `include/desktop/vital_signs.h`

- [ ] Activate when `boot.conf` key `VitalSigns=1` or `HKLM\SYSTEM\Boot\VitalSigns` = 1 (→ XREF `02-kernel-core/TODO-14-registry-completion.md`)
- [ ] `vital_signs_init()`: called from desktop init; allocates a 20 px compositor overlay surface pinned to the bottom of the screen
- [ ] `vital_signs_tick()`: called every 500 ms from a PIT-driven callback; reads: CPU usage % from scheduler stats, RAM used/total from PMM, IRQ count/s from IRQ counter differential, uptime in seconds from PIT ticks, framerate from compositor frame counter
- [ ] Render format (FONT_MONO at 10 px, white on 50% transparent black): `[CPU: 23%] [RAM: 1.2/4.0 GB] [IRQ: 1234/s] [Uptime: 00:03:42] [FPS: 60]`
- [ ] `VitalSignsExtended=1` adds second line: `[Free: 2847 pages] [TCP: 3] [VFS R: 1.2 MB/s W: 0.4 MB/s] [Temp: 62°C]` (CPU temp from ACPI thermal zone if available, 0 if not)
- [ ] The overlay is always rendered above the desktop wallpaper and windows; zorder = top - 1 (below cursor, above everything else)
- [ ] Commit: `"desktop: runtime vital signs strip -- CPU/RAM/IRQ/FPS overlay for developers"`

**Test checkpoint:** With §8 shipped: `VitalSigns=1` shows bottom strip updating ~500 ms; CPU, RAM, IRQ, uptime, FPS plausible. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 9. Boot Timeline Visualization and Import Parity *(deferred)*
Linux `systemd-analyze plot` and Windows performance tooling expose boot as a human-readable timeline. This repo already emits machine-readable `boot-timeline.json` from `boot_timeline_dump_json()`; viewer parity is still open.

**Files:** `docs/` (schema + tooling notes), optional `scripts/` or `user/` offline converter

- [ ] Publish JSON field schema (`stage`, `phase`, `post`, `start_ms`, `duration_ms`) in `docs/boot-timeline-json.md` and link from `CLAUDE.md` boot diagnostics pointer
- [ ] Optional offline converter or in-kernel `boot_timeline_to_svg()` to produce Gantt-style SVG comparable to `systemd-analyze plot` output
- [ ] Optional Chrome trace event JSON export for `chrome://tracing` import (competitive edge vs plain SVG)
- [ ] Commit: `"docs: boot timeline JSON schema + optional trace export"`

**Test checkpoint:** With §9 shipped: JSON from a real boot validates against schema; SVG or Chrome trace opens in target viewer without manual edits. QEMU WHPX + TCG smoke.

---

## OS Comparison

| ⭐ | Feature                 | 🪟 Win11                     | 🐧 Linux                     | 🚀 Impossible OS                  |
| -- | ----------------------- | ---------------------------- | ---------------------------- | --------------------------------- |
| 💎 | Boot POST codes         | ✅ Firmware boot mgr        | ✅ BIOS POST codes           | ✅ §1 §3 POST port 80 + FB hex    |
| 💎 | Named boot progress     | ✅ ETW boot trace           | ✅ dmesg systemd-analyze     | ✅ §2 serial STAGE ms lines       |
| 💎 | Boot timeline viewers   | ⚠️ Performance Toolkit      | ✅ systemd-analyze plot     | ⚠️ JSON §2; §9 viewers pending     |
| 💎 | Panic forensics         | ✅ WER minidump EventLog    | ✅ kdump pstore ramoops      | ⬜ §5 PMM page last-panic txt     |
| 💎 | Multi UI spinner        | ✅ WinUI ProgressRing       | ✅ GTK Qt spinners           | ⬜ §7 spinner_create pool         |
| ⭐ | Panic BSOD QR           | ❌ Text URL BSOD only       | ❌ No kernel QR              | ⬜ §6 phone URL QR matrix         |
| ⭐ | Alive hang pixel        | ❌ No kernel hang pixel     | ❌ Not production default     | ⬜ §4 redesign safe FB path       |
| ⭐ | Live vital overlay      | ⚠️ Task Manager separate    | ⚠️ htop conky third-party    | ⬜ §8 bottom metrics strip        |

> **Parity scan:** Win11+Linux ✅ on POST, named progress, panic dumps, UI spinners -- Impossible OS matches via §1--§3; timeline **export** exists (JSON) but **viewers** match Linux/Win tooling only after §9. ⬜ rows §5--§8 and §9 are open parity or stretch (⭐ rows). **Edges:** §6 QR and §8 always-on strip are planned differentiators once shipped.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_boot_diag()` (pattern: `src/kernel/test/test_runner.c` and `test_suite_register_cat(..., TEST_CAT_BOOT)`; see `include/kernel/test/test.h`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_boot_diag.c` with:
  - `boot_stage_report()` with a test stage records entry in `boot_stage_history[]` (stage history non-empty after call; cap 32, no wrap)
  - `boot_get_elapsed_ms()` returns monotonically increasing values (two calls 1ms apart, second >= first)
  - `boot_stage_history_get()` returns non-NULL pointer with valid `count > 0` after boot
  - POST hex display: `boot_post_write16(0xAABB)` followed by `boot_post_read16()` returns `0xAABB`
  - `boot_progress_poll()` does not crash when called with no active stage
  - Alive blink toggle counter increments over 100 timer ticks when `heartbeat=1`
  - `spinner_create(SPINNER_MEDIUM, 0x0078D4)` returns non-NULL; `spinner_destroy()` frees the slot; re-create succeeds
  - `spinner_create()` returns NULL after 8 allocations (pool exhausted)
  - Panic evidence struct at `0x80000`: `panic_collect_evidence()` writes magic `0xDEADBEEF`, readback matches
  - After §9 schema lands: validate a captured `boot-timeline.json` against `docs/boot-timeline-json.md` (field names, numeric types)
- [ ] Register in `test_runner_init()`: `test_register_boot_diag()`
- [ ] Commit: `"test: add boot_diag test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log shows `[+Nms] BOOT_PMM: Physical memory manager ready` style entries for at least 8 stages
- [ ] POST code visible in top-right corner during QEMU boot; disappears when desktop loads
- [ ] `AliveBlink=1` in `boot.conf` → 4x4 green square blinks in top-left corner during boot and desktop
- [ ] Force `kernel_panic("test")` from shell → BSOD shows QR code in bottom-right corner
- [ ] Force panic twice -> second boot finds `last-panic.txt` in `X:\Crash\` (-> XREF: `01-boot-platform/TODO-24-blackbox-service-partition.md` §7)
- [ ] `VitalSigns=1` → bottom strip shows CPU/RAM/IRQ/uptime/FPS, updates every 500 ms
- [ ] `spinner_create(SPINNER_MEDIUM, 0x0078D4)` in test harness → spinner renders in compositor frame
- [ ] Commit: `"kernel: boot-diagnostics verified -- POST codes, panic forensics, QR code, vital signs, multi-instance spinner"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) -- add `TEST_CAT_BOOT` suites when `test_boot_diag.c` lands (Unit Tests section).

## History

| Date       | Action        | Summary |
| ---------- | ------------- | ------- |
| 2026-04-12 | gap-analysis  | Web: Win11 required diagnostics / WER flow (Learn); Linux ramoops+pstore (kernel.org), systemd-analyze, boot-time trace docs. Code-truth: `panic_evidence` / `spinner_create` / `vital_signs` absent; `boot_timeline_dump_json` in `boot_progress.c` + call `boot_desktop.c:229`; `boot_progress_poll` uncalled. Added **Current state** callout; Inputs `boot_desktop.c` + TODO-15 scope XREF; §4 bullets for timeline JSON; new §9 + Impl row 9 + OS row; Unit Tests wording; History table. |
| 2026-04-12 | validate      | Inputs paths exist; no N.M / continuation-line drift; OS row Impossible cell shortened to five words after glyph; Impl order 7 Depends On now `08-graphics-ui/TODO-06` §8 (blocked until compositor per-frame ticks); Unit Tests XREF replaced the dead kernel-test-framework path with `test_runner.c` + `TEST_CAT_BOOT` pattern; Verification adds `run-boot-tests.bat`; Verification XREF to TODO-24 uses domain-qualified path. **Flag:** legacy phantom kernel-test-framework path should normalize to `00-infrastructure/TODO-03-kernel-test-harness.md` repo-wide. **Note:** `> **Verified:**` lines after §1--§5 checkpoints are audit stamps from `verify-todo-section`, not checklist items. |
