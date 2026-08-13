---
schema_version: 1
id: interrupt-timer-arch
domain: 01-boot-platform
status: active
title: "TODO-11 -- Interrupt Architecture & Unified Timer Subsystem"
---

# TODO-11 -- Interrupt Architecture & Unified Timer Subsystem

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Define the authoritative Phase 1 interrupt and timer architecture: ACPI MADT first, LAPIC/IOAPIC before legacy PIC enable paths where applicable, full 256-vector IDT coverage, dynamic GSI/vector registration (`irq_request_gsi`), and a unified timer HAL (`uptime_ns()`) choosing HPET, LAPIC, or PIT. Historical bug class: PIT before IOAPIC routing on PCAT_COMPAT machines; that init order is fixed in tree. Remaining work: UTS high-res reads via the `mono_clock` contract, one-shot/TSC-deadline mode, IRQ affinity + GSI validation + shared INTx handlers + storm quarantine, driver migration to `irq_request_gsi`, AP LAPIC timers, shell tools (`irq list`, `boot-timeline`), and unit tests below.

> [!IMPORTANT]
> **Current state (2026-04-11):** Phase 1 order matches `boot_interrupts.c`: GDT → IDT → ACPI (MADT) → LAPIC/IOAPIC → PIC path → services → `timer_hal_init()` before `sti` (see `02-kernel-core/TODO-01-kernel-init-sequencing.md` §3). PIT-before-IOAPIC routing hazard is **fixed in tree**. Open work (gap-audit 2026-06-11, §5 shipped 2026-06-11): §6 mono_clock single-clocksource contract + one-shot/TSC-deadline mode + HV TSC page + quiesce/resume; §7 AP LAPIC timer bring-up (BSP-only today); shell tools in `09-desktop-shell/TODO-12-utilities.md` §1; Verification below.
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

| ⭐  | Order | Deliverable                         | Depends On         | Status |
| --- | :---: | ----------------------------------- | ------------------ | :----: |
| 💎  |   1   | ACPI MADT parsing                   | --                 |  [x]   |
| 💎  |   2   | LAPIC / IOAPIC init before PIT      | §1                 |  [x]   |
| 💎  |   3   | Conditional PIC disable             | §1, §2             |  [x]   |
| 💎  |   4   | Full IDT coverage                   | §2, §3             |  [x]   |
| 💎  |   5   | Dynamic IRQ registration API        | §2, §4             |  [x]   |
| 💎  |   6   | Unified timer subsystem (UTS)       | §5, TODO-09 §4     |  [x]   |
| 💎  |   7   | LAPIC timer calibration             | §6                 |  [/]   |
| 💎  |   8   | Migrate boot splash spinner off PIT | §6, §7             |  [x]   |
| ⭐  |   9   | Boot time visualization             | §7, TODO-14 §3, §6 |  [x]   |
| 💎  |  10   | Remove Hyper-V debug workarounds    | §1--§7             |  [x]   |

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
- [x] PCAT_COMPAT=0 + working IOAPIC → all PIC I/O skipped; logs "APIC-only, no PIC"; PCAT_COMPAT=0 + IOAPIC init FAILED → `boot_halt` (no controller can route external IRQs)
- [x] `pic_init()` fallback when no IOAPIC took over AND (no ACPI or PCAT_COMPAT=1)
- [x] Already implemented -- marking complete (no new code needed)
- [x] Commit: "(shipped) conditional PIC disable per ACPI PCAT_COMPAT"

**Test checkpoint:** PCAT_COMPAT=1 shows PIC disabled log; PCAT_COMPAT=0 skips PIC I/O. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | full suite 3500 PASS; dedicated irq_timer suite owned by Unit Tests section

> **Notes:**
> - Review hardened the degraded paths: APIC-only platform with failed IOAPIC now `boot_halt`s instead of booting with zero interrupt controllers.
> - `timer_hal_init()` validates the LAPIC backend; calibration failure or missing LAPIC degrades to PIT only when `pit_present()` and a controller can route it, else halts loud.
> - `lapic_timer_calibrated()` distinguishes measured frequency from the hardcoded all-tiers-failed estimate, which can no longer drive the system tick.
> - Scope: the happy paths (PCAT gated `pic_disable()`, APIC-only log, `pic_init()` fallback) were already correct; this review closed the failure paths the S2 fail-closed work made reachable.

> **Verified:** 2026-06-11 | commit `dc7e7bf0` | 5/5 items | build OK | smoke PASS (KVM 2.4s)
> **Deferred:** [M] no dedicated PIC/timer-fallback unit suite yet (reason: suite owned by TODO-level Unit Tests) -> XREF: 01-boot-platform/TODO-11 Unit Tests (item: "Create `src/kernel/test/test_irq_timer.c`" at line 287)
> **Quality reviewed:** 2026-06-11 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 4H+1M fixed, 1M accepted-XREF | scope: kernel-code-quality

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

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | full suite 3500 PASS; dedicated irq_timer suite owned by Unit Tests section

> **Notes:**
> - Review moved ISA IRQ 8-15 to vectors 0x70-0x77: the historical 0x28-0x2F window put IRQ14 on the architecturally fixed DPL=3 NT syscall gate (INT 0x2E); `isa_irq_to_vector()` is now the only mapping and `irq.c` reserves the window permanently.
> - Unhandled-vector EOI is controller-aware (`irq_vector_to_isa()` + `irq_eoi()`); PIC spurious IRQ7/15 detected via the 8259 ISR before any EOI; spurious 0xFF no longer EOIs (fixed in the S1/S2 pass).
> - All PIC-fallback registrants migrated (mouse, vbox_mouse, virtio-input, rtl8139) with irq_line<16 guards so the sentinel can never land on vector 0.
> - Static asserts in `vectors.h` now pin every DPL=3 software vector outside every hardware IRQ window.

> **Verified:** 2026-06-11 | commit `5ae7bdb8` | 7/7 items | build OK | smoke PASS (KVM 2.4s)
> **Accepted:** [H] AP NMI/#DF/MCE IST delivery needs per-CPU TSS (APs never load TR) -> XREF: 03-memory-concurrency/TODO-07 §9 (item: "Per-CPU TSS + guarded IST stacks with `ltr` on every AP" at line 223)
> **Accepted:** [H] ring-3 INT n on a DPL=0 gate panics the kernel (user DoS; kernel-wide fault-isolation gap) -> XREF: 02-kernel-core/TODO-23 §5 (item: "Delivery failure ... = terminate the process, never `panic_screen()`"). TODO-23 §4 landed the dispatcher routing but the unhandled-user terminal is still a kernel panic; §5 ring-3 delivery + per-process termination is what closes the DoS.
> **Deferred:** [M] no dedicated IDT/vector unit suite yet (reason: suite owned by TODO-level Unit Tests) -> XREF: 01-boot-platform/TODO-11 Unit Tests (item: "Create `src/kernel/test/test_irq_timer.c`" at line 287)
> **Quality reviewed:** 2026-06-11 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 6H+1L fixed, 2H accepted-XREF | scope: kernel-code-quality

---

## 5. Dynamic IRQ Registration API

Replace all hardcoded IRQ-to-vector assignments with a runtime registration API backed by the IOAPIC.

**Files:** `include/kernel/irq.h`, `src/kernel/irq.c`

> [!IMPORTANT]
> `include/kernel/irq.h` already defines `irq_handler_t` as `void (*)(uint8_t vector, void *ctx)` for the existing `irq_register(vector, handler, ctx, name)` API. This section either renames that typedef or introduces a separate `gsi_handler_t` -- resolve the naming before implementing §5 to avoid breaking all current `irq_register()` call sites.

- [x] Existing `irq_handler_t` kept as `void (*)(uint8_t vector, void *ctx)` -- no breaking change needed; GSI API added alongside
- [x] `irq_request_gsi(gsi, handler, ctx, name)`: allocates vector via `irq_alloc_vector()`, registers handler, programs IOAPIC redirection with MADT override flags; returns vector or 0
- [x] `irq_free_gsi(gsi)`: masks IOAPIC entry, unregisters handler, frees vector
- [x] `irq_set_affinity(gsi, cpu_mask)`: online-CPU validated, PASSIVE_LEVEL-only, `ioapic_set_destination()` RMW under `ioapic_lock`, route-lifetime locked
- [x] GSI routing-domain validation: `irq_request_gsi()` returns 0 and rolls back the vector when the GSI exceeds the IOAPIC redirection range (shipped in S2 review, commit 1678de87); GSI-99 unit test asserts the fix when the Unit Tests suite lands
- [x] Shared GSI handlers for PCI INTx: `irq_request_gsi_ex()` claim chains (`IRQ_HANDLED`/`IRQ_NONE`), mask+drain-protected mutation, parked-vector teardown, MADT-override-authoritative flags with loud mismatch refusal on join
- [x] IRQ storm quarantine: all-`IRQ_NONE` streak + idt.c unhandled-vector limit both mask at the owning controller; a joining sharer is the recovery point (Linux `__setup_irq` parity); `irq list` exposure deferred with that item (D09 T12 §1)
- [x] `irq_gsi_count(gsi)`: returns fire count via GSI→vector mapping table
- [x] `irq_dispatch()` already exists in `irq.c` -- registered handler called + EOI sent
- [/] `irq list` shell command -- deferred to `09-desktop-shell/TODO-12-utilities.md` §1 (item: "Kernel diagnostic commands")
- [x] Drivers migrated to shared GSI claims: AHCI, vbox_mouse, rtl8139, virtio-input, ACPI SCI (fixes the S4 SCI vector regression); PS/2 done earlier (5466cc7e); MSI/MSI-X follow `04-drivers-hardware/TODO-02-apic-interrupt-routing.md` §3
- [x] Vector-space hardening: INT 0x80/0x81 gates reserved out of the dynamic allocator (live ALPC-starvation incident), `irq_unregister` only clears IDT slots it owns, `irq_reserve_vector()` API for bare-IDT static claims
- [x] Commit: `"kernel: GSI-based IRQ request API -- irq_request_gsi/free_gsi, IOAPIC-backed"`

**Test checkpoint:** `irq_request_gsi()` returns valid vector for ISA IRQ 1 when IOAPIC present; invalid GSI returns 0. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | irq_timer: 16 suites incl. allocator exhaustion, GSI range/reserve guards | 0 failures
> **Notes:**
> - Shipped: shared-GSI claim chains + storm quarantine + vector parking + affinity in `src/kernel/irq.c`; `ioapic_set_destination()`; `test_irq_timer.c` (16 suites, TEST_CAT_BOOT).
> - Integrates: all PCI INTx consumers (AHCI, vbox_mouse, rtl8139, virtio-input, ACPI SCI) register via `irq_request_gsi_ex()`; registration order no longer matters; MADT overrides authoritative for line flags.
> - Downstream: unblocks `irq list` rendering (D09 T12 §1) and MSI/MSI-X (D04 T02 §3); Codex 13-round adversarial adoption trail lives in the ship commit message.
> - Scope boundary: MSI/MSI-X is D04 T02 §3; IRQ balancing policy is future D03 work (affinity primitive ships here); chain/parking/quarantine internals are platform-validated (live IOAPIC programming is test-banned).
> **Verified:** 2026-06-11 | commit `c05f1883` | 11/12 items | build OK | tests 4287+16 PASS, smoke PASS (KVM 2.44s)
> **Accepted:** [H] PCI INTx consumers pass PCI_INTERRUPT_LINE as the GSI (real routing needs ACPI `_PRT`/AML) -> XREF: 04-drivers-hardware/TODO-01 §5 (item: "Route legacy INTx through `_PRT`/IOAPIC when MSI is unavailable" at line 77)
> **Accepted:** [H] global `irq_chain_lock` entry gate can stall unrelated shared ISR entry during a registration drain (reason: drains are registration-lifecycle only today) -> XREF: 04-drivers-hardware/TODO-02 §3 (item: "Per-vector dispatch gating for shared GSI chains" at line 107)
> **Quality reviewed:** 2026-06-11 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1L fixed, 2H accepted-XREF, 1M rejected | scope: kernel-code-quality

---

## 6. Unified Timer Subsystem (UTS)

A HAL that selects the best available timer clock and exposes a single `uptime_ns()` function to the entire kernel.

**Files:** `include/kernel/timer.h`, `src/kernel/timer.c`, `src/kernel/drivers/pit.c`, `src/kernel/drivers/lapic.c`, `src/kernel/drivers/hpet.c`

- [x] `timer_driver_t` vtable exists with `name`, `init`, `get_ticks`, `sleep_ms`, `get_freq`; added `read_ns` field
- [x] PIT and LAPIC drivers implemented and working (in `pit.c` and `lapic.c`)
- [x] Single clocksource contract: both driver vtables set `.read_ns = mono_ns` (no second HPET path); `MONO_SRC_LAPIC` scales by the LIVE tick frequency via pure `mono_lapic_ticks_to_ns()` (resolution-change safe)
- [x] One-shot / TSC-deadline LAPIC mode: `arm_oneshot()` vtable hook, `IA32_TSC_DEADLINE` CPUID-gated with LVT one-shot fallback, overflow-safe conversion helpers; ISR auto-restores periodic mode until the D02 T26 idle governor owns tickless policy
- [/] Hyper-V reference TSC page consumer (`HV_X64_MSR_REFERENCE_TSC` preferred when enlightened): BLOCKED on the MSR/page init owned by `02-kernel-core/TODO-09 §15` (item: "Hyper-V SynIC + reference TSC MSR init")
- [x] LAPIC timer: calibration via Hyper-V MSR / PIT busy-wait already working
- [x] Selection waterfall: platform_detect() -> Hyper-V MSR -> LAPIC calibration -> PIT fallback; `hv_flags` available
- [x] `uptime_ns()` added: prefers `read_ns()`, falls back to `ticks * (1e9/freq)`
- [x] Serial log: `UTS: LAPIC selected (Hyper-V, 200000 ticks/ms = 200 MHz bus)` emitted
- [x] Commit: `"kernel: UTS uptime_ns() + read_ns vtable extension"`
- [x] Backend-aware `timer_hal_quiesce()`/`resume()` masks the ACTIVE timer (LAPIC LVT or routed PIT line); rt_call adopted it, closing the D01 T10 §10 PIT-into-firmware gap (panic path stays LAPIC-only: ioapic_lock is panic-unsafe)

**Test checkpoint:** Serial shows `UTS:` line with active driver name; `uptime_ns()` increases monotonically. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | irq_timer: 21 suites incl. clocksource contract, one-shot conversion/delegation | 0 failures
> **Notes:**
> - Shipped: `.read_ns = mono_ns` on both drivers, `arm_oneshot` vtable hook + `lapic_timer_arm_oneshot()` (TSC-deadline/LVT), `timer_hal_quiesce()/resume()` in `timer.c`, pure conversion helpers.
> - Integrates: `uefi_runtime.c` rt_call uses quiesce/resume (exercised live on every boot UEFI variable read); one-shot ISR auto-restores the periodic heartbeat; nothing arms one-shot at boot.
> - Downstream: tickless-idle mechanism ready for the D02 T26 idle governor; closes the D01 T10 §10 rt_call PIT gap (normal path).
> - Scope boundary: HV reference-TSC page init is D02 T09 §15 (consumer here stays blocked until it lands); recalibrate-on-freq-change is D02 T26 §15; AP timers are §7.
> **Verified:** 2026-06-12 | commit `00adcb9b` | 9/10 items | build OK | tests 4309+16 PASS, smoke PASS (KVM 2.69s)
> **Accepted:** [H] AP-side timer-resolution requests are refused + rolled back (BSP delegation needs a cross-CPU call) -> XREF: 03-memory-concurrency/TODO-07 §2 (item: "`smp_call_function(cpu, fn, arg)` cross-CPU synchronous call" at line 101)
> **Quality reviewed:** 2026-06-12 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M fixed, 1M accepted-XREF (raw-tick consumers migrated in TODO-08 §16) | scope: kernel-code-quality

---

## 7. LAPIC Timer Calibration

Measure the LAPIC timer frequency per CPU using HPET or PIT as a reference, then switch the scheduler to LAPIC-driven ticks.

**Files:** `src/kernel/drivers/lapic.c`, `src/kernel/sched/task.c` (scheduler tick hookup)

- [x] LAPIC calibration waterfall in `lapic.c`: Tier 1 Hyper-V MSR / VMware-KVM CPUID 0x40000010 / Intel CPUID 0x15 core-crystal rate, Tier 2 HPET (UC-mapped per TODO-10 §1) / ACPI PM Timer measurement, Tier 3 PIT ch2; every tier range-validated before success; stores `cal_ticks_per_ms` (0 on total failure). The old TSC-referenced helper is intentionally NOT called (TSC rate is not the APIC rate).
- [x] Run on BSP during `timer_hal_init()` -- confirmed working in serial log
- [x] Scheduler uses LAPIC periodic timer at 100 Hz; `sched_tick` driven by LAPIC ISR
- [/] AP LAPIC timer bring-up: per-AP LVT timer programmed + unmasked with BSP calibration, per-CPU `sched_tick` (APs masked today). BLOCKED on per-CPU run queues -> XREF: 03-memory-concurrency/TODO-07 §3 (item: "Allocate `g_rq[MAX_CPUS]`")
- [/] Recalibrate hook for CPU frequency changes -- BLOCKED on the cpufreq governor -> XREF: 02-kernel-core/TODO-26 §15 (item: "CPU Frequency Scaling Governor Framework" at line 573)
- [x] Serial log: `LAPIC timer: periodic, vec=34, ICR=N, div=1 (calibrated, 100 Hz target)`
- [x] Already implemented -- marking complete
- [x] Commit: "(shipped) LAPIC timer calibration waterfall + scheduler 100 Hz tick"

**Test checkpoint:** Serial shows `LAPIC timer: periodic` with calibrated ICR; scheduler ticks without triple fault. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | irq_timer: LAPIC calibration state suite | 0 failures
> **Notes:**
> - Shipped (review hardening): every calibration tier range-validated before success; CPUID 0x15 stores the core-crystal rate (not TSC) with CPUID 0x16 derivation when ECX=0; ticks_per_ms stays 0 on total failure; u64 ICR clamps at all four programming sites; `lapic_sleep_ms` ceiling with 1-tick floor.
> - Integrates: `timer_hal_init()` runs the waterfall on the BSP; the 100 Hz tick drives kusd/NT-timers/DPC/schedule; `LAPIC_TIMER_VECTOR` now aliases the central `VECTOR_LAPIC_TIMER` registry symbol.
> - Downstream: per-finding evidence in the review commit; NT-timer wheel and DPC ISR budget filed with their owners (see Accepted lines).
> - Scope boundary: AP LAPIC timers stay masked until per-CPU run queues land (own open item below); recalibrate-on-frequency-change is owned by the cpufreq governor section.
> **Verified:** 2026-06-12 | commit `4208a2e0` | 5/7 items | build OK | tests 4309+16 PASS, smoke PASS (KVM 2.69s)
> **Deferred:** [H] AP LAPIC timer bring-up (BSP-only tick today) blocked on per-CPU scheduler infrastructure -> XREF: 03-memory-concurrency/TODO-07 §3 (item: "Allocate `g_rq[MAX_CPUS]`; initialise each during `sched_init_cpu(cpu_id)` called by each AP" at line 116)
> **Accepted:** [M] recalibrate hook for CPU frequency changes -> XREF: 02-kernel-core/TODO-26 §15 (item: "Timer recalibration on frequency transition" at line 608)
> **Accepted:** [H] `nt_timer_tick()` walks every armed timer in the 100 Hz ISR (O(N) IRQ-off work at scale) -> XREF: 02-kernel-core/TODO-05 (item: "Replace the flat NT timer armed list with an ordered structure" at line 245)
> **Accepted:** [M] DPC drain in the timer ISR caps count (32) but not per-callback runtime -> XREF: 02-kernel-core/TODO-07 §6 (item: "DPC runtime budget in the timer ISR drain" at line 211)
> **Quality reviewed:** 2026-06-12 | Codex 5x (adversarial, consistency, perf, re-adversarial) | 4H+3M fixed, 1H+2M accepted-XREF | scope: kernel-code-quality

---

## 8. Migrate Boot Splash Spinner Off PIT Callback

Remove the boot splash spinner's direct dependency on PIT IRQ 0 so it works on LAPIC-only platforms.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/boot_timing.c`

- [x] Spinner uses `timer_register_tick_callback(spinner_advance, 10)` in `spinner_start()` -- driven by UTS, not PIT directly
- [x] UTS routes to LAPIC timer on Hyper-V/KVM/hardware, PIT fallback on TCG only
- [x] `spinner_advance()` called every 10 ticks (100ms at 100 Hz = 10 fps) via `timer_tick_callback_fire()` in both PIT and LAPIC ISRs
- [x] Legacy unused `pit_register_callback`/`pit_unregister_callback` direct-PIT hook DELETED during review (zero callers; UTS singleton slot supersedes it); `pit_tick_increment()` no longer fires the callback under `pit_lock`
- [x] `pit_irq_handler` post-tick sequence mirrors `lapic_timer_handler` (tick+callback, EOI, KUSD, NT timer scan, DPC drain, schedule)
- [x] Tick-callback writers serialized by `s_tick_cb_lock` (irqsave) with retract-first publication; BSP-only writer gate refuses AP callers (tick ISR is BSP-only)
- [x] Verified working on Hyper-V (LAPIC) and QEMU TCG (PIT)
- [x] Commit: "(shipped) boot splash spinner driven by UTS tick callback"

**Test checkpoint:** Spinner advances on LAPIC path (WHPX) and PIT fallback (TCG); no direct PIT-only hook remains. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | tick-callback suites in `test_timer_tick_cb.c`, 0 failures

> **Notes:**
> - Spinner animation is fully UTS-driven: `spinner.c` registers the singleton tick callback; both PIT and LAPIC ISRs fire it.
> - Review pass deleted the consumerless legacy PIT callback API and hoisted the callback fire out of `pit_lock` (lock-hold-time rule).
> - PIT and LAPIC tick paths now run an identical post-tick sequence so NT timers wake against fresh KUSD time on both backends.
> - Tick-callback register/unregister is BSP-only (gated, klog WARN on AP) with retract-first publication under `s_tick_cb_lock`.
> - Scope boundary: AP/per-CPU tick delivery arrives with AP timer bring-up in `03-memory-concurrency/TODO-07-smp-phase2.md` §3.

> **Verified:** 2026-06-12 | commit `d76bd3e1` | 7/7 items | build OK | smoke PASS (KVM 2.620s)
> **Quality reviewed:** 2026-06-12 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 4M fixed, 0 open | scope: kernel-code-quality

---

## 9. Boot Time Visualization

JSON boot timeline on disk plus optional shell charting later (overlay bar removed).

**Files:** `src/kernel/main/boot_progress.c`, `include/kernel/boot_progress.h` -- `src/shell/cmd/boot_timeline.c` deferred (not in tree)

- [x] Overlay bar removed (per user preference) -- JSON timeline is the diagnostic output
- [x] `boot_timeline_dump_json()`: writes `X:\Perf\boot-timeline.json` (path moved from `X:\Boot\` when FPDT records joined the timeline; see TODO-04 §4) with `[{"stage":"fpdt:reset_end","phase":0,"post":"0x0000","start_ms":0,"duration_ms":50,"target_ms":0,"source":"fpdt","unreliable":false},{"stage":"PMM","phase":0,"post":"0x20","start_ms":86,"duration_ms":7,"target_ms":10,"source":"tsc","unreliable":false},...]` (`target_ms` = per-step boot_perf_budget target, 0 when unbudgeted)
- [x] Called from `boot_desktop.c` (Phase 3 desktop-ready path) after `boot_timing_write_report()`
- [x] Single-open `VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC` write with fail-closed short-write handling (re-truncate to empty; same pattern as `boot_health.c`) -- added during review; previous bare `VFS_O_WRITE` left stale tail bytes across boots
- [/] `boot-timeline` shell command -- deferred to `09-desktop-shell/TODO-12-utilities.md` §1 (item: "Kernel diagnostic commands")
- [x] Commit: `"kernel: boot timeline JSON dump"`

**Test checkpoint:** With BlackBox mounted, `X:\Perf\boot-timeline.json` exists after desktop boot when dump path enabled; JSON contains stage objects. `boot-timeline` shell deferred to `09-desktop-shell/TODO-12-utilities.md` §1. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | FPDT/timing producer suites in `test_boot_timing.c`, 0 failures; JSON dump itself is live-VFS (file presence validated by boot checkpoint)

> **Notes:**
> - `boot_timeline_dump_json()` (boot_progress.c) emits FPDT firmware phases + TSC kernel steps as one JSON array to `X:\Perf\boot-timeline.json` (fallback `C:\Impossible\System\Logs\`).
> - Record schema: stage/phase/post/start_ms/duration_ms/target_ms/source/unreliable; reliability tracking marks saturated or reverse-ordered TSC data.
> - Review pass added create+truncate single-open with fail-closed short-write handling so consumers never see stale tails or partial JSON.
> - Canonical schema contract lives in `include/kernel/boot_progress.h` above `boot_timeline_dump_json()`.
> - Scope boundary: rendering (`boot-timeline` shell command) is owned by `09-desktop-shell/TODO-12-utilities.md` §1.
> - re-adversarial skipped: single-function fail-closed write fix + doc sync; no locking/ISR/lifecycle changes.

> **Verified:** 2026-06-12 | commit `98592c32` | 4/5 items | build OK | smoke PASS (KVM 2.730s)
> **Accepted:** [L] `boot-timeline` shell renderer out of scope for kernel dump (reason: scope) -> XREF: 09-desktop-shell/TODO-12 §1 (item: "Kernel diagnostic commands" at line 82)
> **Quality reviewed:** 2026-06-12 | Codex 3x (adversarial, consistency, perf) | 3M fixed, 0 open | scope: kernel-code-quality

---

## 10. Remove Hyper-V Debug Workarounds

Clean up all `#ifdef HYPERV_WORKAROUND` blocks now that correct ACPI/LAPIC/IOAPIC init order is in place.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/lapic.c`, `src/kernel/smp/smp.c`

- [x] Audit: no `#ifdef HYPERV_WORKAROUND`, `HV_QUIRK`, or `HV_BAR` blocks remain in `src/`/`include/` (TODO-14's HV_BAR origin note is history, not code) -- removed during TODO-02 §2/§4/§8 restructure
- [x] No workaround paths to remove -- correct ACPI/LAPIC/IOAPIC init order established in Phase 1
- [x] APIC-frequency capability gate unified (review fix): `HV_FLAG_APIC_FREQ_MSR` probed for KVM like VMware; `platform_has_apic_freq_msr()` and `cal_try_vmware_cpuid()` both gate on the flag instead of platform identity
- [x] Calibration loops wall-clock bounded (review fix): `cal_deadline()` (4x 10ms window from boot TSC freq, plausibility-gated 1 MHz through 10 GHz) caps the HPET/PM/PIT measurement spins that previously relied on a 200M-iteration count alone
- [/] SMP IPI abstraction (`irq_send_ipi` style) -- deferred to `03-memory-concurrency/TODO-07-smp-phase2.md` §2 (today `lapic_send_ipi()` is the direct path)
- [x] Commit: "(shipped) Hyper-V workaround compile-time blocks removed -- audit clean"

**Test checkpoint:** `rg HYPERV_WORKAROUND src/kernel` returns no matches; SMP bringup logs AP online lines on `-smp 4` QEMU. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** N/A (audit + boot-path capability gating; no pure-helper surface) | validation: smoke PASS + full suite green + serial calibration tier logs on WHPX

> **Notes:**
> - Audit clean: no HYPERV_WORKAROUND/HV_QUIRK/HV_BAR blocks in `src/`/`include/`; AP bringup uses per-AP INIT->SIPI (no deprecated Init Level De-Assert broadcast).
> - Review unified the APIC-frequency capability contract: one `HV_FLAG_APIC_FREQ_MSR` truth across `cpuid_platform.c` detection, `platform_has_apic_freq_msr()`, and the Tier 1 calibration gate.
> - Review added `cal_deadline()` so a dead-but-advertised HPET/PM/PIT cannot burn tens of seconds with interrupts off before the tier falls through.
> - The TCG-selects-PIT branch in `timer_hal_init()` stays: TCG TSC is instruction-counted (LAPIC-derived time skews vs wall clock); replacement is the tracked clocksource watchdog.
> - Scope boundary: IPI HAL wrapper owned by `03-memory-concurrency/TODO-07-smp-phase2.md` §2; AP bringup timing owned by `01-boot-platform/TODO-09` §10.

> **Verified:** 2026-06-12 | commit `7ea3a01f` | 4/5 items | build OK | smoke PASS (KVM 2.590s)
> **Accepted:** [H] `platform_is_tcg()` timer-backend gate is identity-based; proper fix is a clocksource quality watchdog (reason: TCG test platform depends on PIT today) -> XREF: 02-kernel-core/TODO-08 §2 (item: "Clocksource quality watchdog" at line 80)
> **Accepted:** [M] AP bringup serializes 10ms INIT settle + 1ms SIPI per AP (reason: INIT/SIPI restructure needs bare-metal validation) -> XREF: 01-boot-platform/TODO-09 §10 (item: "Phase-split INIT settle + 200us SIPI wait" at line 400)
> **Accepted:** [L] direct `lapic_send_ipi()` callers pending arch-neutral wrapper (reason: scope) -> XREF: 03-memory-concurrency/TODO-07 §2 (item: "irq_send_ipi(cpu, vector) arch-neutral wrapper" at line 98)
> **Quality reviewed:** 2026-06-12 | Codex 5x (adversarial, consistency, perf, re-adversarial x2) | 1H+4M+1L fixed, 0 open, 1H+1M+1L accepted-XREF | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                      | 🪟 Win11                 | 🐧 Linux                 | 🚀 Impossible OS                  |
| --- | ---------------------------- | ------------------------ | ------------------------ | --------------------------------- |
| 💎  | MADT-driven topology         | ✅ HAL MADT first        | ✅ acpi MADT first       | ✅ §1 done                        |
| 💎  | LAPIC/IOAPIC before PIT      | ✅ HAL APIC first        | ✅ APIC before IRQ       | ✅ §2 done                        |
| 💎  | Conditional PIC disable      | ✅ PCAT gated            | ✅ mask 8259A            | ✅ §3 done                        |
| 💎  | Full IDT coverage            | ✅ KiUnexpectedInterrupt | ✅ spurious path         | ✅ §4 IDT full                    |
| 💎  | Dynamic IRQ registration     | ✅ IoConnectInterrupt    | ✅ request_irq           | ✅ §5 GSI + affinity              |
| 💎  | Shared line IRQs (INTx)      | ✅ line-based sharing    | ✅ IRQF_SHARED           | ✅ §5 claim chains + storm guard  |
| 💎  | Unified timer HAL            | ✅ QPC picks source      | ✅ clocksource framework | ✅ §6 mono_clock contract         |
| 💎  | One-shot / TSC-deadline tick | ✅ dynamic tick          | ✅ NO_HZ tsc-deadline    | ✅ §6 mechanism (governor D02T26) |
| 💎  | MSI / MSI-X (PCI)            | ✅ IoConnectInterruptEx  | ✅ pci MSI vectors       | ⬜ D04T02 §3                      |
| 💎  | LAPIC timer calibration      | ✅ HAL per CPU cal       | ✅ calibrate delay       | [/] §7 BSP only                   |
| ⭐  | Boot timeline JSON           | ❌ WPA offline trace     | ❌ systemd analyze post  | ✅ §9 JSON file                   |

> **Parity:** §1--§6, §8, §10 match Windows/Linux for APIC/IDT/IRQ-registration/timer-HAL paths. **[/]** §7 (AP LAPIC timer) remains; §6's HV reference-TSC consumer is blocked on D02 T09 §15. **MSI/MSI-X** is parity owned by `04-drivers-hardware/TODO-02-apic-interrupt-routing.md` §3 (OS table row). Boot timeline JSON (§9) goes beyond both. Bare-metal LAPIC/PIT ISR crash fixed in TODO-10 §3 (`clac` removal).

---

## Unit Tests

> Wire into `src/kernel/test/test_runner.c` / `test_runner_init()` via `test_register_irq_timer()` (same pattern as other `test_register_*` suites).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] `src/kernel/test/test_irq_timer.c` shipped (21 suites, TEST_CAT_BOOT, registered via `test_register_irq_timer()`):
  - MADT consolidated info populated + mirrors legacy accessors; PCAT bit boolean
  - ISA vector translation both directions (incl. IRQ9 SCI -> 0x71, 0x2E non-ISA)
  - allocator exhaustion walk: ISA-window avoidance, dynamic-range bounds, 0x80/0x81 avoidance, depleted-terminal 0, recovery
  - GSI validation: NULL-handler + out-of-range isolated (dummy-handler range guards), reserve-vector collision refusals, affinity validation, reverse-map oracles
  - UTS: `g_system_timer` selected, `uptime_ns()` monotonic, HPET consistency, LAPIC calibration state
  - **Note:** live-GSI registration (valid-GSI vector assertion from the original draft) is test-banned (live IOAPIC programming); covered by smoke + QEMU serial instead
  - Shared GSI: gsi_ex validation covers shared-flag plumbing; live two-handler fire is test-banned (live IOAPIC) -- covered by smoke + QEMU serial
  - One-shot: surface + conversion helpers + pure delegation suites shipped with §6 (fake-driver based; live deadline fire is test-banned)
  - AP timers: per-CPU tick counters advance on all CPUs with `-smp 4` (pending §7 AP bring-up; see §7 Deferred XREF to D03 TODO-07 §3)
  - `hpet_available()` consistent with ACPI HPET table presence (0 or 1); `hpet_ns()` returns 0 when HPET disabled
  - IDT coverage: software `INT 0xFE` does not triple-fault (unhandled vector logs warning + EOI)
  - LAPIC spurious vector (0xFF): software `INT 0xFF` does not crash
- [x] Register in `test_runner_init()`: `test_register_irq_timer()` (`test_runner.c:469`)
- [x] Commit: shipped across §5/§6 section commits `0107203c` + `02d9e908` (suite grew with each section, 21 suites total)

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
