---
schema_version: 1
id: interrupt-timer-arch
domain: 01-boot-platform
status: active
title: "TODO-11 -- Interrupt Architecture & Unified Timer Subsystem"
---

# TODO-11 -- Interrupt Architecture & Unified Timer Subsystem

> **Goal:** Define the authoritative Phase 1 interrupt and timer architecture: ACPI MADT first, LAPIC/IOAPIC before legacy PIC enable paths where applicable, full 256-vector IDT coverage, dynamic GSI/vector registration (`irq_request_gsi`), and a unified timer HAL (`uptime_ns()`) choosing HPET, LAPIC, or PIT. Historical bug class: PIT before IOAPIC routing on PCAT_COMPAT machines; that init order is fixed in tree. Remaining work: UTS high-res reads via the `mono_clock` contract, one-shot/TSC-deadline mode, IRQ affinity + GSI validation + shared INTx handlers + storm quarantine, driver migration to `irq_request_gsi`, AP LAPIC timers, shell tools (`irq list`, `boot-timeline`), and unit tests below.

> [!IMPORTANT]
> **Current state (2026-04-11):** Phase 1 order matches `boot_interrupts.c`: GDT → IDT → ACPI (MADT) → LAPIC/IOAPIC → PIC path → services → `timer_hal_init()` before `sti` (see `02-kernel-core/TODO-01-kernel-init-sequencing.md` §3). PIT-before-IOAPIC routing hazard is **fixed in tree**. Open work (gap-audit 2026-06-11): §5 affinity + GSI routing validation + shared INTx handlers + storm quarantine + driver migration; §6 mono_clock single-clocksource contract + one-shot/TSC-deadline mode + HV TSC page + quiesce/resume; §7 AP LAPIC timer bring-up (BSP-only today); shell tools in `09-desktop-shell/TODO-12-utilities.md` §1; Unit Tests + Verification below.
>
> **Historical:** PCAT_COMPAT machines can see IRQ 0 from both PIC and IOAPIC if the PIT is enabled before IOAPIC routing; MADT-first sequencing avoids that class of bug.

## Inputs

- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c)
- [`include/kernel/acpi.h`](../../include/kernel/acpi.h), [`src/kernel/acpi.c`](../../src/kernel/acpi.c)
- [`include/kernel/drivers/lapic.h`](../../include/kernel/drivers/lapic.h)
- [`include/kernel/drivers/ioapic.h`](../../include/kernel/drivers/ioapic.h)
- [`include/kernel/drivers/pic.h`](../../include/kernel/drivers/pic.h)
- [`include/kernel/idt.h`](../../include/kernel/idt.h)
- [`include/kernel/irq.h`](../../include/kernel/irq.h)
- [`include/kernel/timer.h`](../../include/kernel/timer.h)
- [`include/kernel/drivers/pit.h`](../../include/kernel/drivers/pit.h)
- [`src/kernel/drivers/hpet.c`](../../src/kernel/drivers/hpet.c), [`include/kernel/drivers/hpet.h`](../../include/kernel/drivers/hpet.h)
- → XREF: `04-drivers-hardware/TODO-02-apic-interrupt-routing.md` -- MSI/MSI-X vector allocation, x2APIC, advanced routing (complements ISA `irq_request_gsi` here)
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §3` -- Phase 1 calls every init function listed here in the correct order; this TODO defines what correct order means
- → XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md §1` -- DPC queue init requires LAPIC timer ready (§9); IRQL model depends on the correct interrupt priority assignment established in §2
- → XREF: `02-kernel-core/TODO-08-time-filetime-management.md §4` -- `uptime_ns()` from the UTS (§6) is the clock source for the monotonic nanosecond clock used by FILETIME and `QueryPerformanceCounter`
- → XREF: `TODO-14-boot-diagnostics.md §3` -- `boot_stage_history[]` from the named-stage API feeds downstream visualization
- → XREF: `TODO-14-boot-diagnostics.md §9` -- runtime vital signs strip (developer overlay)
- → XREF: `TODO-15-visual-post-display.md` -- proportional stage bar / VPD (stage colors, timing on screen; supersedes former TODO-14 debug color bar)
- → XREF: `TODO-09-cpu-boot-sequencing.md §4` -- hypervisor detection runs in Phase 0 before §7 UTS probe; `boot_info.hv_flags` set there must be read by `timer_hal_init()` to select the correct clock source
- → XREF: `02-kernel-core/TODO-26-power-management.md §15` -- Intel HWP / AMD CPPC frequency scaling governor framework; frequency changes require LAPIC timer recalibration (§7 `lapic_timer_calibrate()`)
- → XREF: `04-drivers-hardware/INDEX.md` -- device drivers should migrate to `irq_request_gsi(gsi, handler, ctx, name)` (§5); this API replaces hardcoded IRQ-to-vector assignments throughout that domain
- → XREF: `TODO-10-bare-metal-hardening.md §3` -- bare-metal hardware interrupt root cause (`clac` #UD) and fix; IST/UC MMIO/ACPI gating in other TODO-10 sections. Timer HAL sequencing remains owned here.

## Outcome

- `acpi_parse_madt()` runs during `acpi_init()` before LAPIC/IOAPIC and legacy PIC programming; `struct acpi_irq_map` populated with GSI overrides (IDT loads earlier for fault delivery only).
- Init order in `boot_phase1()`: GDT → IDT (`idt_init` / `irq_init`) → ACPI (`acpi_init`, MADT) → LAPIC → IOAPIC → PIC path → platform services → `timer_hal_init()` last before `sti` (see `boot_interrupts.c`).
- All 256 IDT vectors filled; no vector triggers an unhandled-interrupt panic.
- `irq_request_gsi(gsi, handler, ctx, name)` wires any device IRQ at runtime; `irq list` shell command prints the full table (when shell lands).
- `uptime_ns()` returns nanoseconds since boot from the best available clock; no scattered PIT tick counters remain.
- LAPIC timer calibrated per CPU; scheduler uses LAPIC timer, not global PIT tick, for SMP.
- Boot splash spinner no longer hooks PIT IRQ 0 directly; works on LAPIC-only platforms.
- All `#ifdef HYPERV_WORKAROUND` blocks removed; AP bringup verified via `lapic_send_ipi()`.

## Implementation Order

| ⭐  | Order | Deliverable                         | Depends On          | Status |
| --- | :---: | ----------------------------------- | ------------------- | :----: |
| 💎  |   1   | ACPI MADT parsing                   | --                   |  [x]   |
| 💎  |   2   | LAPIC / IOAPIC init before PIT      | §1                  |  [x]   |
| 💎  |   3   | Conditional PIC disable             | §1, §2              |  [x]   |
| 💎  |   4   | Full IDT coverage                   | §2, §3              |  [x]   |
| 💎  |   5   | Dynamic IRQ registration API        | §2, §4              |  [/]   |
| 💎  |   6   | Unified timer subsystem (UTS)       | §5, TODO-09 §4      |  [/]   |
| 💎  |   7   | LAPIC timer calibration             | §6                  |  [/]   |
| 💎  |   8   | Migrate boot splash spinner off PIT | §6, §7              |  [x]   |
| ⭐  |   9   | Boot time visualization             | §7, TODO-14 §3, §6  |  [x]   |
| 💎  |  10   | Remove Hyper-V debug workarounds    | §1--§7              |  [x]   |

> 💎 = parity -- Windows NT HAL and Linux interrupt subsystem both follow this init order and have equivalent abstractions.
> ⭐ = exclusive -- on-disk JSON boot timeline (`boot-timeline.json`) for quick diff/review is not matched by either competitor's default tooling.

---

## 1. ACPI MADT Parsing

Extract interrupt topology from the MADT during `acpi_init()` before LAPIC/IOAPIC and legacy PIC paths are programmed so subsequent routing uses authoritative GSI data (IDT is loaded earlier for fault delivery; that is intentional).

**Files:** `include/kernel/acpi.h`, `src/kernel/acpi.c`

- [x] `acpi_init()` already executes before LAPIC/IOAPIC/PIC in `boot_phase1()` -- verified in boot logs
- [x] Define `struct acpi_irq_override` and `struct acpi_madt_info` in `acpi.h`
- [x] Parse MADT entries: Type 0 (LAPIC), Type 1 (IOAPIC + GSI base), Type 2 (IRQ override), Type 5 (LAPIC address override), Type 9 (x2APIC); unknown types skipped
- [x] `pcat_compat` flag set from `madt->flags & 1`
- [x] `acpi_madt_info()` getter returns const pointer to static `s_madt_info` struct
- [x] Serial log: `MADT: N CPUs, LAPIC=0xbase, IOAPIC=0xbase GSI=base, M overrides, PCAT_COMPAT=N`
- [x] Commit: `"acpi: consolidated MADT info struct with IOAPIC GSI base"`

**Test checkpoint:** Serial shows `MADT:` line with LAPIC/IOAPIC bases and override count before LAPIC enable; `acpi_madt_info()` non-NULL in tests. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | full suite 3500 PASS; dedicated irq_timer suite owned by Unit Tests section

> **Notes:**
> - MADT parse hardened in review: per-type record length guards, two-pass type-5 LAPIC override before the BSP MMIO ID read, x2APIC id>255 skip with one aggregated WARN.
> - x2APIC field order fixed to ACPI 6.x (id@4/flags@8/uid@12; shipped code had id/uid swapped, masked on QEMU where uid==id).
> - `madt_reset_state()` + `madt_publish_info()` keep `acpi_madt_info()` and legacy accessors consistent on every outcome (short table, no MADT, reparse); BSP-only fail-closed when the BSP is not in the usable CPU set.
> - Consumers unchanged: `boot_interrupts.c` Phase 1 order, `smp.c` AP startup, `ioapic.c` ISA routing.

> **Verified:** 2026-06-11 | commit `86f26c5e` | 6/6 items | build OK | smoke PASS (KVM 2.4s)
> **Deferred:** [M] no dedicated MADT/irq unit suite yet (reason: suite owned by TODO-level Unit Tests) -> XREF: 01-boot-platform/TODO-11 Unit Tests (item: "Create `src/kernel/test/test_irq_timer.c`" at line 287)
> **Quality reviewed:** 2026-06-11 | Codex 7x (adversarial, consistency, perf, re-adversarial x4) | 5H+5M+1L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 2. LAPIC / IOAPIC Init Before PIT

Restructure `boot_interrupts.c` so LAPIC and IOAPIC are brought up before the PIT, using MADT data from §1.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/lapic.c`, `src/kernel/drivers/ioapic.c`

- [x] `boot_phase1()` order: GDT → IDT → ACPI (MADT) → LAPIC → IOAPIC → PIC disable when PCAT_COMPAT → ... → `timer_hal_init()` before `sti` -- matches `boot_interrupts.c` (TODO-02 §4)
- [x] `lapic_init()`: reads `IA32_APIC_BASE_MSR`, enables LAPIC, writes SIVR, masks all LVT entries -- already implemented
- [x] `lapic_init()` uses `acpi_get_lapic_base()` (from MADT) for MMIO address
- [x] `ioapic_init()`: maps IOAPIC MMIO at `acpi_get_ioapic_base()`, reads `IOAPIC_VER` for `max_redir_entries`
- [x] MADT IRQ overrides applied via `acpi_get_override()` in IOAPIC ISA routing loop
- [x] Serial log: `LAPIC enabled: base=0xFEE00000` and `I/O APIC at 0xFEC00000: N entries, ISA IRQs routed`
- [x] Already implemented -- marking complete (no new code needed)
- [x] Commit: "(shipped) LAPIC and IOAPIC init before PIT -- boot_phase1 order"

**Test checkpoint:** Serial shows `LAPIC enabled` before PIT/HPET timer init; IOAPIC MMIO mapped from MADT. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | full suite 3500 PASS; dedicated irq_timer suite owned by Unit Tests section

> **Notes:**
> - Review hardened the shipped init: LAPIC + IOAPIC MMIO now UC-mapped via `vmm_map_mmio_uc()` with fail-closed PIC fallback (was WB identity-map access, a bare-metal gotcha violation).
> - IOAPIC IOREGSEL/IOWIN window serialized with `ioapic_lock` (irqsave) across select+data pairs and 64-bit entry RMW; VER validated before programming; base must be page-aligned.
> - GSI routing domain honored: `ioapic_gsi_to_pin()` translates absolute GSI to pin; public APIs take `uint32_t gsi`, return int; mandatory PIT route gates `ioapic_ready` so the PIC is never disabled without a working timer route.
> - Consumers updated: `irq_request_gsi()` rolls back on unroutable GSI (closes the S5 validation item), AHCI INTx falls back to polling, spurious 0xFF no longer EOIs (Intel SDM 11.9).

> **Verified:** 2026-06-11 | commit `1678de87` | 7/7 items | build OK | smoke PASS (KVM 2.4s)
> **Deferred:** [M] no dedicated LAPIC/IOAPIC unit suite yet (reason: suite owned by TODO-level Unit Tests) -> XREF: 01-boot-platform/TODO-11 Unit Tests (item: "Create `src/kernel/test/test_irq_timer.c`" at line 287)
> **Quality reviewed:** 2026-06-11 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 1C+6H+1M fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 3. Conditional PIC Disable

Only mask the 8259 PIC when MADT says PCAT_COMPAT -- virtual platforms may have no PIC at all.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/pic.c`

- [x] `boot_phase1()` checks `acpi_pcat_compat()` before PIC I/O -- already implemented
- [x] PCAT_COMPAT + IOAPIC active → `pic_disable()` called; logs "Switched to LAPIC/IOAPIC (PIC disabled)"
- [x] PCAT_COMPAT=0 → all PIC I/O skipped; logs "APIC-only, no PIC"
- [x] `pic_init()` fallback when no IOAPIC available or no ACPI -- already implemented
- [x] Already implemented -- marking complete (no new code needed)
- [x] Commit: "(shipped) conditional PIC disable per ACPI PCAT_COMPAT"

**Test checkpoint:** PCAT_COMPAT=1 shows PIC disabled log; PCAT_COMPAT=0 skips PIC I/O. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 4. Full IDT Coverage

Fill all 256 IDT vectors with correct stubs so no vector ever triggers an unhandled-interrupt panic.

**Files:** `src/kernel/idt.c`, `src/kernel/idt_stubs.asm`, `include/kernel/idt.h`

- [x] All 256 vectors have stubs in `idt_stubs.asm` (isr0-isr31 for exceptions, irq0-irq15, isr48-isr255 for dynamic)
- [x] Common handler saves registers, dispatches via `idt_common_isr()` in idt.c
- [x] Vectors 0-31 (CPU exceptions): styled panic screen with exception name, RIP, error code
- [x] Vectors 48-63: available for SMP IPIs via `irq_register()` -- IPI constants defined in smp.h
- [x] Vector 255 (0xFF): LAPIC spurious interrupt -- silent EOI, no log spam
- [x] Vectors 0x90-0x9F: Hyper-V synthetic interrupts -- silent EOI, no unhandled-interrupt warnings
- [x] Vectors 32-47 and 64-239: wired dynamically via `irq_register()` -- unhandled vectors log warning + EOI (no crash)
- [x] Commit: `"kernel: full 256-vector IDT -- spurious + Hyper-V silent EOI"`

**Test checkpoint:** Unhandled vector logs warning + EOI; spurious 0xFF and Hyper-V synthetic vectors do not panic. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. Dynamic IRQ Registration API

Replace all hardcoded IRQ-to-vector assignments with a runtime registration API backed by the IOAPIC.

**Files:** `include/kernel/irq.h`, `src/kernel/irq.c`

> [!IMPORTANT]
> `include/kernel/irq.h` already defines `irq_handler_t` as `void (*)(uint8_t vector, void *ctx)` for the existing `irq_register(vector, handler, ctx, name)` API. This section either renames that typedef or introduces a separate `gsi_handler_t` -- resolve the naming before implementing §5 to avoid breaking all current `irq_register()` call sites.

- [x] Existing `irq_handler_t` kept as `void (*)(uint8_t vector, void *ctx)` -- no breaking change needed; GSI API added alongside
- [x] `irq_request_gsi(gsi, handler, ctx, name)`: allocates vector via `irq_alloc_vector()`, registers handler, programs IOAPIC redirection with MADT override flags; returns vector or 0
- [x] `irq_free_gsi(gsi)`: masks IOAPIC entry, unregisters handler, frees vector
- [ ] `irq_set_affinity(gsi, cpu_mask)` -- implement when IRQ balancing is needed; currently all routes to BSP (functional on SMP, just not balanced)
- [x] GSI routing-domain validation: `irq_request_gsi()` returns 0 and rolls back the vector when the GSI exceeds the IOAPIC redirection range (shipped in S2 review, commit 1678de87); GSI-99 unit test asserts the fix when the Unit Tests suite lands
- [ ] Shared GSI handlers for PCI INTx: per-vector handler chain + shared-registration flag, dispatch walks all handlers, single EOI; required before driver migration (`irq_entry` is single-handler today)
- [ ] IRQ storm quarantine: auto-mask a vector/GSI after threshold unhandled fires (today log+EOI forever); expose quarantined state via `irq list` (D09 T12 §1)
- [x] `irq_gsi_count(gsi)`: returns fire count via GSI→vector mapping table
- [x] `irq_dispatch()` already exists in `irq.c` -- registered handler called + EOI sent
- [ ] `irq list` shell command -- deferred to `09-desktop-shell/TODO-12-utilities.md` §1 (item: "Kernel diagnostic commands")
- [ ] Migrate existing drivers to `irq_request_gsi()` -- mechanical pass over `src/kernel/drivers/` ISA call sites (owned here); MSI/MSI-X devices follow `04-drivers-hardware/TODO-02-apic-interrupt-routing.md` §3 instead
- [x] Commit: `"kernel: GSI-based IRQ request API -- irq_request_gsi/free_gsi, IOAPIC-backed"`

**Test checkpoint:** `irq_request_gsi()` returns valid vector for ISA IRQ 1 when IOAPIC present; invalid GSI returns 0. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. Unified Timer Subsystem (UTS)

A HAL that selects the best available timer clock and exposes a single `uptime_ns()` function to the entire kernel.

**Files:** `include/kernel/timer.h`, `src/kernel/timer.c`, `src/kernel/drivers/pit.c`, `src/kernel/drivers/lapic.c`, `src/kernel/drivers/hpet.c`

- [x] `timer_driver_t` vtable exists with `name`, `init`, `get_ticks`, `sleep_ms`, `get_freq`; added `read_ns` field
- [x] PIT and LAPIC drivers implemented and working (in `pit.c` and `lapic.c`)
- [ ] Single clocksource contract: UTS `read_ns` delegates to `mono_ns()` (`mono_clock.c`, D02 T08 §4) instead of growing a second HPET path; UTS keeps tick/event delivery; UC-map any HPET MMIO (TODO-10 §1)
- [ ] One-shot / TSC-deadline LAPIC mode: `timer_driver_t` gains `arm_oneshot(deadline_ns)` via `IA32_TSC_DEADLINE` (CPUID-gated, LVT one-shot fallback); tickless-idle enabler, policy owner D02 T26 idle governor
- [ ] Hyper-V reference TSC page consumer: when `boot_info.hv_flags & HV_FLAG_TSC_ENLIGHTENMENT`, prefer `HV_X64_MSR_REFERENCE_TSC` page over LAPIC calibration; init owner `02-kernel-core/TODO-09 §15`
- [x] LAPIC timer: calibration via Hyper-V MSR / PIT busy-wait already working
- [x] Selection waterfall: platform_detect() -> Hyper-V MSR -> LAPIC calibration -> PIT fallback; `hv_flags` available
- [x] `uptime_ns()` added: prefers `read_ns()`, falls back to `ticks * (1e9/freq)`
- [x] Serial log: `UTS: LAPIC selected (Hyper-V, 200000 ticks/ms = 200 MHz bus)` emitted
- [x] Commit: `"kernel: UTS uptime_ns() + read_ns vtable extension"`
- [ ] Add backend-aware `timer_hal_quiesce()`/`resume()` masking the ACTIVE timer (LAPIC LVT or PIT IOAPIC IRQ0); `uefi_runtime.c` rt_call masks LAPIC only, so the PIT backend (TCG) fires into firmware during UEFI RT calls. Filed from D01 T10 §10.

**Test checkpoint:** Serial shows `UTS:` line with active driver name; `uptime_ns()` increases monotonically. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. LAPIC Timer Calibration

Measure the LAPIC timer frequency per CPU using HPET or PIT as a reference, then switch the scheduler to LAPIC-driven ticks.

**Files:** `src/kernel/drivers/lapic.c`, `src/kernel/sched/task.c` (scheduler tick hookup)

- [x] LAPIC calibration: 4-tier waterfall (Hyper-V MSR → CPUID 0x15 → TSC-referenced → PM Timer → PIT ch2) in `lapic.c`; stores `cal_ticks_per_ms`. HPET tier uses `vmm_map_mmio_uc()` when that path runs (TODO-10 §1); WB caching caused MCE before UC mapping. TSC-referenced tier added 2026-03-28 for bare metal.
- [x] Run on BSP during `timer_hal_init()` -- confirmed working in serial log
- [x] Scheduler uses LAPIC periodic timer at 100 Hz; `sched_tick` driven by LAPIC ISR
- [ ] AP LAPIC timer bring-up: program + unmask the LVT timer per AP with BSP calibration, per-CPU `sched_tick` (today APs are masked, BSP-only tick); consumer D03 T07 §3 per-CPU run queues
- [ ] Recalibrate hook for CPU frequency changes -- deferred to `02-kernel-core/TODO-26-power-management.md` §15
- [x] Serial log: `LAPIC timer: periodic, vec=34, ICR=N, div=1 (calibrated, 100 Hz target)`
- [x] Already implemented -- marking complete
- [x] Commit: "(shipped) LAPIC timer calibration waterfall + scheduler 100 Hz tick"

**Test checkpoint:** Serial shows `LAPIC timer: periodic` with calibrated ICR; scheduler ticks without triple fault. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Migrate Boot Splash Spinner Off PIT Callback

Remove the boot splash spinner's direct dependency on PIT IRQ 0 so it works on LAPIC-only platforms.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/boot_timing.c`

- [x] Spinner already uses `timer_register_tick_callback(spinner_advance, 10)` in `spinner_start()` -- driven by UTS, not PIT directly
- [x] UTS routes to LAPIC timer on Hyper-V/KVM/hardware, PIT fallback on TCG only
- [x] `spinner_advance()` called every 10 ticks (100ms at 100 Hz = 10 fps) via `timer_tick_callback_fire()` in both PIT and LAPIC ISRs
- [x] No direct PIT hook to remove -- architecture is already LAPIC-safe
- [x] Verified working on Hyper-V (LAPIC) and QEMU TCG (PIT)
- [x] Already implemented -- marking complete
- [x] Commit: "(shipped) boot splash spinner driven by UTS tick callback"

**Test checkpoint:** Spinner advances on LAPIC path (WHPX) and PIT fallback (TCG); no direct PIT-only hook required. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. Boot Time Visualization

JSON boot timeline on disk plus optional shell charting later (overlay bar removed).

**Files:** `src/kernel/main/boot_progress.c`, `include/kernel/boot_progress.h` -- `src/shell/cmd/boot_timeline.c` deferred (not in tree)

- [x] Overlay bar removed (per user preference) -- JSON timeline is the diagnostic output
- [x] `boot_timeline_dump_json()`: writes `X:\Perf\boot-timeline.json` (path moved from `X:\Boot\` when FPDT records joined the timeline; see TODO-04 §4) with `[{"stage":"fpdt:reset_end","phase":0,"post":"0x0000","start_ms":0,"duration_ms":50,"source":"fpdt","unreliable":false},{"stage":"PMM","phase":0,"post":"0x20","start_ms":86,"duration_ms":7,"source":"tsc","unreliable":false},...]`
- [x] Called from `boot_phase3()` after `boot_timing_write_report()`
- [ ] `boot-timeline` shell command -- deferred to `09-desktop-shell/TODO-12-utilities.md` §1 (item: "Kernel diagnostic commands")
- [x] Commit: `"kernel: boot timeline JSON dump"`

**Test checkpoint:** With BlackBox mounted, `X:\Perf\boot-timeline.json` exists after desktop boot when dump path enabled; JSON contains stage objects. `boot-timeline` shell deferred to `09-desktop-shell/TODO-12-utilities.md` §1. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. Remove Hyper-V Debug Workarounds

Clean up all `#ifdef HYPERV_WORKAROUND` blocks now that correct ACPI/LAPIC/IOAPIC init order is in place.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/lapic.c`, `src/kernel/smp/smp.c`

- [x] Audit: no `#ifdef HYPERV_WORKAROUND`, `HV_QUIRK`, or `HV_BAR` blocks remain -- all removed during TODO-02 §2/§4/§8 restructure
- [x] No workaround paths to remove -- correct ACPI/LAPIC/IOAPIC init order established in Phase 1
- [ ] SMP IPI abstraction (`irq_send_ipi` style) -- deferred to `03-memory-concurrency/TODO-07-smp-phase2.md` §2 (today `lapic_send_ipi()` is the direct path)
- [x] Commit: "(shipped) Hyper-V workaround compile-time blocks removed -- audit clean"

**Test checkpoint:** `rg HYPERV_WORKAROUND src/kernel` returns no matches; SMP bringup logs AP online lines on `-smp 4` QEMU. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐  | Feature                      | 🪟 Win11                         | 🐧 Linux                          | 🚀 Impossible OS                   |
| --- | ---------------------------- | -------------------------------- | --------------------------------- | ---------------------------------- |
| 💎 | MADT-driven topology         | ✅ HAL MADT first                | ✅ acpi MADT first                | ✅ §1 done                         |
| 💎 | LAPIC/IOAPIC before PIT      | ✅ HAL APIC first                | ✅ APIC before IRQ                | ✅ §2 done                         |
| 💎 | Conditional PIC disable      | ✅ PCAT gated                    | ✅ mask 8259A                     | ✅ §3 done                         |
| 💎 | Full IDT coverage            | ✅ KiUnexpectedInterrupt         | ✅ spurious path                  | ✅ §4 IDT full                     |
| 💎 | Dynamic IRQ registration     | ✅ IoConnectInterrupt            | ✅ request_irq                    | [/] §5 GSI only                    |
| 💎 | Shared line IRQs (INTx)      | ✅ line-based sharing            | ✅ IRQF_SHARED                    | ⬜ §5 single-handler               |
| 💎 | Unified timer HAL            | ✅ QPC picks source              | ✅ clocksource framework          | [/] §6 LAPIC PIT                   |
| 💎 | One-shot / TSC-deadline tick | ✅ dynamic tick                  | ✅ NO_HZ tsc-deadline             | ⬜ §6 periodic only                |
| 💎 | MSI / MSI-X (PCI)            | ✅ IoConnectInterruptEx          | ✅ pci MSI vectors                | ⬜ D04T02 §3                       |
| 💎 | LAPIC timer calibration      | ✅ HAL per CPU cal               | ✅ calibrate delay                | [/] §7 BSP only                    |
| ⭐  | Boot timeline JSON           | ❌ WPA offline trace             | ❌ systemd analyze post           | ✅ §9 JSON file                    |

> **Parity:** §1--§4, §8, §10 match Windows/Linux for APIC/IDT paths. **[/]** §5 (affinity, GSI validation, shared handlers, storm quarantine, driver migration), §6 (mono_clock contract, one-shot/TSC-deadline), §7 (AP LAPIC timer) remain. **MSI/MSI-X** is parity owned by `04-drivers-hardware/TODO-02-apic-interrupt-routing.md` §3 (OS table row). Boot timeline JSON (§9) goes beyond both. Bare-metal LAPIC/PIT ISR crash fixed in TODO-10 §3 (`clac` removal).

---

## Unit Tests

> Wire into `src/kernel/test/test_runner.c` / `test_runner_init()` via `test_register_irq_timer()` (same pattern as other `test_register_*` suites).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_irq_timer.c` with:
  - `acpi_madt_info()` returns non-NULL with `cpu_count >= 1` and valid `lapic_base`
  - `acpi_pcat_compat()` returns 0 or 1 (consistent with MADT flags)
  - `acpi_get_ioapic_base()` returns non-zero address when IOAPIC present
  - `irq_request_gsi(99, dummy_handler, NULL, "test")` returns 0 for invalid GSI (no crash)
  - `irq_request_gsi()` with valid GSI (e.g., ISA IRQ 1) returns non-zero vector in range 32-239
  - `irq_gsi_count()` returns 0 for an unregistered GSI
  - `uptime_ns()` returns > 0 after boot; two calls 1ms apart differ by approximately 1000000 ns (within 50% tolerance)
  - Timer driver active: `g_system_timer` non-NULL and `g_system_timer->name` is "LAPIC" or "PIT"; when §6 mono_clock contract lands, `uptime_ns()` and `mono_ns()` agree within tolerance
  - Shared GSI: two handlers registered shared on one GSI both fire (TEST_PENDING until §5 sharing lands)
  - One-shot: `arm_oneshot(deadline)` fires exactly once near the deadline (TEST_PENDING until §6 lands)
  - AP timers: per-CPU tick counters advance on all CPUs with `-smp 4` (TEST_PENDING until §7 AP bring-up lands)
  - `hpet_available()` consistent with ACPI HPET table presence (0 or 1); `hpet_ns()` returns 0 when HPET disabled
  - IDT coverage: software `INT 0xFE` does not triple-fault (unhandled vector logs warning + EOI)
  - LAPIC spurious vector (0xFF): software `INT 0xFF` does not crash
- [ ] Register in `test_runner_init()`: `test_register_irq_timer()`
- [ ] Commit: `"test: add irq_timer test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log shows the `acpi: MADT:` summary line before any LAPIC or IOAPIC init lines
- [ ] Serial log shows `[LAPIC] Enabled` before `[PIT]` or `[HPET]` lines
- [ ] Serial log shows `[APIC] PIC disabled (PCAT_COMPAT)` in QEMU (PCAT_COMPAT = 1)
- [ ] All 256 IDT vectors filled; trigger an unmapped vector from a test path → no triple-fault, prints `[FAULT]` message
- [ ] `irq_request_gsi()` path verified via unit tests in `## Unit Tests` (until `irq list` lands in `09-desktop-shell/TODO-12-utilities.md` §1)
- [ ] `uptime_ns()` returns monotonically increasing values sampled 1 ms apart; delta ≈ 1 000 000 ns
- [ ] Serial log shows `[LAPIC] CPU0 timer: {N}MHz (calibrated against HPET)` or `PIT` fallback
- [ ] Boot splash spinner animates correctly with `-no-hpet` QEMU flag (LAPIC fallback path)
- [ ] With BlackBox mounted and dump path enabled, `X:\Perf\boot-timeline.json` exists after boot; `boot-timeline` shell deferred to `09-desktop-shell/TODO-12-utilities.md` §1 (§9)
- [ ] QEMU `-smp 4`: serial log shows `[SMP] AP1 online`, `[SMP] AP2 online`, `[SMP] AP3 online`; no Hyper-V workaround blocks compile
- [ ] Commit: `"kernel: irq-timer-arch verified -- MADT APIC, IDT, UTS, LAPIC calibration"`

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)

---

## History

| Date       | Action   | Summary |
| ---------- | -------- | ------- |
| 2026-04-10 | validate | Full-file validate: orphan `## N.` headings removed; §7/§9 Files paths; Goal + exclusive footnote vs JSON; Verification + `run-boot-tests.bat`; Unit Tests → `test_runner.c` + `g_system_timer->name`; `irq list` naming; no blank line before `Commit:`; Outcome `irq_request_gsi` + `lapic_send_ipi`; deferred lines point to TODO-22 §15 / smp-phase2 §2; XREF `lapic_timer_calibrate()`; back-XREF added in `03-memory-concurrency/TODO-07-smp-phase2.md` Inputs. Parity table checked. |
| 2026-04-11 | gap-analysis | Web: MS Learn IoConnectInterrupt/passive ISR; kernel.org timers index; Win QPC vs TSC/HPET; Linux clocksource + IRQ affinity. Code: `boot_phase1` = GDT→IDT→ACPI→LAPIC…→`timer_hal_init`; `hpet_ns` exists; `irq_request_gsi` wired. Patched Outcome/§1/§2/§6 text; Impl §5§6→`[/]`; OS rows MSI + partial §5§6; Inputs `hpet` + TODO-05 XREF; TODO-05 XREF §6 fix; Unit Tests HPET assertions. |
| 2026-04-12 | validate     | validate-todo-file: IMPORTANT + Outcome init order vs `boot_interrupts.c`; OS table five-word cells + padded header; shell deferrals → `TODO-05-desktop-shell.md`; Impl Order comma spacing; Inputs add `acpi.c`; separator `---`; parity note unchanged meaning. |
