# TODO-06 — Bare Metal Boot Hardening & POST Diagnostics

> **Goal:** Make the kernel boot reliably on any x86-64 bare-metal hardware with production-grade error reporting. After this TODO, the boot sequence is robust against absent hardware, misconfigured firmware, and platform-specific quirks — with fine-grained POST codes that pinpoint failures without serial, debugger, or color bars.

> [!IMPORTANT]
> **Origin (2026-03-28):** A full day of bare-metal debugging on an i5-11600K laptop exposed fundamental gaps. Issues found and patched (WIP): SMEP page tables, HPET WB MMIO, GS_BASE clobbered by GDT reload, TSS RSP0 uninitialized, heartbeat ISR reentrancy, RTC hang, calibration timeouts. The **core unsolved issue**: any hardware interrupt (LAPIC timer or PIT) crashes on bare metal while software `INT 0x81` through the same ISR assembly path works fine. Multiple subsystems are disabled as workarounds (mouse, AHCI interrupts, timer). This TODO systematically fixes all of it.

> [!NOTE]
> **Design principle:** Every subsystem init must be independently skippable. If `mouse_init()` crashes, boot continues without a mouse. If AHCI MSI fails, fall back to polled I/O. If HPET MMIO faults, skip to PM Timer. The boot sequence must be **unbreakable** — degrade gracefully, never crash.

> [!CAUTION]
> **Scope boundary — this TODO does NOT own:**
> - CPU security feature implementation (NX, SMEP, SMAP, CET, Spectre) → owned by `TODO-17-kernel-security-hardening.md`
> - CPU activation sequencing (EFER before VMM, CR4 order) → owned by `TODO-04-cpu-boot-sequencing.md`
> - Timer HAL architecture and calibration waterfall design → owned by `TODO-03-interrupt-timer-arch.md`
> - Visual boot progress display (VPD) → owned by `TODO-05-visual-post-display.md`
> - Panic forensic evidence struct and cross-boot persistence → owned by `TODO-02-boot-diagnostics.md §6`
>
> **This TODO owns:** making all of the above **work on real hardware** — diagnostics, detection, fallbacks, IST, ACPI gating, graceful degradation, and the hw interrupt investigation.

## Inputs

- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) — Phase 0
- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c) — Phase 1
- [`src/kernel/main/boot_storage.c`](../../src/kernel/main/boot_storage.c) — Phase 2
- [`src/kernel/main/boot_desktop.c`](../../src/kernel/main/boot_desktop.c) — Phase 3
- [`src/kernel/main/boot_init.c`](../../src/kernel/main/boot_init.c) — `boot_progress()`, `boot_post_write/read()`
- [`src/kernel/main/boot_progress.c`](../../src/kernel/main/boot_progress.c) — POST display, stage reporting
- [`src/kernel/idt.c`](../../src/kernel/idt.c) — ISR handler, IRQL tracking
- [`src/kernel/isr_stubs.asm`](../../src/kernel/isr_stubs.asm) — ISR assembly stubs
- [`src/kernel/gdt.c`](../../src/kernel/gdt.c) — TSS, RSP0, IST
- [`src/kernel/gdt_asm.asm`](../../src/kernel/gdt_asm.asm) — GDT reload (GS preservation)
- [`src/kernel/drivers/lapic.c`](../../src/kernel/drivers/lapic.c) — LAPIC timer, calibration waterfall
- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) — PS/2 mouse init
- [`src/kernel/drivers/ahci/ahci_core.c`](../../src/kernel/drivers/ahci/ahci_core.c) — AHCI MSI setup
- [`src/kernel/drivers/framebuffer.c`](../../src/kernel/drivers/framebuffer.c) — `fb_swap`/`fb_swap_rect` cli/sti
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c) — FADT parsing, MADT, PM Timer
- [`include/kernel/hv_bar.h`](../../include/kernel/hv_bar.h) — debug bars (to be replaced)
- → XREF: `TODO-02-boot-diagnostics.md §6` — panic forensic evidence struct (this TODO validates it works on bare metal)
- → XREF: `TODO-03-interrupt-timer-arch.md` — timer HAL design (this TODO investigates why hw interrupts crash on bare metal)
- → XREF: `TODO-04-cpu-boot-sequencing.md §2,§4` — CPU hardening activation order (this TODO does NOT reimplement; verifies on bare metal)
- → XREF: `TODO-05-visual-post-display.md` — VPD replaces HV_BAR (this TODO's §1-§2 POST codes feed VPD)
- → XREF: `02-kernel-core/TODO-17-kernel-security-hardening.md` — NX/SMEP/SMAP implementation (this TODO does NOT reimplement; handles bare-metal quirks like shared page tables)
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §1` — boot_progress() infrastructure (this TODO consumes it)
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §11` — UC MMIO mapping for HPET/AHCI

## Outcome

- Every boot subsystem emits granular POST codes (4-digit hex) visible on I/O port 0x80, UEFI NVRAM, and on-screen display.
- Next boot after a crash shows "Last boot failed at: SUBSYS (0xNNNN)" without any configuration.
- Hardware interrupts work reliably on bare metal — IST stacks, correct LAPIC delivery, verified ISR frame layout.
- PS/2, RTC, AHCI, and all legacy subsystems are gated by ACPI capability flags — never touch hardware that doesn't exist.
- Any subsystem failure degrades gracefully with a log message instead of crashing.
- `boot.conf` can skip specific subsystems: `skip=mouse,ahci_msi,hpet`.

## Implementation Order

| ⭐  | Order | Deliverable                                          | Depends On | Status |
| --- | :---: | ---------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Fine-grained POST code system (4-digit hex)          | —          |  [ ]   |
| ⭐  |   2   | POST code in every boot function                     | §1         |  [ ]   |
| 💎  |   3   | Minimal UC MMIO mapping (`vmm_map_mmio_uc`)          | —          |  [ ]   |
| 💎  |   4   | IST stacks for critical exceptions                   | —          |  [ ]   |
| 💎  |   5   | Hardware interrupt root cause investigation          | §4         |  [ ]   |
| 💎  |   6   | ACPI FADT boot architecture flags                    | —          |  [ ]   |
| 💎  |   7   | PS/2 controller detection and safe init              | §6         |  [ ]   |
| 💎  |   8   | AHCI interrupt hardening                             | §3, §5     |  [ ]   |
| 💎  |   9   | Resilient boot with graceful degradation             | §1, §2     |  [ ]   |
| 💎  |  10   | Per-process page tables (minimal base)               | §3         |  [ ]   |
| 💎  |  11   | CPU security activation and verification             | §6, §10    |  [ ]   |
| 💎  |  12   | Boot order hardening (timer-last, UEFI-safe)         | §5         |  [ ]   |
| ⭐  |  13   | `boot.conf` subsystem skip list                      | §9         |  [ ]   |
| 💎  |  14   | CPU feature minimum requirements and verification    | §6, §11    |  [ ]   |
| 💎  |  15   | Bare-metal test matrix and validation plan           | §5         |  [ ]   |

> 💎 = parity — Windows and Linux both handle bare-metal quirks, IST, ACPI gating, and graceful degradation.
> ⭐ = exclusive — 4-digit POST codes in every function and a configurable skip list are not standard in any OS kernel.

---

## 1. Fine-Grained POST Code System (4-Digit Hex)
Replace the current 2-digit POST codes (28 values in 0x10–0x63) with a 4-digit system (0x0000–0xFFFF) that gives every subsystem its own range. Each function entry and exit gets a unique code.

**Files:** `include/kernel/boot_init.h`, `src/kernel/main/boot_init.c`, `src/kernel/main/boot_progress.c`

> [!IMPORTANT]
> I/O port 0x80 is 8-bit on most hardware POST cards. For 4-digit codes, write high byte to port 0x80 and low byte to port 0x81 (or use port 0x80 for the most-significant byte only). UEFI NVRAM stores the full 16-bit value. On-screen display renders all 4 hex digits.

- [ ] Define `POST16(code)` macro: writes high byte to I/O 0x80, stores full 16-bit in UEFI NVRAM, updates on-screen display
- [ ] Define POST code ranges — each Phase gets 0x1000 codes:

| Range | Phase | Example codes |
|-------|-------|---------------|
| 0x0000–0x0FFF | Phase 0 — Critical Init | 0x0010=serial, 0x0020=PMM_enter, 0x0021=PMM_exit, 0x0030=VMM_enter |
| 0x1000–0x1FFF | Phase 1 — Platform | 0x1000=GDT, 0x1010=IDT, 0x1020=ACPI, 0x1030=LAPIC, 0x1040=timer_enter, 0x1041=timer_calibrate, 0x1042=timer_init, 0x1043=timer_exit |
| 0x2000–0x2FFF | Phase 2 — System | 0x2000=PCI_enter, 0x2001=PCI_bus0, 0x2010=NIC, 0x2020=AHCI_enter, 0x2021=AHCI_MSI |
| 0x3000–0x3FFF | Phase 3 — Desktop | 0x3000=sched, 0x3010=fonts, 0x3020=compositor |
| 0xF000–0xFFFE | Reserved | 0xFF00=BOOT_OK, 0xFFFE=BOOT_FAILED |

- [ ] `boot_post_write16(uint16_t code)` — stores 2 bytes in UEFI NVRAM (backward-compatible: old reader sees high byte)
- [ ] `boot_post_read16()` — reads 2 bytes if available, 1 byte otherwise
- [ ] On-screen POST display: render 4 hex digits (top-right corner, using existing 8×8 hex font, scaled)
- [ ] Keep backward compatibility: old `boot_progress()` still works, internally calls `POST16()`
- [ ] Commit: `"boot: 4-digit POST code system (0x0000–0xFFFF) with NVRAM persistence"`

**Test checkpoint:** Build, boot on QEMU. Verify serial shows `[PHASE1] TIMER (0x1040)` style codes. Verify UEFI NVRAM stores 16-bit value. Verify on-screen shows 4-digit hex.

## 2. POST Code in Every Boot Function
Instrument every subsystem init function with entry/exit POST codes. Each function calls `POST16(code)` on entry and `POST16(code+1)` on success, so a crash between them is pinpointed to that exact function.

**Files:** all `boot_*.c` files, all driver `*_init()` functions

- [ ] Phase 0: `serial_init` (0x0010/11), `pmm_init` (0x0020/21), `vmm_init` (0x0030/31), `heap_init` (0x0040/41), `klog_early_init` (0x0050/51), `cpuid_init` (0x0060/61), `cpu_harden` (0x0070/71), `vmm_apply_nx_policy` (0x0080/81), `simd_enable_avx` (0x0090/91)
- [ ] Phase 1: `gdt_init` (0x1000/01), `idt_init` (0x1010/11), `acpi_init` (0x1020/21), `lapic_init` (0x1030/31), `ioapic_init` (0x1034/35), `timer_hal_init` (0x1040/41), `lapic_timer_calibrate` (0x1042), `lapic_timer_init` (0x1044), `rtc_init` (0x1050/51), `keyboard_init` (0x1060/61), `mouse_init` (0x1070/71), `fb_init` (0x1080/81), `boot_splash_init` (0x1090/91)
- [ ] Phase 2: `pci_scan` (0x2000/01), `xhci_init` (0x2010/11), `rtl8139_init` (0x2020/21), `net_init` (0x2030/31), `ata_init` (0x2040/41), `ahci_init` (0x2050/51), `ahci_setup_interrupts` (0x2052/53), `vfs_init` (0x2060/61), `partition_scan_all` (0x2070/71), `registry_init` (0x2080/81), `smp_init` (0x2090/91)
- [ ] Phase 3: `task_init` (0x3000/01), `wq_create` (0x3010/11), `font_mgr_init` (0x3020/21), `desktop_init` (0x3030/31), `compositor_run` (0x3040)
- [ ] Verify: bare-metal crash now shows exact function in NVRAM on next boot
- [ ] Commit: `"boot: POST16 instrumentation in all init functions"`

**Test checkpoint:** Force a crash (e.g., re-enable mouse_init). Reboot. Next boot serial shows "Last boot failed at: 0x1070 (mouse_init entry)". Pinpointed in seconds, not hours.

## 3. Minimal UC MMIO Mapping (`vmm_map_mmio_uc`)
Implement a minimal `vmm_map_mmio_uc()` that creates uncacheable mappings for device MMIO regions. This unblocks HPET calibration (§5) and AHCI hardening (§8) on bare metal where WB-cached MMIO causes MCE.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`

> [!NOTE]
> Minimal prerequisite — full `vmm_map_mmio()` / `MmMapIoSpace()` with cache type selection, HPET quirk table, and driver audit is in `03-memory-concurrency/TODO-01-vmm-memory-protection.md §11`. This section implements just enough to map a device BAR as UC using 4 KiB PTEs.

- [ ] `vmm_map_mmio_uc(uint64_t phys_base, uint32_t size)` — allocate 4 KiB page table entries with PCD=1, PWT=1 (UC), return virtual address. Use a simple bump allocator in a dedicated VA range (e.g., 0xFFFF_8000_0000_0000+ or a simpler high-address region above the identity map).
- [ ] `vmm_unmap_mmio(void *virt, uint32_t size)` — unmap and free PTEs (can be a no-op stub initially).
- [ ] Validate: phys_base must be page-aligned, within 64-bit physical address space, size > 0.
- [ ] Test: map LAPIC base (0xFEE00000) as UC, read LAPIC ID register, verify same value as identity-mapped read.
- [ ] Re-enable HPET calibration in `lapic.c`: replace identity-mapped `hpet_read64(base, ...)` with `hpet_read64(uc_mapped_base, ...)`; uncomment `cal_try_hpet()` call in calibration waterfall.
- [ ] Implement `hpet_read_ns()` in `lapic.c` (or new `hpet.c`): map HPET base via `vmm_map_mmio_uc()`, read counter, convert via `COUNTER_CLK_PERIOD`. Wire into `timer_driver_t.read_ns` for UTS (→ XREF: TODO-03 §6).
- [ ] Commit: `"mm: minimal vmm_map_mmio_uc + HPET re-enabled with UC mapping"`

**Test checkpoint:** QEMU: HPET calibration succeeds (`Tier 2: HPET calibration -> N ticks/ms`). Bare metal: HPET mapped via UC, no MCE, calibration succeeds. `hpet_read_ns()` returns monotonically increasing values.

## 4. IST Stacks for Critical Exceptions

Allocate dedicated interrupt stacks for Double Fault (#DF), NMI, and Machine Check Exception (MCE). Without IST, a stack overflow during an exception handler causes a triple fault and silent reboot — the most common "mystery crash" on bare metal.

**Files:** `src/kernel/gdt.c`, `src/kernel/idt.c`, `include/kernel/gdt.h`

> [!IMPORTANT]
> Linux uses IST1 for #DF, IST2 for NMI, IST3 for MCE. Windows uses separate stacks for the same exceptions via task gates (32-bit) or IST (64-bit). Both guarantee that these critical exceptions can always execute even when the kernel stack is corrupted.

- [ ] Allocate 3 IST stacks (4 KiB each) from PMM during `gdt_init()` — identity-mapped, so phys = virt
- [ ] Set `kernel_tss.ist1` = DF stack top, `kernel_tss.ist2` = NMI stack top, `kernel_tss.ist3` = MCE stack top
- [ ] Update IDT entries: vector 8 (#DF) → IST=1, vector 2 (NMI) → IST=2, vector 18 (MCE) → IST=3
- [ ] Add NMI handler: log "NMI received", dump registers, emit POST16(0xE002), halt
- [ ] Add MCE handler: read MCi_STATUS MSRs, log machine check info, emit POST16(0xE018), halt
- [ ] Verify: stack overflow in kernel → #DF fires on IST1 stack → shows BSOD instead of triple fault
- [ ] Commit: `"kernel: IST stacks for #DF, NMI, MCE — no more silent triple faults"`

**Test checkpoint:** Intentionally overflow the kernel stack (recursive function). Verify #DF handler fires and shows a BSOD with register dump instead of a silent reboot.

## 5. Hardware Interrupt Root Cause Investigation
Systematically investigate why hardware interrupts crash on the i5-11600K while software `INT 0x81` works. This section is diagnostic — it may result in a fix or in documenting a platform-specific workaround.

**Files:** `src/kernel/idt.c`, `src/kernel/isr_stubs.asm`, `src/kernel/drivers/lapic.c`, `src/kernel/gdt.c`

> [!IMPORTANT]
> **Known facts from 2026-03-28 debugging session:**
> - Software INT 0x81 through `isr_common_stub` → `isr_handler` → works on bare metal
> - LAPIC timer (vector 34) and PIT (vector 32, via IOAPIC) both crash
> - Crashes happen during PCI bus scan (Phase 2) — after `sti`
> - Timer masked + `sti` → boot completes through Phase 2 (no timer ticks, but other interrupts may fire from keyboard/etc.)
> - IRQL tracking removed, handler reduced to `tick++; EOI; return frame` — still crashes
> - TSS RSP0 set, GS_BASE reasserted — still crashes
> - Heartbeat removed, fb_swap irqsave fixed — still crashes

**Investigation checklist:**

- [ ] **IDT entry binary dump:** Hex-dump IDT entries for vectors 32, 34, 0x81 at boot. Compare the 16-byte descriptor format on QEMU vs bare metal. Verify selector, offset, IST, type_attr are identical.
- [ ] **ISR frame layout verification:** In the timer handler, dump the interrupt frame (RIP, CS, RFLAGS, RSP, SS) to a known physical address (e.g., 0x500). Read it back after halt. Verify CS=0x08 (kernel code), SS=0x10 (kernel data), RIP is within kernel text.
- [ ] **LAPIC delivery mode check:** Before sti, read LAPIC LVT Timer register and dump it. Verify delivery mode is Fixed (000), vector is 34, mask is 0. Compare with QEMU.
- [ ] **IOAPIC redirection table dump:** Read all 24 IOAPIC entries and dump them. Verify no vector conflicts (two sources routed to the same vector).
- [ ] **Spurious interrupt check:** Count unhandled interrupts on all vectors. If bare metal receives unexpected vectors (from chipset, PCH, or UEFI firmware), they could collide with our timer vector.
- [ ] **ISR stub alignment:** Verify that each ISR stub (`irq0`, `irq2`, etc.) is at the correct address in the IDT. Disassemble the kernel binary to confirm stub offsets match IDT entries.
- [ ] **Stack canary test:** Push a known canary value to the stack before `sti`. After the first timer interrupt returns (check via tick counter), verify the canary is intact. If corrupted, the ISR is overwriting the wrong stack region.
- [ ] **Minimal bare-metal ISR test:** Create a dedicated test that: (1) masks all interrupts except the LAPIC timer, (2) sets up a minimal IDT with only vector 34 pointing to a trivial handler (`mov dword [0x500], 0xDEAD; lapic_eoi; iret`), (3) enables interrupts, (4) checks 0x500 for the canary.
- [ ] Document findings and apply fix
- [ ] Commit: `"kernel: bare metal interrupt investigation — [root cause / workaround]"`

**Test checkpoint:** The LAPIC timer fires on bare metal without crashing. PCI scan completes with timer ticks running. If fix is architectural (e.g., IST required for timer), document why.

## 6. ACPI FADT Boot Architecture Flags
Parse the FADT `IAPC_BOOT_ARCH` and `Flags` fields to know which legacy devices exist before touching any I/O ports. This prevents crashes on platforms without PIT, PS/2 controller, or RTC.

**Files:** `src/kernel/acpi.c`, `include/kernel/acpi.h`

> [!IMPORTANT]
> **FADT `IAPC_BOOT_ARCH` bits (ACPI 6.0, Table 5-11):**
> - Bit 0: LEGACY_DEVICES — 8042 required for keyboard/mouse
> - Bit 1: 8042 — i8042 controller present
> - Bit 2: VGA_NOT_PRESENT — do not probe VGA
> - Bit 3: MSI_NOT_SUPPORTED — do not enable MSI
> - Bit 4: PCIe_ASPM — PCIe ASPM must not be disabled
> - Bit 5: CMOS_RTC_NOT_PRESENT — do not access CMOS RTC ports

- [ ] Parse `IAPC_BOOT_ARCH` from FADT (offset 109, 16-bit field, requires FADT length ≥ 113)
- [ ] Expose API: `acpi_has_8042()`, `acpi_has_cmos_rtc()`, `acpi_msi_supported()`, `acpi_has_vga()`
- [ ] `acpi_hw_reduced()` already exists — combine with IAPC_BOOT_ARCH for complete picture
- [ ] Log flags: `[ACPI] IAPC_BOOT_ARCH: 8042=%d RTC=%d MSI=%d VGA=%d HW_REDUCED=%d`
- [ ] Commit: `"kernel: parse ACPI FADT IAPC_BOOT_ARCH flags for legacy device detection"`

**Test checkpoint:** Boot on QEMU. Log shows IAPC_BOOT_ARCH with all flags. On bare metal, log shows actual hardware configuration.

## 7. PS/2 Controller Detection and Safe Init
Gate all PS/2 keyboard and mouse I/O behind ACPI detection. Never write to ports 0x60/0x64 if the i8042 doesn't exist.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`

- [ ] `keyboard_init()`: check `acpi_has_8042()` before any port I/O. If false, log "PS/2: skipped (no i8042)" and return.
- [ ] `mouse_init()`: check `acpi_has_8042()` AND probe for auxiliary port (command 0xA8 + status check). If no aux port, log and return.
- [ ] Add timeouts to ALL PS/2 wait loops (`ps2_wait_input`, `ps2_wait_output`) — currently 100000 iterations, increase to 1000000 but add early-exit on 0xFF status (controller absent).
- [ ] Mouse reset (0xFF): wait up to 500ms for self-test result; if timeout, skip mouse.
- [ ] Remove the current `mouse_init()` skip workaround from `boot_interrupts.c` — replace with proper detection.
- [ ] Migrate keyboard and mouse from hardcoded `irq_register(33/44, ...)` to `irq_request_gsi(1/12, ...)` — uses proper IOAPIC routing, avoids vector collision with dynamic allocator. Same for `vbox_mouse.c`.
- [ ] Commit: `"drivers: PS/2 keyboard/mouse gated by ACPI i8042 detection + GSI-based IRQ"`

**Test checkpoint:** Boot on laptop without PS/2 mouse. Mouse init logs "skipped" and boot continues. Boot on QEMU with PS/2 — mouse works normally.

## 8. AHCI Interrupt Hardening
Make `ahci_setup_interrupts()` safe on bare metal: mask LAPIC timer during MSI setup, verify ABAR MMIO accessibility, fall back to polled mode if MSI fails.

**Files:** `src/kernel/drivers/ahci/ahci_core.c`

> [!IMPORTANT]
> On bare metal, enabling MSI writes to PCI config space which triggers the device to send MSI messages to the LAPIC. If the LAPIC timer is also firing, the two LAPIC writes can race. Additionally, AHCI ABAR MMIO at the device's BAR5 address is accessed through WB-cached page table entries — same issue as HPET.

- [ ] Mask LAPIC timer LVT before MSI enable; unmask after.
- [ ] Validate ABAR address: must be within identity-mapped 4 GiB, page-aligned, not 0 or 0xFFFFFFFF.
- [ ] Wrap the MSI enable sequence in a timeout — if PCI config read returns 0xFFFF after MSI enable, the device is gone (hot-unplug or firmware error).
- [ ] If MSI setup fails, fall back to legacy INTx via IOAPIC with a log warning.
- [ ] If INTx also fails (no valid IRQ line), use polled mode: `ahci_use_polling = 1`, log warning.
- [ ] Remove the current `ahci_setup_interrupts()` skip workaround from `boot_storage.c`.
- [ ] Commit: `"drivers: AHCI interrupt hardening — MSI with LAPIC mask, INTx fallback, polled fallback"`

**Test checkpoint:** Boot on bare metal. AHCI init logs either "MSI vector 0xNN" or "INTx fallback" or "polled mode". No crash. Disk I/O works.

## 9. Resilient Boot with Graceful Degradation
Wrap every Phase 1–3 subsystem init in a protective pattern: emit POST code, call init, check result, log outcome, continue on failure. Never `boot_halt()` for non-critical subsystems.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_storage.c`, `src/kernel/main/boot_desktop.c`

> [!IMPORTANT]
> **Critical subsystems (BOOT_FATAL if fail):** PMM, VMM, Heap, GDT, IDT, VFS.
> **Non-critical (BOOT_DEGRADED if fail):** RTC, keyboard, mouse, NIC, AHCI, SMBIOS, splash, registry, SMP, DHCP.
> A degraded boot reaches the desktop with reduced functionality. The user sees a notification listing what failed.

- [ ] Define `BOOT_TRY(subsys, init_fn)` macro: emits POST16, calls init_fn, catches return code, sets subsystem state, logs result
- [ ] Classify each subsystem as CRITICAL or NON_CRITICAL
- [ ] Non-critical failures: log `[WARN] SUBSYS: init failed — degraded`, set `g_boot_info.degraded_mask |= (1 << subsys)`, continue boot
- [ ] Critical failures: existing `boot_halt()` / `boot_recovery_show()` behavior
- [ ] Phase 3 desktop init: check `g_boot_info.degraded_mask` and display "Some hardware was not detected" toast
- [ ] Remove all bare-metal skip workarounds — replace with proper `BOOT_TRY()` calls
- [ ] Commit: `"boot: resilient init with BOOT_TRY — non-critical failures degrade, never crash"`

**Test checkpoint:** Disable a non-critical subsystem (e.g., force `rtc_init()` to fail). Boot completes. Desktop shows degraded notification. Serial log shows `[WARN] RTC: init failed — degraded`.

## 10. Per-Process Page Tables (Minimal Base)

Implement the minimal per-process page table infrastructure so each task has its own PML4. Kernel pages are supervisor-only, user pages have the User bit. CR3 switches on context switch. This directly unblocks SMEP/SMAP on bare metal and eliminates the user-stacks-in-kernel-heap hack.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`, `src/kernel/sched/task.c`, `include/kernel/sched/task.h`

> [!NOTE]
> Minimal prerequisite — full per-process VM (COW fork, mmap, demand paging) is in `03-memory-concurrency/TODO-04-advanced-virtual-memory.md`. This section implements just enough to have separate user/kernel address spaces with correct U/S bits.

> [!IMPORTANT]
> **What this changes:**
> - Each task gets its own PML4 (cloned from kernel PML4 at task creation)
> - Kernel mappings (0x0–0x7FFFFF, 0x900000+): Present + Writable, NO User bit
> - User mappings (0x800000–0x8FFFFF for ELF, plus user stack): Present + Writable + User bit
> - User stacks allocated from PMM in the user address range, NOT from `kmalloc`
> - `schedule()` / `schedule_now()`: load new task's CR3 before `iretq`
> - Boot task (PID 0): continues using the identity-mapped PML4 (kernel-only task)

- [ ] `vmm_create_user_pml4()` — allocate a new PML4 page from PMM; copy kernel entries (PML4[0] upper half) from boot PML4; clear User bit on all kernel entries; return physical address of new PML4
- [ ] `vmm_map_user_page(pml4_phys, virt, phys, flags)` — map a 4 KiB page in a user PML4 with specified flags (User + Writable + Present); allocate intermediate PDPT/PD/PT pages as needed from PMM
- [ ] `vmm_destroy_user_pml4(pml4_phys)` — free PML4 and all intermediate page table pages (walk and free)
- [ ] Update `task_create_user()`: allocate user PML4 via `vmm_create_user_pml4()`; map ELF segments into user PML4 with User bit; allocate user stack from PMM (not `kmalloc`) and map at a fixed user VA (e.g., 0x7FFF_F000 stack top); store PML4 phys in task struct
- [ ] Update `schedule()` and `schedule_now()`: before returning the new frame, write new task's CR3 if different from current: `mov cr3, new_pml4_phys`
- [ ] Update `task_create()` (kernel tasks): use boot PML4 (no per-process PML4 for kernel threads)
- [ ] Remove `pmm_mark_region_used(0x800000, 0x100000)` hack — user ELF pages are now properly mapped per-process
- [ ] Remove SMEP/SMAP skip for `PLATFORM_BARE_METAL` in `cpu_security.c` — page tables now have correct U/S bits
- [ ] Commit: `"mm: per-process page tables — user/kernel separation, CR3 switch, SMEP/SMAP unblocked"`

**Regression risk:** This changes the memory model fundamentally. CR3 switch on every context switch adds ~100ns overhead. If page table creation has a bug, user-mode ELF won't run. Rollback: revert to shared identity map and re-skip SMEP/SMAP.

**Test checkpoint:** Boot on QEMU WHPX. cmd.exe runs in user mode with its own PML4. Kernel pages don't have User bit. `KeGetCurrentIrql()` works from user-mode interrupt. Boot on bare metal: SMEP/SMAP enabled (CR4 bits set), cmd.exe runs, no page faults.

## 11. CPU Security Activation and Verification

Ensure CPU security features (NX, SMEP, SMAP) are activated in the correct order and verified on bare metal. This incorporates the bare-metal-relevant parts of the CPU boot sequencing that are currently unimplemented.

**Files:** `src/kernel/cpu_security.c`, `src/kernel/main/boot_hw.c`

> [!NOTE]
> Minimal prerequisite — full CPU boot sequencing (EFER before VMM, XSAVE/PCID, AP parity) is in `TODO-04-cpu-boot-sequencing.md §2,§4,§5`. This section implements just enough to ensure NX/SMEP/SMAP work on bare metal and verifies post-activation.

- [ ] Formalize activation order in `boot_phase0()`: (1) `cpu_enable_nx()` → (2) `vmm_apply_nx_policy()` → (3) `cpu_harden_post_pagetable()` (SMEP/SMAP after page tables fixed)
- [ ] `cpu_verify_hardening()` — read back EFER, CR4 after activation. Log discrepancy: `[WARN] cpu: EFER.NXE not set after enable`
- [ ] On bare metal: SMEP/SMAP require per-process page tables (skip with log, not crash). Document this constraint clearly.
- [ ] On VMs: SMEP/SMAP either work (KVM/VBox) or are enforced via EPT (WHPX). Log which path.
- [ ] Emit POST16 codes: 0x0070=cpu_harden_enter, 0x0071=NX_done, 0x0072=SMEP_done, 0x0073=SMAP_done, 0x0074=verify_done
- [ ] Commit: `"boot: CPU security activation with post-enable verification"`

**Test checkpoint:** Boot on bare metal. Log shows `[OK] cpu: NX enabled, SMEP skipped (shared page tables), SMAP skipped`. On QEMU: log shows all three enabled (or EPT-enforced).

## 12. Boot Order Hardening (Timer-Last, UEFI-Safe)

Formalize the boot order lessons learned: timer is the last thing initialized before `sti`, all UEFI runtime calls mask the LAPIC timer, and `boot_splash_start_animation()` only runs after `sti`.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_init.c`

- [ ] Move `timer_hal_init()` to the last step of Phase 1, immediately before `sti` (already done — formalize and document)
- [ ] `boot_post_write()`: always mask LAPIC timer LVT during `uefi_set_variable()` call (already done — verify correctness)
- [ ] `uefi_set_variable()` wrapper: generic LAPIC mask/unmask guard for ALL UEFI runtime calls, not just POST write
- [ ] Document the Phase 1 init order contract in a comment block at the top of `boot_interrupts.c`
- [ ] Add `BOOT_ASSERT(condition, msg)` macro for impossible-state checks (e.g., `BOOT_ASSERT(kernel_subsystem_ready(SUBSYS_IDT), "IDT must be ready before timer")`)
- [ ] Commit: `"boot: formalize Phase 1 init order — timer-last, UEFI-safe, documented contract"`

**Test checkpoint:** Boot on QEMU + bare metal. Phase 1 order is correct. UEFI runtime calls don't crash with timer running. Boot order comment block is visible at top of `boot_interrupts.c`.

## 13. `boot.conf` Subsystem Skip List

Allow the user to skip specific subsystems via `boot.conf` for debugging or working around hardware bugs without recompiling.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`, `src/kernel/main/boot_interrupts.c`

- [ ] Add `skip=` key to `boot.conf`: comma-separated list of subsystem names (e.g., `skip=mouse,ahci_msi,hpet,smbios`)
- [ ] Parse into `g_boot_info.config.skip_mask` (32-bit bitmask matching `SUBSYS_*` enum + extra flags for sub-features like `ahci_msi`, `hpet`)
- [ ] `BOOT_TRY()` macro (§8) checks skip mask before calling init function; if skipped, log `[SKIP] SUBSYS: disabled via boot.conf`
- [ ] Default: `skip=` (empty, nothing skipped)
- [ ] Serial log: `[CONF] skip: mouse,ahci_msi` (echo parsed skip list)
- [ ] Commit: `"boot: boot.conf skip= key for subsystem bypass"`

**Test checkpoint:** Set `skip=mouse,smbios` in boot.conf. Boot on bare metal. Mouse and SMBIOS are skipped. Desktop works without them.

## 14. CPU Feature Minimum Requirements and Verification

Define the minimum CPU feature set required to boot, verify features are actually enabled after activation, and provide clear diagnostics when features are missing or fail to enable.

**Files:** `src/kernel/main/boot_hw.c`, `src/kernel/cpu_security.c`, `include/kernel/cpuid.h`

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-17-kernel-security-hardening.md` — owns the IMPLEMENTATION of NX/SMEP/SMAP/CET. This section owns VERIFICATION that features actually took effect on real hardware, and DIAGNOSIS when they don't.

- [ ] Define `MINIMUM_CPU_FEATURES`: Long Mode (implicit), NX, SSE2. Boot halts with user-friendly message if missing.
- [ ] Define `RECOMMENDED_CPU_FEATURES`: SMEP, SMAP, PCID, XSAVE, RDRAND, InvTSC. Boot warns if missing but continues.
- [ ] `cpu_verify_hardening()` — called after `cpu_harden()` + `cpu_harden_post_pagetable()`: read back EFER, CR4, CR0 and verify expected bits are set. Log any discrepancy: `[WARN] cpu: EFER.NXE not set after enable (firmware override?)`
- [ ] For each feature: if CPUID says supported but CR4/EFER write failed, emit specific POST code and log the platform (vendor, model, stepping) for the quirk database.
- [ ] Document known bare-metal quirks: SMEP/SMAP need per-process page tables (memory saved), HPET needs UC MMIO (memory saved), GS_BASE clobbered by GDT reload (memory saved).
- [ ] Commit: `"boot: CPU feature minimum requirements + post-activation verification"`

**Test checkpoint:** Boot on CPU without SMAP. Log shows `[WARN] cpu: SMAP not available — skipped`. Boot continues. On CPU with SMAP: verification confirms CR4.SMAP is set.

## 15. Bare-Metal Test Matrix and Validation Plan

Define the hardware platforms to test on, expected boot timings per phase, and a regression detection framework so bare-metal issues are caught early.

**Files:** `docs/bare-metal-test-matrix.md` (new), `todo/01-boot-platform/TODO-06-bare-metal-hardening.md`

- [ ] Document test platforms:

| Platform | CPU | GPU | PCI | Storage | PS/2 | Status |
|----------|-----|-----|-----|---------|------|--------|
| QEMU WHPX | Virtual (host passthrough) | Bochs VGA | Emulated | Emulated AHCI | Yes | ✅ Primary dev |
| QEMU TCG | Virtual (software) | Bochs VGA | Emulated | Emulated AHCI | Yes | ✅ CI |
| VirtualBox | Virtual (VT-x) | VMSVGA | Emulated | Emulated AHCI | Yes | ✅ Secondary |
| i5-11600K laptop | Rocket Lake | Intel iGPU | Real PCIe | NVMe + SATA | Touchpad (USB/I2C) | 🔄 In progress |

- [ ] Define expected Phase timings: Phase 0 < 200ms, Phase 1 < 2s, Phase 2 < 10s, Phase 3 < 15s. Log warning if exceeded.
- [ ] Define bare-metal boot checklist: (1) POST codes visible in NVRAM, (2) serial log complete if serial present, (3) splash appears, (4) desktop renders, (5) timer ticks running, (6) keyboard responsive.
- [ ] Define regression test: after any interrupt/timer/ISR change, test on bare metal before merge.
- [ ] Commit: `"docs: bare-metal test matrix and validation plan"`

**Test checkpoint:** Bare-metal boot on i5-11600K matches the checklist. All phases within timing thresholds.

---

## OS Comparison


| ⭐ | Feature                              | Win11                                   | Linux                                            | Impossible OS                                     |
|----|--------------------------------------|-----------------------------------------|--------------------------------------------------|----------------------------------------------------|
| 💎 | IST stacks                           | ✅ Separate stacks for all critical    | ✅ IST1=#DF, IST2=NMI, IST3=MCE, IST4=debug      | ⬜ §3 — currently all exceptions share kernel     |
| 💎 | ACPI FADT boot arch detection        | ✅ HAL checks all IAPC_BOOT_ARCH flags | ✅ Parses FADT, gates PIT/RTC/PS2 access         | ⬜ §5 — currently only PCAT_COMPAT and HW_REDUCED |
| 💎 | PS/2 detection before port access    | ✅ HAL detects i8042 via ACPI          | ✅ `i8042.nopnp`, ACPI _HID match                | ⬜ §6 — currently touches ports blindly           |
| 💎 | Graceful boot degradation            | ✅ Last Known Good, Safe Mode,         | ✅ systemd continues on unit failure             | ⬜ §8 — currently crashes on any init             |
| ⭐ | 4-digit POST codes in every function | ❌ POST codes are BIOS-only (2-digit)  | ❌ No POST code system (relies                   | ⬜ §1+§2 — 0x0000–0xFFFF visible on screen +      |
| 💎 | UC MMIO mapping                      | ✅ MmMapIoSpace                        | ✅ ioremap_uc                                    | ⬜ §3 — minimal vmm_map_mmio_uc                   |
| 💎 | Per-process page tables               | ✅ Each process has own CR3            | ✅ mm_struct per task, CR3 switch                 | ⬜ §10 — minimal PML4-per-task + CR3 switch        |
| ⭐ | boot.conf subsystem skip list        | ⚠️ `bcdedit /set safeboot` (coarse)    | ⚠️ Kernel params like `i8042.noaux` (per-driver) | ⬜ §12 — fine-grained `skip=mouse,ahci_msi`       |
| 💎 | AHCI MSI with fallback               | ✅ StorAHCI driver with INTx fallback  | ✅ libahci with MSI/INTx/polled fallback         | ⬜ §8 — MSI/INTx/polled fallback                  |
| 💎 | CPU security verify post-enable      | ✅ HAL verifies CR4/EFER              | ✅ Kernel checks feature enable                  | ⬜ §10 — read-back EFER/CR4 after activation      |
| 💎 | CPU feature minimum requirements     | ✅ NX required since Vista            | ✅ Minimum feature checks at boot                | ⬜ §13 — define minimum + recommended features    |
| ⭐ | Bare-metal test matrix               | ❌ Internal only (WHQL)               | ❌ Community-driven (no matrix)                  | ⬜ §14 — documented platforms + timing thresholds  |

> **After §1–§15:** Impossible OS boots on any x86-64 hardware with the same reliability as Windows and Linux, plus superior diagnostics: every function has a POST code, every failure is logged and survived, users can bypass broken subsystems without recompiling, and user/kernel address spaces are properly separated with SMEP/SMAP enforced. No external TODO dependency blocks execution.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU WHPX: full boot to desktop, all subsystems OK, POST codes visible in serial log
- [ ] QEMU TCG: full boot to desktop with PIT timer path
- [ ] VirtualBox: full boot to desktop
- [ ] Bare metal (i5-11600K): full boot to desktop with timer running, no workaround skip flags
- [ ] Bare metal crash test: force `mouse_init()` failure → boot continues → desktop shows degraded notification
- [ ] NVRAM persistence: crash at known POST code → reboot → serial shows "Last boot failed at: 0xNNNN"
- [ ] IST verification: stack overflow → #DF fires on IST1 → BSOD shown (not silent triple fault)
- [ ] `skip=mouse,smbios` in boot.conf → those subsystems skipped → boot completes
- [ ] No `HV_BAR()` calls remaining in codebase (replaced by POST16 system)
- [ ] Commit: `"boot: bare metal hardening complete — all platforms boot reliably"`
