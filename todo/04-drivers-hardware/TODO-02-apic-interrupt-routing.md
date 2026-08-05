---
schema_version: 1
id: apic-interrupt-routing
domain: 04-drivers-hardware
status: active
title: "TODO-02 -- APIC Architecture & Advanced Interrupt Routing"
---

# TODO-02 -- APIC Architecture & Advanced Interrupt Routing

> **Goal:** Complete the advanced interrupt subsystem on top of the working base xAPIC/IOAPIC: x2APIC MSR mode, LAPIC timer calibration via HPET, TLB shootdown IPI (hardware side), NMI watchdog via PMI counter overflow, MSI vector allocation, and a per-vector interrupt profiler exposed at `/sys/interrupts`.

> [!IMPORTANT]
> **Base APIC is complete:** LAPIC init (SVR, TPR, ESR, EOI), IOAPIC routing of 16 ISA IRQs with ISO flags, PIC conditional disable (`PCAT_COMPAT`), SMP INIT-SIPI-SIPI, and basic `lapic_send_ipi` all work. This TODO delivers the advanced layer on top. All LAPIC/IOAPIC MMIO regions must be mapped with **Strong Uncacheable (UC)** page attributes -- cached APIC reads return stale interrupt state.

## Inputs

- [`src/kernel/drivers/lapic.c`](../../src/kernel/drivers/lapic.c), [`include/kernel/drivers/lapic.h`](../../include/kernel/drivers/lapic.h)
- [`src/kernel/drivers/ioapic.c`](../../src/kernel/drivers/ioapic.c)
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c) -- MADT type-4 (NMI) and type-9 (x2APIC) entries already parsed; consumed by §1 and §5
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md` §5 -- ISA/IOAPIC `irq_request_gsi` path; MSI/MSI-X allocation in this file §3 must reuse the same vector allocator rules
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §4` -- `hpet_read_ns()` from HPET driver consumed by §4 calibration; implement HPET before this section
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §3` -- `pci_enable_msi(dev, vector)` calls `apic_alloc_msi_vector()` from §3 to obtain the vector; implement §3 before TODO-08 §3
- → XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §2` -- TLB shootdown VMM/scheduler side (calls `tlb_shootdown(cpu_mask, vaddr, len)`); the APIC IPI send mechanism and `invlpg` handler defined here in §4 are what that side calls
- → XREF: `03-memory-concurrency/TODO-10-concurrency-diagnostics.md §5` -- kernel watchdog thread (tick-based, software); the NMI watchdog here (§5) is the hardware PMI backup that fires even when `CLI` is active
- → XREF: `01-boot-platform/TODO-11-interrupt-timer-arch.md` §6 -- unified timer HAL (`timer_hal_init`, `uptime_ns`); LAPIC-via-HPET calibration in this file §2 must stay consistent with that HAL

## Outcome

- `lapic_read()` / `lapic_write()` transparently dispatch to MSR (`0x800+` offset) or MMIO based on a runtime `x2apic_mode` flag; IPI generation uses a single 64-bit MSR write (atomic, no delivery-status polling) on x2APIC systems.
- LAPIC timer initial count is derived from a HPET 10 ms reference window, producing accurate 1 ms ticks on any hardware; `g_tick_ns` is set to the calibrated value.
- `tlb_shootdown(cpu_mask, vaddr, len)` sends `IPI_TLB_FLUSH` to target CPUs; each receives the vector, runs `invlpg` over the range, and atomically decrements the ack counter; the sender spins until all acks arrive.
- LAPIC PMI (Performance Monitoring Interrupt) is configured as NMI; `IA32_PERFEVTSEL0` fixed-counter overflow fires every ~1 s regardless of `CLI`; the NMI handler checks the per-CPU heartbeat counter and prints a backtrace to serial on stall.
- `apic_alloc_msi_vector()` reserves a vector (32–254) from a bitmap with `ticket_lock_t` protection; `apic_free_msi_vector(vec)` releases it; both are called by `pci_enable_msi/msix`.
- `/sys/interrupts` VFS file and `irqstats` shell command show per-vector `irq_count` and `irq_ns` totals; `NtQuerySystemInformation(SystemInterruptInformation)` returns the same data to Win32 callers.

## Implementation Order

| ⭐  | Order | Deliverable                                             | Depends On                    | Status |
| --- | :---: | ------------------------------------------------------- | ----------------------------- | :----: |
| 💎  |   1   | §1 x2APIC mode -- MSR read/write, 32-bit APIC IDs       | CPUID, MADT type-9            |  [ ]   |
| 💎  |   2   | §2 LAPIC timer calibration via HPET                     | §1, TODO-08 §4 (hpet_read_ns) |  [ ]   |
| 💎  |   3   | §3 MSI vector bitmap allocator                          | §1 (APIC IDs stable)          |  [ ]   |
| 💎  |   4   | §4 TLB shootdown IPI -- IPI send + `invlpg` handler     | §1, §3 (vector from bitmap)   |  [ ]   |
| ⭐  |   5   | §5 NMI watchdog -- PMI/NMI configuration                | §1, §3 (NMI vector or pin)    |  [ ]   |
| ⭐  |   6   | §6 Interrupt profiler -- `/sys/interrupts` + `irqstats` | §1–§3 (vectors registered)    |  [ ]   |

> 💎 = parity -- x2APIC, LAPIC calibration, TLB shootdown, and MSI vector allocation are standard in Windows HAL and Linux `arch/x86/`.
> ⭐ = exclusive -- the NMI watchdog via PMI counter overflow is a deeper hardware mechanism than Windows/Linux expose in-kernel APIs; `/sys/interrupts` with `irq_ns` timing data goes beyond Linux `/proc/interrupts` (counts only) and Windows has no readable equivalent.

---

## 1. x2APIC Mode -- MSR Register Access `[Opus]`

Detect `CPUID.01H:ECX[21]` and switch from xAPIC MMIO to x2APIC MSR register access. Refactor `lapic_read()` / `lapic_write()` to dispatch based on a runtime flag. Widen APIC IDs to 32 bits for systems with >255 cores. The ICR becomes a single atomic 64-bit MSR write in x2APIC mode, eliminating the xAPIC write-ordering hazard.

**Files:** `src/kernel/drivers/lapic.c`, `include/kernel/drivers/lapic.h`

> [!CAUTION]
> Transition to x2APIC by setting both EN (bit 11) and EXTD (bit 10) in `IA32_APIC_BASE` MSR (`0x1B`) in a **single** `wrmsr` -- a two-step write is architecturally undefined. After transition, re-programme all LVT entries (they reset on mode change). There is no path back to xAPIC without a full system reset; once x2APIC is enabled, it stays enabled.

- [ ] Detect x2APIC: `cpuid(1, ...)` → check ECX bit 21; set `static bool x2apic_mode`
- [ ] `lapic_read_msr(offset)` -- `rdmsr(0x800 + (offset >> 4))`; `lapic_write_msr(offset, val)` -- `wrmsr(0x800 + (offset >> 4), val)`
- [ ] Refactor `lapic_read(offset)` / `lapic_write(offset, val)`: branch on `x2apic_mode`; MMIO path unchanged
- [ ] Mode transition in `lapic_init()`: if x2APIC supported, `wrmsr(IA32_APIC_BASE, current | (1<<11) | (1<<10))`; re-init SVR, TPR, ESR, all LVT entries
- [ ] `lapic_id()`: in x2APIC mode, `rdmsr(0x802)` returns full 32-bit ID; update `cpu_info_t.apic_id` from `uint8_t` to `uint32_t`
- [ ] `lapic_send_ipi(dest, cmd)` in x2APIC: single `wrmsr(0x830, (uint64_t)dest << 32 | cmd)` -- no delivery-status poll needed
- [ ] Wire MADT type-9 (x2APIC Local APIC) entries (already parsed in `acpi.c`) into per-CPU APIC ID array
- [ ] Locked-mode detect: read `IA32_XAPIC_DISABLE_STATUS` (0xBD) bit 0 (LEGACY_XAPIC_DISABLED); on 2022+ Intel SGX/TDX the firmware locks x2APIC -- use the MSR path from boot, never attempt an xAPIC fallback (-> XREF: `D02 T09 §11`)
- [ ] Boot log: `[LAPIC] x2APIC mode active` or `[LAPIC] xAPIC mode (x2APIC not supported)`
- [ ] Commit: `"lapic: x2APIC MSR register access -- mode detection, 32-bit APIC IDs, atomic IPI"`

## 2. LAPIC Timer Calibration via HPET `[Opus]`

Replace the hardcoded `ICR=10000000` with a `lapic_calibrate_timer()` that measures the LAPIC bus frequency against a 10 ms HPET reference window. Store the result in `g_lapic_ticks_per_ms`; set periodic mode with the derived ICR. Write `g_tick_ns` for the unified timer HAL.

**Files:** `src/kernel/drivers/lapic.c`, `include/kernel/drivers/lapic.h`

> [!IMPORTANT]
> → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §4` -- `hpet_read_ns()` must be available before this calibration runs. Call `hpet_init()` earlier in `kernel_main()` than `lapic_timer_init()`.
> Calibration window: configure LAPIC timer as one-shot with `0xFFFFFFFF` initial count, divide-by-1; spin for exactly 10 ms using HPET; read `CURRENT_COUNT`; compute `ticks_per_ms = (0xFFFFFFFF - current_count) / 10`. Repeat 3 times and average to reduce HPET jitter.

- [ ] `lapic_calibrate_timer()`:
  - [ ] Set LAPIC timer to one-shot mode, divide-by-1, `INITIAL_COUNT = 0xFFFFFFFF`
  - [ ] Record `t0 = hpet_read_ns()`; spin until `hpet_read_ns() - t0 >= 10_000_000` (10 ms)
  - [ ] Read `CURRENT_COUNT`; compute `ticks = 0xFFFFFFFF - current_count`; `ticks_per_ms = ticks / 10`
  - [ ] Repeat 3×; use median to discard outliers
- [ ] Set LAPIC timer to periodic mode with `INITIAL_COUNT = ticks_per_ms` (1 ms tick)
- [ ] Store `g_lapic_ticks_per_ms`; set `g_tick_ns = 1_000_000` (1 ms in ns)
- [ ] `lapic_read_ns()` -- `rdtsc()`-based high-resolution read using `ticks_per_ms` to ns conversion
- [ ] Boot log: `[LAPIC] Timer calibrated: %u ticks/ms via HPET (±%u ticks jitter)`
- [ ] Commit: `"lapic: HPET-calibrated timer -- 10 ms reference window, median ticks/ms, periodic 1 ms tick"`

## 3. MSI Vector Bitmap Allocator `[Sonnet]`

Maintain a bitmap of vectors 32–254. `apic_alloc_msi_vector()` finds the lowest free bit, marks it, and returns the LAPIC `Message Address` + `Message Data` pair ready for `pci_enable_msi`. `apic_free_msi_vector(vec)` clears the bit. Both are protected by a `ticket_lock_t`.

**Files:** `src/kernel/drivers/lapic.c`, `include/kernel/drivers/lapic.h`

- [ ] Define `g_msi_vector_bitmap[223 / 64 + 1]` (bits 32–254) and `g_msi_vector_lock` (`ticket_lock_t`) in `lapic.c`
- [ ] `apic_alloc_msi_vector()`: take lock; `bsf` over bitmap to find lowest 0 bit in range 32–254; set bit; release lock; return vector or 0 on exhaustion
- [ ] `apic_free_msi_vector(vec)`: take lock; clear bit; release lock
- [ ] `apic_msi_address(dest_lapic_id)` → `0xFEE00000 | (dest_lapic_id << 12)` (fixed destination, physical mode)
- [ ] `apic_msi_data(vector)` → `vector | (0 << 15) | (0 << 14) | (0 << 11)` (edge-triggered, fixed delivery)
- [ ] `EXPORT_SYMBOL(apic_alloc_msi_vector)`, `EXPORT_SYMBOL(apic_free_msi_vector)`, `EXPORT_SYMBOL(apic_msi_address)`, `EXPORT_SYMBOL(apic_msi_data)` -- consumed by `pci_enable_msi/msix` in TODO-08
- [ ] Boot log: `[APIC] MSI vector pool: 32–254 (%u vectors available)`
- [ ] Per-vector dispatch gating for shared GSI chains: split the global `irq_chain_lock` entry gate in `src/kernel/irq.c` (`irq_shared_dispatch_wrapper` entry increment, `irq_chain_drain` callers) into per-`irq_entry` locks so a registration/release drain on one GSI cannot stall ISR entry on unrelated shared lines; filed from `01-boot-platform/TODO-11` §5 perf review
- [ ] Commit: `"lapic: MSI vector bitmap allocator -- apic_alloc/free_msi_vector, address/data helpers"`

## 4. TLB Shootdown IPI `[Opus]`

Install the `IPI_TLB_FLUSH` ISR that runs `invlpg` over the requested VA range and atomically acks. The VMM/scheduler side (`03-memory-concurrency/TODO-07-smp-phase2.md §2`) calls `tlb_shootdown(cpu_mask, vaddr, len)` which invokes `lapic_send_ipi` per target CPU; this section provides the hardware send path and the per-CPU handler.

**Files:** `src/kernel/drivers/lapic.c`, `src/kernel/mm/tlb.c` (new), `include/kernel/mm/tlb.h` (new)

> [!IMPORTANT]
> The `invlpg` loop and atomic ack must run entirely in the ISR -- do not defer to a workqueue. The ack counter must be stack-allocated or in a per-CPU slot (never global) to allow concurrent shootdowns from two different CPUs. See `03-memory-concurrency/TODO-07-smp-phase2.md §2` for the allocation discipline.

- [ ] Define `tlb_shootdown_work_t { uintptr_t vaddr; size_t len; atomic_int pending; }` in `include/kernel/mm/tlb.h`
- [ ] `IPI_TLB_FLUSH` vector: register `tlb_shootdown_isr` in IDT via `idt_register_handler(IPI_VECTOR_TLB_SHOOTDOWN, tlb_shootdown_isr)`
- [ ] `tlb_shootdown_isr`: load per-CPU pending work pointer; loop `invlpg(work->vaddr + i * PAGE_SIZE)` for range; `atomic_fetch_sub(&work->pending, 1)`; `lapic_eoi()`
- [ ] `lapic_send_tlb_shootdown_ipi(cpu_mask, work_ptr)`: store `work_ptr` into each target CPU's per-CPU pending slot; for each CPU in mask call `lapic_send_ipi(cpu_id, IPI_VECTOR_TLB_SHOOTDOWN | LAPIC_FIXED)` (use x2APIC or xAPIC path via §1)
- [ ] Single-CPU fast path: if only local CPU in mask, just `invlpg` directly -- no IPI overhead
- [ ] Boot log: `[APIC] TLB shootdown IPI registered (vector 0x%02X)`
- [ ] Commit: `"lapic: TLB shootdown IPI -- invlpg handler, atomic ack counter, per-CPU work slot"`

## 5. NMI Watchdog via PMI Counter Overflow `[Opus]`

Configure `IA32_PERFEVTSEL0` as a fixed CPU-cycles counter and set its overflow to deliver an NMI via the LAPIC PMI vector. The NMI fires approximately every 1 s regardless of `CLI` or held spinlocks. The NMI handler checks the per-CPU `cpu_heartbeat` counter; a stall triggers a serial backtrace and the red framebuffer banner.

**Files:** `src/kernel/drivers/lapic.c`, `src/kernel/drivers/nmi_watchdog.c` (new), `include/kernel/drivers/nmi_watchdog.h` (new)

> [!IMPORTANT]
> The NMI handler runs on the NMI stack (a dedicated per-CPU IST entry in the TSS). It must not call any sleeping primitive, allocate memory, or acquire any non-NMI-safe lock. Use `serial_write()` and framebuffer direct pixel writes only. The PMI counter period must be set so that a single-core machine at 1 GHz fires at ≥ 1 Hz; use `IA32_FIXED_CTR_CTRL` for TSC-independent fixed counters if `CPUID.0AH` reports architectural performance monitoring.

- [ ] Check `CPUID.01H:ECX[8]` (TM2) and `CPUID.0AH` (APM version ≥ 1) for PMI support
- [ ] `nmi_watchdog_init()`: program `IA32_PERFEVTSEL0` with `OS=1, USR=1, EN=1, INT=1, event=0x3C` (CPU_CLK_UNHALTED.THREAD); set `IA32_PMC0` to `-(cpu_freq_hz / 1)` (overflow every ~1 s)
- [ ] Configure LAPIC PMI LVT (offset `0x340`): `DELIVERY_MODE=NMI, MASKED=0`; consume MADT type-4 NMI source for LINTx wiring
- [ ] `nmi_handler()` (ISR): read `cpu_heartbeat[this_cpu()]`; compare to snapshot from previous NMI; if unchanged: `serial_write("[NMI WD] Hard lockup CPU%u -- RIP=0x%016llx\n", ...)` + `watchdog_draw_banner()`; re-arm `IA32_PMC0`; `iret`
- [ ] Re-arm PMI: write `IA32_PMC0` initial value and clear overflow status in `IA32_PERF_GLOBAL_OVF_CTRL` before returning from NMI handler
- [ ] Registry: `HKLM\SYSTEM\Watchdog\NmiEnabled` (`REG_DWORD 1` default); `HKLM\SYSTEM\Watchdog\NmiIntervalSeconds` (default 1)
- [ ] Boot log: `[NMI WD] NMI watchdog armed: counter=0x%016llx, ~%u s interval` or `[NMI WD] PMI not available, NMI watchdog disabled`
- [ ] Commit: `"lapic: NMI watchdog -- IA32_PERFEVTSEL0 PMI, NMI LVT, heartbeat check, backtrace on stall"`

## 6. Interrupt Profiler -- `/sys/interrupts` `[Sonnet]`

Increment a per-vector `irq_count` and accumulate `irq_ns` in each IDT stub before dispatching to the registered handler. Expose the table via a `/sys/interrupts` VFS file and `irqstats` shell command. Wire `NtQuerySystemInformation(SystemInterruptInformation)` to return the same data.

**Files:** `src/kernel/drivers/idt.c`, `include/kernel/drivers/idt.h`, `src/kernel/fs/sysfs_interrupts.c` (new), `src/shell/cmd_irqstats.c` (new)

> [!IMPORTANT]
> Per-vector counters must be updated in the ISR -- at the highest possible IRQL -- so they must be `atomic_uint64_t` incremented with `atomic_fetch_add` (no lock). Reading them for `/sys/interrupts` does not need a lock; a torn 64-bit read on a live counter is acceptable (monotonic, never destructive).

- [ ] Add `irq_stats_t { atomic_uint64_t count; atomic_uint64_t ns_total; const char *name; }` array `g_irq_stats[256]` to `idt.c`
- [ ] In each IDT stub (at entry, before dispatching): `t0 = rdtsc()`; on return: `atomic_fetch_add(&g_irq_stats[vec].count, 1); atomic_fetch_add(&g_irq_stats[vec].ns_total, rdtsc_to_ns(rdtsc() - t0))`
- [ ] `idt_register_handler(vec, handler, name)` -- store `name` in `g_irq_stats[vec].name`; update existing callers to pass name string
- [ ] `/sys/interrupts` VFS read callback: iterate `g_irq_stats[0..255]`; skip zero-count entries; format: `VEC  NAME                 COUNT       TOTAL_US`
- [ ] `irqstats` shell command: reads `/sys/interrupts`, pretty-prints; flag `--sort-count` / `--sort-time`
- [ ] `NtQuerySystemInformation(SystemInterruptInformation, buf, size)` -- fill array of `SYSTEM_INTERRUPT_INFORMATION { Count, Time }` per vector
- [ ] Commit: `"apic: interrupt profiler -- per-vector count+ns, /sys/interrupts VFS, irqstats command"`

---

## OS Comparison


| ⭐  | Feature                                             | 🪟 Win11                                                      | 🐧 Linux                                                           | 🚀 Impossible OS                                                     |
| --- | --------------------------------------------------- | ------------------------------------------------------------- | ------------------------------------------------------------------ | -------------------------------------------------------------------- |
| 💎  | x2APIC MSR mode -- 32-bit APIC IDs, atomic IPI      | ✅ HAL enables x2APIC on systems                              | ✅ `arch/x86/kernel/apic/x2apic_*.c`; `x2apic_enabled()` check     | ⬜ §1 -- `lapic_read/write` dispatch, 32-bit `apic_id`, single-MSR   |
| 💎  | LAPIC timer calibration via HPET reference          | ✅ HAL `HalCalibratePerformanceCounter` via HPET              | ✅ `lapic_calibrate()`; HPET 10 ms window;                         | ⬜ §2 -- 3-sample median, `g_lapic_ticks_per_ms`, `g_tick_ns` update |
| 💎  | TLB shootdown IPI -- `invlpg` handler + ack counter | ✅ `KiIpiTlbFlush`; per-processor IPI; KeSynchronizeExecution | ✅ `flush_tlb_others_ipi()`; `on_each_cpu` IPI + `invlpg`          | ⬜ §4 -- `IPI_TLB_FLUSH` ISR, per-CPU work slot,                     |
| ⭐  | NMI watchdog via PMI counter overflow               | ⚠️ NMI used internally; not exposed                           | ✅ `CONFIG_X86_NMI_WATCHDOG`; PMU overflow → NMI;                  | ⬜ §5 -- `IA32_PERFEVTSEL0` PMI, NMI LVT, backtrace                  |
| 💎  | MSI vector bitmap allocator                         | ✅ `HalGetInterruptVectorForMsi`; vector pool in HAL          | ✅ `arch/x86/kernel/apic/vector.c`; `irq_domain` vector allocation | ⬜ §3 -- bitmap 32–254, `ticket_lock_t`, `apic_alloc_msi_vector`     |
| ⭐  | Per-vector interrupt profiler with timing           | ❌ ETW performance counters only; no                          | ⚠️ `/proc/interrupts` -- counts only, no                           | ⬜ §6 -- `irq_count` + `irq_ns` per vector,                          |

> **After §1–6:** Impossible OS matches Windows NT HAL and Linux `arch/x86/` on the full APIC feature set needed for production SMP hardware. Two exclusive differentiators: the NMI watchdog (§5) goes beyond the software heartbeat in TODO-10 by firing an NMI even when `CLI` masks all maskable interrupts -- catching spinlock-induced hard lockups that a timer ISR can never detect. The interrupt profiler (§6) adds per-vector `irq_ns` timing data that Linux `/proc/interrupts` lacks and Windows exposes only through heavyweight ETW tracing.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU `-cpu host`: boot log shows `[LAPIC] x2APIC mode active`; `[LAPIC] Timer calibrated: N ticks/ms via HPET`
- [ ] QEMU `-cpu qemu64` (no x2APIC): boot log shows `[LAPIC] xAPIC mode`; timer still calibrated via HPET
- [ ] SMP (QEMU `-smp 2`): trigger a VMM `mprotect` call; serial log shows `[APIC] TLB shootdown: CPU1 acked` within 1 ms
- [ ] `irqstats` shell command output shows entries for vectors `0x20` (PIT), `0x21` (keyboard), and any NVMe MSI vector; counts increment across calls
- [ ] MSI: `apic_alloc_msi_vector()` returns distinct vectors for 4 consecutive calls; all in range 32–254
- [ ] NMI watchdog (QEMU `-cpu host`): boot log shows `[NMI WD] NMI watchdog armed`; blocking a CPU with `cli` + spin for 2 s triggers `[NMI WD] Hard lockup CPU0`
- [ ] `NtQuerySystemInformation(SystemInterruptInformation)` returns non-zero counts for active vectors
- [ ] Commit: `"apic: APIC architecture complete -- x2APIC, calibration, TLB shootdown, NMI WD, MSI alloc, profiler"`
