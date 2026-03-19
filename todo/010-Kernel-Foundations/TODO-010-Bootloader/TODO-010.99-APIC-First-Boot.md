# TODO-010.99 — APIC-First Boot Refactor

> **Goal:** Eliminate the 8259 PIC dependency from the early boot path by moving
> ACPI MADT parsing and LAPIC/IOAPIC initialization **before** the boot splash,
> PIT timer, and device driver init. This fixes Hyper-V Gen 2 (no PIC) and
> aligns the kernel with modern APIC-only interrupt routing from the start.

> [!IMPORTANT]
> **Motivation:** The current boot sequence programs the PIT and routes IRQ0
> through the 8259 PIC for `sleep_ms()`. On Hyper-V Gen 2, the PIC doesn't
> exist, so PIT IRQs never fire and `sleep_ms()` hangs. With APIC-first boot,
> PIT IRQ0 routes through the IOAPIC, which exists on **every** x86-64 platform
> including Hyper-V Gen 2.

> [!WARNING]
> → XREF: `TODO-063.09-APIC-Architecture.md` — Full APIC subsystem TODO
> including x2APIC, LVT, TLB shootdown, MSI, NMI watchdog.
> This TODO focuses exclusively on the **boot sequence reordering**.

---

## Current Boot Sequence (Problem)

```
boot_hw_init()
  ├── serial_init()
  ├── boot_info parse (UEFI or Multiboot2)
  ├── pmm_init()
  ├── vmm_init()
  ├── heap_init()
  ├── cpuid_init()
  ├── simd_enable_avx()
  ├── ata_init() / virtio_blk_init() / ahci_init()
  └── blkdev_register_all()

boot_interrupts_init()              ◄── PIC dependency here
  ├── gdt_init()
  ├── idt_init()
  ├── pic_init()                    ◄── Programs 8259 PIC (doesn't exist on HV Gen 2)
  ├── pit_init()                    ◄── Routes IRQ0 through PIC (dead on HV Gen 2)
  ├── rtc_init()
  ├── keyboard_init()               ◄── Hangs on HV Gen 2 (no i8042)
  ├── mouse_init()                  ◄── Hangs on HV Gen 2 (no i8042)
  ├── fb_init()
  ├── boot_splash_init()            ◄── sleep_ms() hangs (no PIT ticks)
  ├── pci_scan()
  ├── sti
  ├── boot_splash_start_animation() ◄── PIT callback (no ticks)
  └── dhcp_discover()

boot_storage_init()
  ├── acpi_init()                   ◄── ACPI parsed HERE (too late!)
  ├── lapic_init()                  ◄── LAPIC initialized HERE (too late!)
  ├── ioapic_init()                 ◄── IOAPIC initialized HERE (too late!)
  ├── smp_init()
  └── storvsc_init() / hv_input
```

**Problem:** ACPI/LAPIC/IOAPIC init happens in `boot_storage_init()` — long after
the PIT, splash, and keyboard were already needed. On platforms without a PIC,
this ordering causes hangs.

---

## Target Boot Sequence (Solution)

```
boot_hw_init()
  ├── serial_init()
  ├── boot_info parse
  ├── pmm_init()
  ├── vmm_init()
  ├── heap_init()
  ├── cpuid_init()
  ├── simd_enable_avx()
  └── blkdev_register_all()

boot_interrupts_init()              ◄── APIC-first path
  ├── gdt_init()
  ├── idt_init()
  ├── acpi_init()                   ◄── MOVED: parse MADT for LAPIC/IOAPIC
  ├── lapic_init()                  ◄── MOVED: enable LAPIC, set SVR
  ├── ioapic_init()                 ◄── MOVED: route ISA IRQs via IOAPIC
  ├── pic_conditional_init()        ◄── Only remap+disable if PCAT_COMPAT=1
  ├── pit_init()                    ◄── IRQ0 now routed through IOAPIC ✓
  ├── rtc_init()
  ├── keyboard_init()               ◄── With existing timeouts ✓
  ├── mouse_init()                  ◄── With existing timeouts ✓
  ├── fb_init()
  ├── boot_splash_init()            ◄── sleep_ms() works (PIT via IOAPIC) ✓
  ├── pci_scan()
  ├── sti
  └── ...

boot_storage_init()
  ├── smp_init()                    ◄── LAPIC already up, SIPI works
  ├── storvsc_init()
  └── ...
```

---

## 1. Move ACPI MADT Parsing Before Interrupt Setup

**Prompt:** The ACPI MADT (Multiple APIC Description Table) must be parsed before `pic_init()` and `pit_init()` so the kernel knows whether a PIC exists (`PCAT_COMPAT` flag) and has the IOAPIC base address for IRQ routing. Currently `acpi_init()` lives in `boot_storage_init()`. Move it to run right after `idt_init()` in `boot_interrupts_init()`. The only prerequisite is heap (for ACPI table parsing) — which is already available after `boot_hw_init()`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: move ACPI MADT parsing before interrupt setup"`. Add notes directly in this TODO section.

> [!NOTE]
> **Prerequisite check:** `acpi_init()` calls `kmalloc()` for ACPI table copies
> and uses `klog()` for output. Both are available after `boot_hw_init()`.

- [ ] Verify `acpi_init()` has no dependency on PCI, GDT, or interrupt state
- [ ] Remove `acpi_init()` call from `boot_storage_init()`
- [ ] Add `acpi_init()` to `boot_interrupts_init()` right after `idt_init()`
- [ ] Verify `acpi_pcat_compat()` returns correct value after early `acpi_init()`
- [ ] Verify MADT LAPIC/IOAPIC base addresses are available after early parse
- [ ] Build and test on QEMU: confirm ACPI tables parse correctly in new position
- [ ] Commit: `"boot: move ACPI MADT parsing before interrupt setup"`

---

## 2. Move LAPIC/IOAPIC Init Before PIT

**Prompt:** With ACPI MADT parsed, initialize the LAPIC and IOAPIC immediately so that PIT IRQ0 can be routed through the IOAPIC instead of the PIC. This means `lapic_init()` and `ioapic_init()` must run before `pit_init()`. Currently these live in `boot_storage_init()`. The LAPIC enables the local interrupt controller and EOI mechanism. The IOAPIC sets up the I/O interrupt redirect table for ISA IRQs (including IRQ0=PIT, IRQ1=keyboard, IRQ12=mouse). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: LAPIC/IOAPIC init before PIT for APIC-routed IRQ0"`. Add notes directly in this TODO section.

> [!CAUTION]
> **ISR stale-bit drain:** The current `boot_storage_c` has ISR drain logic that
> clears stale PIC bits when transitioning from PIC to APIC. With APIC-first boot,
> the PIC is never fully enabled, so drain logic may need adjustment.

- [ ] Remove `lapic_init()` from `boot_storage_init()`
- [ ] Remove `ioapic_init()` from `boot_storage_init()`
- [ ] Add `lapic_init()` to `boot_interrupts_init()` after `acpi_init()`
- [ ] Add `ioapic_init()` to `boot_interrupts_init()` after `lapic_init()`
- [ ] Verify `ioapic_init()` routes IRQ0 (PIT) to BSP LAPIC via IOAPIC redirect table
- [ ] Verify `ioapic_init()` routes IRQ1 (keyboard) and IRQ12 (mouse) too
- [ ] Update `irq_eoi()` — must call `lapic_eoi()` for IOAPIC-routed interrupts
  - [ ] Currently `irq_eoi()` calls `pic_eoi()` — add LAPIC path
- [ ] Move or remove ISR drain logic from `boot_storage.c` (no PIC→APIC transition)
- [ ] Build and test on QEMU: PIT ticks still fire, keyboard/mouse still work
- [ ] Commit: `"boot: LAPIC/IOAPIC init before PIT for APIC-routed IRQ0"`

---

## 3. Make PIC Init Conditional on PCAT_COMPAT

**Prompt:** Refactor `pic_init()` to only run when the ACPI MADT `PCAT_COMPAT` flag is set (`acpi_pcat_compat() == 1`). On APIC-only platforms (Hyper-V Gen 2, modern UEFI boards), the PIC doesn't exist and all its I/O port writes (`0x20`, `0x21`, `0xA0`, `0xA1`) are silently dropped. With APIC-first boot, the PIC is no longer needed for IRQ routing — it's only needed for legacy remap-and-mask on systems that have one. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: conditional PIC init based on PCAT_COMPAT"`. Add notes directly in this TODO section.

- [ ] In `boot_interrupts_init()`, guard `pic_init()` with `if (acpi_pcat_compat())`
- [ ] When `PCAT_COMPAT=1`: remap PIC vectors 0x20–0x2F, then mask all PIC IRQs
  - [ ] This prevents PIC from delivering stale interrupts during IOAPIC transition
- [ ] When `PCAT_COMPAT=0`: skip `pic_init()` entirely — no PIC exists
- [ ] Guard `pic_unmask_irq()` / `pic_mask_irq()` calls throughout the kernel:
  - [ ] `pit_init()` calls `pic_unmask_irq(IRQ_TIMER)` — skip when APIC-only
  - [ ] `keyboard_init()` calls `pic_unmask_irq(IRQ_KEYBOARD)` — skip when APIC-only
  - [ ] `mouse_init()` calls `pic_unmask_irq(IRQ_MOUSE)` — skip when APIC-only
  - [ ] `rtl8139_init()` and other drivers — audit all `pic_unmask_irq` calls
- [ ] Add `bool pic_available(void)` API to `pic.h` for drivers to check
- [ ] Build and test on QEMU: confirm PIC is initialized (PCAT_COMPAT=1)
- [ ] Build and test on Hyper-V: confirm PIC is skipped (PCAT_COMPAT=0)
- [ ] Commit: `"boot: conditional PIC init based on PCAT_COMPAT"`

---

## 4. Unify IRQ EOI Path (PIC vs LAPIC)

**Prompt:** Currently `irq_eoi()` in the interrupt handlers calls `pic_eoi()` which sends EOI to the 8259 PIC. With APIC-routed interrupts, EOI must go to the LAPIC via `lapic_eoi()`. Create a unified `irq_eoi()` function that dispatches based on whether interrupts are routed through PIC or IOAPIC. This must work correctly during the transition period and on both PIC-present and PIC-absent systems. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: unified IRQ EOI for PIC and LAPIC"`. Add notes directly in this TODO section.

> [!NOTE]
> **Codebase fact:** `irq_eoi()` is already a function in `pic.c` that takes an
> IRQ number and sends EOI to the master/slave PIC. It's called from `pit.c`,
> `keyboard.c`, `mouse.c`, `rtc.c`, and `rtl8139.c`.

- [ ] Rename current `irq_eoi()` to `pic_eoi_legacy()` (internal to `pic.c`)
- [ ] Create new `irq_eoi(uint8_t irq)` in a central location (e.g., `irq.c`):
  - [ ] If IOAPIC is active: call `lapic_eoi()` (same register write regardless of IRQ)
  - [ ] If PIC fallback: call `pic_eoi_legacy(irq)` with master/slave dispatch
- [ ] Add `static bool ioapic_active` flag, set by `ioapic_init()`
- [ ] Update all callers: `pit.c`, `keyboard.c`, `mouse.c`, `rtc.c`, `rtl8139.c`
  - [ ] No signature change needed — same `irq_eoi(irq_num)` API
- [ ] Handle special case: LAPIC timer uses a different vector (0x20 vs IOAPIC-routed)
  - [ ] PIT timer via IOAPIC: EOI goes to LAPIC
  - [ ] LAPIC timer (future): EOI goes to LAPIC
  - [ ] PIT timer via PIC (legacy): EOI goes to PIC
- [ ] Build and test on QEMU: verify PIT, keyboard, mouse all work with LAPIC EOI
- [ ] Commit: `"kernel: unified IRQ EOI for PIC and LAPIC"`

---

## 5. Remove Hyper-V Workarounds

**Prompt:** After APIC-first boot is complete and tested, remove the temporary workarounds added for the Hyper-V Gen 2 black-screen debugging effort. These include: framebuffer debug bars in `bootx64.c`, `main.c`, `boot_hw.c`, `boot_interrupts.c`; the `sleep_ms()` stall detection in `pit.c`; and the early serial output in `bootx64.c` (keep as opt-in debug feature, not removed entirely). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: remove Hyper-V debug workarounds"`. Add notes directly in this TODO section.

- [ ] Remove `HV_BAR` debug macros and framebuffer bar writes from:
  - [ ] `src/boot/uefi/bootx64.c` (DRAW_BAR macro and all calls)
  - [ ] `src/kernel/main.c` (magenta bar at row 60)
  - [ ] `src/kernel/main/boot_hw.c` (HV_BAR calls at rows 72–144)
  - [ ] `src/kernel/main/boot_interrupts.c` (HV_BAR calls at rows 160–268)
- [ ] Remove `sleep_ms()` stall detection in `pit.c` (IOAPIC routes IRQ0 now)
- [ ] Keep `serial_early_init()` and `serial_early_print()` in `bootx64.c`
  - [ ] Wrap behind `#ifdef BOOT_DEBUG` or always-on (it's harmless and useful)
- [ ] Remove PS/2 flush-loop timeouts? **NO** — keep them as defensive coding
- [ ] Build and test on both QEMU and Hyper-V
- [ ] Commit: `"boot: remove Hyper-V debug workarounds"`

---

## 6. SMP Init Adjustment

**Prompt:** With LAPIC initialized early, `smp_init()` can still run in `boot_storage_init()` — it only needs LAPIC for sending INIT-SIPI-SIPI. However, verify that moving LAPIC init earlier doesn't break the AP boot sequence. The APs need the GDT, IDT, and page tables to be ready when they wake up — all of which are set up before `boot_storage_init()`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: verify SMP works with early LAPIC init"`. Add notes directly in this TODO section.

- [ ] Verify `smp_init()` only depends on `lapic_init()` having been called (not its position)
- [ ] Verify AP trampoline has access to GDT, IDT, page tables set up by BSP
- [ ] Verify AP `lapic_init_ap()` call works with early-initialized LAPIC base address
- [ ] Test SMP on QEMU with 4 cores: all APs should boot successfully
- [ ] Commit: `"boot: verify SMP works with early LAPIC init"`

---

## 7. Full IDT Coverage — Proper Handlers + Defensive Safety Net

**Prompt:** A `GENERAL_PROTECTION_FAULT` BSOD was observed on Hyper-V with error code `0x7B3`. Decoding: bit 0=1 (external event), bits 1-2=01 (IDT reference), bits 3-15=0xF6 (vector 246). Hyper-V delivered a **VMBus synthetic interrupt** to IDT vector 0xF6 which has no handler — the CPU faulted on the null descriptor. The **real fix** is to register proper ISRs for known synthetic vectors (VMBus channel callbacks, STIMER, synthetic keyboard/mouse). Catch-all stubs are only a **safety net** for truly unexpected vectors — they must log warnings, not silently swallow interrupts, because silently absorbing VMBus callbacks would mask StorVSC disk I/O completions, NetVSC packet delivery, and synthetic timer ticks. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"idt: full vector coverage with proper Hyper-V ISRs"`. Add notes directly in this TODO section.

> [!CAUTION]
> **Observed BSOD (Hyper-V Gen 2):**
> ```
> Stop code:  GENERAL_PROTECTION_FAULT
> Error code: 0x00000000000007B3
> RIP:        0x00000000001311A2
> Source:     idt.c:0
> ```
> Decoded: External event → IDT reference → vector 0xF6 (246). The hypervisor
> delivered a synthetic interrupt to a vector with no IDT entry, causing #GP.

> [!WARNING]
> **Do NOT silently absorb synthetic interrupts.** Vector 0xF6 is Hyper-V
> actively trying to communicate with the guest OS (VMBus channel callback,
> StorVSC disk completion, NetVSC packet arrival, or STIMER tick). A catch-all
> stub that swallows it would mask real I/O failures and make disk/network
> drivers appear to hang for no reason.

### 7a. Register Proper ISRs for Known Hyper-V Synthetic Vectors (Real Fix)

- [ ] Identify which vector Hyper-V assigns for VMBus callbacks:
  - [ ] The VMBus driver writes the callback vector to the SINT (Synthetic Interrupt Source) MSR
  - [ ] `hv_vmbus_init()` should register its ISR at this vector in the IDT
  - [ ] → XREF: `TODO-063-Drivers.md` or VMBus driver source for SINT setup
- [ ] Register VMBus channel ISR at the SINT-assigned vector:
  - [ ] ISR reads the VMBus interrupt page to determine which channel fired
  - [ ] Dispatches to the per-channel callback (StorVSC, NetVSC, HID, etc.)
  - [ ] Sends LAPIC EOI
- [ ] Register Hyper-V STIMER (Synthetic Timer) ISR if used:
  - [ ] Can replace PIT as the kernel timer source on Hyper-V
  - [ ] Higher precision than PIT (100ns resolution vs 10ms)
- [ ] Register Hyper-V synthetic keyboard/mouse ISRs via `hv_input.c`:
  - [ ] These replace PS/2 keyboard/mouse on Gen 2 (no i8042)
  - [ ] → XREF: `hv_input.c` — already partially implemented

### 7b. Defensive Catch-All Stubs (Safety Net Only)

- [ ] Populate remaining unpopulated IDT entries (vectors 48–0xFB) with warning stubs:
  - [ ] **Log a warning**: `[IDT] WARNING: Unhandled interrupt vector=%u, RIP=%p`
  - [ ] Send LAPIC EOI (idempotent — safe even if no pending interrupt)
  - [ ] Do **NOT** silently absorb — the warning log makes unhandled vectors visible
  - [ ] Consider: rate-limit the warning (e.g., once per vector) to avoid log spam
- [ ] Generate stubs efficiently via macro or loop in `idt.c` or `idt_stubs.asm`:
  - [ ] Each stub pushes its vector number and jumps to a common `idt_default_handler`
  - [ ] Preserve existing specific handlers for vectors 0–47 and 0xFC–0xFF
- [ ] The `idt_default_handler` should NOT panic — treat as a warning, not a fatal error:
  - [ ] First occurrence: log full details (vector, RIP, error code)
  - [ ] Subsequent: count only (avoid flooding serial/log)
- [ ] Build and test on QEMU: no regression (stubs are never triggered)
- [ ] Build and test on Hyper-V: #GP replaced by warning log identifying which vector
- [ ] Commit: `"idt: full vector coverage with proper Hyper-V ISRs"`

---

## Key Files

| File                                 | Change                                                  |
| ------------------------------------ | ------------------------------------------------------- |
| `src/kernel/main/boot_interrupts.c`  | Reorder: add acpi_init, lapic_init, ioapic_init early   |
| `src/kernel/main/boot_storage.c`     | Remove: acpi_init, lapic_init, ioapic_init from here    |
| `src/kernel/drivers/pic.c`           | Guard init with `acpi_pcat_compat()` check              |
| `src/kernel/drivers/pit.c`           | Update `irq_eoi()` call for LAPIC path                  |
| `src/kernel/drivers/keyboard.c`      | Guard `pic_unmask_irq()` with `pic_available()` check   |
| `src/kernel/drivers/mouse.c`         | Guard `pic_unmask_irq()` with `pic_available()` check   |
| `src/kernel/drivers/rtc.c`           | Guard `pic_unmask_irq()` with `pic_available()` check   |
| `include/kernel/drivers/pic.h`       | Add `pic_available()` API                               |
| `src/kernel/idt.c`                   | Populate all 256 entries with catch-all stubs            |
| `src/boot/uefi/bootx64.c`           | Remove debug bars (keep serial_early as opt-in)         |
| `src/kernel/main.c`                  | Remove debug bars                                       |
| `src/kernel/main/boot_hw.c`          | Remove debug bars                                       |

---

## Priority Order

| Priority | Section                              | Description                                             |
| :------: | ------------------------------------ | ------------------------------------------------------- |
| 🔴 P0   | 1. Move ACPI MADT Parsing Early      | Prerequisite for everything — must know if PIC exists   |
| 🔴 P0   | 2. Move LAPIC/IOAPIC Before PIT      | Core change — route IRQ0 through IOAPIC, not PIC        |
| 🔴 P0   | 7. Full IDT Population               | Prevents #GP BSOD on Hyper-V (observed crash)           |
| 🟠 P1   | 3. Conditional PIC Init              | Guard all PIC calls with PCAT_COMPAT check              |
| 🟠 P1   | 4. Unified IRQ EOI Path              | PIC EOI → LAPIC EOI dispatch for all IRQ handlers       |
| 🟡 P2   | 5. Remove Debug Workarounds          | Cleanup after APIC-first is verified                    |
| 🟡 P2   | 6. SMP Init Adjustment               | Verify AP boot still works with early LAPIC             |

---

## OS Comparison

| Feature                        | 🪟 Windows 11                      | 🐧 Linux 6.x                       | 🚀 Impossible OS (Current)         | 🚀 After Refactor                   |
| ------------------------------ | ---------------------------------- | ----------------------------------- | ----------------------------------- | ------------------------------------ |
| APIC-first boot                | ✅ HAL always uses APIC            | ✅ APIC init before PIT             | ❌ PIC → PIT → APIC (too late)      | ✅ ACPI → LAPIC → IOAPIC → PIT       |
| PIC fallback                   | ✅ Only if MADT says PCAT_COMPAT   | ✅ Only if no IOAPIC found          | ⚠️ PIC always initialized          | ✅ Conditional on PCAT_COMPAT        |
| Hyper-V Gen 2 boot             | ✅ Native support                  | ✅ Native support                   | ❌ Hangs (no PIC = no PIT ticks)    | ✅ IOAPIC routes IRQ0               |
| PIT timer on APIC-only         | ✅ IRQ0 via IOAPIC pin 2           | ✅ IRQ0 via IOAPIC (ISO override)   | ❌ PIT IRQ0 routed through dead PIC | ✅ IRQ0 via IOAPIC                   |
| EOI dispatch                   | ✅ Unified HAL EOI                 | ✅ `apic_eoi()`                     | ⚠️ `pic_eoi()` only               | ✅ `irq_eoi()` → LAPIC or PIC       |
| Full IDT coverage              | ✅ All 256 entries populated       | ✅ All 256 entries populated        | ❌ ~48 entries, rest null → #GP     | ✅ All 256 with catch-all stubs      |
