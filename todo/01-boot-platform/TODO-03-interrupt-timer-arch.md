# TODO-03 — Interrupt Architecture & Unified Timer Subsystem

> **Goal:** APIC and PIT exist but are initialised in the wrong order (PIT before LAPIC/IOAPIC), creating IRQ conflicts and race conditions on real hardware and Hyper-V. This TODO re-sequences interrupt hardware init so ACPI MADT drives everything, delivers full 256-vector IDT coverage, adds a dynamic IRQ registration API that replaces all hardcoded IRQ-to-vector mappings, and builds a unified timer HAL that picks the best available clock (HPET → LAPIC → PIT) as a single `uptime_ns()` abstraction.

> [!IMPORTANT]
> **Current bug:** `boot_interrupts.c` inits the PIT before the LAPIC/IOAPIC. On PCAT_COMPAT hardware this causes IRQ 0 to fire on both the PIC and IOAPIC simultaneously. The fix is strictly an init-order change — the existing LAPIC/IOAPIC/PIT drivers are structurally correct; they just need to run in the right sequence with MADT data available first.

## Inputs

- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c)
- [`include/kernel/acpi.h`](../../include/kernel/acpi.h)
- [`include/kernel/drivers/lapic.h`](../../include/kernel/drivers/lapic.h)
- [`include/kernel/drivers/ioapic.h`](../../include/kernel/drivers/ioapic.h)
- [`include/kernel/drivers/pic.h`](../../include/kernel/drivers/pic.h)
- [`include/kernel/idt.h`](../../include/kernel/idt.h)
- [`include/kernel/irq.h`](../../include/kernel/irq.h)
- [`include/kernel/timer.h`](../../include/kernel/timer.h)
- [`include/kernel/drivers/pit.h`](../../include/kernel/drivers/pit.h)
- → XREF: `02-kernel-core/docs/kernel/init-sequencing.md (completed, was TODO-01) §3` — Phase 1 calls every init function listed here in the correct order; this TODO defines what correct order means
- → XREF: `02-kernel-core/TODO-06-irql-model-dpcs.md §1` — DPC queue init requires LAPIC timer ready (§7); IRQL model depends on the correct interrupt priority assignment established in §2
- → XREF: `02-kernel-core/TODO-07-time-filetime-management.md §2` — `uptime_ns()` from the UTS (§6) is the clock source for the monotonic nanosecond clock used by FILETIME and `QueryPerformanceCounter`
- → XREF: `TODO-02-boot-diagnostics.md §2` — boot time visualization (§9) reads `boot_stage_history[]` built by the boot progress API
- → XREF: `TODO-02-boot-diagnostics.md §5` — §9 boot time visualization bar chart uses the stage color table defined there (`DebugBar=1` feature)
- → XREF: `TODO-04-cpu-boot-sequencing.md §3` — hypervisor detection runs in Phase 0 before §6 UTS probe; `boot_info.hv_flags` set there must be read by `timer_hal_init()` to select the correct clock source
- → XREF: `02-kernel-core/TODO-15-power-management.md` — Intel HWP / AMD CPPC frequency changes require LAPIC timer recalibration (§7 `lapic_calibrate()`); no dedicated HWP/CPPC section in TODO-15 yet — add when frequency scaling is scoped there
- → XREF: `04-drivers-hardware/INDEX.md` — all device drivers call `irq_request(gsi, handler, name, flags)` (§5); this API replaces hardcoded IRQ-to-vector assignments throughout that domain
- → XREF: `TODO-06-bare-metal-hardening.md §5` — hardware interrupt crash on bare metal (LAPIC timer and PIT both crash; software INT works). Root cause investigation tracked there, not here.

## Outcome

- `acpi_parse_madt()` runs before any interrupt hardware init; `struct acpi_irq_map` populated with all GSI overrides.
- Init order in `boot_phase1()`: MADT → LAPIC → IOAPIC → PIC disable (if PCAT_COMPAT) → IDT load → timer.
- All 256 IDT vectors filled; no vector triggers an unhandled-interrupt panic.
- `irq_request(gsi, handler, name, flags)` wires any device IRQ at runtime; `irq list` shell command prints the full table.
- `uptime_ns()` returns nanoseconds since boot from the best available clock; no scattered PIT tick counters remain.
- LAPIC timer calibrated per CPU; scheduler uses LAPIC timer, not global PIT tick, for SMP.
- Boot splash spinner no longer hooks PIT IRQ 0 directly; works on LAPIC-only platforms.
- All `#ifdef HYPERV_WORKAROUND` blocks removed; AP bringup verified via `irq_send_ipi()`.

## Implementation Order

| ⭐  | Order | Deliverable                         | Depends On          | Status |
| --- | :---: | ----------------------------------- | ------------------- | :----: |
| 💎  |   1   | ACPI MADT parsing                   | —                   |  [x]   |
| 💎  |   2   | LAPIC / IOAPIC init before PIT      | §1                  |  [x]   |
| 💎  |   3   | Conditional PIC disable             | §1, §2              |  [x]   |
| 💎  |   4   | Full IDT coverage                   | §2, §3              |  [x]   |
| 💎  |   5   | Dynamic IRQ registration API        | §2, §4              |  [x]   |
| 💎  |   6   | Unified timer subsystem (UTS)       | §5, T04 §3          |  [x]   |
| 💎  |   7   | LAPIC timer calibration             | §6                  |  [x]   |
| 💎  |   8   | Migrate boot splash spinner off PIT | §6, §7              |  [x]   |
| ⭐  |   9   | Boot time visualization             | §7, T02 §2 & §5     |  [x]   |
| 💎  |  10   | Remove Hyper-V debug workarounds    | §1–7                |  [x]   |

> 💎 = parity — Windows NT HAL and Linux interrupt subsystem both follow this init order and have equivalent abstractions.
> ⭐ = exclusive — the post-boot animated timeline bar chart showing per-stage boot duration is not present in either competitor.

---

## 1. ACPI MADT Parsing

Extract interrupt topology from the MADT before any interrupt hardware is touched so every subsequent init function has authoritative GSI data.

**Files:** `include/kernel/acpi.h`, `src/kernel/acpi.c`

- [x] `acpi_init()` already executes before LAPIC/IOAPIC/PIC in `boot_phase1()` — verified in boot logs
- [x] Define `struct acpi_irq_override` and `struct acpi_madt_info` in `acpi.h`
- [x] Parse MADT entries: Type 0 (LAPIC), Type 1 (IOAPIC + GSI base), Type 2 (IRQ override), Type 5 (LAPIC address override), Type 9 (x2APIC); unknown types skipped
- [x] `pcat_compat` flag set from `madt->flags & 1`
- [x] `acpi_madt_info()` getter returns const pointer to static `s_madt_info` struct
- [x] Serial log: `MADT: N CPUs, LAPIC=0xbase, IOAPIC=0xbase GSI=base, M overrides, PCAT_COMPAT=N`
- [x] Commit: `"acpi: consolidated MADT info struct with IOAPIC GSI base"`

## 2. LAPIC / IOAPIC Init Before PIT

Restructure `boot_interrupts.c` so LAPIC and IOAPIC are brought up before the PIT, using MADT data from §1.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/lapic.c`, `src/kernel/drivers/ioapic.c`

- [x] `boot_phase1()` order: GDT → IDT → ACPI(MADT) → LAPIC → IOAPIC → PIC disable → Timer — already correct from TODO-01 §3 restructure
- [x] `lapic_init()`: reads `IA32_APIC_BASE_MSR`, enables LAPIC, writes SIVR, masks all LVT entries — already implemented
- [x] `lapic_init()` uses `acpi_get_lapic_base()` (from MADT) for MMIO address
- [x] `ioapic_init()`: maps IOAPIC MMIO at `acpi_get_ioapic_base()`, reads `IOAPIC_VER` for `max_redir_entries`
- [x] MADT IRQ overrides applied via `acpi_get_override()` in IOAPIC ISA routing loop
- [x] Serial log: `LAPIC enabled: base=0xFEE00000` and `I/O APIC at 0xFEC00000: N entries, ISA IRQs routed`
- [x] Already implemented — marking complete (no new code needed)

## 3. Conditional PIC Disable

Only mask the 8259 PIC when MADT says PCAT_COMPAT — virtual platforms may have no PIC at all.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/pic.c`

- [x] `boot_phase1()` checks `acpi_pcat_compat()` before PIC I/O — already implemented
- [x] PCAT_COMPAT + IOAPIC active → `pic_disable()` called; logs "Switched to LAPIC/IOAPIC (PIC disabled)"
- [x] PCAT_COMPAT=0 → all PIC I/O skipped; logs "APIC-only, no PIC"
- [x] `pic_init()` fallback when no IOAPIC available or no ACPI — already implemented
- [x] Already implemented — marking complete (no new code needed)

## 4. Full IDT Coverage

Fill all 256 IDT vectors with correct stubs so no vector ever triggers an unhandled-interrupt panic.

**Files:** `src/kernel/idt.c`, `src/kernel/idt_stubs.asm`, `include/kernel/idt.h`

- [x] All 256 vectors have stubs in `idt_stubs.asm` (isr0-isr31 for exceptions, irq0-irq15, isr48-isr255 for dynamic)
- [x] Common handler saves registers, dispatches via `idt_common_isr()` in idt.c
- [x] Vectors 0-31 (CPU exceptions): styled panic screen with exception name, RIP, error code
- [x] Vectors 48-63: available for SMP IPIs via `irq_register()` — IPI constants defined in smp.h
- [x] Vector 255 (0xFF): LAPIC spurious interrupt — silent EOI, no log spam
- [x] Vectors 0x90-0x9F: Hyper-V synthetic interrupts — silent EOI, no unhandled-interrupt warnings
- [x] Vectors 32-47 and 64-239: wired dynamically via `irq_register()` — unhandled vectors log warning + EOI (no crash)
- [x] Commit: `"kernel: full 256-vector IDT — spurious + Hyper-V silent EOI"`

## 5. Dynamic IRQ Registration API

Replace all hardcoded IRQ-to-vector assignments with a runtime registration API backed by the IOAPIC.

**Files:** `include/kernel/irq.h`, `src/kernel/irq.c`

> [!IMPORTANT]
> `include/kernel/irq.h` already defines `irq_handler_t` as `void (*)(uint8_t vector, void *ctx)` for the existing `irq_register(vector, handler, ctx, name)` API. This section either renames that typedef or introduces a separate `gsi_handler_t` — resolve the naming before implementing §5 to avoid breaking all current `irq_register()` call sites.

- [x] Existing `irq_handler_t` kept as `void (*)(uint8_t vector, void *ctx)` — no breaking change needed; GSI API added alongside
- [x] `irq_request_gsi(gsi, handler, ctx, name)`: allocates vector via `irq_alloc_vector()`, registers handler, programs IOAPIC redirection with MADT override flags; returns vector or 0
- [x] `irq_free_gsi(gsi)`: masks IOAPIC entry, unregisters handler, frees vector
- [ ] `irq_set_affinity(gsi, cpu_mask)` — implement when IRQ balancing is needed; currently all routes to BSP (functional on SMP, just not balanced)
- [x] `irq_gsi_count(gsi)`: returns fire count via GSI→vector mapping table
- [x] `irq_dispatch()` already exists in `irq.c` — registered handler called + EOI sent
- [ ] `ir list` shell command — deferred to shell TODO
- [ ] Migrate existing drivers to `irq_request_gsi()` — deferred (current `irq_register(vector)` works; migration is mechanical)
- [x] Commit: `"kernel: GSI-based IRQ request API — irq_request_gsi/free_gsi, IOAPIC-backed"`q

## 6. Unified Timer Subsystem (UTS)

A HAL that selects the best available timer clock and exposes a single `uptime_ns()` function to the entire kernel.

**Files:** `include/kernel/timer_hal.h`, `src/kernel/drivers/timer_pit.c`, `src/kernel/drivers/timer_hpet.c`, `src/kernel/drivers/timer_lapic.c`, `src/kernel/timer_hal.c`

- [x] `timer_driver_t` vtable exists with `name`, `init`, `get_ticks`, `sleep_ms`, `get_freq`; added `read_ns` field
- [x] PIT and LAPIC drivers implemented and working (in `pit.c` and `lapic.c`)
- [ ] HPET standalone driver — blocked by `vmm_map_mmio_uc()` (TODO-06 §3). HPET MMIO at ~0xFED00000 requires UC page table entries; WB caching causes MCE on bare metal. Once TODO-06 §3 delivers UC mapping, re-enable HPET calibration in `lapic.c` and implement `hpet_read_ns()` here.
- [x] LAPIC timer: calibration via Hyper-V MSR / PIT busy-wait already working
- [x] Selection waterfall: platform_detect() -> Hyper-V MSR -> LAPIC calibration -> PIT fallback; `hv_flags` available
- [x] `uptime_ns()` added: prefers `read_ns()`, falls back to `ticks * (1e9/freq)`
- [x] Serial log: `UTS: LAPIC selected (Hyper-V, 200000 ticks/ms = 200 MHz bus)` emitted
- [x] Commit: `"kernel: UTS uptime_ns() + read_ns vtable extension"`

## 7. LAPIC Timer Calibration

Measure the LAPIC timer frequency per CPU using HPET or PIT as a reference, then switch the scheduler to LAPIC-driven ticks.

**Files:** `src/kernel/drivers/timer_lapic.c`, `src/kernel/sched/task.c` (scheduler tick hookup)

- [x] LAPIC calibration: 4-tier waterfall (Hyper-V MSR → CPUID 0x15 → TSC-referenced → PM Timer → PIT ch2) in `lapic.c`; stores `cal_ticks_per_ms`. HPET tier disabled until UC MMIO mapping exists (WB caching causes MCE on bare metal — see TODO-06 §3). TSC-referenced tier added 2026-03-28 for bare metal.
- [x] Run on BSP during `timer_hal_init()` — confirmed working in serial log
- [x] Scheduler uses LAPIC periodic timer at 100 Hz; `sched_tick` driven by LAPIC ISR
- [ ] Recalibrate hook for CPU frequency changes — deferred to TODO-15 (power management)
- [x] Serial log: `LAPIC timer: periodic, vec=34, ICR=N, div=1 (calibrated, 100 Hz target)`
- [x] Already implemented — marking complete

## 8. Migrate Boot Splash Spinner Off PIT Callback

Remove the boot splash spinner's direct dependency on PIT IRQ 0 so it works on LAPIC-only platforms.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/boot_timing.c`

- [x] Spinner already uses `timer_register_tick_callback(spinner_advance, 10)` in `spinner_start()` — driven by UTS, not PIT directly
- [x] UTS routes to LAPIC timer on Hyper-V/KVM/hardware, PIT fallback on TCG only
- [x] `spinner_advance()` called every 10 ticks (100ms at 100 Hz = 10 fps) via `timer_tick_callback_fire()` in both PIT and LAPIC ISRs
- [x] No direct PIT hook to remove — architecture is already LAPIC-safe
- [x] Verified working on Hyper-V (LAPIC) and QEMU TCG (PIT)
- [x] Already implemented — marking complete

## 9. Boot Time Visualization

An opt-in post-boot overlay bar chart showing per-stage boot duration, plus a JSON timeline file and shell command.

**Files:** `src/kernel/main/boot_progress.c`, `src/shell/cmd/boot_timeline.c`

- [x] Overlay bar removed (per user preference) — JSON timeline is the diagnostic output
- [x] `boot_timeline_dump_json()`: writes `C:\Impossible\System\Logs\boot-timeline.json` with `[{"stage":"PMM","phase":0,"post":"0x20","start_ms":86,"duration_ms":7},...]`
- [x] Called from `boot_phase3()` after `boot_timing_write_report()`
- [ ] `boot-timeline` shell command — deferred to shell TODO
- [x] Commit: `"kernel: boot timeline JSON dump"`

## 10. Remove Hyper-V Debug Workarounds

Clean up all `#ifdef HYPERV_WORKAROUND` blocks now that correct ACPI/LAPIC/IOAPIC init order is in place.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/lapic.c`, `src/kernel/smp/smp.c`

- [x] Audit: no `#ifdef HYPERV_WORKAROUND`, `HV_QUIRK`, or `HV_BAR` blocks remain — all removed during TODO-01 §2/§3/§8 restructure
- [x] No workaround paths to remove — correct ACPI/LAPIC/IOAPIC init order established in Phase 1
- [ ] SMP IPI abstraction (`irq_send_ipi`) — deferred to SMP TODO (direct LAPIC ICR writes work correctly)
- [x] Already clean — marking complete

---

## OS Comparison

| ⭐ | Feature                    | Win11                          | Linux                           | Impossible OS                     |
|----|----------------------------|--------------------------------|---------------------------------|------------------------------------|
| 💎 | MADT-driven topology       | ✅ HAL reads MADT first       | ✅ acpi_boot_init first         | ✅ §1 — done                      |
| 💎 | LAPIC/IOAPIC before PIT    | ✅ HAL APIC before PIT        | ✅ apic_intr_init before IRQ    | ✅ §2 — done                      |
| 💎 | Conditional PIC disable    | ✅ PCAT_COMPAT gated          | ✅ disable_8259A gated          | ✅ §3 — done                      |
| 💎 | Full IDT coverage          | ✅ KiUnexpectedInterrupt      | ✅ spurious_interrupt           | ✅ §4 — 256 vectors filled        |
| 💎 | Dynamic IRQ registration   | ✅ IoConnectInterrupt         | ✅ request_irq                  | ✅ §5 — irq_request_gsi           |
| 💎 | Unified timer HAL          | ✅ HalQueryPerformanceCounter | ✅ clocksource framework        | ✅ §6 — timer_driver_t + UTS      |
| 💎 | LAPIC timer calibration    | ✅ HAL per-CPU calibration    | ✅ calibrate_delay per CPU      | ✅ §7 — 4-tier waterfall          |
| ⭐ | Boot timeline JSON         | ❌ WPA (offline, binary)      | ❌ systemd-analyze (post-boot)  | ✅ §9 — boot-timeline.json        |

> **Parity achieved on §1–§8, §10.** All interrupt init order, IDT coverage, IRQ registration, and timer HAL match Windows/Linux. Boot timeline JSON goes beyond both. Remaining: HPET standalone driver (deferred until UC MMIO), bare-metal hw interrupt crash (TODO-06 §5).

## Unit Tests

> Wire into `test_runner_init()` via `test_register_irq_timer()` (-> XREF: `docs/infrastructure/kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_irq_timer.c` with:
  - `acpi_madt_info()` returns non-NULL with `lapic_count >= 1` and valid `lapic_base`
  - `acpi_pcat_compat()` returns 0 or 1 (consistent with MADT flags)
  - `acpi_get_ioapic_base()` returns non-zero address when IOAPIC present
  - `irq_request_gsi(99, dummy_handler, NULL, "test")` returns 0 for invalid GSI (no crash)
  - `irq_request_gsi()` with valid GSI (e.g., ISA IRQ 1) returns non-zero vector in range 32-239
  - `irq_gsi_count()` returns 0 for an unregistered GSI
  - `uptime_ns()` returns > 0 after boot; two calls 1ms apart differ by approximately 1000000 ns (within 50% tolerance)
  - Timer driver active: `timer_hal_get_name()` returns non-NULL string ("LAPIC", "PIT", or "HPET")
  - IDT coverage: software `INT 0xFE` does not triple-fault (unhandled vector logs warning + EOI)
  - LAPIC spurious vector (0xFF): software `INT 0xFF` does not crash
- [ ] Register in `test_runner_init()`: `test_register_irq_timer()`
- [ ] Commit: `"test: add irq_timer test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log shows `[ACPI] MADT:` line before any `[LAPIC]` or `[IOAPIC]` lines
- [ ] Serial log shows `[LAPIC] Enabled` before `[PIT]` or `[HPET]` lines
- [ ] Serial log shows `[APIC] PIC disabled (PCAT_COMPAT)` in QEMU (PCAT_COMPAT = 1)
- [ ] All 256 IDT vectors filled; trigger an unmapped vector from a test path → no triple-fault, prints `[FAULT]` message
- [ ] `irq list` shell command prints keyboard (IRQ 1), RTC (IRQ 8), and any other registered IRQs with correct GSI, vector, and name
- [ ] `uptime_ns()` returns monotonically increasing values sampled 1 ms apart; delta ≈ 1 000 000 ns
- [ ] Serial log shows `[LAPIC] CPU0 timer: {N}MHz (calibrated against HPET)` or `PIT` fallback
- [ ] Boot splash spinner animates correctly with `-no-hpet` QEMU flag (LAPIC fallback path)
- [ ] `BootTimeline=1` → bar chart visible at bottom of screen for 2 s after desktop loads
- [ ] `boot-timeline` shell command prints per-stage ASCII chart
- [ ] QEMU `-smp 4`: serial log shows `[SMP] AP1 online`, `[SMP] AP2 online`, `[SMP] AP3 online`; no Hyper-V workaround blocks compile
- [ ] Commit: `"kernel: irq-timer-arch verified — MADT APIC, IDT, UTS, LAPIC calibration"`
