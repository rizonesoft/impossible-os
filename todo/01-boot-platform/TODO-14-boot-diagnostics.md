---
schema_version: 1
id: boot-diagnostics
domain: 01-boot-platform
status: active
title: "TODO-14 -- Boot Diagnostics, Heartbeat & Spinner"
---

# TODO-14 -- Boot Diagnostics, Heartbeat & Spinner

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** The arc spinner and boot splash are done. This TODO builds the production diagnostics layer: a named-stage boot progress API that feeds the splash, POST-style hex codes visible on hardware debug cards, cross-boot panic forensics, a panic QR code, runtime vital-signs overlay, and a multi-instance compositor-integrated spinner -- turning the ad-hoc debug tooling into production-grade features. The former alive-blink visual heartbeat is permanently deferred; keep only the shipped safety invariant that forbids framebuffer swaps from ISR context.

> [!IMPORTANT]
> **Current state:** §1--§3, §5, §10, §11 are shipped/verified; §4 is permanently deferred (`[/]` + Deferred stamp, safety invariant only -- no blink feature is planned). §6--§9 remain open. `boot_progress_poll()` has **no in-tree callers** yet. `spinner_create` (§7) / `vital_signs` (§8) sources are not in the tree.

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
- → XREF: `02-kernel-core/TODO-14-registry-completion.md` -- `VitalSigns` registry key (visual POST / debug bar: [TODO-15 -- Visual POST Display](TODO-15-visual-post-display.md)); `HKLM\SYSTEM\Boot\AliveBlink` is N/A because §4 is permanently deferred.
- → XREF: `TODO-02-uefi-hardening-secureboot.md §7` -- boot UX polish calls `boot_splash_status()` via the §2 API
- → XREF: `08-graphics-ui/TODO-08-window-manager.md` §8 (item: "Per-frame `spinner_tick()` over the active-spinner list" at line 197) -- the compositor frame loop must call `spinner_tick()` on every active `g_active_spinners[]` entry per frame
- → XREF: `TODO-03-bootloader-error-recovery.md §13` -- bootloader-stage NVRAM error codes; §3 here persists kernel-stage panic evidence, §13 there persists UEFI-stage boot error codes -- complementary
- → XREF: `TODO-03-bootloader-error-recovery.md §14` -- bootloader QR code on UEFI error screen; §4 here does the same for kernel-stage panic BSOD -- share QR encoder if both are implemented
- → XREF: `TODO-15-visual-post-display.md` -- full-screen VPD (tiered bars, splash integration, NVRAM last-boot) complements this file; TODO-14 owns POST16 I/O port `0x80`, framebuffer corner digits, named-stage serial, and panic evidence specs

## Outcome

- Serial log shows `[+NNNms] BOOT_PMM: Physical memory manager ready` style entries for every major stage.
- `boot-timeline.json` written under BlackBox `X:\Perf\` (or klog fallback) after successful boot summarizing FPDT firmware phases + TSC bootloader/kernel steps (`boot_timeline_dump_json()`; path moved from `X:\Boot\` by TODO-04 FPDT normalization).
- Four-digit hex POST code (native 8 px glyphs) visible in the top-right corner of the framebuffer from kernel entry until `BOOT_DESKTOP_READY`; high byte of each 16-bit code is output to I/O port 0x80.
- On panic: `struct panic_evidence` captured at `0x80000`; next boot saves `last-panic.txt` and shows "System shut down unexpectedly" toast.
- Panic BSOD shows a QR code in the bottom-right corner linking to the troubleshooting page.
- Alive blink is permanently deferred. Only the safety invariant remains: ISR paths must not call `fb_swap_rect()`.
- **§7** multi-instance `spinner_create()` / `spinner_tick()` and per-frame compositor ticks ship with that section (→ XREF: `08-graphics-ui/TODO-08-window-manager.md` §8).
- `VitalSigns=1` activates a 20 px bottom-strip showing CPU %, RAM, IRQ rate, uptime, and FPS.

## Implementation Order

| ⭐  | Order | Deliverable                                                   | Depends On                         | Status |
| --- | :---: | ------------------------------------------------------------- | ---------------------------------- | :----: |
| 💎  |   1   | UEFI pre-kernel POST codes                                    | --                                 |  [/]   |
| 💎  |   2   | Boot progress named-stage API                                 | §1                                 |  [/]   |
| 💎  |   3   | POST-style hex code display                                   | §2                                 |  [x]   |
| 💎  |   4   | Alive blink / visual heartbeat                                | permanently deferred; hang=TODO-23 |  [/]   |
| 💎  |   5   | Panic forensic evidence                                       | §2                                 |  [/]   |
| ⭐  |   6   | Panic QR code                                                 | §5; T03 §14 (QR seed)              |  [/]   |
| 💎  |   7   | System-wide multi-instance spinner                            | D08 T08 §8 (compositor)            |  [/]   |
| ⭐  |   8   | Runtime vital signs strip                                     | D02 T25 §7 (CPU accounting)        |  [/]   |
| 💎  |   9   | Boot timeline visualization/import                            | §2                                 |  [/]   |
| ⭐  |  10   | Bootloader build identity dump in BlackBox                    | TODO-01 §20                        |  [x]   |
| 💎  |  11   | Boot load status log (ntbtlog parity)                         | §2                                 |  [/]   |
| 💎  |  12   | Boot load status granularity (per-driver, probe, NVMe)        | §11; D02 T33 §7 (image ceiling)    |  [/]   |
| 💎  |  13   | klog format-width contract (`-Wformat` on every call site)    | D02 T33 §7 (image ceiling)         |  [/]   |
| 💎  |  14   | Panic-evidence page reserved by the bootloader (`0x80000`)    | §5                                 |  [x]   |
| ⭐  |  15   | Anti-rollback terminal give-up durable record                 | TODO-01 §25; D02 T33 §7 (ceiling)  |  [/]   |
| ⭐  |  16   | Boot timeline export formats (SVG + Chrome trace)             | §9                                 |  [/]   |
| 💎  |  17   | Restore panic evidence before Phase 0 can overwrite it        | §14; §5; D02 T33 §7 (ceiling)      |  [/]   |
| 💎  |  18   | Pre-serial `serial_early_print` drives a poisoned UART port   | §14                                |  [x]   |
| 💎  |  19   | Record the panic-page pin outcome in the boot_info handoff    | §14; D02 T33 §7 (image ceiling)    |  [/]   |
| 💎  |  20   | Complete the `.bss` poison reset discipline across the loader | §18                                |  [x]   |

> 💎 = parity -- Windows and Linux both have equivalent diagnostics; Impossible OS must match them.
> ⭐ = exclusive -- the QR code on BSOD and always-visible vital-signs strip are not present in either competitor at the kernel level.

---

## 1. UEFI Pre-Kernel POST Codes
Write I/O port 0x80 POST codes from the bootloader so hardware POST-code reader cards decode boot progress before the kernel even starts.

**Files:** `src/boot/uefi/bootx64.c`

- [x] Add `post_code(uint8_t code)` inline in `bootx64.c`: `outb(0x80, code)`; add `post_code16(uint16_t code)` (writes high byte to port `0x80`, logs full value on serial) plus legacy 8-bit names `POST_ENTRY` through `POST_KERNEL_JUMP` for reference
- [x] Insert `post_code16()` at bootloader milestones: `0xB001` entry, `0xB090`/`0xB091` boot device (LoadedImage) before GOP, `0xB010` GOP init, `0xB020`/`0xB021` ELF open/load, `0xB030` RSDP, `0xB080`-`0xB085` USB discovery and xHCI DMA/takeover substeps, `0xB040` memory map, `0xB050` ExitBootServices, `0xB060` page tables, `0xB070` kernel jump
- [x] QEMU ignores port 0x80 writes silently -- no fault, verified by clean build
- [/] operator-gated (architect call + a POST card on bare metal): **POST-card milestone discrimination (deferred):** `post_code16` writes the high byte `0xB0` to port 0x80,
  - so an 8-bit card shows `0xB0` for every bootloader milestone. Evaluate low-byte/sequence emission -- architect call.
- [x] Commit: `"boot: UEFI pre-kernel POST codes to I/O port 0x80"`

**Test checkpoint:** UEFI build; milestones emit `outb(0x80, code)` in order. QEMU WHPX, QEMU TCG, VirtualBox: no fault on port writes. Bare metal: an 8-bit POST card shows the high byte `0xB0` (bootloader-range) if present; per-milestone detail is on serial (the low-byte improvement is the deferred item above).
> **Notes:**
> - Shipped: `post_code(uint8_t)` + `post_code16(uint16_t)` (high byte to port 0x80, full value on serial) in `bootx64.c`, emitted at bootloader milestones 0xB001 through 0xB070.
> - Integration: QEMU ignores port 0x80 writes silently (no fault); on bare metal an 8-bit POST card shows the high byte `0xB0` (bootloader range) -- per-milestone discrimination is on serial only (low-byte card emission is deferred).
> - Scope boundary: bootloader-stage codes only; kernel-stage POST display is §2/§3.
> **Verified:** 2026-04-12 -- Milestone table reconciled to `post_code16`; Codex: disarm watchdog on `init_gop` error return before firmware UI. Accepted: none.
> **Deferred:** [M] 8-bit POST card shows only the high byte `0xB0`, no per-milestone discrimination (serial is per-milestone) -> XREF: 01-boot-platform/TODO-14 §1 (item: "POST-card milestone discrimination (deferred)" at line 94)
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf) | 1M deferred (card byte encoding), doc-overstatement fixed | scope: boot-code-quality (re-adversarial skipped -- doc-accuracy fix only, finding deferred not coded)

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
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): **Wire + harden the named-stage API (deferred):** `boot_stage_report`/`boot_progress_poll` have zero callers. Wire boot-path callers, map `s_meta`->`POST16_*`, saturate the tsc-delta,
  - guard `s_meta` completeness, forward percent/splash.
- [x] Commit: `"kernel: boot progress named-stage API + stage history + elapsed-ms tracking"` (exact subject varies across bring-up commits)

**Test checkpoint:** Serial shows `[+NNNms]` lines with stage names for at least 8 transitions; `boot_stage_history_get()` does not fault when called from panic paths. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Notes:**
> - Shipped: `boot_stage_report()` (15-stage `boot_stage_t`, 32-entry capped history, TSC + elapsed-ms) wrapping the existing `boot_progress()`, plus `boot_timeline_dump_json()` (FPDT + TSC step timeline) in `boot_progress.c`.
> - Integration: serial `[+NNNms] STAGE: msg`; `boot_timeline_dump_json()` runs at desktop-ready (`boot_desktop.c`); `boot_stage_history_get()` feeds §5 panic forensics.
> - Scope boundary: `boot_progress_poll()` has no in-tree caller yet (wire with splash/timer when the UI needs a refresh without new history rows).
> **Verified:** 2026-04-12 -- Named-stage API + `boot_timeline_dump_json` wiring; Codex: safe TSC-to-ms when `freq < 1000`, close timeline parent dir after `create`. Accepted: none.
> **Deferred:** [H] named-stage API (`boot_stage_report`/`boot_progress_poll`/`boot_get_elapsed_ms`) is unwired (zero callers) + latent issues (stale `s_meta` postcodes, unsaturated tsc-delta, no completeness guard, percent/splash not forwarded) -> XREF: 01-boot-platform/TODO-14 §2 (item: "Wire + harden the named-stage API (deferred)" at line 123)
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf) | 2H+3M deferred (unwired API + latent fixes) | scope: kernel-code-quality (re-adversarial skipped -- findings deferred not coded; boot_timeline_dump_json live path unaffected in practice)

---

## 3. POST-Style Hex Code Display
Render a 4-digit hex POST code in the top-right framebuffer corner visible on every boot, cleared when the desktop is ready.

**Files:** `src/kernel/main/boot_progress.c`, `include/kernel/boot_progress.h`

- [x] 8x8 hex font (16 glyphs, 128 bytes) embedded in `boot_progress.c`
- [x] `post_display16(code)`: renders 4 hex digits at native 8 px scale (gray on black) at top-right via direct VRAM before `SUBSYS_FB` or `fb_put_pixel()` + `fb_swap_rect()` after framebuffer init
- [x] Port `0x80` is written from `boot_post_write16()` inside `boot_progress()` (also invoked by `boot_stage_report()`); `post_display16()` skips pixel writes when FB unavailable or `postcode=0` after config parse
- [x] Called from `boot_stage_report()` after serial write (and again from `boot_progress()` / `boot_post_write16()` for the same stage)
- [x] Clear-on-`DESKTOP_READY` (`fb_fill_rect` + `fb_swap_rect`, `POST16_TOTAL_W` x `POST16_TOTAL_H`) lives in `boot_stage_report` (unwired -- see §2); the live `boot_progress` path relies on desktop overdraw to cover the digits.
- [x] Commit: `"kernel: POST-style hex code display in framebuffer corner + I/O port 0x80"` (exact subject varies; see `git log -- src/kernel/main/boot_progress.c`)

**Test checkpoint:** From kernel entry through desktop ready: four hex digits top-right; I/O port `0x80` high byte tracks the current 16-bit POST; digits clear on desktop ready. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Notes:**
> - Shipped: 8x8 hex font (16 glyphs) + `post_display16()` rendering 4 hex digits top-right via direct VRAM (pre-`SUBSYS_FB`) or `fb_put_pixel()`+`fb_swap_rect()` (post-FB), in `boot_progress.c`.
> - Integration: called from `boot_stage_report()` after the serial write; port 0x80 high byte tracks the 16-bit POST; cleared at `BOOT_STAGE_DESKTOP_READY` over the `POST16_*` geometry.
> - Scope boundary: skips pixel writes when the framebuffer is unavailable or postcode=0 after config parse.
> **Verified:** 2026-04-12 -- Desktop-ready clear uses `POST16_*` geometry; same TSC-ms guard as §2. Accepted: none.
> **Accepted:** [M] explicit clear-on-DESKTOP_READY only runs via the unwired `boot_stage_report`; the live path relies on desktop overdraw -> XREF: 01-boot-platform/TODO-14 §2 (item: "Wire + harden the named-stage API (deferred)" at line 123)
> **Quality reviewed:** 2026-06-17 | Codex 3x (adversarial, consistency, perf) | 0 fixed, 1M accepted-XREF (clear-wiring owned by §2); doc-accuracy fixed | scope: kernel-code-quality (re-adversarial skipped -- doc fix, finding accepted to §2)

---

## 4. Alive Blink / Visual Heartbeat Indicator *(permanently deferred -- ISR `fb_swap_rect` caused recursive interrupts on bare metal)*
The 4x4 px visual liveness indicator is permanently deferred. Keep the already-shipped safety invariant only: framebuffer swaps must never run from ISR context. This section does not own hang detection, timeout, reboot, watchdog petting, or rollback. Actual hang detection (LAPIC NMI / ACPI TCO watchdog, timeout, auto-reboot, A/B rollback) is owned by → XREF: [`01-boot-platform/TODO-23-boot-watchdog.md`](TODO-23-boot-watchdog.md) (Boot Watchdog & Hang Detection); the soft-signal boot-heartbeat telemetry is → XREF: [`01-boot-platform/TODO-29`](TODO-29-boot-perf-health-observability.md) §8.

**Files:** `src/kernel/drivers/pit.c` (or `timer.c`), `src/kernel/main/boot_progress.c`

- [x] Remove/gate any `fb_swap_rect()` from `timer_tick_callback_fire()` / ISR (`alive_blink_tick` regression): no `alive_blink_tick` tick callback exists (only `spinner_advance`); `fb_swap_rect` is ISR-free -- the hazard path is gone.
- [~] Safe visual-commit design -- N/A; the visual blink is not planned.
- [~] Re-enable `heartbeat=` / `HKLM\SYSTEM\Boot\AliveBlink` -- N/A for the visual blink. Serial boot-heartbeat telemetry remains owned by TODO-29 §8.
- [~] 4x4 px green square at top-left (4,4) -- N/A.
- [~] CPU usage threshold -- N/A here; scheduler per-second CPU accounting remains owned by future scheduler/vital-signs work.
- [~] Commit -- N/A.

**Test checkpoint:** N/A (visual blink permanently deferred). Safety regression remains manual/symbol-audit only: no `alive_blink_tick` callback and no `fb_swap_rect()` reachable from timer ISR context.
> **Test runner:** N/A (no visual feature to test) | validation: manual symbol audit for ISR-free framebuffer swaps
> **Notes:**
> - Shipped: only the safety invariant -- the `alive_blink_tick`/ISR `fb_swap_rect` regression path is confirmed absent (no such tick callback registered; `fb_swap_rect` is ISR-free).
> - Permanently deferred: the visual blink feature (safe FB-path design + `heartbeat=` re-enable + the 4x4 square) is not planned. Do not reintroduce it without a new roadmap decision and bare-metal validation plan.
> - Scope boundary: actual hang DETECTION (NMI/TCO watchdog, timeout, auto-reboot, A/B rollback) is owned by TODO-23; this section is the passive visual signal only.
> **Verified:** 2026-06-15 | PERMANENTLY DEFERRED | safety invariant only | 1/7 items | doc-only decision | manual (symbol audit)
> **Deferred:** [M] alive-blink visual feature permanently deferred (reason: framebuffer-from-ISR hazard on bare metal; no visual-blink product requirement remains) -> XREF: 01-boot-platform/TODO-23-boot-watchdog.md (Boot Watchdog & Hang Detection)

---

## 5. Panic Forensic Evidence
Capture a `panic_evidence` struct at fault time into a fixed physical page that survives a warm reboot (the Linux pstore/ramoops equivalent), and restore + emit it on the next boot.

**Files:** `src/kernel/panic.c`, `include/kernel/panic.h`, `src/kernel/klog.c`, `src/kernel/main.c`, `src/kernel/main/boot_init.c`, `src/kernel/main/boot_desktop.c`

> [!NOTE]
> The `0x80000` page is kept out of the allocator by the existing `pmm_init()` first-1-MiB low-memory reservation (verified). Coordinate with `02-kernel-core/TODO-27-crash-dump-generation.md` to avoid reusing that fixed address for the minidump workspace.

- [x] `struct panic_evidence` versioned header (`panic.h`); `panic_evidence_restore` validates magic+version+size+crc32 so stale `0x80000` is never misread. `_Static_assert`: fits 4 KiB, multiple-of-8.
  - Header RESHAPED to v3 by the evidence-lifecycle rework: `magic`+`epoch` now lead the struct as one 64-bit publication word -> XREF: `01-boot-platform/TODO-10` §23.
- [x] Crash identity: bugcheck code + params (params only when KeBugCheckEx-sourced), fault vector + err_code, cr0/cr2/cr3/cr4, cpu_id (CPUID APIC id), GPRs from `interrupt_frame`, irq_mask (PIC), pmm_free_pages, file:line.
- [x] Payload: last 16 `boot_stage_history` + last 8 klog entries (both serialized INLINE / pointer-free), last POST code (RAM shadow `boot_post_last_shadow`, not NVRAM), `message[256]`.
- [x] `panic_collect_evidence(...)`: raw physical writes to `0x80000`, no kmalloc/VFS/printk/lock; hooked at the top of `panic_screen` after `cli`.
  - The entry-time first-caller-wins claim was REPLACED by per-invocation ownership arbitrated at terminal declaration -> XREF: `01-boot-platform/TODO-10` §23.
- [x] `0x80000` kept out of the allocator by the existing pmm_init first-1-MiB reservation (identity-mapped, verified). -> XREF: D02 T27 crash-dump (minidump-addr coordination).
- [x] `panic_evidence_restore_early()` (`kernel_main` after `boot_phase0`) restores and logs; `panic_evidence_write_blackbox()` -> `X:\Crash\last-panic.txt` at desktop-ready. -> XREF: D01 T24 §7.
  - Restore RETAINS the page (it does NOT clear the magic), so a boot that dies before the write retries next boot; only `panic_evidence_consume()` clears it, after a successful write plus flush.
- [x] Unexpected-shutdown notice: kernel-side `panic_had_previous_crash()` flag (NOT `g_boot_info` -- avoids the boot ABI change) -> `boot_splash_diag` + klog before `boot_splash_finish`.
- [/] PARKED: nothing preserves `0x80000` between the reset and Phase 0 -- firmware and BOOTX64 allocate before the kernel restores it -> XREF: `01-boot-platform/TODO-10` §28 (item: "Decide and implement the memory-type policy").
  - The kernel-side half above is real and verified: `pmm_init`'s first-1-MiB reservation keeps the page out of THIS boot's allocator. The pre-kernel window is what nothing covers -- firmware and the bootloader run and allocate before `panic_evidence_restore_early` reads the page, and neither reserves nor copies it first.
  - TODO-10 §28 made the record reach memory before the reset, which was the missing half on the write side. The gap has since SPLIT and is half closed: §14 shipped the `AllocateAddress` reservation at bootloader entry (2026-08-30, `41753ca67`), so firmware and BOOTX64 can no longer hand the page out. What remains is reporting the pin outcome through the `boot_info` handoff so the kernel can distinguish "no crash" from "record lost", and that half is owned by this file's §19, parked on the kernel-image ceiling.
- [x] Commit: `"kernel: panic forensic evidence -- cross-boot PMM page + last-panic.txt"`

**Test checkpoint:** Force a panic (`crash_test=1`), reboot: serial shows `[PANIC] Previous crash evidence found`; `X:\Crash\last-panic.txt` contains the fault RIP + POST code. QEMU WHPX, QEMU TCG, VirtualBox, bare metal: evidence survives warm reboot.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2633 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Shipped: `panic_evidence` record + `panic_collect_evidence`/`_restore`/`_write_blackbox` (`panic.c`/`.h`), lock-free `klog_panic_snapshot` (`klog.c`), fault-safe `boot_post_last_shadow` (`boot_init.c`); cross-boot page at `0x80000`.
> - Integration: collector hooked at the top of `panic_screen` (after `cli`); restore in `kernel_main` after `boot_phase0`; `X:\Crash\last-panic.txt` write + unexpected-shutdown notice at desktop-ready (Codex adoptions in commit message).
> - Tests: `test_boot_diag.c` (TEST_CAT_BOOT) -- crc32 canonical vector + restore magic/crc/version rejection + retain-until-consumed; run over a fixture page, never the live `0x80000` one; full suite 2633 kernel + 16 user-mode 0 failures; smoke PASS.
> - Canonical doc: [`docs/boot/black-box-artifacts.md`](../../docs/boot/black-box-artifacts.md) (`X:\Diag\*` + `X:\Crash\` artifact index).
> - Scope boundary: §5 is the warm-reboot evidence page; full minidump (MEMORY.DMP) generation is `02-kernel-core/TODO-27`; the `X:\Crash\` path is owned by TODO-24 §7.
> **Verified:** 2026-06-14 | ship `a59b8f64` (+ this review commit) | 7/9 items | build OK | smoke PASS + tests 2633
> **Accepted:** [H] `0x80000` is only kernel-reserved, not bootloader/reboot-reserved (firmware/BOOTX64 can clobber it pre-restore on bare metal) -> XREF: 01-boot-platform/TODO-14 §14 (item: "Pin the page with `AllocatePages(AllocateAddress, EfiLoaderData, 1)` as the FIRST firmware call `efi_main` makes" at line 458). (RESOLVED 2026-08-30 by §14 commit `41753ca67`: the page is pinned ahead of every firmware call, `ClearScreen` included. The original pointer named a §5 item that does not exist; §14 is where the work landed.)
> **Accepted:** [H] IXFS `vfs_flush` (C:\ fallback) does not `blkdev_sync`, so consume-after-flush can lose the retry copy (reason: FS-layer durability contract) -> XREF: 01-boot-platform/TODO-14 §12 (item: "last-panic.txt durability is parked on the IXFS owner: flush never reaches the device" at line 414). (RELOCATED 2026-08-30: the original pointer named a §5 item that does not exist; the live park now sits in §12 and carries the true owner, `05-storage-filesystems/TODO-06` §1 "**Flush must reach the DEVICE**".)
> **Quality reviewed:** 2026-06-14 | Codex 14x (design + test-coverage + adversarial + re-adversarial + adversarial-impl + consistency + perf) | 1C+6H+8M+1L fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 6. Panic QR Code *(deferred -- multi-version QR encoder needs segno module-diff + phone-scan validation, unavailable in the autonomous env; bootloader V3 encoder is the reuse seed)*
Embed a minimal QR code encoder and render a phone-scannable URL in the BSOD corner. §5 (the dependency) has shipped, but the encoder itself is gated on a validation capability boot-code-quality Gate 11 makes mandatory.

**Files:** `src/kernel/qr_encode.c`, `include/kernel/qr_encode.h`, `src/kernel/panic.c`

> [!IMPORTANT]
> **Validation blocker (recorded, not skipped).** boot-code-quality Gate 11 makes segno module-for-module comparison plus a real phone scan **non-negotiable** for any QR encoder change. Neither `segno` (the Python reference) nor a scanning phone is available in this autonomous environment, so a new encoder cannot be proven correct here. A **V3/ECL-L** segno-verified encoder already exists at `src/boot/uefi/bootx64.c` (`qr_encode_data` / `qr_reed_solomon` / `qr_place_*`, shipped under the error-screen QR feature). That encoder is the reuse seed, but it is **not drop-in**: V3/ECL-L holds ~53 byte-mode bytes (it encodes the short `...co/err/XXXX`), whereas the panic URL below runs ~120 chars and needs **Version 5/6 plus ECL-M**, which adds multi-block Reed-Solomon interleaving and per-version alignment-pattern tables. Ship §6 only on a host with `segno` plus a scanning phone (or bare metal for the on-screen scan).

- [/] operator-gated (segno + phone scan, Gate 11): Extend the segno-verified bootloader V3/ECL-L encoder into freestanding `src/kernel/qr_encode.c`
  - `qr_encode(text, matrix, &size)` for QR **version 3-6 at ECL-M** (deltas from the V3 seed in the callout above)
- [/] operator-gated (segno + phone scan, Gate 11): `panic_qr_url(buf, bufsize, message, post_code, os_version)`
  - format `https://docs.impossible-os.dev/panic?msg=<short>&post=0x{post}&v={ver}` (message truncated to 40 chars; not shipped ahead of the encoder)
- [/] operator-gated (segno + phone scan, Gate 11): In `kernel_panic()` BSOD renderer: after the main panic screen, call `qr_encode(url, matrix, &qr_size)`; render at bottom-right (12 px from corner), module = 4 px, white-on-black,
  - quiet zone = 4 modules
- [/] operator-gated (segno + phone scan, Gate 11): Ensure `qr_encode.c` is freestanding: no libc, no floating point; uses only `kernel/types.h` and `kernel/libc/string.h`
- [/] operator-gated (segno + phone scan, Gate 11): Validate before ship (Gate 11): diff each test URL against `segno.make(url, error='M', boost_error=False)` module-for-module (0 diffs), then phone-scan the rendered BSOD on bare metal
- [/] operator-gated (segno + phone scan, Gate 11): Commit: `"kernel: minimal QR encoder + panic BSOD QR code for phone-scannable troubleshooting"`

**Test checkpoint:** With §5+§6 shipped: forced panic shows scannable QR bottom-right; URL resolves to docs panic page; encoder matrix matches segno for every test URL. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** N/A (deferred -- no code shipped; QR encoder needs segno reference + phone-scan validation) | validation: segno module-diff + phone-scan on bare metal, pending
> **Notes:**
> - Deferred (no code shipped): the panic BSOD QR encoder + render -- needs a multi-version V5/6 ECL-M encoder for the ~120-char URL plus segno + phone-scan validation (Gate 11), unavailable autonomously.
> - Reuse seed: a segno-verified V3/ECL-L encoder exists in `src/boot/uefi/bootx64.c` (error-screen QR), LANDED 2026-05-02 by TODO-03 §14 commit `134702ae`; extend it to V5/6 + ECL-M when validation is available (see the section's IMPORTANT callout). The seed was never the blocker, so every §6 item is marked operator-gated.
> - Scope boundary: §6 owns the kernel panic QR; the bootloader error-screen QR encoder is owned by → XREF: `01-boot-platform/TODO-03-bootloader-error-recovery.md` §14.
> **Verified:** 2026-06-14 | deferred -- no code shipped | 0/6 items | build OK (no code change) | manual (feasibility analysis)
> **Deferred:** [M] panic BSOD QR encoder + render unimplemented (reason: needs V5/6 ECL-M encoder + segno reference + phone-scan validation per boot-code-quality Gate 11, unavailable autonomously) -> XREF: 01-boot-platform/TODO-03-bootloader-error-recovery.md §14 (item: "Standalone encoder ... not yet implemented" at line 513) (SEED LANDED 2026-05-02 by TODO-03 §14 commit `134702ae`: the segno-verified V3/ECL-L encoder exists in `bootx64.c`. It was never the blocker -- the remaining gate is validation capability alone, so every §6 item is now marked operator-gated.)

---

## 7. System-Wide Multi-Instance Spinner *(deferred -- desktop polish, single spinner works)*
Extend the existing single-instance `spinner.h` to support up to 8 simultaneous named spinner instances for use across the desktop.

**Files:** `include/kernel/spinner.h`, `src/kernel/spinner.c`

> [!NOTE]
> The existing `spinner_init/start/advance/stop` API covers the boot splash single spinner (shipped with §1--§3 above). This section adds a multi-instance layer without breaking boot splash.

- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): Define `spinner_t` struct: `int32_t cx, cy, radius, stroke; uint32_t color; int32_t angle, sweep; uint8_t active; uint8_t size_class`
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `spinner_create(uint8_t size_class, uint32_t color)`: allocate from a static pool of 8 `spinner_t` slots; size classes: `SPINNER_SMALL=16`, `SPINNER_MEDIUM=32`, `SPINNER_LARGE=48`,
  - `SPINNER_XLARGE=64` (px radius); returns `spinner_t*` or NULL if pool full
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `spinner_destroy(spinner_t *s)`: mark slot as inactive; stop animation
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `spinner_set_position(spinner_t *s, int32_t cx, int32_t cy)`: update position without restarting
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `spinner_tick(spinner_t *s)`: advance angle + breathing (port animation logic from existing `spinner_advance()` -- single instance → multi); call `spinner_render(s, cx, cy)`
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `spinner_render(spinner_t *s, struct fb_surface *surface, int32_t x, int32_t y)`: draw arc ring onto `surface` (compositor surface or direct framebuffer)
- [/] blocked on 08-graphics-ui/TODO-08 §8 (compositor tick): Compositor integration: WM maintains a `spinner_t *g_active_spinners[8]` list; compositor loop calls `spinner_tick()` on each non-NULL entry per frame; used by loading dialogs,
  - Start Menu search, download progress, Service Manager
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): Backward compatibility: existing `spinner_init/start/advance/stop` calls remain valid; they operate on `g_active_spinners[0]` (the boot splash slot)
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): Commit: `"kernel: multi-instance spinner_t pool for compositor-integrated loading indicators"`

**Test checkpoint:** With §7 shipped: pool of 8; compositor ticks each active `spinner_t` per frame per → XREF: `08-graphics-ui/TODO-08-window-manager.md` §8; boot splash still uses slot 0. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** N/A (deferred -- no code shipped; desktop-polish blocked on WM compositor integration) | validation: on-screen multi-spinner under the WM compositor loop, pending
> **Notes:**
> - Deferred (no code shipped): the multi-instance `spinner_t` pool + compositor integration. Single boot-splash spinner works; this is desktop polish (spinners across loading dialogs, Start Menu, etc.) needing WM compositor surfaces.
> - Blocker (WM half): full delivery depends on the WM maintaining `g_active_spinners[]` and calling `spinner_tick()` per frame, owned cross-domain by → XREF: `08-graphics-ui/TODO-08-window-manager.md` §8 (item: "Per-frame `spinner_tick()` over the active-spinner list" at line 197), still `[ ]` on 2026-09-03.
> - Blocker (kernel half, binds first): the `spinner_t` pool and renderer are kernel `.text`, and the image has 47 bytes of slack below `USER_BASE` measured 2026-09-03 -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7.
> - Scope boundary: §7 owns the kernel `spinner_t` pool/render; the per-frame compositor tick wiring is owned by the WM TODO.
> **Verified:** 2026-06-14 | deferred -- no code shipped | 0/9 items | build OK (no code change) | manual (dependency analysis)
> **Deferred:** [M] multi-instance spinner_t pool + compositor integration unimplemented (reason: desktop-polish; single boot spinner works; full delivery needs WM per-frame spinner_tick wiring) -> XREF: 08-graphics-ui/TODO-08-window-manager.md §8 (item: "Per-frame `spinner_tick()` over the active-spinner list" at line 197) (STILL OPEN 2026-09-03: TODO-08 §8 is `[ ]`. A second gate now binds first -- the kernel-image ceiling leaves 47 bytes of `.text`, so the pool/render items are parked on -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7 (item: "Remove the `scripts/build.sh` BSS-collision guard" at line 254), and only the compositor-tick item is WM-owned.)

---

## 8. Runtime Vital Signs Strip *(deferred -- developer tool, needs scheduler stats first)*
An always-visible 20 px overlay strip at the bottom of the desktop showing live system metrics for developers.

**Files:** `src/desktop/vital_signs.c`, `include/desktop/vital_signs.h`

- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): Activate when `boot.conf` key `VitalSigns=1` or `HKLM\SYSTEM\Boot\VitalSigns` = 1 (→ XREF `02-kernel-core/TODO-14-registry-completion.md`)
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `vital_signs_init()`: called from desktop init; allocates a 20 px compositor overlay surface pinned to the bottom of the screen
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `vital_signs_tick()`: called every 500 ms from a PIT-driven callback; reads: CPU usage % from scheduler stats, RAM used/total from PMM, IRQ count/s from IRQ counter differential,
  - uptime in seconds from PIT ticks, framerate from compositor frame counter
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): Render format (FONT_MONO at 10 px, white on 50% transparent black): `[CPU: 23%] [RAM: 1.2/4.0 GB] [IRQ: 1234/s] [Uptime: 00:03:42] [FPS: 60]`
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): `VitalSignsExtended=1` adds second line: `[Free: 2847 pages] [TCP: 3] [VFS R: 1.2 MB/s W: 0.4 MB/s] [Temp: 62°C]` (CPU temp from ACPI thermal zone if available, 0 if not)
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): The overlay is always rendered above the desktop wallpaper and windows; zorder = top - 1 (below cursor, above everything else)
- [/] blocked on 02-kernel-core/TODO-33 §7 (image ceiling): Commit: `"desktop: runtime vital signs strip -- CPU/RAM/IRQ/FPS overlay for developers"`

**Test checkpoint:** With §8 shipped: `VitalSigns=1` shows bottom strip updating ~500 ms; CPU, RAM, IRQ, uptime, FPS plausible. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** N/A (deferred -- no code shipped; blocked on scheduler CPU% accounting) | validation: on-screen overlay with live metrics, pending
> **Notes:**
> - Deferred (no code shipped): the vital-signs overlay strip (CPU/RAM/IRQ/uptime/FPS). The CPU% source SHIPPED 2026-07-20 (`task_acct_sample()`, TODO-25 §7 commit `4b541c54`), so the original blocker is gone; the section is now parked on the kernel-image ceiling, measured 2026-09-03 at 47 bytes of `.text`.
> - Blocker (current): kernel-image size -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7. A whole new desktop overlay translation unit cannot link into 47 bytes of `.text` slack. The former blocker (per-process CPU-time accounting, TODO-25 §7) is CLOSED.
> - Scope boundary: §8 owns the desktop overlay (`src/desktop/vital_signs.c`); the scheduler CPU-time accounting source is owned by TODO-25 §7.
> **Verified:** 2026-06-14 | deferred -- no code shipped | 0/7 items | build OK (no code change) | manual (dependency analysis)
> **Deferred:** [M] runtime vital-signs strip unimplemented (reason: needs scheduler per-CPU CPU% accounting) -> XREF: 02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §7 (item: "Track per-process and per-job CPU time with user/kernel split") (BLOCKER RESOLVED 2026-07-20 by 02-kernel-core/TODO-25 §7 commit `4b541c54`: `task_acct_sample()` publishes per-process user/kernel CPU time under one timestamp, which is the CPU% source this section named. §8 is NOT re-opened: it is re-owned to the kernel-image ceiling, MEASURED 2026-09-03 at 47 bytes of `.text` against a new desktop overlay -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7 (item: "Remove the `scripts/build.sh` BSS-collision guard" at line 254).)

---

## 9. Boot Timeline Visualization and Import Parity *(deferred)*
Linux `systemd-analyze plot` and Windows performance tooling expose boot as a human-readable timeline. This repo already emits machine-readable `boot-timeline.json` from `boot_timeline_dump_json()`; viewer parity is still open.

**Files:** `docs/` (schema + tooling notes), optional `scripts/` or `user/` offline converter

- [x] Published v1 wire format `docs/boot/boot-timeline-schema.md` (8 fields, FPDT/TSC sources, anchoring + unreliable semantics, emit guards); linked from `docs/boot/black-box-artifacts.md`
- [/] Converters MUST preserve `target_ms`, `unreliable`, and §18 critical-chain/cause-class fields (→ XREF: [`TODO-29 §18`](TODO-29-boot-perf-health-observability.md) owns attribution; §9 owns the visual).
- [x] Commit: `"docs: boot timeline JSON schema (v1 wire format) -- docs/boot/boot-timeline-schema.md"`

**Test checkpoint:** With §9 shipped: JSON from a real boot validates against the schema; SVG or Chrome trace opens in target viewer without manual edits. The v1 schema doc is shipped; converters (items 2-4) remain open. QEMU WHPX + TCG smoke.
> **Test runner:** N/A (docs-only -- schema documents the existing `boot_timeline_dump_json()` emitter) | validation: Codex consistency review of doc-vs-emitter, 2 medium drifts fixed
> **Notes:**
> - Shipped: `docs/boot/boot-timeline-schema.md` -- v1 wire format for `boot-timeline.json` (8-field record table, FPDT-prepend + TSC ordering, anchoring + `unreliable` reliability semantics, emit guards, 16 KiB buffer behavior).
> - Structure / consumers: under `docs/boot/` with the `*-schema.md` siblings; linked from the BlackBox artifact catalog `docs/boot/black-box-artifacts.md`. Draft's root path + CLAUDE.md link corrected (detail in commit msg).
> - Codex consistency review (7 rounds to convergence) fixed 7M+1L drifts across the emitter contract, schema doc, header comment, and artifact catalog (post width, duration source-locality, 4-outcome write-failure taxonomy, stage truncation, X:\Perf path); per-finding detail in the commit messages.
> - Scope boundary: §9 owns the published schema; SVG / `chrome://tracing` converters (items 2-4) stay open; `target_ms`/`unreliable` preservation is consumed by → XREF: `01-boot-platform/TODO-29`.
> **Verified:** 2026-06-14 | ship `24d41523` (+ this review commit) | 1/4 items | build OK | Codex 7x consistency: emitter+doc+header+catalog aligned
> **Quality reviewed:** 2026-06-14 | Codex 7x (consistency) | 0H+7M+1L fixed, 0 open | scope: N/A (docs-only schema; adversarial/perf inapplicable to markdown)

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
- [x] Test `test_boot_load_status` (`test_boot_diag.c`, TEST_CAT_BOOT): mix + format + summary + all-LOADED + begin/finish + overflow + fail-closed + 64/65 boundary + pool-full token + name truncation; save/restore seam.
- [x] Commit: `"diag: per-subsystem boot load/status log -> X:\Diag\boot-load-status.txt (ntbtlog parity)"`

**Test checkpoint:** On a boot with an absent/failing device (e.g. no AHCI), `X:\Diag\boot-load-status.txt` lists that subsystem as FAILED/DEGRADED with an error code; a clean boot lists every core subsystem LOADED; serial shows the degraded summary only when something failed. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.
> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2605 kernel + 16 user-mode, 0 failures
> **Notes:**
> - Shipped: `boot_load_status.{c,h}` -- 64-entry lock-free pool, SMP-safe `boot_load_record`/`begin`/`finish`, pure `format` + `degraded_summary`, `dump_to_blackbox` (`X:\Diag\boot-load-status.txt`), `report_summary` (serial + splash).
> - Integration: storage probe block + `deferred_net_init`/`net_init` record real outcomes; report + dump both run after `boot_run_deferred` so the record is complete; serial klog is authoritative (Codex adoptions in commit message).
> - Tests: `test_boot_diag.c` (TEST_CAT_BOOT) -- 44 asserts; full suite 2605 kernel + 16 user-mode 0 failures; smoke PASS shows `[BOOT-LOAD] all subsystems loaded clean` (NIC absent -> SKIPPED) + dump.
> - Canonical doc: [`docs/boot/black-box-artifacts.md`](../../docs/boot/black-box-artifacts.md) (`X:\Diag\*` artifact index).
> - Scope boundary: §11 ships infra + storage/network wiring. Granular per-driver coverage, probe-result aggregation and NVMe per-controller status are NOT open here -- all seven §11 items are `[x]`; those three were moved verbatim to §12, where they are parked on the kernel-image ceiling.
> **Verified:** 2026-06-14 | ship `657637ec` (+ this review commit) | 7/7 items | build OK | smoke PASS + tests 2605/2605 (count corrected 2026-09-03: the stamp read 6/9 against the pre-split body; the three unshipped items became §12)
> **Accepted:** [H] sequential storage publishes LOADED on probe failure + async marks AHCI/VirtIO absence DEGRADED (reason: needs driver absent-vs-failed split) -> XREF: 01-boot-platform/TODO-14 §11 (item: "Probe-result aggregation (storage + network)" at line 381)
> **Accepted:** [M] NVMe partial multi-controller failure reads BOOT_OK (reason: nvme_init exposes only the success count) -> XREF: 01-boot-platform/TODO-14 §11 (item: "NVMe per-controller status" at line 382)
> **Quality reviewed:** 2026-06-14 | Codex 8x (design + test-coverage + adversarial + re-adversarial + consistency + perf) | 3H+8M+2L fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## 12. Boot Load Status Granularity (per-driver records, probe aggregation, NVMe partial init)

> **Spawned-by:** root

> **Deferred:** 2026-08-30 | kernel-image ceiling. The implementation is COMPLETE and design-reviewed; it is parked on size alone, not on any missing capability -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard").
>
> MEASURED 2026-08-30: the change is **+1,904 bytes of `.text`** (+160 `.rodata`, +192 `.bss`) against **79 bytes** of `.text` headroom -- `.text` ends at `0x427FB1` and its page boundary is `0x428000`. Crossing it cascades `.rodata`, `.data` and `.bss` each up one 4 KiB page and lands `__kernel_end` on exactly `0x800000`, which `scripts/build.sh` refuses. HEAD builds green at `__kernel_end == 0x7ff000`, so the tree admits 79 bytes of new kernel code and this section needs 24x that.
>
> The diff is preserved at `.claude/state/deferred-todo14-s12.patch` (gitignored: survives a rollover, NOT a fresh clone). It applies to `include/kernel/boot_probe.h` (new), `boot_load_status.{h,c}`, `boot_storage.c`, `boot_interrupts.c`, and the ata/ahci/nvme/rtl8139/virtio-blk/virtio-input/vbox-mouse/mouse/xhci_dev/framebuffer drivers.
>
> Design settled, so the redo is apply-then-re-verify rather than re-design. Four findings were raised by the pre-implementation Codex design review, all verified at source and all fixed in the preserved diff:
>
> - **A worker must not own a `boot_load_status` entry.** `boot_load_begin` release-publishes ATTEMPTED immediately (`src/kernel/main/boot_load_status.c:85`) and `boot_load_finish` mutates that published entry in place, so a barrier-overrunning worker finishing late would overwrite a terminal verdict the BSP had already written. The BSP claims all four storage entries before dispatch and writes every one itself; workers publish only into their own `storage_probe_slot`, storing `done` last with a release. A slot that never publishes is recorded FAILED, not SKIPPED.
> - **`nvme_init`'s count is not a usable-controller count.** `num_controllers++` at `src/kernel/drivers/nvme.c:642` runs BEFORE `nvme_identify` (`:648`) and `nvme_create_io_queues` (`:652`, whose return was discarded), so a controller with no addressable namespace still counted. The diff adds attempted/usable counters where usable requires a valid namespace AND live I/O queues.
> - **USB HID absent-vs-failed cannot be read from `is_hid`.** That flag is set on the last line of a successful probe (`src/kernel/drivers/xhci_dev.c:1751`), after several present-but-failed `return -1` exits (`:1621`, `:1630`). The diff counts attempts once the descriptor walk confirms a HID boot interface, and successes at the tail.
> - **The aggregate storage entry must stay.** It is the only carrier of async dispatch/barrier/recovery health (`src/kernel/main/boot_storage.c:528-535`): a barrier timeout whose worker finishes late leaves every per-driver entry LOADED and no unsafe bit set. It is retained as `storage-group` beside the per-driver records.
>
> One behavior change rides along and is deliberate: with ABSENT and FAILED separated, an absent AHCI or virtio-blk controller no longer returns `BOOT_DEGRADED` from its async wrapper, so an NVMe-only machine stops logging a spurious "Async storage init degraded" on every clean boot.

`boot_load_record` records ONE aggregate `storage` entry and cannot tell an absent device from a failed one, so the `ntbtlog` parity shipped by section 11 is shape-only: a machine that booted with a dead AHCI controller and a live NVMe logs the same line as a machine with neither. This section makes each probe self-report. The items below were moved VERBATIM from the stamped section 11, where the triage oracle could never reach them (cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3).

**Files:** `src/kernel/boot_load_status.c`, `include/kernel/boot_load_status.h`, the storage/input/ACPI/GFX probe call sites

- [/] Granular per-driver `boot_load_record`, blocked on the kernel-image ceiling with the code written
  - Storage (ata/ahci/nvme/virtio-blk), input (PS/2 keyboard, PS/2 mouse, USB HID, virtio-input, vbox-mouse), ACPI and GFX each self-report, instead of one aggregate `storage` entry plus one aggregate `input` entry and nothing at all for ACPI or GFX.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Probe-result aggregation (storage + network): absent must be distinguishable from failed, same blocker
  - `rtl8139`, `ahci` and `virtio-blk` each return `-1` for both "no such device" and "device present but init failed", and the sequential storage path always records LOADED, so SKIPPED and FAILED are not separable today.
  - The preserved diff answers it with a shared `enum boot_probe_result` whose ABSENT keeps the historical `-1`, so no existing `rc != 0` or `rc < 0` caller changes behavior and FAILED is a new value.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] NVMe per-controller status: report attempted-vs-usable, not the enabled count, same blocker
  - `nvme_init` returns the count of controllers that reached ENABLED, and that counter is incremented before Identify and I/O-queue setup run, so a partial multi-controller failure records BOOT_OK today.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] last-panic.txt durability is parked on the IXFS owner: flush never reaches the device -> XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §1` (item: "**Flush must reach the DEVICE**")
  - Confirmed at source 2026-08-30: `ixfs_cache_flush` ([`src/kernel/fs/ixfs/ixfs_core.c:97`](../../src/kernel/fs/ixfs/ixfs_core.c)) writes dirty blocks with `blkdev_write` and returns without `blkdev_sync`, so the `flushed == 0` gate in `panic.c` retires the `0x80000` evidence page on an FS-cache write rather than a durable one.
  - The owner item already exists and already carries the reciprocal park; nothing in this file can fix it without changing IXFS flush semantics for every caller.

**Test checkpoint:** a boot with one failing and one working storage controller records the failing one FAILED and the working one LOADED, not one aggregate LOADED; an absent controller records SKIPPED, never FAILED.

---

## 13. klog Format-Width Contract, Compiler-Checked

> **Spawned-by:** root

> **Deferred:** 2026-08-30 | kernel-image ceiling, MEASURED, plus a sequencing correction recorded below -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard").
>
> The engine change alone does not fit. A minimal probe of `vformat_buf` -- track whether `l` was seen, and read a bare `%d`/`%u`/`%x` as 32-bit -- measured **+112 bytes of `.text`** against **79 bytes** of headroom (`.text` ends at `0x427FB1`, page boundary `0x428000`), landing `__kernel_end` on `0x800000`. The probe was reverted and the tree left green. That is the whole section's first line of work, so nothing after it is reachable either.
>
> Surface, measured the same day so the redo does not re-count it: **2,731 `%u`, 709 `%x`, 291 `%d`, 249 `%X`** conversions and **7,119 `(uint64_t)` casts** across `src/kernel`. That is an order of magnitude past the "73 `%d` call sites" this section was written against.
>
> **Sequencing correction, and it is the useful part of this deferral.** The section requires every call site to convert in the SAME commit as the engine change, on the grounds that the intermediate state is silently wrong. That is right, and it makes the change a ~3,700-site big bang -- but it also names its own tool: `-Wformat` is the only thing that can pair a conversion with its argument, and the attribute that enables it can only be added once the semantics are standard. So the order is not "audit every site, then flip"; it is flip the engine, add the attribute, and then let the compiler ENUMERATE every mismatch, fixing until the build is clean. The migration is mechanical that way and a hand audit of 3,700 sites is not.
>
> A two-phase split was considered and REJECTED for the same reason. Phase A (teach the engine `l`, leave bare conversions 64-bit, convert sites `%u` -> `%lu` in batches) is behaviorally a no-op at every step and costs no `.text`, which is exactly why it is attractive under this ceiling. But `-Wformat` is still off during all of it, so each batch is an unverifiable hand pairing, and a wrong one stays invisible until the phase-B flip -- which is the same silent-wrongness the atomic requirement exists to prevent, just spread over more commits. Do it in one commit, with the compiler checking.

`vformat_buf` reads EVERY numeric conversion as a full 64-bit vararg (`src/kernel/klog.c:177`, `:193`, `:204`), so a caller passing a 32-bit value is relying on luck: the compiler usually zero-extends into a register slot and it prints correctly, but a value that lands in a STACK slot (roughly the 4th vararg onward) leaves the upper 4 bytes uninitialized. Found live, not projected -- the test runner's failing-assertion line passed `int line` as its 7th argument and rendered `test_harness.c:-194693637781585198`. That ONE site is fixed at its source (the record is now composed without varargs); this section closes the CLASS -> XREF: `00-infrastructure/TODO-03 §10` (item: "A failing `TEST_ASSERT` whose composed klog line exceeds the 256-byte message buffer wedges the boot").

- [/] Move `%d`/`%u`/`%x` to standard C width semantics in `vformat_buf`, blocked by the kernel-image ceiling
  - 32-bit by default, 64-bit only under `l`/`ll`. `vformat_buf` (`src/kernel/klog.c:99`) today SKIPS length modifiers outright at `klog.c:171`.
  - Measured 2026-08-30: +112 bytes of `.text` against 79 available.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
  - The migration is the work, not the parse change: every call site passing a deliberate `(uint64_t)` cast against a bare `%u` starts truncating silently, and addresses printed with `%x` are the common case.
  - Sites must be converted in the SAME commit as the engine change, never after it, because the intermediate state is silently wrong rather than broken.
  - Measured surface as of 2026-08-14: 73 `klog(...)` call sites use `%d`; the `%u`/`%x` population is not yet counted. Three VERIFIED-corrupting sites were cast at source when found (`src/kernel/image.c`, `src/kernel/symtab.c` x2) -- each passed a negative `int` error code, which the 64-bit read renders as a huge positive; they are evidence the class is live, not a substitute for the sweep.
- [/] Add `__attribute__((format(printf, 3, 4)))` to `klog`/`klog_unrated`/`klog_receipted` once the semantics are standard, so `-Wformat` checks every call site and the class cannot come back. This is the Linux `printk` model.
- [/] Delete the hand-written width warning from `include/kernel/klog.h` once the compiler enforces it -- a documented contract the compiler could check instead is a contract that drifts.
- [/] Commit: `"klog: standard printf width semantics with -Wformat on every call site"` -- lands with the engine flip and the attribute in ONE commit, once the ceiling is gone

**Test checkpoint:** a `klog` call with eight 32-bit arguments renders all eight correctly (the stack-slot case that fails today); a build with a deliberately mismatched conversion FAILS with `-Wformat`; the full suite stays green and no existing log line changes shape.

---

## 14. Bootloader Reservation of the Panic-Evidence Page (`0x80000`)

> **Spawned-by:** §12 (split)

Section 5 captures `struct panic_evidence` at physical `0x80000` and the next boot restores it, but nothing reserves that page before the kernel runs: the PMM only protects it from kernel entry onward, so UEFI firmware or BOOTX64's own allocations can legally hand the page out and overwrite the record between the reset and the restore. Moved verbatim from the stamped section 5, where it was unreachable.

**Files:** [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)

- [x] Pin the page with `AllocatePages(AllocateAddress, EfiLoaderData, 1)` as the FIRST firmware call `efi_main` makes, ahead of every allocation ([`src/boot/uefi/bootx64.c:16269`](../../src/boot/uefi/bootx64.c))
  - Placed ahead of `ClearScreen` too, not merely ahead of the first `AllocatePool`: a firmware protocol implementation may allocate internally, so the only safe position is before every firmware call the image issues.
  - `EfiLoaderData`, matching the `boot_info` pin beside it. UEFI reserves `EfiReservedMemoryType` for firmware and forbids a loader from allocating it; `EfiACPIMemoryNVS` would misdescribe loader-owned evidence as ACPI state. Reclaimability costs nothing: the page is inside the first MiB the PMM reserves wholesale regardless of type.
  - The pin claims the page; it does NOT preserve what is in it. UEFI guarantees allocation and memory-map reclassification, not that the prior bytes survive, so a conforming allocator could scrub the page and still return `EFI_SUCCESS`. That is the same non-contractual platform behaviour the whole cross-boot record already rests on, and it fails safe: the record is magic- and CRC-validated on restore ([`src/kernel/panic.c:2293`](../../src/kernel/panic.c)), so a scrubbed page reads as "no record", never as a false one.
  - NOT fatal on failure, unlike the `boot_info` pin: reported on serial and the boot continues.
- [x] Give the address exactly ONE definition, `PANIC_EVIDENCE_PHYS_ADDR`, and derive the kernel's spelling from it
  - It lives in [`include/kernel/boot_version_constants.h`](../../include/kernel/boot_version_constants.h), the UEFI-safe header the bootloader can include; [`include/kernel/panic.h`](../../include/kernel/panic.h) now defines `PANIC_EVIDENCE_ADDR` as a cast of it rather than a second literal.
  - An equality `_Static_assert` between two literals was the first shape tried and the consistency review was right that it is weaker: it lets two values exist and only notices divergence. Deriving makes divergence unrepresentable, and the cast preserves the pre-existing `uint32` type so every cast site in `src/kernel/panic.c` is unchanged.
  - The bootloader cannot include `panic.h` (kernel-only types), so a loader-side literal would have been a second uncoordinated copy: the loader could pin one page while the kernel restored from another, both halves building green.
- [x] `POST16_BL_PANIC_PAGE` (`0xB002`) classified REQUIRED in [`tools/post16-manifest/generate.sh`](../../tools/post16-manifest/generate.sh), emitted on the SUCCESS path only so the smoke gate asserts the page was pinned
  - Emitted unconditionally it would prove only that the code RAN, and the gate would stay green on a boot that lost the page. Raised by the section 14 consistency review.
  - The pin itself leaves the 8-bit `POST_PANIC_PAGE` (`0x0A`) on port 0x80 instead. `post_code16` also prints through `serial_early_print`, and this loader does not zero `.bss`, so `s_serial_port` there holds firmware poison (`0xAF...`) which is NON-ZERO and passes `serial_early_putchar`'s guard straight into `inb`/`outb` on an arbitrary I/O port. Raised by the section 14 performance review, confirmed at [`src/boot/uefi/bootx64.c:138`](../../src/boot/uefi/bootx64.c) and [`:789`](../../src/boot/uefi/bootx64.c).
- [x] Commit: `"boot: reserve the panic-evidence page at 0x80000 before other allocations"`

**Test checkpoint:** the 4-leg smoke matrix boots green and the serial log carries `[BOOT] Panic evidence: 0x00080000 pinned`; the smoke test's required-code layer fails if `POST16_BL_PANIC_PAGE` is absent, and because that code is emitted only on the success path the gate fails on a boot where the page was NOT pinned, not merely on one where the code stopped running.

**Note:** No kernel test surface -- the pin is pure UEFI code running before the kernel exists, and the one kernel-side artifact is a `_Static_assert`, which is its own gate. A unit test asserting `PANIC_EVIDENCE_ADDR == 0x80000` would be the tautological-constant test the test policy forbids. Validation is the required POST16 code plus the serial line, both asserted by the 4-leg smoke matrix.

> **Test runner:** `bash scripts/test-smoke-matrix.sh` -- 4/4 legs (kvm/tcg x 1/2 cpu) must pass; the POST16 required-code layer covers this section.

> **Notes:**
> - Shipped: `efi_main` pins `PANIC_EVIDENCE_PHYS_ADDR` with `AllocateAddress` before any other firmware call, leaves `POST_PANIC_PAGE` on port 0x80 there, and reports the outcome on serial once `serial_early_init` has run, emitting `POST16_BL_PANIC_PAGE` on success.
> - Integrates by closing the window the PMM cannot cover: the blanket first-MiB reservation protects the page from kernel entry onward, and this pin protects it from the reset to that point.
> - Downstream: `PANIC_EVIDENCE_PHYS_ADDR` is now the single definition of the address, so a change on either side fails the build instead of silently splitting loader and kernel.
> - Canonical doc: the pin's placement rationale lives with the code at `src/boot/uefi/bootx64.c`; the shared constant's rationale lives in `include/kernel/boot_version_constants.h`.
> - Scope boundary: this section pins the page. Recording the outcome in the handoff is section 19, stopping the kernel's own Phase-0 writer from overwriting the record is section 17, and the pre-serial UART hazard the review surfaced is section 18.

> **Verified:** 2026-08-30 | commit `41753ca67` | 4/4 items | build OK | smoke-matrix 4/4 (kvm+tcg x 1+2 cpu) | 32845 kernel + 17 user tests | WHPX boots to shell
> **Deferred:** [H] A Phase-0 panic overwrites the prior record before `panic_evidence_restore_early()` runs -> XREF: 01-boot-platform/TODO-14-boot-diagnostics.md §17 (item: "Capture and validate the evidence page at the very start of `kernel_main`, before `boot_phase0()` can panic, using a helper that neither logs nor allocates" at line 565)
> **Deferred:** [H] `boot_set_section()` reaches `serial_early_print` before `serial_early_init`, driving a poisoned `.bss` UART port -> XREF: 01-boot-platform/TODO-14-boot-diagnostics.md §18 (item: "Gate the early-serial path on an explicit \"serial is configured\" flag rather than on `s_serial_port` being non-zero" at line 596). (RESOLVED 2026-09-03 by §18 commit `8cf0a2f33`: the early path now gates on the wide `EARLY_DIAG_READY` cookie, `src/boot/uefi/bootx64.c:913`.)
> **Deferred:** [H] The kernel writes the evidence page even when the pin failed, because the outcome never reaches it (reason: needs a boot_info field the run may not land) -> XREF: 01-boot-platform/TODO-14-boot-diagnostics.md §19 (item: "Gate the kernel's access to the evidence page on the decoded state, so a boot whose pin failed does not write a page firmware may own" at line 648)
> **Quality reviewed:** 2026-08-30 | Codex 7x (design, adversarial, re-adversarial, consistency, perf) | 3H+6M+0L fixed, 0 open | scope: boot-code-quality

---

## 15. Anti-Rollback Terminal Give-Up: Durable Record

> **Spawned-by:** §12 (split)
> **Deferred:** 2026-08-30 | kernel-image ceiling, MEASURED. The implementation is written and design-reviewed; it is parked on size alone -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard").

`boot_rollback_report_terminal()` ([`src/kernel/main/boot_rollback.c:433`](../../src/kernel/main/boot_rollback.c)) emits one `klog_unrated(LOG_ERROR, ...)` naming the final firmware status when the anti-rollback floor write exhausts its bounded retries, and that line lives solely in the serial log of the boot that produced it. -> XREF: `01-boot-platform/TODO-01-boot-protocol-abi-handoff.md §25` (item: "Report exhaustion once, at `LOG_ERROR`, naming the last firmware status" at line 949)

**User impact:** an operator enables `anti_rollback_raise` to retire a vulnerable image on an unattended or headless box, the write exhausts its retries, and nothing survives the boot to say the policy never took effect. Windows records the equivalent SVN-update outcome durably as Event ID 1042.

**Files:** [`src/kernel/main/boot_rollback.c`](../../src/kernel/main/boot_rollback.c), the boot-decision Registry record, the BlackBox transcript writer

**Measured blocker (2026-08-30).** The image has **1,323 bytes** of growth left: baseline `.bss` runs to `0x7FEAD5` (`llvm-readelf -S build/kernel.exe`) and `scripts/build.sh` refuses once the page-aligned end reaches the user base at `0x800000`. The trimmed implementation costs **4,208 bytes** (+4,144 text, +64 bss), so it overruns by 2,885. Two smaller shapes were considered and rejected: registry-only fits but is RAM-only (hive persistence has no production caller, so `registry_flush()` early-returns), which does not survive the boot and therefore does not answer the user impact at all; BlackBox-only is still ~2 KiB and would spend the remaining headroom on half the section.

- [/] Write the terminal give-up (attempt count plus final firmware status) into the boot-decision Registry record -- code written, blocked on the kernel-image ceiling
  - Values under the existing `HKLM\SYSTEM\Boot\Decision` key: `AntiRollbackAttempts`, `AntiRollbackTargetVersion`, `AntiRollbackShippedVersion`, `AntiRollbackRequiredVersion`, `AntiRollbackGiveUpUptimeMs`, `AntiRollbackStatus` (QWORD), `AntiRollbackReason`, and `AntiRollbackGiveUp` written LAST as the completion marker.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Mirror the same record into the BlackBox transcript, so a headless box keeps it without a mounted registry -- same blocker
  - `X:\Diag\boot-antirollback.txt`, single create+trunc, checked full-length write, stamped with the RTC wall clock and the loader build label so a file found on a LATER boot is attributable to the boot that wrote it.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Reuse the record shape TODO-01 already ships for stale-loader mismatch and boot provenance rather than inventing a third one -- same blocker
  - Shapes reused: the boot-decision Registry key (`boot_decision_populate_registry`, `src/kernel/main/boot_hw.c:1039`) and the single create+trunc transcript (`boot_loader_identity_dump_to_blackbox`, `src/kernel/main/boot_version.c:732`).
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Commit: `"boot: durable record for the anti-rollback terminal give-up"` -- lands with the two sinks in ONE commit, once the ceiling is gone

**Design settled 2026-08-30 (Codex design review received; do not re-derive).** The give-up path only LATCHES; it performs no I/O. `boot_rollback_report_terminal()` can run on the sys_wq worker, the kworker retry tick, or the compositor loop in the sys_wq-unavailable fallback, and `registry.c` is lock-free pending the registry-wide SMP synchronization owned by `02-kernel-core/TODO-14-registry-completion.md §14`, so a mutation from any of those could race the compositor's own `registry_flush()`. Publishing therefore runs from the compositor loop's periodic-maintenance block (`src/kernel/main/compositor.c:517`), next to `registry_flush()` and `ahci_flush_error_counters()` -- the same thread, so no self-race, and a failed sink is retried for free on the next iteration under a bounded cap. Each sink carries its OWN completion flag set only after a checked success. Under `KERNEL_TESTS`, `boot_rollback_reset_for_test()` turns publishing OFF so the six existing terminal tests cannot stamp a false anti-rollback failure into the registry and `X:\Diag` of every real test boot; the one test that proves the record re-enables it and cleans up. The working implementation is preserved at `.claude/state/deferred-todo14-s15.diff`.

**Test checkpoint:** a forced-exhaustion unit test leaves a Registry record and a BlackBox line naming the final firmware status; a successful floor write leaves neither.

---

## 16. Boot Timeline Export Formats (SVG and Chrome Trace)

> **Spawned-by:** §12 (split)
> **Verified:** 2026-08-30 | commit `e919cafc3` | 3/4 items | build OK | 32845 kernel + 17 user tests | lint 0 errors | renderer 30 self-check + 71 test assertions
> **Deferred:** [M] The renderer's parity gate binds the emitter's record SCHEMA, not the values it computes, and nothing else covers those either -> XREF: `01-boot-platform/TODO-14-boot-diagnostics.md §2` (item: "`boot_timeline_dump_json()`: writes unified FPDT + TSC step timeline as JSON to `boot-timeline.json`" at line 119)
> **Quality reviewed:** 2026-08-30 | Codex 19x (design + 9 adversarial + 9 consistency + perf) | 0H+13M+0L fixed, 1M rejected on scope with the premise stated | scope: host tool, no kernel surface

Section 9 ships `boot_timeline_dump_json()` and the `boot-timeline.json` artifact under BlackBox `X:\Perf\`, but neither consumer-facing rendering that makes the artifact readable by a human shipped with it. Both items were moved verbatim from the stamped section 9. The data already exists, so neither item changes the boot path.

**Files:** [`tools/boot-timeline/boot_timeline.py`](../../tools/boot-timeline/boot_timeline.py), [`tools/boot-timeline/test_boot_timeline.py`](../../tools/boot-timeline/test_boot_timeline.py), wired into [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh)

- [x] Offline converter producing a Gantt-style SVG comparable to `systemd-analyze plot` output
  - Host-side, as the item preferred. That is decisive here rather than merely tidy: measured 2026-08-30, the image has 1,323 bytes of headroom before the `0x800000` user base, and §15 was deferred the same day for wanting 4,208.
  - Axis policy, pinned because the wrong version of it shipped once in review: ONLY the `0xFFFFFFFF` saturation sentinel in `start_ms` or `duration_ms` leaves the axis. Every other record contributes, `unreliable` included -- that flag is advisory per the schema, and the emitter makes `tsc_unreliable` sticky and sets it for whole timelines under fallback anchoring, so excluding it collapsed a reliable-FPDT-plus-unreliable-TSC boot to a 1,000 ms axis with both kernel stages pinned at the edge.
  - The sentinel exclusion earns its place separately: one saturated record was measured to make the axis 49 days wide and squash every honest bar to a single pixel. Off-scale is decided on a record's END, so a saturated duration with an in-range start is still caught, and such records keep a row, a marker, and their true numbers in the tooltip and in the trace `args`.
- [x] Chrome trace-event JSON export for `chrome://tracing` import, which neither Windows nor `systemd-analyze` offers
  - Complete (`X`) events on per-phase lanes with firmware FPDT records on their own lane, timestamps scaled to microseconds as the format defines. The scale is asserted directly because getting it wrong renders a 20-second boot as 20 milliseconds and looks entirely plausible.
- [/] Bind the emitter's COMPUTED values to expected outputs -- the renderer's parity gate deliberately does not cover this, and nothing else does either
  - `tools/boot-timeline/test_boot_timeline.py` proves WHICH value reaches which field. It cannot prove the kernel computes `start_ms`, `dur_ms` or `tsc_unreliable` correctly, and a Codex round-5 finding recommending an AST/dataflow check was rejected because it would turn a host renderer's test into a C dataflow analyzer.
  - Measured gap: `src/kernel/test/test_boot_timing.c` covers `boot_timing_fpdt_unreliable_eval` only. The TSC anchor and the `tsc_unreliable` ladder inside `boot_timeline_dump_json()` have no test, so a regression assigning the wrong value there is undetected on both sides.
  - Owner: §2, which ships the emitter -- NOT §9, which ships only the wire-format doc. -> XREF: `01-boot-platform/TODO-14-boot-diagnostics.md §2` (item: "`boot_timeline_dump_json()`: writes unified FPDT + TSC step timeline as JSON to `boot-timeline.json`"). Cheapest shape is to extract the anchor plus reliability ladder as a pure helper the way `boot_timing_fpdt_unreliable_eval` already is, then assert it from `test_boot_timing.c`.
- [x] Commit: `"boot: boot-timeline SVG and Chrome trace-event export"`

**Test checkpoint:** the converter turns a captured `boot-timeline.json` into an SVG that opens in a browser and a trace file `chrome://tracing` imports without error; if the converter is host-side, the kernel image size is unchanged.

**Verification (2026-08-30).** "Opens in a browser" and "imports into `chrome://tracing`" cannot be driven from here, so each is asserted at its mechanical precondition rather than claimed: the SVG is parsed with `ElementTree` and its root checked for the SVG namespace, and every trace event is checked for the fields the format requires with no negative timestamp or duration. `--self-check` runs 30 assertions and `test_boot_timeline.py` 71, the loader's rejection cases and both writers' saturation handling included. Kernel image unchanged: the section touches no file under `src/`, `include/`, `user/` or `resources/`.

---

## 17. Restore Panic Evidence Before Phase 0 Can Overwrite It

> **Spawned-by:** §14 (review)
> **User impact:** a machine that crashes during Phase 0 and then crashes again on the next boot reports only the SECOND crash. The first one -- the one that started the failure -- is gone, so the user debugging a boot loop is handed the symptom and never the cause.
> **Deferred:** 2026-09-03 | kernel-image ceiling, MEASURED at 79 bytes of `.text`. Both candidate mechanisms and the mandatory `TEST_CAT_BOOT` fixture are kernel `.text`; the section is parked on size alone, not on any missing capability -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard").

Section 14 stops firmware and BOOTX64 from handing out the evidence page, but the kernel then overwrites it itself. `kernel_main` runs all of `boot_phase0()` BEFORE calling `panic_evidence_restore_early()` ([`src/kernel/main.c:24`](../../src/kernel/main.c)), and the panic collector unpublishes, zeroes and rewrites the page directly ([`src/kernel/panic.c:2103`](../../src/kernel/panic.c)) while its prior-record guard only arms once the restore has recorded `s_prev_crash_epoch`. So any panic inside Phase 0 destroys the previous boot's record despite a successful loader pin. No SMP is needed to reach this: BSP ordering alone is sufficient.

Found by the section 14 design review, verified at both file:line refs above.

**Files:** [`src/kernel/main.c`](../../src/kernel/main.c), [`src/kernel/panic.c`](../../src/kernel/panic.c)

- [/] Capture and validate the evidence page at the very start of `kernel_main`, before `boot_phase0()` can panic, using a helper that neither logs nor allocates -- blocked on the kernel-image ceiling
  - The restore currently depends on PMM having reserved low memory and klog being up, which is why it sits after Phase 0. Splitting capture from reporting removes that dependency: copy the page into kernel static storage first, report it once klog exists.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Alternatively, have the panic collector refuse to overwrite a page whose record predates this boot until the restore has run, rather than moving the restore -- same blocker
  - Pick one and record why in the section's Notes; shipping both would leave two mechanisms owning the same invariant. The choice is deliberately NOT made here: it is a design call best made against the image budget that actually exists when the ceiling lifts.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Add a `TEST_CAT_BOOT` assertion in [`src/kernel/test/test_boot_diag.c`](../../src/kernel/test/test_boot_diag.c) that a publish into a fixture page carrying a prior-boot record does not destroy it before a restore has run
  - Parked on the same ceiling, and it is the item that makes the whole section unreachable rather than merely expensive: a fixture case seeding a prior-boot record costs hundreds of bytes of `.text` against 79 available.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Commit: `"panic: capture cross-boot evidence before Phase 0 can overwrite it"` -- lands once the ceiling is gone
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")

**Test checkpoint:** a fixture page seeded with a valid prior-boot record survives a simulated Phase-0 panic and is still restorable; the 4-leg smoke matrix stays green.

**Measured blocker (re-measured 2026-09-03).** `scripts/overnight/bss-headroom.py` reports `__kernel_end` at `0x7fead5`, page-aligning to `0x7ff000`, with **79 bytes** of `.text` growth before `scripts/build.sh` refuses at `USER_BASE == 0x800000`. Nothing in this section is bootloader-side: both candidate mechanisms live in `src/kernel/main.c` / `src/kernel/panic.c` and the mandatory fixture lives in `src/kernel/test/test_boot_diag.c`. The neighbouring measurements in `02-kernel-core/TODO-33` §7 price a comparable production-only change at +96 bytes and a comparable test block in the thousands, so this is unreachable by an order of magnitude rather than trimmable. No code was written: parking before implementing is what the section-pack gate asks for on a TIGHT image.

---

## 18. Pre-Serial `serial_early_print` Drives a Poisoned UART Port

> **Spawned-by:** §14 (review)
> **User impact:** on real hardware whose firmware pool-poisons `.bss`, every boot does `inb`/`outb` against I/O port `0xAFAF` before the UART is configured -- writing bytes into whatever device answers at that address. Emulators hide it completely, so the first symptom would be a machine that hangs or misbehaves at boot on one vendor's firmware and nowhere else.

This loader does NOT zero `.bss`; firmware pool-poisons it with `0xAF` ([`src/boot/uefi/bootx64.c:138`](../../src/boot/uefi/bootx64.c)). `s_serial_port` is an uninitialised `.bss` static ([`:412`](../../src/boot/uefi/bootx64.c)), and `serial_early_putchar` guards only on `if (!s_serial_port) return;` ([`:789`](../../src/boot/uefi/bootx64.c)). Poison is non-zero, so the guard passes and the polling loop reads `inb(0xAFAF + 5)` and writes `outb(0xAFAF, c)`.

`boot_set_section()` reaches that path before `serial_early_init()` runs: it is called from `efi_main` as soon as `gST`/`gBS` are wired and calls `serial_early_print` on every section transition ([`:266`](../../src/boot/uefi/bootx64.c)). Section 14 removed its own instance of this by using the 8-bit `post_code` for its pre-serial breadcrumb; the `boot_set_section` instance predates section 14 and remains.

Found by the section 14 performance review, which was right where the boot-quality auditor was wrong: the auditor called the same call site harmless on the assumption that `s_serial_port` is still zero, which is exactly the `.bss` guarantee this loader documents it does not provide.

**Files:** [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)

- [x] Gate the early-serial path on an explicit "serial is configured" flag rather than on `s_serial_port` being non-zero, so poisoned `.bss` cannot be mistaken for a configured UART
  - Shipped as a wide exact-match cookie (`EARLY_DIAG_READY`, `s_serial_ready`), not the boolean the item proposed: a boolean occupies the same unzeroed `.bss`, and `0xAF` poison is non-zero, so it would read as "configured" exactly as the port did. `serial_early_putchar` tests the cookie; only `serial_early_init` publishes it, at both success exits and after the port writes.
  - The cookie means "the initialisation sequence ran to completion on a validated base", NOT that the hardware acknowledged anything: `serial_init_port_baud` returns void and nothing reads the port back, so a UART that accepted no writes is still published. The review caught the first draft of this claim overstating it. What the ordering does buy is that a fault or hang inside those writes leaves the cookie unset and the path silent.
  - The design review upgraded the primary mechanism: a cookie is the sole barrier ahead of `boot_set_section`, and a warm reboot can leave the PREVIOUS boot's cookie at the same address, since the image reloads at the same base and nothing scrubs RAM across a reset. So `early_diag_reset()` runs as the FIRST statement of `efi_main` and is the load-bearing invariant; the cookie is defence in depth for anything running ahead of it. A constant cookie discriminates poison from configured, never this boot from the last.
  - The cookies are `volatile` because the property is otherwise optimised away. Measured 2026-09-03 on the real build object: without it clang-19 at `-O2` narrowed both to 1-byte booleans with `cmpb $1` consumers and the 32-bit constant absent from the image. That narrowing is legal only under the assumption that nothing outside the program writes the object, which is the assumption firmware poison violates.
  - Zeroing `.bss` wholesale was rejected as the item anticipated: this file's deliberate poison workarounds stay observable, and the reset is scoped to the ten statics the early-diagnostics path reads.
- [x] Audit every `serial_early_print` call site reachable before `serial_early_init()` and confirm each is either gated or moved after it
  - The window is `efi_main` entry to the `serial_early_init()` call in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c). The only application calls in it are `early_diag_reset()` (now first), `boot_set_section()` and `post_code()`; the remainder are the firmware `AllocatePages` and `ClearScreen`. `post_code` is a bare `outb $0x80` with no static state, so `boot_set_section` was the sole reachable emitter, exactly as the section-14 comment predicted.
  - No diagnostic the operator could rely on is lost by gating it. Where `.bss` came up zero the old `if (!s_serial_port)` guard already dropped the line; where `.bss` was poisoned it went to I/O port `0xAFAF`, an address derived from the poison pattern rather than from any selection this boot made, so whatever it reached was accidental and unvalidated.
  - The review corrected a stronger claim in this item's first draft. `0xAFAF` is not structurally impossible as a genuine base: `serial_spcr_probe` accepts any nonzero I/O base through `0xFFF8`, and `0xAFAF` is inside that range, so firmware could declare it. The defect was never that the address could not be real, it was writing before anything validated it.
  - One further static in the same path was poison-readable and is now reset: `s_spcr_skipped`, read as a boolean at the serial report block, made every machine without an SPCR table print a "non-standard port" line naming a poison address. `g_boot_section` is reset alongside it but is NOT a poison fix and the review corrected the first draft of this claim: it carries a non-zero initialiser, so it lands in `.data` (measured at section index 7, not `.bss`) which the PE loader reloads from the image every boot. It is cleared only so the function's contract is uniform.
  - `boot_log_append` carried the same defect in the same call path and is fixed with it: `boot_log_buf` is a poisoned non-NULL pointer that passes `if (!boot_log_buf)`, and `boot_log_pos` was never reset anywhere in the file, so on poisoning firmware the ESP boot log recorded nothing for the whole boot even after `AllocatePool` succeeded.
- [x] Commit: `"boot: gate early serial on a configured flag, not a poisoned .bss port"`

**Test checkpoint:** a boot with `s_serial_port` pre-poisoned in a fixture emits nothing to any I/O port before `serial_early_init()`; the 4-leg smoke matrix stays green and serial output after init is unchanged.

> **Note:** No kernel test surface. These are file-scope statics in `bootx64.c`, which links into `BOOTX64.EFI` and not `kernel.exe`, so no `TEST_CAT_BOOT` case can reach them to seed the poison the checkpoint describes; a kernel-side fixture is separately barred by the 79-byte `.text` headroom. Validation is the 4-leg smoke matrix plus a direct object-level measurement of the cookie width.

> **Notes:**
> - Shipped `early_diag_reset()` in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) as `efi_main`'s first statement, clearing the ten early-diagnostics statics, plus a `volatile` `EARLY_DIAG_READY` cookie gating `serial_early_putchar` and `boot_log_append`.
> - Integrates ahead of everything: the reset precedes the `gST`/`gBS` saves and makes no firmware call, which is what lets it precede `boot_set_section`, the sole pre-init emitter.
> - Downstream, a machine whose firmware poisons `.bss` stops driving I/O port `0xAFAF`, stops printing a fabricated "non-standard SPCR port" line, and gets a populated ESP boot log for the first time.
> - Canonical rationale is the comment block above the cookie declarations; the object-level measurement behind `volatile` is recorded there.
> - Scope boundary: bootloader only. The kernel-side evidence-page work stays with sections 17 and 19, both of which need kernel `.text` this image has no room for.
> **Verified:** 2026-09-03 | commit `8cf0a2f33` | 3/3 items | build OK | 32845 kernel + 17 user tests, smoke matrix 4/4
> **Accepted:** [H] the `.bss` poison discipline stops at the serial path -- 13 further statics are poison-readable, and an early `boot_fatal` renders through a poisoned framebuffer pointer -> XREF: `01-boot-platform/TODO-14-boot-diagnostics.md` §20 (item: "Reset the fatal-path and framebuffer statics before any fallible operation in `efi_main`, so a failure that occurs before GOP init reports through predicates this boot actually wrote" at line 657)
> **Quality reviewed:** 2026-09-03 | Codex 10x (design, adversarial, re-adversarial, consistency, perf) | 1H+4M fixed, 0 open | scope: boot-code-quality

---

## 19. Record the Panic-Page Pin Outcome in the `boot_info` Handoff

> **Spawned-by:** §14 (review)
> **User impact:** on a machine where firmware refuses the pin, the kernel still writes the evidence page -- a page firmware may own -- because nothing tells it the pin failed. The user sees no warning, and a crash report that reads "no prior crash" is indistinguishable from one whose record was never protected in the first place.
> **Deferred:** 2026-09-03 | kernel-image ceiling, MEASURED at 79 bytes of `.text`. The producer half is bootloader-side and would fit; the decode helper, the kernel-side gate and the mandatory `TEST_CAT_BOOT` fixture are all kernel `.text`, and shipping the field without the gate that reads it would be false completeness -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard").

Section 14 pins the page and reports the outcome on serial, but the outcome never reaches the kernel. `panic_evidence_take` and the publish path dereference `PANIC_EVIDENCE_ADDR` unconditionally ([`src/kernel/panic.c:1605`](../../src/kernel/panic.c), [`src/kernel/panic.c:2103`](../../src/kernel/panic.c)), and the PMM's first-MiB reservation stops allocator REUSE, not direct writes. That exposure predates section 14 -- before the pin there was none at all, on every boot -- and a recorded outcome is what finally lets the kernel decline.

Raised by the section 14 adversarial review, verified at both refs.

**Files:** [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h), [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h), [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c), [`src/kernel/panic.c`](../../src/kernel/panic.c)

> **Blocked (operator-gated):** the field needs a one-line `F(panic_page_reservation);` row in [`tools/boot-info-manifest/dump-fields.inc`](../../tools/boot-info-manifest/dump-fields.inc). `receipt_surface_guard.py` classifies `tools/boot-info-manifest/*` as receipt machinery the unattended run may not edit, and `check-doc-coverage.py` refuses any `boot_info` field lacking that row plus an ownership-matrix row, so the field cannot land from an unattended session. Measured 2026-08-30 by adding the field and running the checker. Filed as a runner finding in `todo/overnight-runner-improvements/overnight-runner-improvements-v18.md`. **Unblocked 2026-09-03 at the v18 close-out:** `receipt_surface_guard.py` now classifies `tools/boot-info-manifest/` as machinery by file type (`*.c`, `*.h`, `*.sh`, `*.py`), so the declarative `dump-fields.inc` row is ordinary work and this item is runnable unattended; the operator gate no longer applies. The section is now parked on a DIFFERENT blocker -- the kernel-image ceiling -- and that one is not operator-gated, so do not read the sentence above as "runnable now".

- [/] Add `uint32_t panic_page_reservation` at the tail of `struct boot_info` with the matching mirror field, offset `_Static_assert`s, and a `BOOT_INFO_VERSION` 24 -> 25 bump in BOTH headers
  - A tail field, not a pad carve: every reserved pad in the struct was consumed by the v21/v22 carves and there is no trailing pad.
  - Lands with the `dump-fields.inc` row, a `docs/boot/boot-info-fields.md` ownership-matrix row, and a `docs/boot/boot-protocol-changelog.md` entry in the same commit.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Encode it as one self-describing word so "the producer never wrote this" stays distinct from every outcome it can report, and decode it with a pure helper
  - Whole-word zero is what a pre-v25 loader leaves behind, so it must decode as UNREPORTED and route through today's restore behaviour unchanged. A nonzero word that is not a known encoding decodes as MALFORMED rather than being read as UNREPORTED.
  - States are protected / unprotected / unreported / malformed. It reports PROTECTION, never DESTRUCTION: `EFI_NOT_FOUND` proves only that the page was unavailable, and a success proves only that nothing took it after firmware init. Never render "clobbered" from it.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Gate the kernel's access to the evidence page on the decoded state, so a boot whose pin failed does not write a page firmware may own -- kernel `.text`, blocked on the image ceiling
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Unit-test the decode helper in [`src/kernel/test/test_boot_diag.c`](../../src/kernel/test/test_boot_diag.c) (`TEST_CAT_BOOT`) against a fixture word, not the live page
  - Four fixture words plus a malformed control; the block is the section's largest `.text` cost and is what puts it out of reach rather than merely over budget.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")
- [/] Commit: `"boot: carry the panic-page pin outcome in the boot_info handoff"` -- lands whole, once the ceiling is gone
  - Splitting producer from consumer across two commits was considered and rejected: a `boot_info` field that nothing reads is a version bump spent on nothing, and it would leave `BOOT_INFO_VERSION` 25 meaning two different things depending on which half shipped.
  - -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7` (item: "Remove the `scripts/build.sh` BSS-collision guard")

**Measured blocker (2026-09-03).** `scripts/overnight/bss-headroom.py` reports 79 bytes of `.text` headroom before `__kernel_end` crosses `USER_BASE`. The bootloader half of this section costs the kernel image nothing (`BOOTX64.EFI` is a separate binary), but the decode helper, the `panic.c` gate and the fixture block are all kernel `.text`, and `02-kernel-core/TODO-33` §7 records a comparable production-only change measured at +96 bytes. No code was written; the section is parked before implementation as the section-pack gate asks on a TIGHT image.

**Test checkpoint:** the decode helper returns each of the four states for its corresponding fixture word and MALFORMED for an unknown nonzero encoding; a boot whose pin failed leaves the evidence page untouched; the 4-leg smoke matrix stays green and `compare.sh` reports no kernel-vs-mirror drift.

---

## 20. The `.bss` Poison Discipline Stops at the Serial Path

> **Spawned-by:** §18 (review)
> **User impact:** on firmware that pool-poisons `.bss`, a boot failure occurring before GOP init writes a graphical error screen through a poisoned framebuffer pointer instead of printing a readable one, so the diagnostic path becomes its own second fault. Separately the watchdog silently never refreshes after arming, and a warm reboot can select the previous boot's kernel path. Each looks like a different intermittent fault on one vendor's machines and none reproduce under an emulator.

Section 18 fixed the early-serial statics. The consistency review of that section then found the same class alive across the rest of the loader: the file's remedy for non-zero `.bss` is an explicit runtime reset, applied where an author remembered it, and nothing enumerates the places it is missing.

Verified 2026-09-03 by reading the emitted object rather than the source, because a non-zero initialiser lands in `.data` and is genuinely safe while a zero one is not: `llvm-readelf-19 -s src/boot/uefi/bootx64.o` puts all thirteen of the symbols below in section index 8 (`.bss`), against index 7 (`.data`) for an initialised control such as `g_boot_section`.

The review disputed this paragraph twice, so it now states only per-predicate facts verified at `file:line` and makes no end-to-end claim about what an early fatal does. Three predicates on the fatal path read poison, in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c):

- `:2078` -- `if (gST && gST->ConOut && !g_ebs_in_progress)`. Poison leaves `g_ebs_in_progress` non-zero, so the ConOut error screen is skipped.
- `:2156` -- the `else if (bsod_can_render_graphical())` arm then runs, and that predicate passes on ALL FOUR of its terms under `0xAF` fill: `gFramebuffer` is non-NULL, `gFbWidth`/`gFbHeight` read `0xAFAFAFAF` and clear the 800/600 minimums, and `gFbPixelFormat` is not 2. `bsod_render_graphical()` therefore writes through a poisoned framebuffer pointer. This is the most severe consequence found and the reason the section leads with it.
- `:1957` -- `boot_fatal_dwell` carries its OWN `g_ebs_in_progress` test and skips its Stall-and-keypress branch. Its `GetTime`/TSC fallback dwell runs only if execution survives the render above, which is exactly what is not established.

Two earlier drafts of this paragraph were wrong and both errors are recorded here because they bear on how the sweep should be done: the first attributed the Stall-and-keypress predicate to `boot_fatal` (a probe had matched `boot_fatal_dwell` instead), and the second asserted the machine still pauses. Both functions contain separate `g_ebs_in_progress` tests.

**Files:** [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c), [`src/boot/uefi/boot_entries_parser.c`](../../src/boot/uefi/boot_entries_parser.c)

**Correction to this section's own premise, 2026-09-03.** The item list below was written on the belief that this file relies on observable poison in at least three places by design. It does not. All eighteen `0xAF` comment sites were re-read and every one is DEFENSIVE. The belief mattered because it would have told the sweep to leave unnamed statics alone; the sweep as shipped names every one.

**Scope correction, same date.** The unit of this invariant is the LINKED IMAGE, not `bootx64.c`. `boot_entries_parser.c` links into `BOOTX64.EFI` and carried the identical defect shape section 18 fixed, which a file-scoped sweep would have missed for the second time.

- [x] Reset the fatal-path and framebuffer statics before any fallible operation in `efi_main`, so a failure that occurs before GOP init reports through predicates this boot actually wrote
  - Shipped as `boot_fatal_statics_reset()`, covering the seven the item named plus `g_panic_page_attempted`, reached through a new `boot_early_reset_all()` wrapper that calls `early_diag_reset()` then this and is now the single first statement of `efi_main`.
  - The wrapper is the design review's correction and is load-bearing rather than tidiness: the two resets carry DIFFERENT deadlines (early-diag must precede `boot_set_section`, the fatal group must precede the first `boot_fatal`-reachable call), and as two adjacent bare calls a later edit could reorder them or slip a diagnostic between them. One call cannot be split that way.
  - `g_panic_page_attempted` was not in the item and is the subtle one: it had NO emitted `.bss` object, because clang promoted it out of memory on the strength of its `= 0` initialiser, which is the single assumption this file documents as false. It was correct only by codegen accident. Resetting it forces the object to be emitted, which is what moved `bootx64.o` from 65 to 66 `.bss` objects.
  - Zeroing `gFramebuffer` alone would fix `bsod_can_render_graphical` but not the QR fallback beside it, which reads the same statics behind its own test; and `g_ebs_in_progress` independently controls the ConOut branch. All are reset together.
  - `gST`/`gBS`/`gImageHandle` were deliberately NOT added: they are assigned from the firmware arguments immediately after the wrapper returns, so including them would imply a hazard that does not exist.
- [x] Clear `g_boot_image_file_path` alongside `g_boot_device_handle` on the LoadedImage lookup-failure path, which today clears only the handle while the adjacent comment claims it clears both
  - Confirmed at source before fixing: the path is assigned ONLY in the success branch, so the failure branch kept firmware poison, which is non-NULL and therefore passes the `if (!g_boot_image_file_path)` guard in `self_measure_resolve_path()` before `GetDevicePathSize()` is handed the pointer. `network_boot_discover()` consumes it on the same branch.
  - The pre-existing comment that claimed both globals were cleared is now true rather than deleted.
- [x] Reset `g_wd_refresh_disabled` before the watchdog is armed, so poison or warm-reboot residue cannot disable every refresh for the whole boot
  - Placed beside the existing `g_wd_armed = 0` at the arm site rather than in the entry group, so the two watchdog statics stay together where a reader looking at the watchdog will find them. Dominance verified: all three `watchdog_reset()` call sites are inside `boot_menu_run`/`boot_policy_invoke`, which run after the arm.
  - The latch's only write is inside its own reader, so nothing else in the image ever cleared it. Left poisoned, the failure is a firmware reset 60 seconds in with no diagnostic, on a machine where nothing went wrong.
- [x] Reset `g_policy_decoded` at `boot_policy_invoke()` entry so the fallback and allocation-failure paths cannot inherit a previous boot's authoritative kernel selection
  - The whole object is zeroed, not just `.valid`, so determinism does not depend on every present and future consumer being `.valid`-gated. Consumers read `.valid` and then dereference `.u.split.kernel` as a C string, so a spuriously-true `.valid` is a wild pointer, not merely a wrong boolean.
  - Measured 2,840 bytes, NOT the 10,304 an earlier draft of this section recorded. That first figure came from reading `llvm-readelf`'s decimal Size column as hex; the control in use at the time was a 4-byte object, which reads identically under both hypotheses and so could not discriminate them. The design review caught it.
- [x] Reset `g_conf_res_width` / `g_conf_res_height` at `parse_boot_conf()` entry so an absent `Resolution=` leaves automatic GOP selection working rather than suppressed
  - First statement, ahead of every early return in the function, per the design review's constraint. Dominance verified: `parse_boot_conf()` is called unconditionally at `efi_main` top level and precedes `init_gop()`, which contains the only reader.
- [x] Sweep the remaining file-scope statics and record the ones deliberately left alone
  - Shipped as the inventory comment block in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c). It classifies every static in the LINKED IMAGE by emitted section into reset / deliberately-not-reset-because-write-before-read / count-bounded staging, and records the method so a later sweep is reproducible.
  - The unit is the linked image, not the file. Scoping to `bootx64.c` alone is exactly what let the parser's poisoned CRC table survive section 18, so the sweep covers every object: `bootx64.o` 66 `.bss` objects, `boot_entries_parser.o` 2, `boot_history.o` and `boot_sticky.o` `.data` only, the other five no statics at all.
  - The item's premise was WRONG and is corrected rather than satisfied: this file does not rely on observable poison anywhere. All eighteen `0xAF` comment sites were re-read and every one is DEFENSIVE, explaining why a reset exists. The two things that genuinely depend on memory not being zeroed are neither of them `.bss` statics -- the cross-boot panic-evidence page at `0x80000`, and `BOOT_KSTACK_POISON`, a deliberate fill of an allocated stack run. The design review independently reached the same conclusion.
  - The emitted section is a LOWER BOUND on the source-level static list: clang may promote a static out of memory entirely on the strength of a `= 0` initialiser, which `g_panic_page_attempted` demonstrates. A sweep that trusts the symbol table alone will miss exactly the statics whose safety is least robust.
- [x] Extend the reset discipline to `boot_entries_parser.c`, whose CRC readiness flag is the same defect shape as section 18's serial-port guard
  - Not in the original item list; found by the design review and confirmed at source. `g_crc32_ready` was a plain `int` tested with `if (!g_crc32_ready)`, so poison skipped `crc32_init()` and every CRC was computed over a poisoned 1 KiB table. The effect is a VALID boot-entry store rejected as `CRC_MISMATCH`, sending the loader to its invalid-store fallback, which can select a different kernel than the store asked for.
  - Fixed with the section-18 shape: a `volatile` wide exact-match cookie published LAST after the table is filled, plus a `crc32_reset()` as the first statement of `boot_entries_parse()`, ahead of its early returns. The reset is the load-bearing half; the cookie is defence in depth.
  - The `volatile` is measured, not defensive style: the declared `int` was emitted at SIZE 1, clang having narrowed the flag to a byte exactly as it did to the section-18 cookies. It is size 4 now.
- [x] Assert the poisoned-CRC-flag behaviour in an automated regression, since the parser IS reachable from a test
  - Raised by the consistency review and independently by this section's own diff-facts pass: the section's original "no kernel test surface" note was false for the parser half.
  - Shipped host-side as [`tools/boot-entries-parser-tests/`](../../tools/boot-entries-parser-tests/) (5 checks), wired into `scripts/test-tooling.sh` as the `boot-entries parser gate`. Host-side costs the kernel image nothing, which is why `tools/boot-header-tests/` already works this way.
  - The kernel-side `TEST_CAT_BOOT` version was written FIRST and measured: it overran the image ceiling and the build failed. Preserved at `.claude/state/deferred-todo14-s20-test.patch` in case an in-kernel case is preferred once the ceiling lifts; the host suite makes that a nicety rather than a gap.
  - Includes a CONTROL that must report a WRONG crc for a poisoned-but-believed-ready table, so a vacuous pass is detectable rather than reassuring.
- [x] Commit: `"boot: complete the uninitialised-.bss reset discipline across the loader"`

**Test checkpoint:** each reset lands before the first read of the static it covers, demonstrated by the emitted-section classification above plus a walk of the enclosing call path; the 4-leg smoke matrix stays green and serial output is unchanged.

> **Test runner:** `bash tools/boot-entries-parser-tests/run.sh` -- 5 checks, expect `PASS (0 failure(s))`; also runs inside `bash scripts/test-tooling.sh` as the `boot-entries parser gate`.

> **Note:** The test surface is SPLIT, and the first draft of this note got it wrong in the section's own favour. For the `bootx64.c` statics the claim holds: they live in `BOOTX64.EFI`, not `kernel.exe`, nothing includes that file into a test, and no `TEST_CAT_BOOT` case can reach them to seed the poison. It does NOT hold for the parser half -- [`src/kernel/test/test_boot_entry_parser.c`](../../src/kernel/test/test_boot_entry_parser.c) `#include`s `boot_entries_parser.c` directly, putting `g_crc32_ready` and `g_crc32_table` in the same translation unit as the tests. The consistency review caught the false claim independently of the same conclusion reached here from the diff-facts test list.
> - A kernel-side case was therefore WRITTEN, and MEASURED: it does not fit. It took `__kernel_end` past `USER_BASE` and the build failed on the BSS-collision guard, with 47 bytes of `.text` headroom (itself down from 79 because `crc32_reset()` compiles into the kernel test binary too). Preserved at `.claude/state/deferred-todo14-s20-test.patch`.
> - The regression therefore ships HOST-side as [`tools/boot-entries-parser-tests/`](../../tools/boot-entries-parser-tests/), which costs the kernel image nothing. That is not a workaround: [`tools/boot-header-tests/`](../../tools/boot-header-tests/) exists for exactly this reason and its own header records the same measurement against the same ceiling.
> - The suite carries a CONTROL case that reproduces the pre-fix state (poisoned table, cookie forced to READY) and asserts the CRC comes out WRONG. Without it the passing cases would read identically against an implementation that never had the bug.
> - The `bootx64.c` half still has no automated regression and honestly cannot: validation there is the 4-leg smoke matrix plus `llvm-readelf-19` classification of the emitted objects, which is what caught both the `g_crc32_ready` narrowing and the `g_panic_page_attempted` promotion.

> **Notes:**
> - Shipped `boot_fatal_statics_reset()` and the `boot_early_reset_all()` wrapper in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c), four consumer-entry resets, and the poison-proof CRC cookie plus `crc32_reset()` in [`src/boot/uefi/boot_entries_parser.c`](../../src/boot/uefi/boot_entries_parser.c).
> - Integrates as one first statement of `efi_main`: the wrapper is what stops a later edit separating two resets whose deadlines differ, which was the design review's correction.
> - Downstream, a machine whose firmware poisons `.bss` stops rendering the fatal screen through an unvalidated framebuffer pointer, stops silently disabling every watchdog refresh, stops suppressing automatic GOP mode selection, and stops rejecting a valid boot-entry store as `CRC_MISMATCH`.
> - Canonical rationale is the inventory comment block in `bootx64.c`, which records the classification method and every symbol's disposition.
> - Scope boundary: bootloader image only. No kernel `.text` is added, so the 79-byte ceiling parking sections 17 and 19 does not apply here.
> - Validation: build OK, 32845 kernel + 17 user-mode tests pass, smoke matrix 4/4, a WHPX boot reaching `Boot complete in 8.720s` and `C:\>`, and the new host gate proven by mutation (deleting the parser-entry reset makes it fail).
> - Not filed: a mechanical check for the inventory comment. The adversarial review proposed it after the hand-written count went stale twice in this section, but a stale inventory costs a maintainer, not a user, and the review caught it both times; parking it in the section being stamped would strand it where the fixpoint loop never looks.
> **Verified:** 2026-09-03 | commit `f88d04941` | 9/9 items | build OK | 32845 kernel + 17 user tests, smoke matrix 4/4, host parser gate 6/6
> **Quality reviewed:** 2026-09-03 | Codex 8x (design, adversarial, re-adversarial, consistency, perf) | 1H+4M+1L fixed, 1M rejected, 0 open | scope: boot-code-quality

---

## OS Comparison

| ⭐  | Feature                    | 🪟 Win11                  | 🐧 Linux                  | 🚀 Impossible OS                    |
| --- | -------------------------- | ------------------------- | ------------------------- | ----------------------------------- |
| 💎  | Boot POST codes            | ✅ Firmware boot mgr      | ✅ BIOS POST codes        | ✅ §1 §3 POST port 80 + FB hex      |
| 💎  | Named boot progress        | ✅ ETW boot trace         | ✅ dmesg systemd-analyze  | ✅ §2 serial STAGE ms lines         |
| 💎  | Boot load/status log       | ✅ ntbtlog.txt driver log | ✅ dmesg drivers loaded   | ✅ §11 boot-load-status.txt         |
| ⭐  | Bootloader build identity  | ⚠️ bcdedit/msinfo32       | ⚠️ /proc/version uname    | ✅ §10 boot-loader-identity.txt     |
| 💎  | Boot timeline viewers      | ⚠️ Performance Toolkit    | ✅ systemd-analyze plot   | ✅ §16 SVG + Chrome trace host tool |
| 💎  | Panic forensics            | ✅ WER minidump EventLog  | ✅ kdump pstore ramoops   | ✅ §5 0x80000 page last-panic.txt   |
| 💎  | Crash-page pre-OS pinning  | ⚠️ Firmware-reserved only | ✅ pstore/ramoops in DT   | ✅ §14 loader pins 0x80000          |
| 💎  | Uninit-.bss boot hardening | ⚠️ Compiler/CRT zeroing   | ⚠️ Compiler/CRT zeroing   | ✅ §18 §20 enumerated reset sweep   |
| 💎  | Multi UI spinner           | ✅ WinUI ProgressRing     | ✅ GTK Qt spinners        | ⬜ §7 spinner_create pool           |
| ⭐  | Panic BSOD QR              | ❌ Text URL BSOD only     | ❌ No kernel QR           | ⬜ §6 segno+phone-gated QR          |
| ⭐  | Alive hang pixel           | ❌ No kernel hang pixel   | ❌ Not production default | [~] §4 permanently deferred         |
| ⭐  | Live vital overlay         | ⚠️ Task Manager separate  | ⚠️ htop conky third-party | ⬜ §8 bottom metrics strip          |

> **Parity scan:** Win11+Linux ✅ on POST, named progress, panic dumps, UI spinners -- Impossible OS matches via §1--§3 plus §10 bootloader identity and §11 ntbtlog-parity load/status log; timeline **export** + **v1 schema** exist (§9 `docs/boot/boot-timeline-schema.md`) and the **viewers** shipped in §16 as the host-side `tools/boot-timeline/` converter (Gantt SVG matching `systemd-analyze plot`, plus a Chrome trace-event export neither Windows nor `systemd-analyze` offers). §4 is permanently deferred; the ⬜ rows §6/§7/§8 remain deferred with recorded blockers (§6 segno+phone QR validation, §7 WM compositor integration, §8 the kernel-image ceiling -- its former scheduler-CPU% blocker closed 2026-07-20 with TODO-25 §7); **Edges:** §6 QR and §8 always-on strip are planned differentiators once unblocked.

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
  - Alive blink toggle-counter test -- N/A; §4 is permanently deferred
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

- [x] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===` (re-run at close-out 2026-09-03: `=== BUILD OK ===`; full suite `PASS: 32845 kernel + 17 user-mode tests passed`)
- [ ] Serial log shows `[+Nms] BOOT_PMM: Physical memory manager ready` style entries for at least 8 stages (manual -- serial capture on QEMU/hardware)
- [ ] POST code visible in top-right corner during QEMU boot; disappears when desktop loads (manual -- visual)
- [~] `AliveBlink=1` visual-blink verification -- N/A; §4 is permanently deferred
- [ ] Force `kernel_panic("test")` from shell → BSOD shows QR code in bottom-right corner (deferred -- §6 not implemented)
- [ ] Force panic twice -> second boot finds `last-panic.txt` in `X:\Crash\` (-> XREF: `01-boot-platform/TODO-24-blackbox-service-partition.md` §7) (manual -- §5 shipped; needs forced-panic reboot on QEMU/hardware)
- [ ] `VitalSigns=1` → bottom strip shows CPU/RAM/IRQ/uptime/FPS, updates every 500 ms (deferred -- §8 not implemented)
- [ ] `spinner_create(SPINNER_MEDIUM, 0x0078D4)` in test harness → spinner renders in compositor frame (deferred -- §7 not implemented)
- [ ] Commit: `"kernel: boot-diagnostics verified -- POST codes, panic forensics, QR code, vital signs, multi-instance spinner"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `test_boot_diag.c` landed with 5 `TEST_CAT_BOOT` suites (boot stage/POST + panic evidence §5 + boot-load-status §11); deferred-feature tests (§4/§6/§7/§8) pending with their sections.

---

## History

| Date       | Action        | Summary |
| ---------- | ------------- | ------- |
| 2026-04-12 | gap-analysis  | Web: Win11 required diagnostics / WER flow (Learn); Linux ramoops+pstore (kernel.org), systemd-analyze, boot-time trace docs. Code-truth: `panic_evidence` / `spinner_create` / `vital_signs` absent; `boot_timeline_dump_json` in `boot_progress.c` + call `boot_desktop.c:229`; `boot_progress_poll` uncalled. Added **Current state** callout; Inputs `boot_desktop.c` + TODO-15 scope XREF; §4 bullets for timeline JSON; new §9 + Impl row 9 + OS row; Unit Tests wording; History table. |
| 2026-04-12 | validate      | Inputs paths exist; no N.M / continuation-line drift; OS row Impossible cell shortened to five words after glyph; Impl order 7 Depends On now `08-graphics-ui/TODO-06` §8 (blocked until compositor per-frame ticks); Unit Tests XREF replaced the dead kernel-test-framework path with `test_runner.c` + `TEST_CAT_BOOT` pattern; Verification adds `run-boot-tests.bat`; Verification XREF to TODO-24 uses domain-qualified path. **Flag:** legacy phantom kernel-test-framework path should normalize to `00-infrastructure/TODO-03-kernel-test-harness.md` repo-wide. **Note:** `> **Verified:**` lines after §1--§5 checkpoints are audit stamps from `verify-todo-section`, not checklist items. |
