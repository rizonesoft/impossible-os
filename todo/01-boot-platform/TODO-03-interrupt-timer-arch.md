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
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §3` — Phase 1 calls every init function listed here in the correct order; this TODO defines what correct order means
- → XREF: `02-kernel-core/TODO-06-irql-model-dpcs.md §1` — DPC queue init requires LAPIC timer ready (§7); IRQL model depends on the correct interrupt priority assignment established in §2
- → XREF: `02-kernel-core/TODO-07-time-filetime-management.md §2` — `uptime_ns()` from the UTS (§6) is the clock source for the monotonic nanosecond clock used by FILETIME and `QueryPerformanceCounter`
- → XREF: `TODO-02-boot-diagnostics.md §2` — boot time visualization (§9) reads `boot_stage_history[]` built by the boot progress API
- → XREF: `TODO-02-boot-diagnostics.md §5` — §9 boot time visualization bar chart uses the stage color table defined there (`DebugBar=1` feature)
- → XREF: `TODO-04-cpu-boot-sequencing.md §3` — hypervisor detection runs in Phase 0 before §6 UTS probe; `boot_info.hv_flags` set there must be read by `timer_hal_init()` to select the correct clock source
- → XREF: `02-kernel-core/TODO-15-power-management.md` — Intel HWP / AMD CPPC frequency changes require LAPIC timer recalibration (§7 `lapic_calibrate()`); no dedicated HWP/CPPC section in TODO-15 yet — add when frequency scaling is scoped there
- → XREF: `04-drivers-hardware/INDEX.md` — all device drivers call `irq_request(gsi, handler, name, flags)` (§5); this API replaces hardcoded IRQ-to-vector assignments throughout that domain

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
| 💎  |   3   | Conditional PIC disable             | §1, §2              |  [ ]   |
| 💎  |   4   | Full IDT coverage                   | §2, §3              |  [ ]   |
| 💎  |   5   | Dynamic IRQ registration API        | §2, §4              |  [ ]   |
| 💎  |   6   | Unified timer subsystem (UTS)       | §5, T04 §3          |  [ ]   |
| 💎  |   7   | LAPIC timer calibration             | §6                  |  [ ]   |
| 💎  |   8   | Migrate boot splash spinner off PIT | §6, §7              |  [ ]   |
| ⭐  |   9   | Boot time visualization             | §7, T02 §2 & §5     |  [ ]   |
| 💎  |  10   | Remove Hyper-V debug workarounds    | §1–7                |  [ ]   |

> 💎 = parity — Windows NT HAL and Linux interrupt subsystem both follow this init order and have equivalent abstractions.
> ⭐ = exclusive — the post-boot animated timeline bar chart showing per-stage boot duration is not present in either competitor.

---

## 1. ACPI MADT Parsing `[Sonnet]`

Extract interrupt topology from the MADT before any interrupt hardware is touched so every subsequent init function has authoritative GSI data.

**Files:** `include/kernel/acpi.h`, `src/kernel/acpi.c`

- [x] `acpi_init()` already executes before LAPIC/IOAPIC/PIC in `boot_phase1()` — verified in boot logs
- [x] Define `struct acpi_irq_override` and `struct acpi_madt_info` in `acpi.h`
- [x] Parse MADT entries: Type 0 (LAPIC), Type 1 (IOAPIC + GSI base), Type 2 (IRQ override), Type 5 (LAPIC address override), Type 9 (x2APIC); unknown types skipped
- [x] `pcat_compat` flag set from `madt->flags & 1`
- [x] `acpi_madt_info()` getter returns const pointer to static `s_madt_info` struct
- [x] Serial log: `MADT: N CPUs, LAPIC=0xbase, IOAPIC=0xbase GSI=base, M overrides, PCAT_COMPAT=N`
- [x] Commit: `"acpi: consolidated MADT info struct with IOAPIC GSI base"`

## 2. LAPIC / IOAPIC Init Before PIT `[Opus]`

Restructure `boot_interrupts.c` so LAPIC and IOAPIC are brought up before the PIT, using MADT data from §1.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/lapic.c`, `src/kernel/drivers/ioapic.c`

- [x] `boot_phase1()` order: GDT → IDT → ACPI(MADT) → LAPIC → IOAPIC → PIC disable → Timer — already correct from TODO-01 §3 restructure
- [x] `lapic_init()`: reads `IA32_APIC_BASE_MSR`, enables LAPIC, writes SIVR, masks all LVT entries — already implemented
- [x] `lapic_init()` uses `acpi_get_lapic_base()` (from MADT) for MMIO address
- [x] `ioapic_init()`: maps IOAPIC MMIO at `acpi_get_ioapic_base()`, reads `IOAPIC_VER` for `max_redir_entries`
- [x] MADT IRQ overrides applied via `acpi_get_override()` in IOAPIC ISA routing loop
- [x] Serial log: `LAPIC enabled: base=0xFEE00000` and `I/O APIC at 0xFEC00000: N entries, ISA IRQs routed`
- [x] Already implemented — marking complete (no new code needed)

## 3. Conditional PIC Disable `[Sonnet]`

Only mask the 8259 PIC when MADT says PCAT_COMPAT — virtual platforms may have no PIC at all.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/pic.c`

- [ ] Read `acpi_madt_info()->flags & MADT_PCAT_COMPAT` before any PIC I/O port access
- [ ] If `PCAT_COMPAT` set: call `pic_disable()` (mask all IRQs on master + slave: `outb(0x21, 0xFF)` / `outb(0xA1, 0xFF)`; issue EOIs); log `[APIC] PIC disabled (PCAT_COMPAT)`
- [ ] If `PCAT_COMPAT` not set: skip all PIC I/O entirely; log `[APIC] No PIC (virtual platform)`
- [ ] Add `pic_init_fallback()`: only called if LAPIC init returns `BOOT_DEGRADED` or LAPIC is completely absent; sets up PIC as the sole interrupt controller with legacy IRQ 0–15 vectors 32–47
- [ ] Commit: `"drivers: conditional PIC disable — PCAT_COMPAT MADT flag, virtual-platform safe"`

## 4. Full IDT Coverage `[Opus]`

Fill all 256 IDT vectors with correct stubs so no vector ever triggers an unhandled-interrupt panic.

**Files:** `src/kernel/idt.c`, `src/kernel/idt_stubs.asm`, `include/kernel/idt.h`

- [ ] Generate stubs for all 256 vectors in `idt_stubs.asm`; each stub pushes a synthetic error code (for vectors without hardware error code), pushes the vector number, then jumps to `idt_common_handler`
- [ ] `idt_common_handler` (C): saves all registers (`push rax`…`push r15`), calls `idt_dispatch(vector, error_code, rip, rsp)`
- [ ] `idt_dispatch()`: vectors 0–31 (CPU exceptions): log `[FAULT] #{N} <name> at RIP=0x{x} CS=0x{x} ERR=0x{x}`; call `kernel_panic()` for unhandled faults; return for benign ones (`#DB`=1, `#BP`=3, `#OF`=4)
- [ ] Vectors 48–63: SMP IPI range — `IPI_TLB_SHOOTDOWN=0x30`, `IPI_RESCHEDULE=0x31`, `IPI_PANIC=0x32`; stubs call the appropriate IPI handler + send LAPIC EOI
- [ ] Vector 255 (`0xFF`): LAPIC spurious interrupt — send LAPIC EOI and return immediately, no panic
- [ ] Vectors 0x90–0x9F: Hyper-V synthetic interrupt vectors — register via `irq_request()` with a no-op handler to prevent unhandled-interrupt panics on Gen2 VMs
- [ ] Vectors 32–47 and 64–239: wired dynamically via `irq_request()` (§5); stubs call `irq_dispatch(vector)` which looks up the registered handler
- [ ] Commit: `"kernel: full 256-vector IDT coverage — CPU faults, SMP IPIs, spurious, Hyper-V synthetic"`

## 5. Dynamic IRQ Registration API `[Opus]`

Replace all hardcoded IRQ-to-vector assignments with a runtime registration API backed by the IOAPIC.

**Files:** `include/kernel/irq.h`, `src/kernel/irq.c`

> [!IMPORTANT]
> `include/kernel/irq.h` already defines `irq_handler_t` as `void (*)(uint8_t vector, void *ctx)` for the existing `irq_register(vector, handler, ctx, name)` API. This section either renames that typedef or introduces a separate `gsi_handler_t` — resolve the naming before implementing §5 to avoid breaking all current `irq_register()` call sites.

- [ ] Define `irq_handler_t` as `void (*)(uint32_t gsi, void *ctx)` (or introduce `gsi_handler_t` if the old signature must coexist during transition)
- [ ] `irq_request(uint32_t gsi, irq_handler_t handler, const char *name, uint32_t flags)` → allocates next free IDT vector from `[64, 239]`, writes IOAPIC redirection entry (GSI → vector, polarity + trigger mode from MADT override, delivery mode = Fixed, destination = BSP APIC ID, mask = 0); returns allocated vector or -1 on failure
- [ ] `irq_free(uint32_t gsi)` → mask IOAPIC redirection entry, release vector back to the free pool
- [ ] `irq_set_affinity(uint32_t gsi, uint64_t cpu_mask)` → update IOAPIC redirection entry destination field
- [ ] `irq_stats(uint32_t gsi, uint64_t *out_count)` → return fire count since boot from per-GSI counter incremented in `irq_dispatch()`
- [ ] `irq_dispatch(uint32_t vector)`: look up registered handler by vector, call it, send LAPIC EOI (`lapic_write(LAPIC_EOI, 0)`)
- [ ] `irq list` shell command: prints table with columns GSI, Vector, Name, CPU affinity mask, fire count
- [ ] Migrate existing hardcoded vector assignments (keyboard IRQ 1, RTC IRQ 8, etc.) to use `irq_request()` in their respective driver init functions
- [ ] Commit: `"kernel: dynamic IRQ registration API — irq_request/free/affinity/stats, IOAPIC-backed"`

## 6. Unified Timer Subsystem (UTS) `[Opus]`

A HAL that selects the best available timer clock and exposes a single `uptime_ns()` function to the entire kernel.

**Files:** `include/kernel/timer_hal.h`, `src/kernel/drivers/timer_pit.c`, `src/kernel/drivers/timer_hpet.c`, `src/kernel/drivers/timer_lapic.c`, `src/kernel/timer_hal.c`

- [ ] Define `struct timer_driver { const char *name; int (*probe)(void); boot_result_t (*init)(void); uint64_t (*read_ns)(void); boot_result_t (*set_oneshot_ns)(uint64_t ns, void (*cb)(void)); boot_result_t (*set_periodic_ns)(uint64_t ns, void (*cb)(void)); void (*calibrate)(void); }`
- [ ] Implement `timer_pit.c` driver: 8254 PIT divisor programming, `read_ns()` via tick counter × period, `set_periodic_ns()` sets PIT mode 2; always probes successfully (guaranteed fallback)
- [ ] Implement `timer_hpet.c` driver: locate HPET ACPI table, map MMIO, read `GCAP_ID` for tick period (femtoseconds), enable HPET (`GEN_CONF` bit 0), `read_ns()` = `main_counter × period / 1e6`; probe returns 0 if no ACPI HPET table
- [ ] Implement `timer_lapic.c` driver: per-CPU LAPIC timer; requires calibration against HPET or PIT before `read_ns()` is valid; `set_oneshot_ns()` programs LVT timer + initial count; `set_periodic_ns()` uses LAPIC mode 2
- [ ] Selection waterfall in `timer_hal_init()`: first read `boot_info.hv_flags` (→ XREF `TODO-04-cpu-boot-sequencing.md §3`); if `HV_TSC_ENLIGHTENMENT` set, register a `timer_hyperv.c` driver using `HV_X64_MSR_TIME_REF_COUNT` as the high-resolution wall clock and probe it first; otherwise probe HPET → if found, select HPET as global wall clock; probe LAPIC → calibrate against HPET (§7) → use LAPIC for per-CPU scheduler tick; if no HPET, calibrate LAPIC against PIT, use PIT as wall-clock fallback
- [ ] `uptime_ns()`: calls `g_wall_clock_driver->read_ns()` — single function replaces all scattered `timer_ticks`, `pit_ms` variables throughout the codebase
- [ ] Serial log: `[TIMER] Wall clock: {HPET|PIT}, per-CPU: LAPIC @ {freq}MHz`
- [ ] Commit: `"kernel: unified timer subsystem — HPET/LAPIC/PIT HAL, single uptime_ns()"`

## 7. LAPIC Timer Calibration `[Opus]`

Measure the LAPIC timer frequency per CPU using HPET or PIT as a reference, then switch the scheduler to LAPIC-driven ticks.

**Files:** `src/kernel/drivers/timer_lapic.c`, `src/kernel/sched/task.c` (scheduler tick hookup)

- [ ] `lapic_calibrate()`: set LAPIC timer divide config = 16; write initial count = `0xFFFFFFFF`; wait exactly 10 ms using HPET `read_ns()` or PIT busy-wait; read LAPIC current count; `lapic_hz = (0xFFFFFFFF - current_count) × 100 × 16` (ticks per second); store in per-CPU `struct cpu_data.lapic_hz`
- [ ] Run `lapic_calibrate()` on BSP during `timer_hal_init()`; run again on each AP during SMP AP bringup
- [ ] Scheduler integration: replace `pit_set_periodic(HZ, sched_tick)` with `timer_set_periodic_ns(1000000000/HZ, sched_tick)` via UTS; UTS routes to LAPIC timer on each CPU for true per-CPU scheduling tick
- [ ] Recalibrate hook: `lapic_recalibrate_on_freq_change()` called by Intel HWP / AMD CPPC driver when CPU frequency changes (→ XREF `02-kernel-core/TODO-15-power-management.md` — section to be scoped when HWP/CPPC frequency scaling is added to TODO-15)
- [ ] Serial log: `[LAPIC] CPU{N} timer: {freq}MHz (calibrated against {HPET|PIT})`
- [ ] Commit: `"drivers: LAPIC timer calibration per CPU, scheduler migrated to LAPIC tick"`

## 8. Migrate Boot Splash Spinner Off PIT Callback `[Sonnet]`

Remove the boot splash spinner's direct dependency on PIT IRQ 0 so it works on LAPIC-only platforms.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/boot_timing.c`

- [ ] Locate the current PIT IRQ 0 callback that calls `spinner_advance()` / `boot_splash_tick()`
- [ ] Replace with `timer_set_periodic_ns(16666667, spinner_tick_callback)` (≈ 60 fps = 16.7 ms) via the UTS after `timer_hal_init()` completes; UTS routes to LAPIC or HPET as available
- [ ] `spinner_tick_callback()`: calls `spinner_advance()` + `boot_splash_tick()` + `boot_progress_poll()` (updates splash progress bar percentage from `boot_stage_history[]`)
- [ ] Remove `pit_register_callback(spinner_advance)` or equivalent direct PIT hook from `boot_splash.c`
- [ ] Verify spinner still animates correctly in QEMU (LAPIC timer) and with forced PIT fallback (pass `-no-hpet` to QEMU)
- [ ] Commit: `"boot: migrate splash spinner from PIT IRQ 0 to UTS timer_set_periodic_ns — LAPIC-only safe"`

## 9. Boot Time Visualization `[Sonnet]`

An opt-in post-boot overlay bar chart showing per-stage boot duration, plus a JSON timeline file and shell command.

**Files:** `src/kernel/main/boot_progress.c`, `src/shell/cmd/boot_timeline.c`

- [ ] Activate when `boot.conf` key `BootTimeline=1`
- [ ] At `BOOT_STAGE_DESKTOP_READY`: read `boot_stage_history[]` (from TODO-02 §2), compute per-stage duration in ms; render a 24 px tall horizontal bar chart across the bottom of the screen for 2 s (each bar width ∝ duration, color matches TODO-02 §5 stage color table); fade out after 2 s
- [ ] `boot_profile_dump_json()`: write `C:\Impossible\System\Logs\boot-timeline-{version}-{date}.json` with format `[{"stage":"PMM","start_ms":12,"duration_ms":3},...]`
- [ ] `boot-timeline` shell command: reads the most recent JSON file, prints an ASCII bar chart: `PMM    [██████████] 12 ms` one line per stage; `boot-timeline --compare` diffs the last two files and marks regressions with `▲`
- [ ] Commit: `"kernel: opt-in boot time visualization — overlay bar chart + JSON timeline + shell command"`

## 10. Remove Hyper-V Debug Workarounds `[Sonnet]`

Clean up all `#ifdef HYPERV_WORKAROUND` blocks now that correct ACPI/LAPIC/IOAPIC init order is in place.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/drivers/lapic.c`, `src/kernel/smp/smp.c`

- [ ] Audit all `#ifdef HYPERV_WORKAROUND`, `#ifdef HV_QUIRK`, and equivalent conditional blocks across the kernel; list them in the commit message with an explanation of why each is no longer needed
- [ ] Remove each block (keep the non-workaround path); verify QEMU Gen2 / Hyper-V Gen2 still boots after removal
- [ ] SMP AP bringup cleanup: replace any raw LAPIC ICR writes for INIT/STARTUP IPIs with `irq_send_ipi(cpu_id, IPI_INIT)` + `irq_send_ipi(cpu_id, IPI_STARTUP)` via the dynamic IRQ API (§5)
- [ ] Verify AP bringup: serial log must show `[SMP] AP{N} online` for each CPU core in QEMU `-smp 4`
- [ ] Commit: `"kernel: remove Hyper-V debug workarounds — correct ACPI/LAPIC/IOAPIC init order makes them obsolete"`

---

## OS Comparison

| ⭐  | Feature                               | 🪟 Windows NT / 11                                | 🐧 Linux                                           | 🚀 Impossible OS                                           |
| --- | ------------------------------------- | -------------------------------------------------- | -------------------------------------------------- | ----------------------------------------------------------- |
| 💎  | MADT-driven interrupt topology        | ✅ HAL reads MADT before any interrupt init       | ✅ `acpi_boot_init()` parses MADT first            | ⬜ Planned — §1; MADT parse moved before interrupt init    |
| 💎  | LAPIC/IOAPIC init before PIT          | ✅ HAL always inits APIC before legacy PIT        | ✅ `apic_intr_init()` before `init_IRQ()`          | ⬜ Planned — §2; init order restructured                   |
| 💎  | Conditional PIC disable               | ✅ HAL checks PCAT_COMPAT flag                    | ✅ `disable_8259A()` gated on MADT flag            | ⬜ Planned — §3                                            |
| 💎  | Full IDT coverage (all 256 vectors)   | ✅ KiUnexpectedInterrupt fills unused vectors     | ✅ `spurious_interrupt()` for unregistered vectors | ⬜ Planned — §4                                            |
| 💎  | Dynamic IRQ registration              | ✅ `IoConnectInterrupt` / `IoConnectInterruptEx`  | ✅ `request_irq()` / `devm_request_irq()`          | ⬜ Planned — §5; `irq_request(gsi, handler, name, flags)`  |
| 💎  | Unified timer HAL (HPET→LAPIC→PIT)    | ✅ `HalQueryPerformanceCounter` backend selection | ✅ `clocksource` + `clockevent` framework          | ⬜ Planned — §6; `timer_driver_t` + `uptime_ns()`          |
| 💎  | Per-CPU LAPIC timer calibration       | ✅ HAL calibrates LAPIC per logical processor     | ✅ `calibrate_delay_direct()` per CPU              | ⬜ Planned — §7                                            |
| ⭐  | Post-boot animated timeline bar chart | ❌ WPA boot trace (offline, binary format)        | ❌ `systemd-analyze plot` (SVG, post-boot only)    | ⬜ **Planned — §9; inline overlay chart + ASCII CLI diff** |

> **After parity items:** Impossible OS will fully match Windows NT and Linux on interrupt init order, IDT coverage, dynamic IRQ registration, and the timer HAL. The exclusive §9 boot timeline goes further by showing an animated bar chart at desktop-ready and offering a regression-diffing CLI tool — giving developers instant visibility into boot regressions without external profiling tools.

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
