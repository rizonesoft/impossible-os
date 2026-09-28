<!-- docs: covers=todo/01-boot-platform/TODO-11-interrupt-timer-arch.md sources=src/kernel/main/boot_interrupts.c,src/kernel/acpi.c,include/kernel/acpi.h,src/kernel/drivers/lapic.c,include/kernel/drivers/lapic.h,src/kernel/drivers/ioapic.c,include/kernel/drivers/ioapic.h,src/kernel/drivers/pic.c,include/kernel/drivers/pic.h,src/kernel/idt.c,include/kernel/idt.h,src/kernel/irq.c,include/kernel/irq.h,src/kernel/timer.c,include/kernel/timer.h,src/kernel/drivers/pit.c,include/kernel/drivers/pit.h,src/kernel/drivers/hpet.c,include/kernel/drivers/hpet.h,src/kernel/main/boot_progress.c,include/kernel/boot_progress.h reviewed=2026-09-28 order=11 -->
# Interrupt Architecture and the Unified Timer Subsystem

## What is it?

This is the Phase 1 contract that brings interrupts and timekeeping online: ACPI's MADT table is parsed first, LAPIC and IOAPIC come up before any legacy PIT or 8259 programming, every one of the 256 IDT vectors has a handler, and one HAL (`uptime_ns()`) is the sole source of monotonic time for the rest of the kernel.

It exists because getting this order wrong is a known bug class: a PIT enabled before IOAPIC routing lets IRQ 0 arrive through both the 8259 and the IOAPIC on a PCAT-compatible machine, which is exactly the hazard the current `boot_phase1()` ordering avoids.

It does not own MSI/MSI-X vector allocation or x2APIC (that is the [APIC interrupt routing roadmap](../../todo/04-drivers-hardware/TODO-02-apic-interrupt-routing.md)), and it does not own AP CPU state replication (that is [CPU boot sequencing](cpu-boot-sequencing.md)). This page is the sequencing and timekeeping contract those subsystems sit on top of.

## How does it work?

`boot_phase1()` in `src/kernel/main/boot_interrupts.c` runs GDT, then IDT (`idt_init()` and `irq_init()`), then ACPI (`acpi_init()`, which parses the MADT), then LAPIC (`lapic_init()`), then IOAPIC (`ioapic_init()`), then a conditional PIC step, then platform services, with `timer_hal_init()` as the last thing armed before `sti`. The IDT loads before ACPI purely so early faults have somewhere to go; it does no interrupt routing of its own at that point.

`acpi_init()` walks the MADT and builds a consolidated `struct acpi_madt_info` covering CPU count, the LAPIC and IOAPIC base addresses, IRQ overrides, and the PCAT_COMPAT flag (MADT flags bit 0: 1 means a legacy 8259 pair exists). `boot_phase1()` only initializes LAPIC and IOAPIC when ACPI is available and an IOAPIC base was found.

With both up, the PIC step is conditional. PCAT_COMPAT with an available IOAPIC calls `pic_disable()` and logs `Switched to LAPIC/IOAPIC (PIC disabled)`. No PCAT_COMPAT with a working IOAPIC skips PIC I/O entirely and logs `LAPIC/IOAPIC active (APIC-only, no PIC)`. An APIC-only platform (PCAT_COMPAT=0) whose IOAPIC init failed has no controller left to route interrupts, so `boot_phase1()` calls `boot_halt()` rather than boot with dead input, timer and device IRQs.

All 256 IDT vectors are filled so nothing ever falls through to a triple fault. Vectors 0-31 are CPU exceptions; 0x30-0xEF is the dynamic range `irq_alloc_vector()` hands out (`IRQ_DYNAMIC_BASE`/`IRQ_DYNAMIC_END` in `irq.h`); ISA IRQ 8-15 sit at 0x70-0x77 so they cannot collide with the fixed DPL=3 syscall gate; vector 0xFF is the LAPIC spurious interrupt and gets no EOI; vectors 0x90-0x9F are Hyper-V synthetic interrupts and get a silent EOI; anything else unhandled logs a warning and still sends an EOI instead of crashing.

Device drivers do not hardcode vectors. `irq_request_gsi(gsi, handler, ctx, name)` allocates a vector, programs the IOAPIC redirection entry using any MADT override for that GSI, and registers the handler; `irq_free_gsi()` and `irq_set_affinity()` release and re-route it. `irq_request_gsi_ex()` adds shared-line support for PCI INTx: several handlers can chain on one vector, each reporting `IRQ_HANDLED` or `IRQ_NONE`, and a line that reports all-`IRQ_NONE` too many times in a row is masked at the controller as a storm quarantine rather than left to flood the CPU.

Timekeeping is unified behind a `timer_driver_t` vtable (`name`, `init`, `get_ticks`, `sleep_ms`, `get_freq`, `read_ns`, `arm_oneshot`) selected once by `timer_hal_init()` into the global `g_system_timer`. `uptime_ns()` prefers the backend's `read_ns()` and falls back to `ticks * (1e9 / freq)` only if a backend does not provide it, so every caller sees one clock contract regardless of which backend is active.

LAPIC timer frequency is calibrated through a waterfall (Hyper-V MSR or VMware/KVM CPUID, then HPET or the ACPI PM timer, then the PIT as a last resort), each tier range-validated before it is trusted. QEMU TCG is a deliberate carve-out that stays on the PIT, because TCG's TSC is instruction-counted and skews against wall-clock time, which a LAPIC-derived rate would inherit. The scheduler's periodic tick runs off this calibrated LAPIC rate at 100 Hz on the BSP today; Application Processors do not yet run their own LAPIC tick.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `acpi_init()` | Parses ACPI tables including the MADT; must run before LAPIC/IOAPIC/PIC ([`acpi.c`](../../src/kernel/acpi.c)) |
| `acpi_madt_info()` | Returns the consolidated `struct acpi_madt_info` (CPU count, LAPIC/IOAPIC base, overrides, PCAT_COMPAT) ([`acpi.h`](../../include/kernel/acpi.h)) |
| `lapic_init()` / `lapic_available()` | Brings up the local APIC from the MADT-derived base address ([`lapic.c`](../../src/kernel/drivers/lapic.c)) |
| `ioapic_init()` / `ioapic_available()` | Maps the IOAPIC MMIO window and routes ISA IRQs using MADT overrides ([`ioapic.c`](../../src/kernel/drivers/ioapic.c)) |
| `idt_init()` / `irq_init()` | Fill all 256 IDT vectors and set up the dispatch table ([`idt.c`](../../src/kernel/idt.c), [`irq.c`](../../src/kernel/irq.c)) |
| `irq_request_gsi(gsi, handler, ctx, name)` / `irq_free_gsi(gsi)` | Runtime GSI-to-vector registration backed by the IOAPIC ([`irq.h`](../../include/kernel/irq.h)) |
| `irq_request_gsi_ex(...)` / `irq_release_gsi_shared(...)` | Shared-line (PCI INTx) claim chains with `IRQ_HANDLED`/`IRQ_NONE` reporting ([`irq.h`](../../include/kernel/irq.h)) |
| `irq_set_affinity(gsi, cpu_mask)` | Routes a GSI's delivery to an online CPU ([`irq.h`](../../include/kernel/irq.h)) |
| `timer_hal_init()` | Selects the timer backend and calibrates the LAPIC waterfall; runs after LAPIC/IOAPIC init ([`timer.h`](../../include/kernel/timer.h)) |
| `uptime_ns()` | The single nanosecond clock every subsystem should read ([`timer.h`](../../include/kernel/timer.h), [`timer.c`](../../src/kernel/timer.c)) |
| `timer_driver_t` | The vtable a timer backend implements ([`timer.h`](../../include/kernel/timer.h)) |
| `hpet_available()` / `hpet_ns()` | HPET presence check and raw nanosecond read, used as a calibration reference ([`hpet.h`](../../include/kernel/drivers/hpet.h)) |
| `boot_timeline_dump_json()` | Writes the per-boot stage timeline to disk ([`boot_progress.h`](../../include/kernel/boot_progress.h)) |

## How do I use it?

```bash
bash scripts/test.sh SUITE=boot        # MADT, LAPIC/IOAPIC, IDT, GSI allocator, timer suites
make test-boot                         # same, as a make target
bash scripts/test-smoke-matrix.sh      # boots TCG+KVM x 1+2 CPUs; exercises this path on every leg
```

On a live boot, serial shows the sequence in order: `MADT: N CPUs, LAPIC=0x..., IOAPIC=0x... GSI=..., M overrides, PCAT_COMPAT=N`, then `LAPIC enabled: base=0x..., ID=N, ver=0x..., maxLVT=N`, then `I/O APIC at 0x...: N entries, ISA IRQs routed to BSP (LAPIC N)`, then either `Switched to LAPIC/IOAPIC (PIC disabled)` or `LAPIC/IOAPIC active (APIC-only, no PIC)`, then a `UTS: <name> selected (...)` line naming the chosen timer backend, and finally `LAPIC timer: periodic, vec=N, ICR=N, div=1 (calibrated, N Hz target)` (or `fallback` when no tier measured a usable frequency).

After a desktop boot, `X:\Perf\boot-timeline.json` (BlackBox mounted) or `C:\Impossible\System\Logs\boot-timeline.json` (fallback) holds the machine-readable per-stage timeline; its wire format is documented in [`boot-timeline.json` Wire Format](boot-timeline-schema.md).

## What is not implemented yet?

- Application Processors do not run their own LAPIC timer tick; the 100 Hz scheduler heartbeat is BSP-only today, blocked on per-CPU run queues: [LAPIC Timer Calibration](../../todo/01-boot-platform/TODO-11-interrupt-timer-arch.md#7-lapic-timer-calibration).
- LAPIC recalibration on a CPU frequency change is not wired; it is blocked on the CPU frequency scaling governor: [LAPIC Timer Calibration](../../todo/01-boot-platform/TODO-11-interrupt-timer-arch.md#7-lapic-timer-calibration).
- The Hyper-V reference TSC page is not consumed as a clocksource; it is blocked on Hyper-V SynIC/reference-TSC MSR init owned by the x86-64 architecture roadmap: [Unified Timer Subsystem (UTS)](../../todo/01-boot-platform/TODO-11-interrupt-timer-arch.md#6-unified-timer-subsystem-uts).
- No `irq list` shell command renders the live GSI/vector table yet; it is deferred to the desktop shell utilities roadmap: [Dynamic IRQ Registration API](../../todo/01-boot-platform/TODO-11-interrupt-timer-arch.md#5-dynamic-irq-registration-api).
- No `boot-timeline` shell command renders `boot-timeline.json` interactively yet; the JSON file itself ships: [Boot Time Visualization](../../todo/01-boot-platform/TODO-11-interrupt-timer-arch.md#9-boot-time-visualization).
- An arch-neutral `irq_send_ipi(cpu, vector)` wrapper does not exist; `lapic_send_ipi()` is called directly today: [Remove Hyper-V Debug Workarounds](../../todo/01-boot-platform/TODO-11-interrupt-timer-arch.md#10-remove-hyper-v-debug-workarounds).
- MSI/MSI-X vector allocation and x2APIC are owned by [APIC Interrupt Routing](../../todo/04-drivers-hardware/TODO-02-apic-interrupt-routing.md), not by this roadmap.

## How does it compare with Windows 11 and Linux?

MADT-first topology discovery, LAPIC/IOAPIC-before-PIT ordering, conditional PIC disable, full IDT coverage with a named spurious and unhandled path, dynamic IRQ registration with shared-line support, and a unified timer HAL all match what the Windows HAL (`IoConnectInterrupt`, `KiUnexpectedInterrupt`, QPC) and Linux (MADT parsing, `request_irq`, the clocksource framework) already do; this is table-stakes parity. MSI/MSI-X is the one row where Impossible OS is behind both today, and that gap is owned by the APIC interrupt routing roadmap. LAPIC timer calibration is per-CPU on Windows and Linux; here it is BSP-only until AP LAPIC timer bringup lands, so that row is partial. The one place Impossible OS goes further is the on-disk `boot-timeline.json`: the Windows equivalent is an offline WPA trace and the Linux one is a `systemd-analyze` report, neither of which ships as a structured on-disk artifact by default.

## See also

- [Interrupt Architecture and Unified Timer Subsystem roadmap](../../todo/01-boot-platform/TODO-11-interrupt-timer-arch.md)
- [CPU Boot Sequencing and AP Bringup](cpu-boot-sequencing.md)
- [`boot-timeline.json` Wire Format](boot-timeline-schema.md)
- [Bare Metal Boot Hardening](bare-metal-hardening.md)
