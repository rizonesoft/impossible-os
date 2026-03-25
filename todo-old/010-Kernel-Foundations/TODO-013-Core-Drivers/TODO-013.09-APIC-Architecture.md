# TODO-013.09-APIC-Architecture — APIC Architecture

> **Goal:** Implement a complete, production-quality APIC subsystem that replaces the
> legacy 8259 PIC, enabling multi-core interrupt routing, per-core timers, and
> Inter-Processor Interrupts (IPIs). Support both xAPIC (MMIO) and x2APIC (MSR) modes.

> [!IMPORTANT]
> **Spec Reference:** [`docs/specs/drivers/APIC-Architecture.md`](../../../../docs/specs/drivers/APIC-Architecture.md)
> — Contains full register layouts, MADT parsing details, calibration procedure,
> and the end-to-end initialization sequence.
> **XREF:** [TODO-013-Core-Drivers.md](../TODO-013-Core-Drivers.md) — master built-in driver overview

> [!CAUTION]
> **Memory Rule:** LAPIC and IOAPIC MMIO regions must be mapped with **Strong Uncacheable (UC)**
> page attributes. Using Write-Back (WB) caching will cause the CPU to read stale interrupt
> status from cache lines. See spec §3.1.

> [!WARNING]
> → XREF: `TODO-010-Bootloader.md §1.6` — On Hyper-V Gen 2, the MADT `PCAT_COMPAT` flag
> (bit 0 at offset 36) is cleared to 0, meaning **no 8259 PIC exists**. All PIC I/O port
> accesses (`0x20`, `0x21`, `0xA0`, `0xA1`) must be skipped when `PCAT_COMPAT=0`.

---

## Current Status

| Component                | File             | Status  | Notes                                                        |
| ------------------------ | ---------------- | :-----: | ------------------------------------------------------------ |
| ACPI MADT parsing        | `acpi.c`         | ✅ Done | Parses LAPIC IDs, x2APIC (type 9), IOAPIC base, ISOs, NMIs  |
| LAPIC init               | `lapic.c`        | ✅ Done | SVR, TPR, ESR, EOI — xAPIC MMIO mode only                   |
| IOAPIC init              | `ioapic.c`       | ✅ Done | Routes 16 ISA IRQs with ISO flags to BSP                    |
| PIC conditional disable  | `pic.c`          | ✅ Done | Skips PIC when `PCAT_COMPAT=0`                               |
| LAPIC timer              | `lapic.c`        | ⚠️ Partial | Hardcoded ICR=10000000 (xv6-style, no calibration)       |
| SMP / AP boot            | `smp.c`          | ✅ Done | INIT-SIPI-SIPI sequence                                     |
| ISR drain (PIC→APIC)     | `boot_storage.c` | ✅ Done | Drains stale ISR bits on transition                          |
| IPI send functions       | `lapic.c`        | ✅ Done | `lapic_send_ipi()`, `lapic_send_ipi_all_but_self()`         |
| LVT configuration        | `lapic.c`        | ⚠️ Partial | All LVT masked (xv6 pattern), Error=0xFE but no ISR     |
| ISO polarity/trigger     | `ioapic.c`       | ⚠️ Partial | Applied for ISA IRQs only, single-IOAPIC assumed         |

> [!NOTE]
> **Codebase Observations:**
> - `lapic_read()` / `lapic_write()` are raw MMIO only — no x2APIC MSR path
> - `lapic_id()` returns 8-bit ID (`>> 24 & 0xFF`) — breaks on >255 cores
> - `ioapic_route_irq()` already applies ISO polarity (bit 13) and trigger (bit 15) flags
> - `ioapic_init()` only handles a single IOAPIC — no multi-IOAPIC GSI routing
> - LAPIC Error LVT is set to vector 0xFE but no actual error ISR exists in IDT
> - IPI vectors defined (`IPI_VECTOR_TLB_SHOOTDOWN=0xFE`) but no handler installed
> - MADT Type 4 (Local APIC NMI) is parsed by `acpi.c` but never consumed by `lapic.c`

---

## 1. x2APIC Mode Support

**Prompt:** Extend the LAPIC driver to detect and enable x2APIC mode when supported by the CPU. x2APIC eliminates MMIO overhead by mapping APIC registers into MSR space (base `0x800`). The current `lapic_read()` / `lapic_write()` functions are raw MMIO and `lapic_id()` returns only 8 bits — insufficient for systems with >255 logical CPUs. Detect x2APIC support via `CPUID.01H:ECX[bit 21]`. Transition by setting both EN (bit 11) and EXTD (bit 10) in `IA32_APIC_BASE` MSR (`0x1B`). Provide an abstraction layer so callers use the same API regardless of xAPIC vs x2APIC mode underneath. The ACPI MADT parser already handles x2APIC entries (type 9) with 32-bit APIC IDs — wire them through. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"lapic: x2APIC MSR-based register access"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Detect x2APIC support via `CPUID.01H:ECX[bit 21]`
- [ ] Add `static bool x2apic_mode` flag to `lapic.c`
- [ ] Implement `lapic_read_msr(offset)` / `lapic_write_msr(offset, value)` — MSR access path
  - [ ] MSR address formula: `0x800 + (mmio_offset >> 4)`
- [ ] Implement mode transition: set EN + EXTD in `IA32_APIC_BASE` MSR
  - [ ] Must reinitialize all programmable registers after transition
  - [ ] Verify: writes to reserved bits raise `#GP` — zero all reserved fields
  - [ ] Cannot transition from x2APIC back to xAPIC without full APIC reset
- [ ] Refactor `lapic_read()` / `lapic_write()` — dispatch to MMIO or MSR based on `x2apic_mode`
- [ ] Update `lapic_id()` — return full 32-bit ID in x2APIC mode (vs 8-bit in xAPIC)
  - [ ] Update `struct cpu_info.apic_id` from `uint8_t` to `uint32_t`
- [ ] Update `lapic_init()` — prefer x2APIC if CPU supports it, fallback to xAPIC
- [ ] ICR becomes single 64-bit MSR write (atomic IPI generation, no write-ordering bug)
  - [ ] Update `lapic_send_ipi()` for x2APIC: single `wrmsr(0x830, id << 32 | command)`
  - [ ] No delivery status polling needed in x2APIC (immediate delivery)
- [ ] Update `lapic_init_ap()` for x2APIC mode on Application Processors
- [ ] Test: verify timer, EOI, IPI all work in x2APIC mode on QEMU (`-cpu host`)
- [ ] Commit: `"lapic: x2APIC MSR-based register access"`

---

## 2. PIT-Based Timer Calibration

**Prompt:** Replace the hardcoded LAPIC timer Initial Count Register (ICR) with a proper PIT-based calibration sequence. The current `lapic_timer_init()` uses a hardcoded `ICR=10000000` with divide-by-1 (xv6-style) — the `hz` parameter is ignored entirely. This works on QEMU TCG (~26 MHz bus) but produces wildly wrong tick rates on real hardware where bus frequencies vary from 100 MHz to 400+ MHz. Use the PIT (1.193182 MHz) as a reference clock: configure LAPIC timer to one-shot with max initial count (`0xFFFFFFFF`), spin-wait for 10ms via PIT channel 2, then calculate `ticks_per_second = (0xFFFFFFFF - current_count) × 100`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"lapic: PIT-calibrated timer frequency"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!CAUTION]
> The current hardcoded ICR means `lapic_timer_init(100)` does NOT actually produce
> 100 Hz ticks. The scheduler quantum and `sleep()` timers are all wrong on non-QEMU
> hardware. This is the **#1 real-hardware blocker**.

- [ ] Set Divide Configuration Register (`0x3E0`) to divide-by-16 (`0x03`)
  - [ ] Avoids Bochs emulator bugs with divide-by-1
- [ ] Configure Timer LVT (`0x320`) in one-shot mode with a calibration vector
- [ ] Write `0xFFFFFFFF` to Initial Count Register (`0x380`)
- [ ] Spin-wait 10ms using PIT channel 2 (one-shot countdown)
  - [ ] PIT channel 2 gate at port `0x61`, counter at port `0x42`, mode at `0x43`
  - [ ] 10ms = 11932 PIT ticks at 1.193182 MHz
- [ ] Read Current Count Register (`0x390`)
- [ ] Calculate: `ticks_per_10ms = 0xFFFFFFFF - current_count`
- [ ] Derive: `ticks_per_second = ticks_per_10ms × 100`
- [ ] Store calibrated frequency, compute Initial Count for desired Hz (e.g., 100 Hz)
- [ ] Fallback chain if PIT calibration reads 0 ticks:
  - [ ] Try `CPUID.15H` — TSC/Core Crystal Clock ratio (newer Intel CPUs)
  - [ ] Try `CPUID.16H` — processor bus frequency (newer Intel CPUs)
  - [ ] Last resort: keep xv6 hardcoded value with klog warning
- [ ] Log: `[LAPIC] Timer calibrated: %u ticks/sec (divider=16)`
- [ ] Commit: `"lapic: PIT-calibrated timer frequency"`

---

## 3. Multi-IOAPIC and Redirection Hardening

**Prompt:** The current `ioapic.c` only supports a single I/O APIC — it reads one base address from the MADT and routes all 16 ISA IRQs through it. Server hardware (and even some desktop boards) have 2+ IOAPICs, each responsible for a GSI range. Additionally, while ISO polarity/trigger flags are already applied in `ioapic_route_irq()`, the GSI→IOAPIC pin mapping is hardcoded (GSI=pin). Implement full multi-IOAPIC support: store an array of IOAPIC descriptors from the MADT, route GSIs to the correct IOAPIC by checking `gsi_base ≤ GSI < gsi_base + max_entries`, and calculate the pin as `GSI - gsi_base`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"ioapic: multi-IOAPIC and GSI routing hardening"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!NOTE]
> **Codebase fact:** `ioapic_route_irq()` already correctly applies ISO polarity (bit 13)
> and trigger mode (bit 15) from MADT flags — this was verified by reading `ioapic.c` lines
> 192–206. The gap is multi-IOAPIC support and dynamic GSI→pin routing.

- [ ] Replace single `ioapic_base` global with array: `struct ioapic_desc ioapics[MAX_IOAPICS]`
  - [ ] Each entry: `{ volatile uint32_t *base, uint32_t gsi_base, uint32_t max_entries, uint8_t id }`
- [ ] Parse all MADT Type 1 (IOAPIC) entries during `acpi_init()`:
  - [ ] Store base address, GSI base, IOAPIC ID for each
  - [ ] Currently `acpi_get_ioapic_base()` returns only a single address — extend API
- [ ] Implement `ioapic_find(uint32_t gsi)` — find IOAPIC descriptor for a given GSI:
  - [ ] Iterate all IOAPICs, return one where `gsi_base ≤ gsi < gsi_base + max_entries`
  - [ ] Pin index = `gsi - ioapic->gsi_base`
- [ ] Refactor `ioapic_route_irq()` to accept GSI (not pin) and dispatch to correct IOAPIC
- [ ] Refactor `ioapic_mask_irq()` / `ioapic_unmask_irq()` to use GSI-based lookup
- [ ] Update ISA IRQ routing loop in `ioapic_init()` to use `ioapic_find(gsi)`
- [ ] Support GSIs > 23 (second IOAPIC covers pins 24–47 on server boards)
- [ ] Test: verify IRQ routing still works on single-IOAPIC QEMU
- [ ] Commit: `"ioapic: multi-IOAPIC and GSI routing hardening"`

---

## 4. LAPIC Error Handler

**Prompt:** The LAPIC Error LVT is currently set to vector `0xFE` in `lapic_init()` but no ISR is registered in the IDT for that vector. If the LAPIC generates an error (illegal vector, send/receive error, illegal register access), the CPU takes interrupt 0xFE and hits an unhandled vector — likely a silent hang or triple fault. Implement a proper error ISR that reads the Error Status Register (ESR), logs the error type, and clears the register. This is essential for debugging hardware interrupt issues on real hardware. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"lapic: APIC error ISR handler"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Register ISR for vector `0xFE` in the IDT (LAPIC Error)
- [ ] Error ISR implementation:
  - [ ] Write 0 to ESR (`0x280`) — latches current errors (Intel manual requirement)
  - [ ] Read ESR — get latched error bits
  - [ ] Decode and log error flags:
    - [ ] Bit 0: Send Checksum Error
    - [ ] Bit 1: Receive Checksum Error
    - [ ] Bit 2: Send Accept Error
    - [ ] Bit 3: Receive Accept Error
    - [ ] Bit 4: Redirectable IPI (reserved)
    - [ ] Bit 5: Send Illegal Vector
    - [ ] Bit 6: Received Illegal Vector
    - [ ] Bit 7: Illegal Register Address
  - [ ] Log: `[LAPIC] Error on CPU %u: ESR=0x%02x (%s)`
  - [ ] Send EOI
- [ ] Separate error vector from IPI_VECTOR_TLB_SHOOTDOWN (currently both 0xFE):
  - [ ] LAPIC Error → vector `0xFC`
  - [ ] Update `LAPIC_REG_LVT_ERROR` write in `lapic_init()` and `lapic_init_ap()`
- [ ] Commit: `"lapic: APIC error ISR handler"`

---

## 5. Local Vector Table (LVT) Complete Configuration

**Prompt:** Configure all LVT entries for production use. Currently all LVT entries are masked (xv6 pattern) with only the Error LVT getting a bare vector assignment. The remaining entries (LINT0, LINT1, Thermal, PMC, CMCI) should be configured based on MADT Local APIC NMI records (Entry Type 4) and sensible defaults. The MADT parser in `acpi.c` already parses Type 4 records (line 165–171) but `lapic.c` never reads them. LINT1 is typically wired to NMI on all platforms — leaving it masked means NMI-based debugging (watchdog, hang detection) is broken. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"lapic: complete LVT configuration"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!NOTE]
> **Codebase fact:** `acpi.h` defines `struct madt_lapic_nmi` (type 4) with
> `acpi_processor_id`, `flags`, and `lint` fields, but no API exposes this
> data to `lapic.c`. An accessor function is needed.

- [ ] Add `acpi_get_lapic_nmi()` API to `acpi.h` / `acpi.c`:
  - [ ] Returns MADT Type 4 records (processor ID, flags, LINT pin)
  - [ ] Needed by `lapic_init()` to configure correct LINT pin as NMI
- [ ] Configure LINT0 (`0x350`):
  - [ ] BSP: ExtINT mode (`111b`) for legacy PIC passthrough (if `acpi_pcat_compat() == 1`)
  - [ ] APs: Masked (APs never receive PIC interrupts)
  - [ ] APIC-only platforms (`PCAT_COMPAT=0`): Masked on all CPUs
- [ ] Configure LINT1 (`0x360`) per MADT Type 4 records:
  - [ ] Default (if no MADT record): NMI mode (`100b`), edge-triggered, unmasked
  - [ ] Honor MADT flags for polarity and trigger mode
- [ ] Configure Thermal Sensor LVT (`0x330`): Fixed mode, vector `0xFB`, unmasked
  - [ ] ISR: read thermal status MSR, log temperature threshold event
  - [ ] → XREF: `TODO-011-x86-64.md §19.2` — Thermal Monitoring
- [ ] Configure Performance Counter LVT (`0x340`): Fixed mode, masked (unmask when profiling)
  - [ ] → XREF: `TODO-011-x86-64.md §8.2` — Performance Monitoring Counters
- [ ] Configure CMCI LVT (`0x2F0`): Fixed mode, masked (enable for RAS/ECC logging)
  - [ ] Check `CPUID.(EAX=1):EDX[bit 0]` — IA32_MCG_CAP.MCG_CMCI_P
- [ ] Apply same LVT configuration in `lapic_init_ap()` for Application Processors
- [ ] Commit: `"lapic: complete LVT configuration"`

---

## 6. IPI Abstraction and TLB Shootdown

**Prompt:** The LAPIC driver already has `lapic_send_ipi()` and `lapic_send_ipi_all_but_self()` functions, along with pre-defined IPI vectors (`IPI_VECTOR_RESCHEDULE=0xFD`, `IPI_VECTOR_TLB_SHOOTDOWN=0xFE`). However, no ISRs are installed for these vectors — they are dead code. The primary use case beyond SMP boot is TLB shootdown: when a page table entry is modified, all cores that may have cached the old mapping must invalidate their TLB. Without TLB shootdown, stale mappings cause silent data corruption in SMP. Also add a reschedule IPI so the scheduler can preempt threads on other cores. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: IPI handlers and TLB shootdown"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!CAUTION]
> **SMP correctness.** Without TLB shootdown, `vmm_unmap_page()` only invalidates
> the calling core's TLB. Other cores continue using stale mappings — reading freed
> memory, writing to remapped pages. This is a **silent data corruption** bug.

- [ ] Install ISR for `IPI_VECTOR_TLB_SHOOTDOWN` (0xFE) in IDT:
  - [ ] Fix vector collision: currently shares 0xFE with LAPIC Error LVT (see §4)
  - [ ] Reassign: TLB shootdown = `0xFE`, LAPIC Error = `0xFC`, Reschedule = `0xFD`
- [ ] Install ISR for `IPI_VECTOR_RESCHEDULE` (0xFD) in IDT:
  - [ ] Handler: call `scheduler_yield()` to trigger preemption on receiving core
  - [ ] Send EOI
- [ ] Implement TLB shootdown mechanism:
  - [ ] `tlb_shootdown(virt_addr)` — send IPI to all other cores with target address
  - [ ] Per-CPU shootdown mailbox: `volatile uintptr_t tlb_shootdown_addr[MAX_CPUS]`
  - [ ] IPI handler: read `tlb_shootdown_addr[my_cpu_id]`, execute `invlpg [addr]`, send EOI
  - [ ] Barrier: sender waits for all receivers to acknowledge (atomic counter)
- [ ] Broadcast shootdown variant: `tlb_shootdown_all()` — flush entire TLB on all cores
- [ ] Wire `vmm_unmap_page()` to call `tlb_shootdown(virt_addr)` on SMP systems
- [ ] Wire `vmm_map_page()` (remap case) to call `tlb_shootdown(virt_addr)`
- [ ] x2APIC optimization: use single atomic `wrmsr` for IPI (no delivery status poll)
- [ ] Commit: `"kernel: IPI handlers and TLB shootdown"`

---

## 7. Directed EOI Support

**Prompt:** Implement Directed EOI to reduce unnecessary APIC bus traffic for level-triggered interrupts. Without Directed EOI, every `lapic_eoi()` broadcasts an EOI message to all IOAPICs to clear the Remote IRR bit — this generates bus traffic proportional to the number of IOAPICs × the number of level-triggered IRQs. With Directed EOI enabled (SVR Bit 12), the OS takes manual responsibility for clearing Remote IRR on the specific IOAPIC pin. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"lapic: directed EOI for level-triggered interrupts"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Check APIC Version Register Bit 24 — `1` = Directed EOI supported
- [ ] If supported, set SVR Bit 12 (Directed EOI enable)
  - [ ] Update `lapic_init()` SVR write: `LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR | (1 << 12)`
- [ ] Maintain a per-vector metadata table: `{ uint8_t gsi, bool level_triggered }`
  - [ ] Populated during `ioapic_route_irq()` based on IOREDTBL bit 15
- [ ] Modify level-triggered IRQ handlers:
  - [ ] After `lapic_eoi()`, also write to IOAPIC EOI register (offset `0x40`) with the vector
  - [ ] Only needed for entries where `level_triggered == true`
- [ ] Provide `lapic_eoi_directed(uint8_t vector)` for drivers to call instead of bare `lapic_eoi()`
- [ ] Edge-triggered IRQs: no change (Remote IRR not used)
- [ ] Test: verify PCI device interrupts work correctly with Directed EOI
- [ ] Commit: `"lapic: directed EOI for level-triggered interrupts"`

---

## 8. MSI/MSI-X Interrupt Support

**Prompt:** Modern PCI/PCIe devices (NVMe, xHCI, VirtIO, Intel NIC) use Message Signaled Interrupts (MSI/MSI-X) instead of legacy pin-based IRQs. MSI bypasses the IOAPIC entirely — the device writes a message directly to the LAPIC's interrupt message address (`0xFEE00000 + destination`). This reduces latency, eliminates shared IRQ conflicts, and enables per-queue interrupt steering. There is currently zero MSI/MSI-X infrastructure in the kernel. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: MSI/MSI-X interrupt support"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Implement PCI capability list walker: `pci_find_capability(dev, cap_id)`
  - [ ] Follow capability linked list starting from PCI config offset `0x34`
- [ ] Detect MSI capability (Capability ID `0x05`)
- [ ] Detect MSI-X capability (Capability ID `0x11`)
- [ ] Implement interrupt vector allocator:
  - [ ] Dynamic vector pool (e.g., vectors `0x80`–`0xEF`) for MSI/MSI-X
  - [ ] `irq_alloc_vector()` → returns available vector, `irq_free_vector()` releases it
  - [ ] Avoids conflicts with fixed IOAPIC vectors (32–47) and system vectors (0xFC–0xFF)
- [ ] Implement `pci_enable_msi(dev, vector)`:
  - [ ] Write Message Address: `0xFEE00000 | (dest_apic_id << 12)`
  - [ ] Write Message Data: `vector | (delivery_mode << 8)`
  - [ ] Set MSI Enable bit in MSI Control register
  - [ ] Disable IOAPIC pin routing for this device's legacy IRQ
- [ ] Implement `pci_enable_msix(dev, table_entry, vector)`:
  - [ ] Map MSI-X table from BAR (BIR field in MSI-X capability)
  - [ ] Write Message Address, Message Data, unmask in table entry
  - [ ] MSI-X supports up to 2048 vectors per device (one per queue)
- [ ] Implement `pci_disable_msi(dev)` / `pci_disable_msix(dev)` — revert to legacy
- [ ] Test with VirtIO-blk (MSI) and QEMU xHCI (MSI-X)
- [ ] Commit: `"kernel: MSI/MSI-X interrupt support"`

---

## 9. NMI Watchdog (🚀 Impossible OS Feature)

**Prompt:** Implement an NMI-based kernel watchdog that detects CPU lockups (hard hangs) and spinlock deadlocks. Configure the LAPIC timer or a performance counter to fire an NMI at regular intervals (e.g., every 1 second). If the NMI handler detects that the per-CPU heartbeat counter hasn't incremented since the last NMI, the CPU is stuck — dump its register state, call stack, and held locks to serial/log, then either panic or attempt recovery. This is far more robust than a timer-based watchdog because NMIs cannot be masked by `CLI`. Neither Windows nor Linux exposes NMI watchdog status in a GUI — Impossible OS can show a "CPU Health" indicator per-core in Task Manager. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: NMI watchdog for lockup detection"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Competitive Edge:** Linux has `nmi_watchdog=1` but it's a CLI boot parameter with
> no user-visible feedback. Windows has a kernel watchdog (WHEA) but no user indicator.
> Impossible OS can show per-core ❤️ heartbeat status in Task Manager — green = healthy,
> yellow = slow, red = locked up — making system hangs visible before they become fatal.

- [ ] Implement per-CPU heartbeat counter: `volatile uint64_t heartbeat[MAX_CPUS]`
  - [ ] Incremented by the scheduler on every timer tick (proves CPU is progressing)
- [ ] Configure NMI source — two options:
  - [ ] Option A: LAPIC Performance Counter overflow → NMI (delivery mode `100b`)
  - [ ] Option B: Dedicated LAPIC timer in NMI mode (separate from scheduler timer)
- [ ] Implement NMI handler (vector 2):
  - [ ] Read `heartbeat[my_cpu]` — compare with last-seen value
  - [ ] If unchanged for N consecutive NMIs → CPU is locked up
  - [ ] On lockup detection:
    - [ ] Log: `[NMI] CPU %u LOCKUP: heartbeat stuck at %llu for %u seconds`
    - [ ] Dump: RIP, RSP, RFLAGS, CR3, current task name
    - [ ] Dump: backtrace (walk RBP chain)
    - [ ] Dump: held spinlocks (if lock debugging enabled)
    - [ ] Action: configurable — `panic()` or log-and-continue
- [ ] Configurable timeout: default 5 seconds, settable via Registry
- [ ] Expose to Task Manager: per-core health status indicator
- [ ] Commit: `"kernel: NMI watchdog for lockup detection"`

---

## 10. Interrupt Affinity and Load Balancing (🚀 Impossible OS Feature)

**Prompt:** Implement interrupt affinity (CPU steering) and optional automatic IRQ load balancing. Currently all hardware IRQs are routed to the BSP (CPU 0) via `ioapic_init()` — this creates a bottleneck where the BSP handles ALL device interrupts while other cores sit idle waiting for work. Allow per-IRQ affinity assignment (which CPU handles each interrupt) and implement a periodic rebalancer that redistributes IRQs based on measured interrupt rates. Neither Windows nor Linux exposes per-IRQ CPU affinity in a user-facing GUI — Impossible OS can be the first to show and control interrupt routing in a graphical panel. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: interrupt affinity and load balancing"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Competitive Edge:** Windows requires the undocumented `IntPolicy` registry key
> or third-party tools (MSI Utility) to set IRQ affinity. Linux has `irqbalance`
> daemon (background, no GUI). Impossible OS can provide a **Device Manager → IRQ Affinity**
> panel showing per-device interrupt rates and CPU assignment — first OS to make this
> a native, visual feature.

- [ ] Implement `ioapic_set_affinity(uint32_t gsi, uint32_t target_apic_id)`:
  - [ ] Update IOREDTBL destination field (bits 56–63) for the target LAPIC
  - [ ] For MSI: update Message Address destination field
- [ ] Per-IRQ interrupt counter: `uint64_t irq_count[256]` — incremented in each ISR
- [ ] Implement IRQ load balancer (runs every N seconds on a kernel timer):
  - [ ] Read per-CPU total interrupt counts
  - [ ] Identify overloaded CPUs (>2× average interrupt rate)
  - [ ] Migrate highest-rate IRQs from overloaded to underloaded CPUs
  - [ ] Skip: timer interrupt (always on local LAPIC), IPI vectors (not routable)
- [ ] Win32 API: `SetDeviceInterruptAffinity(hDevice, cpuMask)` — per-device steering
- [ ] Task Manager integration: show per-device interrupts/sec and current CPU assignment
- [ ] Commit: `"kernel: interrupt affinity and load balancing"`

---

## 11. Interrupt Latency Profiler (🚀 Impossible OS Feature)

**Prompt:** Instrument the interrupt entry/exit path to measure per-vector interrupt latency (time from hardware interrupt assertion to ISR execution). Use TSC timestamps at IDT entry and before EOI to calculate dispatch latency. Store per-vector statistics (min, max, average, 99th percentile). This data is invaluable for diagnosing DPC/ISR storms, identifying slow interrupt handlers, and proving real-time responsiveness. No consumer OS provides this natively. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: interrupt latency profiler"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> [!TIP]
> **Competitive Edge:** Windows has xperf/WPA for interrupt latency (requires ETW setup,
> developer tools, CLI collection). Linux has `perf sched latency` (CLI). Neither exposes
> this in a GUI for end users. Impossible OS can show a live **interrupt latency heatmap** in
> Task Manager — each vector as a row, latency as a color gradient.

- [ ] Add TSC timestamp at interrupt entry (IDT stub, before any C handler code)
  - [ ] Store in per-CPU scratch: `tsc_isr_entry[MAX_CPUS]`
- [ ] At EOI time, compute `latency_ns = tsc_to_ns(rdtsc() - tsc_isr_entry[cpu])`
- [ ] Per-vector statistics: `struct irq_latency_stats { min, max, total, count, p99 }`
- [ ] Rolling window: keep last 1024 samples per vector for percentile calculation
- [ ] Expose via syscall: `QueryInterruptLatency(vector, &stats)` → used by Task Manager
- [ ] Task Manager panel: per-vector interrupt rate + latency bars
  - [ ] Color-code: green (<10µs), yellow (10–100µs), red (>100µs)
- [ ] Optional: per-CPU DPC (Deferred Procedure Call) latency tracking
- [ ] Commit: `"kernel: interrupt latency profiler"`

---

## Key Files

| File                             | Purpose                                                  |
| -------------------------------- | -------------------------------------------------------- |
| `src/kernel/drivers/lapic.c`     | LAPIC init, EOI, IPI, timer — add x2APIC mode, LVT cfg  |
| `include/kernel/drivers/lapic.h` | LAPIC register offsets, API, IPI vectors                 |
| `src/kernel/drivers/ioapic.c`    | IOAPIC init, redirect table — add multi-IOAPIC, affinity |
| `include/kernel/drivers/ioapic.h`| IOAPIC API — extend for GSI-based routing                |
| `src/kernel/acpi.c`              | MADT parsing — expose LAPIC NMI and multi-IOAPIC data    |
| `include/kernel/acpi.h`          | MADT structures — add LAPIC NMI accessor API             |
| `src/kernel/drivers/pic.c`       | Legacy PIC disable — no changes needed                   |
| `src/kernel/pci.c`               | PCI config space — add capability walker, MSI enable     |
| `src/kernel/irq.c`               | [NEW] Interrupt vector allocator, latency profiler       |
| `include/kernel/irq.h`           | [NEW] IRQ management API                                 |

---

## Priority Order

| ⭐ | Priority | Section                                | Description                                               |
| -- | :------: | -------------------------------------- | --------------------------------------------------------- |
| 💎 | 🔴 P0    | 4. LAPIC Error Handler                 | Vector 0xFE has no ISR — triple fault risk on real HW     |
| 💎 | 🟠 P1    | 2. PIT-Based Timer Calibration         | Hardcoded ICR fails on real hardware — must calibrate     |
| 💎 | 🟠 P1    | 3. Multi-IOAPIC + Redirection Hardening| Multi-IOAPIC and GSI routing required for server boards   |
| 💎 | 🟠 P1    | 6. IPI Handlers + TLB Shootdown        | Required for SMP correctness — silent corruption without  |
| 💎 | 🟡 P2    | 5. LVT Complete Config                 | NMI, Error, Thermal — needed for crash diagnostics        |
| 💎 | 🟡 P2    | 1. x2APIC Mode                         | High-core-count CPUs need 32-bit APIC IDs                 |
| 💎 | 🟡 P2    | 8. MSI/MSI-X                           | Required by NVMe, xHCI, modern NICs                       |
| ⭐ | 🟡 P2    | 9. NMI Watchdog                     | Detect CPU lockups — no OS shows this in GUI              |
| 💎 | 🟢 P3    | 7. Directed EOI                        | Performance optimization, not correctness-critical        |
| ⭐ | 🟢 P3    | 10. Interrupt Affinity              | Per-device IRQ CPU steering — first native GUI for this   |
| ⭐ | 🟢 P3    | 11. Interrupt Latency Profiler      | Live latency heatmap — no consumer OS has this            |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## OS Comparison

| ⭐ | Feature                              | 🪟 Windows 11                          | 🐧 Linux 6.x                            | 🚀 Impossible OS                                 |
| -- | ------------------------------------ | -------------------------------------- | ---------------------------------------- | ------------------------------------------------- |
| 💎 | xAPIC (MMIO)                         | ✅ HAL APIC driver                      | ✅ `arch/x86/kernel/apic/`                | ✅ `lapic.c` — xAPIC init, EOI, IPI               |
| 💎 | x2APIC (MSR)                         | ✅ Enabled by default                   | ✅ Enabled by default                     | ⬜ §1 — MADT type 9 parsed, no MSR path           |
| 💎 | LAPIC timer calibration              | ✅ PIT + HPET + TSC calibration         | ✅ PIT + HPET + CPUID.15H                 | ⚠️ Hardcoded ICR — §2 P1                         |
| 💎 | Multi-IOAPIC support                 | ✅ Full (HAL)                           | ✅ Full (`ioapic.c`)                      | ⬜ §3 — single IOAPIC only                        |
| 💎 | ISO polarity/trigger flags           | ✅ Full                                 | ✅ Full                                   | ⚠️ Applied for ISA IRQs only                     |
| 💎 | LAPIC Error ISR                      | ✅ WHEA error handler                   | ✅ `error_interrupt()` in apic.c           | ⬜ §4 P0 — vector assigned, no ISR                |
| 💎 | LVT NMI/Thermal/PMC                  | ✅ Full LVT config                      | ✅ Full LVT config                        | ⬜ §5 — all LVT entries masked (xv6 pattern)      |
| 💎 | TLB shootdown                        | ✅ `KeFlushTb()`                        | ✅ `native_flush_tlb_multi()`             | ⬜ §6 — IPI vectors defined but no handlers        |
| 💎 | Directed EOI                         | ✅ Enabled when supported               | ✅ `apic_set_eoi_cb()`                    | ⬜ §7 — broadcast EOI only                        |
| 💎 | MSI/MSI-X                            | ✅ Full (WDM/WDF)                       | ✅ Full (`pci_enable_msi*()`)             | ⬜ §8 — zero MSI infrastructure                   |
| 💎 | SMP Boot (SIPI)                      | ✅ `HalpStartProcessor()`               | ✅ `do_boot_cpu()`                        | ✅ `smp.c` — INIT-SIPI-SIPI                       |
| 💎 | PCAT_COMPAT check                    | ✅ HAL checks MADT flags                | ✅ `acpi_sci_override_gsi`                | ✅ `acpi_pcat_compat()` — PIC skipped when 0       |
| ⭐ | IRQ load balancing                   | ⚠️ `IntPolicy` registry (hidden)       | ✅ `irqbalance` daemon (CLI, no GUI)      | ⬜ §10 — **per-device GUI affinity panel**      |
| ⭐ | **NMI watchdog GUI**              | ❌ WHEA (kernel only, no GUI)           | ⚠️ `nmi_watchdog=1` (boot param, no GUI) | ⬜ §9 — per-core ❤️ health in Task Manager        |
| ⭐ | **Interrupt latency profiler**    | ❌ Requires xperf/WPA (developer tools) | ❌ `perf sched latency` (CLI only)        | ⬜ §11 — live latency heatmap in Task Manager      |
| ⭐ | **IRQ affinity GUI**             | ❌ Third-party tools only               | ❌ `/proc/irq/N/smp_affinity` (CLI only)  | ⬜ §10 — first native visual IRQ steering          |
