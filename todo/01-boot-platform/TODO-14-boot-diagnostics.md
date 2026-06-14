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
> **Current state:** §1--§3, §5, §10, §11 are shipped/verified; §4 is deferred (`[/]`, safety invariant only -- the blink feature needs a bare-metal-validated safe FB path). §6--§9 remain open. `boot_progress_poll()` has **no in-tree callers** yet. `spinner_create` (§7) / `vital_signs` (§8) sources are not in the tree.

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
- `boot-timeline.json` written under BlackBox `X:\Perf\` (or klog fallback) after successful boot summarizing FPDT firmware phases + TSC bootloader/kernel steps (`boot_timeline_dump_json()`; path moved from `X:\Boot\` by TODO-04 FPDT normalization).
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
| 💎  |   4   | Alive blink / visual heartbeat     | §2 (visual only; hang=TODO-23) |  [/]   |
| 💎  |   5   | Panic forensic evidence            | §2                            |  [/]   |
| ⭐  |   6   | Panic QR code                      | §5                            |  [ ]   |
| 💎  |   7   | System-wide multi-instance spinner | 08-graphics-ui/TODO-06 §8     |  [ ]   |
| ⭐  |   8   | Runtime vital signs strip          | §7                            |  [ ]   |
| 💎  |   9   | Boot timeline visualization/import | §2                            |  [ ]   |
| ⭐  |  10   | Bootloader build identity dump in BlackBox | TODO-01 §20                |  [x]   |
| 💎  |  11   | Boot load status log (ntbtlog parity)      | §2                         |  [/]   |

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
> **Notes:**
> - Shipped: `post_code(uint8_t)` + `post_code16(uint16_t)` (high byte to port 0x80, full value on serial) in `bootx64.c`, emitted at bootloader milestones 0xB001 through 0xB070.
> - Integration: QEMU ignores port 0x80 writes silently (no fault); a hardware POST-code reader card decodes the sequence on bare metal before the kernel starts.
> - Scope boundary: bootloader-stage codes only; kernel-stage POST display is §2/§3.
> **Verified:** 2026-04-12 -- Milestone table reconciled to `post_code16`; Codex: disarm watchdog on `init_gop` error return before firmware UI. Accepted: none.

---

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
- [x] `boot_timeline_dump_json()`: writes unified FPDT + TSC step timeline as JSON to `boot-timeline.json` (BlackBox `X:\Perf\` or klog dir fallback); uses `boot_timing_get_fpdt_entries()` + `boot_timing_get_steps()` + `boot_prog_tsc_delta_ms()` (`src/kernel/main/boot_progress.c`); path moved from `X:\Boot\` by TODO-04 FPDT and Boot Timing Normalization
- [x] `boot_timeline_dump_json()` invoked after successful desktop-ready init (`src/kernel/main/boot_desktop.c` ~229, after NVRAM POST success + timing reports)
- [x] Commit: `"kernel: boot progress named-stage API + stage history + elapsed-ms tracking"` (exact subject varies across bring-up commits)

**Test checkpoint:** Serial shows `[+NNNms]` lines with stage names for at least 8 transitions; `boot_stage_history_get()` does not fault when called from panic paths. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Notes:**
> - Shipped: `boot_stage_report()` (15-stage `boot_stage_t`, 32-entry capped history, TSC + elapsed-ms) wrapping the existing `boot_progress()`, plus `boot_timeline_dump_json()` (FPDT + TSC step timeline) in `boot_progress.c`.
> - Integration: serial `[+NNNms] STAGE: msg`; `boot_timeline_dump_json()` runs at desktop-ready (`boot_desktop.c`); `boot_stage_history_get()` feeds §5 panic forensics.
> - Scope boundary: `boot_progress_poll()` has no in-tree caller yet (wire with splash/timer when the UI needs a refresh without new history rows).
> **Verified:** 2026-04-12 -- Named-stage API + `boot_timeline_dump_json` wiring; Codex: safe TSC-to-ms when `freq < 1000`, close timeline parent dir after `create`. Accepted: none.

---

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
> **Notes:**
> - Shipped: 8x8 hex font (16 glyphs) + `post_display16()` rendering 4 hex digits top-right via direct VRAM (pre-`SUBSYS_FB`) or `fb_put_pixel()`+`fb_swap_rect()` (post-FB), in `boot_progress.c`.
> - Integration: called from `boot_stage_report()` after the serial write; port 0x80 high byte tracks the 16-bit POST; cleared at `BOOT_STAGE_DESKTOP_READY` over the `POST16_*` geometry.
> - Scope boundary: skips pixel writes when the framebuffer is unavailable or postcode=0 after config parse.
> **Verified:** 2026-04-12 -- Desktop-ready clear uses `POST16_*` geometry; same TSC-ms guard as §2. Accepted: none.

---

## 4. Alive Blink / Visual Heartbeat Indicator *(deferred -- ISR `fb_swap_rect` caused recursive interrupts on bare metal; redesign before ship)*
A 4x4 px VISUAL liveness indicator (a blinking pixel) once a timer-driven path can update the framebuffer without swapping from ISR context. This is a passive visual signal ONLY: it never detects a hang, times out, reboots, or pets a watchdog. Actual hang detection (LAPIC NMI / ACPI TCO watchdog, timeout, auto-reboot, A/B rollback) is owned by → XREF: [`01-boot-platform/TODO-23-boot-watchdog.md`](TODO-23-boot-watchdog.md) (Boot Watchdog & Hang Detection); the soft-signal boot-heartbeat telemetry is → XREF: [`01-boot-platform/TODO-29`](TODO-29-boot-perf-health-observability.md) §8.

**Files:** `src/kernel/drivers/pit.c` (or `timer.c`), `src/kernel/main/boot_progress.c`

- [x] Remove/gate any `fb_swap_rect()` from `timer_tick_callback_fire()` / ISR (`alive_blink_tick` regression): no `alive_blink_tick` tick callback exists (only `spinner_advance`); `fb_swap_rect` is ISR-free -- the hazard path is gone.
- [ ] Design safe commit: compositor tick, DPC, or flag set in ISR plus deferred FB flush
- [ ] Re-enable `heartbeat=` from `g_boot_info.config` (0=off, 1=on); optional `HKLM\SYSTEM\Boot\AliveBlink` after → XREF: `02-kernel-core/TODO-14-registry-completion.md`
- [ ] 4x4 px green square at top-left (4,4); toggles ~0.5 s using the safe path only
- [ ] CPU usage threshold deferred: needs scheduler per-second CPU accounting (future)
- [ ] Commit: `"kernel: alive blink hang indicator -- safe FB path, no ISR swap"`

**Test checkpoint:** With §4 shipped: QEMU WHPX, QEMU TCG, VirtualBox: `heartbeat=1` shows stable blink, no lockup or FB corruption. Bare metal: same; confirm no recursive interrupt storm.
> **Test runner:** N/A (no kernel test surface yet -- the blink feature is deferred) | validation: serial-log + on-screen, pending the safe FB path
> **Notes:**
> - Shipped: only the safety invariant -- the `alive_blink_tick`/ISR `fb_swap_rect` regression path is confirmed absent (no such tick callback registered; `fb_swap_rect` is ISR-free).
> - Deferred: the visual blink feature (safe FB-path design + `heartbeat=` re-enable + the 4x4 square) needs a deferred-flush path (compositor tick / DPC) and BARE-METAL validation ("no recursive interrupt storm") that cannot be proven on QEMU alone.
> - Scope boundary: actual hang DETECTION (NMI/TCO watchdog, timeout, auto-reboot, A/B rollback) is owned by TODO-23; this section is the passive visual signal only.
> **Verified:** 2026-06-14 | safety invariant only | 1/7 items | build OK (no code change) | manual (symbol audit)
> **Deferred:** [M] alive-blink visual feature unimplemented (reason: needs safe deferred-FB path + bare-metal validation per bare-metal-first) -> XREF: 01-boot-platform/TODO-23-boot-watchdog.md (Boot Watchdog & Hang Detection)

---

## 5. Panic Forensic Evidence
Capture a `panic_evidence` struct at fault time into a fixed physical page that survives a warm reboot (the Linux pstore/ramoops equivalent), and restore + emit it on the next boot.

**Files:** `src/kernel/panic.c`, `include/kernel/panic.h`, `src/kernel/klog.c`, `src/kernel/main.c`, `src/kernel/main/boot_init.c`, `src/kernel/main/boot_desktop.c`

> [!NOTE]
> The `0x80000` page is kept out of the allocator by the existing `pmm_init()` first-1-MiB low-memory reservation (verified). Coordinate with `02-kernel-core/TODO-27-crash-dump-generation.md` to avoid reusing that fixed address for the minidump workspace.

- [x] `struct panic_evidence` versioned header `{magic, version, size, crc32, boot_seq}` (`panic.h`); `panic_evidence_restore` validates magic+version+size+crc32 so stale `0x80000` is never misread. `_Static_assert`: fits 4 KiB, multiple-of-8.
- [x] Crash identity: bugcheck code + params (params only when KeBugCheckEx-sourced), fault vector + err_code, cr0/cr2/cr3/cr4, cpu_id (CPUID APIC id), GPRs from `interrupt_frame`, irq_mask (PIC), pmm_free_pages, file:line.
- [x] Payload: last 16 `boot_stage_history` + last 8 klog entries (both serialized INLINE / pointer-free), last POST code (RAM shadow `boot_post_last_shadow`, not NVRAM), `message[256]`.
- [x] `panic_collect_evidence(frame, code, msg, file, line)`: raw physical writes to `0x80000`, no kmalloc/VFS/printk/lock; atomic first-caller-wins; hooked at the top of `panic_screen` after `cli`.
- [x] `0x80000` kept out of the allocator by the existing pmm_init first-1-MiB reservation (identity-mapped, verified). -> XREF: D02 T27 crash-dump (minidump-addr coordination).
- [x] `panic_evidence_restore_early()` (`kernel_main` after `boot_phase0`) restores/clears magic/logs; `panic_evidence_write_blackbox()` -> `X:\Crash\last-panic.txt` at desktop-ready. -> XREF: D01 T24 §7.
- [x] Unexpected-shutdown notice: kernel-side `panic_had_previous_crash()` flag (NOT `g_boot_info` -- avoids the boot ABI change) -> `boot_splash_diag` + klog before `boot_splash_finish`.
- [ ] Bare-metal reboot-reservation of `0x80000`: reserve it in the bootloader (AllocateAddress) before other allocations -- PMM only protects it post-kernel-entry, so firmware/BOOTX64 could clobber it pre-restore. Owner: `TODO-02` UEFI.
- [ ] last-panic.txt true durability: IXFS `vfs_flush` (C:\ fallback) flushes FS cache but not the device (`blkdev_sync`), so the consume-after-flush could lose the retry copy on reset; IXFS flush must sync the device. Owner: IXFS/storage.
- [x] Commit: `"kernel: panic forensic evidence -- cross-boot PMM page + last-panic.txt"`

**Test checkpoint:** Force a panic (`crash_test=1`), reboot: serial shows `[PANIC] Previous crash evidence found`; `X:\Crash\last-panic.txt` contains the fault RIP + POST code. QEMU WHPX, QEMU TCG, VirtualBox, bare metal: evidence survives warm reboot.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2633 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Shipped: `panic_evidence` record + `panic_collect_evidence`/`_restore`/`_write_blackbox` (`panic.c`/`.h`), lock-free `klog_panic_snapshot` (`klog.c`), fault-safe `boot_post_last_shadow` (`boot_init.c`); cross-boot page at `0x80000`.
> - Integration: collector hooked at the top of `panic_screen` (after `cli`); restore in `kernel_main` after `boot_phase0`; `X:\Crash\last-panic.txt` write + unexpected-shutdown notice at desktop-ready (Codex adoptions in commit message).
> - Tests: `test_boot_diag.c` (TEST_CAT_BOOT) -- crc32 canonical vector + restore magic/crc/version rejection + clear-after-read; full suite 2633 kernel + 16 user-mode 0 failures; smoke PASS.
> - Canonical doc: [`docs/boot/black-box-artifacts.md`](../../docs/boot/black-box-artifacts.md) (`X:\Diag\*` + `X:\Crash\` artifact index).
> - Scope boundary: §5 is the warm-reboot evidence page; full minidump (MEMORY.DMP) generation is `02-kernel-core/TODO-27`; the `X:\Crash\` path is owned by TODO-24 §7.
> **Verified:** 2026-06-14 | ship `a59b8f64` (+ this review commit) | 7/9 items | build OK | smoke PASS + tests 2633
> **Accepted:** [H] `0x80000` is only kernel-reserved, not bootloader/reboot-reserved (firmware/BOOTX64 can clobber it pre-restore on bare metal) -> XREF: 01-boot-platform/TODO-14 §5 (item: "Bare-metal reboot-reservation of `0x80000`" at line 177)
> **Accepted:** [H] IXFS `vfs_flush` (C:\ fallback) does not `blkdev_sync`, so consume-after-flush can lose the retry copy (reason: FS-layer durability contract) -> XREF: 01-boot-platform/TODO-14 §5 (item: "last-panic.txt true durability" at line 178)
> **Quality reviewed:** 2026-06-14 | Codex 14x (design + test-coverage + adversarial + re-adversarial + adversarial-impl + consistency + perf) | 1C+6H+8M+1L fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 6. Panic QR Code *(deferred -- depends on §5)*
Embed a minimal QR code encoder and render a phone-scannable URL in the BSOD corner.

**Files:** `src/kernel/qr_encode.c`, `include/kernel/qr_encode.h`, `src/kernel/panic.c`

- [ ] Implement or embed a minimal MIT-licensed QR code encoder (~500 lines): `qr_encode(const char *text, uint8_t *matrix, int *size)` where `matrix` is a `size` by `size` bit grid (1=dark, 0=light); support QR version 3-6 (covers URLs up to ~150 chars); error correction level M
- [ ] `panic_qr_url(buf, bufsize, message, post_code, os_version)`: format `https://docs.impossible-os.dev/panic?msg=<short>&post=0x{post}&v={ver}` (truncate message to 40 chars to keep URL under 120 chars)
- [ ] In `kernel_panic()` BSOD renderer: after drawing the main panic screen, call `qr_encode(url, matrix, &qr_size)`; render QR module grid at bottom-right (12 px from corner), module size = 4 px, white modules on black background; quiet zone = 4 modules
- [ ] Ensure `qr_encode.c` is freestanding: no libc, no floating point; uses only `kernel/types.h` and `kernel/libc/string.h`
- [ ] Commit: `"kernel: minimal QR encoder + panic BSOD QR code for phone-scannable troubleshooting"`

**Test checkpoint:** With §5+§6 shipped: forced panic shows scannable QR bottom-right; URL resolves to docs panic page. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

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

---

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

---

## 9. Boot Timeline Visualization and Import Parity *(deferred)*
Linux `systemd-analyze plot` and Windows performance tooling expose boot as a human-readable timeline. This repo already emits machine-readable `boot-timeline.json` from `boot_timeline_dump_json()`; viewer parity is still open.

**Files:** `docs/` (schema + tooling notes), optional `scripts/` or `user/` offline converter

- [ ] Publish the v1 schema in `docs/boot-timeline-json.md`: `stage`/`phase`/`post`/`start_ms`/`duration_ms`/`target_ms`/`source` (`fpdt`|`tsc`)/`unreliable` -- `boot_timeline_dump_json()` already emits all of these; link from `CLAUDE.md`.
- [ ] The SVG + chrome://tracing converters MUST preserve `target_ms` and `unreliable` so the boot-blame / regression surfaces (→ XREF: [`01-boot-platform/TODO-29`](TODO-29-boot-perf-health-observability.md)) are not silently discarded.
- [ ] Optional offline converter or in-kernel `boot_timeline_to_svg()` to produce Gantt-style SVG comparable to `systemd-analyze plot` output
- [ ] Optional Chrome trace event JSON export for `chrome://tracing` import (competitive edge vs plain SVG)
- [ ] Commit: `"docs: boot timeline JSON schema + optional trace export"`

**Test checkpoint:** With §9 shipped: JSON from a real boot validates against schema; SVG or Chrome trace opens in target viewer without manual edits. QEMU WHPX + TCG smoke.

---

## 10. Bootloader Build Identity Dump in BlackBox

[Bootloader Build Identity](TODO-01-boot-protocol-abi-handoff.md#20-bootloader-build-identity) added `boot_info.loader_identity` (git_sha + build_unix_time + label) populated at handoff. The kernel fatal screen and `boot_version_blackbox_transcribe` (which writes to `X:\Diag\boot-proto-fault.txt`) already surface identity for FAULT records, but on a healthy boot the identity is only visible via `HKLM\SYSTEM\Boot\Decision\Loader*` registry values. Dump it as a standalone BlackBox artifact for offline triage that doesn't require a registry query.

- [x] `boot_loader_identity_format()` (pure; all-zero -> `unavailable`) + `boot_loader_identity_dump_to_blackbox()` in `boot_version.c` -> `X:\Diag\boot-loader-identity.txt` (git_sha 40-hex + decimal/ISO-8601 time + label).
- [x] Wired into the late-boot BlackBox sequence in `boot_desktop.c` after `boot_version_blackbox_transcribe()`; single-open `O_WRITE|O_CREATE|O_TRUNC` (idempotent overwrite).
- [x] Test `test_loader_identity_format` (`test_boot_diag.c`, TEST_CAT_BOOT): populated + zero-sentinel + cap-0/1 bounds + NUL-termination + `kdate_iso8601` known vectors; no live VFS.
- [x] `docs/boot/black-box-artifacts.md` created: index of every `X:\Diag\*` kernel artifact (boot-loader-identity.txt + 11 others).
- [x] Commit: `"diag: dump boot_info.loader_identity to X:\Diag\boot-loader-identity.txt"`

**Test checkpoint:** On a clean boot, `X:\Diag\boot-loader-identity.txt` contains the bootloader's git_sha (matches `git rev-parse HEAD` of the build), build_unix_time, and label. Format matches the documented schema. QEMU WHPX + TCG.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2561 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Shipped: `boot_loader_identity_format()` (pure formatter; all-zero git_sha/time/label -> `unavailable` sentinel, fail-closed on cap-0/NULL) + `boot_loader_identity_dump_to_blackbox()` in `boot_version.c`, writing `X:\Diag\boot-loader-identity.txt`.
> - Integration: wired in `boot_desktop.c` after `boot_version_blackbox_transcribe()`; single-open `O_WRITE|O_CREATE|O_TRUNC`, best-effort; uses the shared `kdate_iso8601()` (`time_iso.h`) extracted from `firmware_tables_json.c`.
> - Tests: `test_boot_diag.c` (TEST_CAT_BOOT) -- formatter populated/zero-sentinel/cap-bounds/NUL + `kdate_iso8601` vectors.
> - Canonical doc: [`docs/boot/black-box-artifacts.md`](../../docs/boot/black-box-artifacts.md) (index of every `X:\Diag\*` artifact).
> - Scope boundary: §10 dumps the healthy-boot bootloader identity only; the fault-path transcript is `boot_version_blackbox_transcribe` (boot-proto-fault.txt).
> **Verified:** 2026-06-14 | ship `9f31c16a` (+ this review commit) | 4/4 items | build OK | tests 2561 kernel + 16 user-mode
> **Quality reviewed:** 2026-06-14 | Codex 4x (design + adversarial + consistency + perf) | 2M fixed | scope: kernel-code-quality (re-adversarial skipped: formatter control-flow gate, <50 LOC, 1 fn, no lock/ISR/lifecycle)

---

## 11. Boot Load Status Log (ntbtlog Parity)

Win11 `ntbtlog.txt` records every driver/service that loaded or failed during boot; Linux exposes the same via dmesg + `systemd-analyze`. §2 records only 15 coarse named stages, so a storage/input/network boot regression reads as generic `DRIVERS` progress instead of a diagnosable "this subsystem failed/degraded". This adds a durable per-subsystem boot load/status artifact (the observability surface boot triage actually needs).

- [x] `struct boot_load_entry` (64-entry pool, no wrap) + `boot_load_class`/`boot_load_state` enums in `include/kernel/boot_load_status.h`: name[32], err/post u16, start/duration ms u32, publish flag.
- [x] Lock-free SMP-safe append (`atomic_fetch_add` claim + release/acquire publish; async storage probes fan across CPUs): `boot_load_record` point event + `boot_load_begin`/`boot_load_finish` measured span. Reuses `boot_get_elapsed_ms()` (§2).
- [x] `boot_load_status_dump_to_blackbox()` -> `X:\Diag\boot-load-status.txt` (2-page pmm buffer; header `N recorded / 64 cap, D dropped [TRUNCATED]`); wired in `boot_desktop.c` after `boot_run_deferred()`.
- [x] `boot_load_status_report_summary()`: serial `klog` (authoritative) + best-effort `boot_splash_diag()` FAILED/DEGRADED one-liner, emitted after `boot_run_deferred` so the summary reflects the complete record (incl. deferred network/input).
- [x] Initial call sites: storage probe block (`begin`/`finish`, captures async DEGRADED/FATAL-fallback) + network (`deferred_net_init` real outcome or in-phase `net_init`).
- [ ] Granular per-driver `boot_load_record`: storage (ata/ahci/nvme/virtio-blk), input (PS2/USB HID), ACPI, GFX -- each self-reports vs the single aggregate `storage` entry. Owner: this section.
- [ ] Probe-result aggregation (storage + network): split driver return codes so absent-vs-failed is distinguishable (rtl8139/ahci/virtio-blk return -1 for both; sequential storage always LOADED) for accurate SKIPPED/FAILED. Owner: this section.
- [ ] NVMe per-controller status: expose attempted-vs-initialized count from `nvme_init` (today returns only the success count) so a partial multi-controller failure records DEGRADED, not BOOT_OK. Owner: this section / NVMe driver.
- [x] Test `test_boot_load_status` (`test_boot_diag.c`, TEST_CAT_BOOT): mix + format + summary + all-LOADED + begin/finish + overflow + fail-closed + 64/65 boundary + pool-full token + name truncation; save/restore seam.
- [x] Commit: `"diag: per-subsystem boot load/status log -> X:\Diag\boot-load-status.txt (ntbtlog parity)"`

**Test checkpoint:** On a boot with an absent/failing device (e.g. no AHCI), `X:\Diag\boot-load-status.txt` lists that subsystem as FAILED/DEGRADED with an error code; a clean boot lists every core subsystem LOADED; serial shows the degraded summary only when something failed. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2605 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Shipped: `boot_load_status.{c,h}` -- 64-entry lock-free pool, SMP-safe `boot_load_record`/`begin`/`finish`, pure `format` + `degraded_summary`, `dump_to_blackbox` (`X:\Diag\boot-load-status.txt`), `report_summary` (serial + splash).
> - Integration: storage probe block + `deferred_net_init`/`net_init` record real outcomes; report + dump both run after `boot_run_deferred` so the record is complete; serial klog is authoritative (Codex adoptions in commit message).
> - Tests: `test_boot_diag.c` (TEST_CAT_BOOT) -- 44 asserts; full suite 2605 kernel + 16 user-mode 0 failures; smoke PASS shows `[BOOT-LOAD] all subsystems loaded clean` (NIC absent -> SKIPPED) + dump.
> - Canonical doc: [`docs/boot/black-box-artifacts.md`](../../docs/boot/black-box-artifacts.md) (`X:\Diag\*` artifact index).
> - Scope boundary: §11 ships infra + storage/network wiring; granular per-driver coverage + probe-result aggregation + NVMe per-controller status are the open `[ ]` items in this section.
> **Verified:** 2026-06-14 | ship `657637ec` (+ this review commit) | 6/9 items | build OK | smoke PASS + tests 2605/2605
> **Accepted:** [H] sequential storage publishes LOADED on probe failure + async marks AHCI/VirtIO absence DEGRADED (reason: needs driver absent-vs-failed split) -> XREF: 01-boot-platform/TODO-14 §11 (item: "Probe-result aggregation (storage + network)" at line 278)
> **Accepted:** [M] NVMe partial multi-controller failure reads BOOT_OK (reason: nvme_init exposes only the success count) -> XREF: 01-boot-platform/TODO-14 §11 (item: "NVMe per-controller status" at line 279)
> **Quality reviewed:** 2026-06-14 | Codex 8x (design + test-coverage + adversarial + re-adversarial + consistency + perf) | 3H+8M+2L fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## OS Comparison

| ⭐ | Feature                 | 🪟 Win11                     | 🐧 Linux                     | 🚀 Impossible OS                  |
| -- | ----------------------- | ---------------------------- | ---------------------------- | --------------------------------- |
| 💎 | Boot POST codes         | ✅ Firmware boot mgr        | ✅ BIOS POST codes           | ✅ §1 §3 POST port 80 + FB hex    |
| 💎 | Named boot progress     | ✅ ETW boot trace           | ✅ dmesg systemd-analyze     | ✅ §2 serial STAGE ms lines       |
| 💎 | Boot load/status log    | ✅ ntbtlog.txt driver log   | ✅ dmesg drivers loaded      | ✅ §11 boot-load-status.txt       |
| ⭐ | Bootloader build identity | ⚠️ bcdedit/msinfo32        | ⚠️ /proc/version uname       | ✅ §10 boot-loader-identity.txt   |
| 💎 | Boot timeline viewers   | ⚠️ Performance Toolkit      | ✅ systemd-analyze plot     | ⚠️ JSON §2; §9 viewers pending     |
| 💎 | Panic forensics         | ✅ WER minidump EventLog    | ✅ kdump pstore ramoops      | ✅ §5 0x80000 page last-panic.txt |
| 💎 | Multi UI spinner        | ✅ WinUI ProgressRing       | ✅ GTK Qt spinners           | ⬜ §7 spinner_create pool         |
| ⭐ | Panic BSOD QR           | ❌ Text URL BSOD only       | ❌ No kernel QR              | ⬜ §6 phone URL QR matrix         |
| ⭐ | Alive hang pixel        | ❌ No kernel hang pixel     | ❌ Not production default     | ⬜ §4 redesign safe FB path       |
| ⭐ | Live vital overlay      | ⚠️ Task Manager separate    | ⚠️ htop conky third-party    | ⬜ §8 bottom metrics strip        |

> **Parity scan:** Win11+Linux ✅ on POST, named progress, panic dumps, UI spinners -- Impossible OS matches via §1--§3 plus §10 bootloader identity and §11 ntbtlog-parity load/status log; timeline **export** exists (JSON) but **viewers** match Linux/Win tooling only after §9. ⬜ rows §5--§8 and §9 are open parity or stretch (⭐ rows). **Edges:** §6 QR and §8 always-on strip are planned differentiators once shipped.

---

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
  - Panic evidence (§5): `panic_crc32` returns the canonical IEEE check value (`crc32("123456789") == 0xCBF43926`); a well-formed record built at `0x80000` is restored, the magic is cleared, and a second restore returns nothing (idempotent)
  - Panic evidence header (§5): a record with a bad `crc32`, wrong `version`, or absent magic is REJECTED on restore (not misread as valid) and its magic dropped
  - Boot load status (§11): `boot_load_record()` appends entries; the in-memory format + the FAILED/DEGRADED summary string are correct; an all-LOADED pool yields an empty degraded summary
  - After §9 schema lands: validate a captured `boot-timeline.json` against `docs/boot-timeline-json.md` (field names incl `target_ms`/`source`/`unreliable`, numeric types)
- [ ] Register in `test_runner_init()`: `test_register_boot_diag()`
- [ ] Commit: `"test: add boot_diag test suite"`

---

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

---

## History

| Date       | Action        | Summary |
| ---------- | ------------- | ------- |
| 2026-04-12 | gap-analysis  | Web: Win11 required diagnostics / WER flow (Learn); Linux ramoops+pstore (kernel.org), systemd-analyze, boot-time trace docs. Code-truth: `panic_evidence` / `spinner_create` / `vital_signs` absent; `boot_timeline_dump_json` in `boot_progress.c` + call `boot_desktop.c:229`; `boot_progress_poll` uncalled. Added **Current state** callout; Inputs `boot_desktop.c` + TODO-15 scope XREF; §4 bullets for timeline JSON; new §9 + Impl row 9 + OS row; Unit Tests wording; History table. |
| 2026-04-12 | validate      | Inputs paths exist; no N.M / continuation-line drift; OS row Impossible cell shortened to five words after glyph; Impl order 7 Depends On now `08-graphics-ui/TODO-06` §8 (blocked until compositor per-frame ticks); Unit Tests XREF replaced the dead kernel-test-framework path with `test_runner.c` + `TEST_CAT_BOOT` pattern; Verification adds `run-boot-tests.bat`; Verification XREF to TODO-24 uses domain-qualified path. **Flag:** legacy phantom kernel-test-framework path should normalize to `00-infrastructure/TODO-03-kernel-test-harness.md` repo-wide. **Note:** `> **Verified:**` lines after §1--§5 checkpoints are audit stamps from `verify-todo-section`, not checklist items. |
