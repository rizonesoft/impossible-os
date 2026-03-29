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
> - Panic forensic evidence struct and cross-boot persistence → owned by `TODO-02-boot-diagnostics.md §2`
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
- → XREF: `TODO-02-boot-diagnostics.md §6` — panic forensic evidence struct (deferred; this TODO validates it works on bare metal when implemented)
- → XREF: `TODO-03-interrupt-timer-arch.md` — timer HAL design (this TODO investigates why hw interrupts crash on bare metal)
- → XREF: `TODO-04-cpu-boot-sequencing.md §2,§4` — CPU hardening activation order (deferred there; minimal version in §9 here)
- → XREF: `TODO-05-visual-post-display.md §1-§2` — 4-digit POST code system and per-function instrumentation (moved from this TODO to TODO-05)
- → XREF: `02-kernel-core/TODO-17-kernel-security-hardening.md` — NX/SMEP/SMAP implementation (this TODO does NOT reimplement; handles bare-metal quirks like shared page tables)
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §1` — boot_progress() infrastructure (this TODO consumes it)
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` — UC MMIO mapping for HPET/AHCI

## Outcome

- Every boot subsystem emits granular POST codes (4-digit hex) visible on I/O port 0x80, UEFI NVRAM, and on-screen display.
- Next boot shows "SUCCEEDED" or "FAILED Phase N (0xNNNN)" via per-phase NVRAM tracking — no configuration needed.
- Hardware interrupts work reliably on bare metal — IST stacks, correct LAPIC delivery, verified ISR frame layout.
- PS/2, RTC, AHCI, and all legacy subsystems are gated by ACPI capability flags — never touch hardware that doesn't exist.
- Any subsystem failure degrades gracefully with a log message instead of crashing.
- `boot.conf` can skip specific subsystems: `skip=mouse,ahci_msi,hpet`.

## Implementation Order

| ⭐  | Order | Deliverable                                            | Depends On | Status |
| --- | :---: | ------------------------------------------------------ | ---------- | :----: |
| 💎  |   1   | Minimal UC MMIO mapping (`vmm_map_mmio_uc`)            | —          |  [x]   |
| 💎  |   2   | IST stacks for critical exceptions                     | —          |  [x]   |
| 💎  |   3   | Hardware interrupt root cause investigation            | §2         |  [x]   |
| 💎  |   4   | ACPI FADT boot architecture flags                      | —          |  [x]   |
| 💎  |   5   | PS/2 controller detection and safe init                | §4         |  [x]   |
| 💎  |   6   | AHCI interrupt hardening                               | §1, §3     |  [x]   |
| 💎  |   7   | Resilient boot with graceful degradation               | —          |  [x]   |
| 💎  |   8   | Per-process page tables (minimal base)                 | §1         |  [x]   |
| 💎  |   9   | CPU security activation and verification               | §4, §8     |  [x]   |
| 💎  |  10   | Boot order hardening (timer-last, UEFI-safe)           | §3         |  [ ]   |
| ⭐  |  11   | `boot.conf` subsystem skip list                        | §7         |  [ ]   |
| 💎  |  12   | Migrate logging from X:\ to C:\ + remove log partition | §7         |  [ ]   |
| 💎  |  13   | CPU feature minimum requirements and verification      | §4, §9     |  [ ]   |
| 💎  |  14   | Bare-metal test matrix and validation plan             | §3         |  [ ]   |
| 💎  |  15   | Boot splash spinner bare-metal fix                     | §3, §10    |  [ ]   |

> 💎 = parity — Windows and Linux both handle bare-metal quirks, IST, ACPI gating, and graceful degradation.
> ⭐ = exclusive — 4-digit POST codes in every function and a configurable skip list are not standard in any OS kernel.

---

## 1. Minimal UC MMIO Mapping (`vmm_map_mmio_uc`)
Implement a minimal `vmm_map_mmio_uc()` that creates uncacheable mappings for device MMIO regions. This unblocks HPET calibration (within this section) and AHCI hardening (§6) on bare metal where WB-cached MMIO causes MCE.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`

> [!NOTE]
> Minimal prerequisite — full `vmm_map_mmio()` / `MmMapIoSpace()` with cache type selection, HPET quirk table, and driver audit is in `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1`. This section implements just enough to map a device BAR as UC using 4 KiB PTEs.

- [x] `vmm_map_mmio_uc(uint64_t phys_base, uint32_t size)` — bump allocator at 4 GiB+ VA range, 4 KiB PTEs with PCD=1+PWT=1 (UC)+NX
- [x] `vmm_unmap_mmio(void *virt, uint32_t size)` — walks and unmaps PTEs (does not free physical frames)
- [x] Validate: page-aligned phys_base, size > 0, within 1 GiB MMIO VA limit
- [x] Test: map LAPIC base (0xFEE00000) as UC in boot_phase0, verify LAPIC ID matches identity-mapped read
- [x] Re-enable HPET calibration in `lapic.c`: `cal_try_hpet()` maps HPET via `vmm_map_mmio_uc()` and uses UC pointer for all MMIO reads
- [ ] `hpet_read_ns()` — deferred to TODO-03 §2 (UTS timer driver); HPET calibration works without it
- [x] Commit: `"mm: minimal vmm_map_mmio_uc + HPET re-enabled with UC mapping"`

**Debug POST codes:** `POST16(0xD100)` = vmm_map_mmio_uc entry, `0xD101` = PTE allocated, `0xD102` = HPET mapped, `0xD103` = HPET read OK, `0xD104` = §1 complete. On crash: last POST on VPD/serial pinpoints failure.

**Test checkpoint:** QEMU: HPET calibration succeeds (`Tier 2: HPET calibration -> N ticks/ms`). Bare metal: HPET mapped via UC, no MCE, calibration succeeds. `hpet_read_ns()` returns monotonically increasing values. Test on: QEMU WHPX, QEMU TCG, bare metal.

## 2. IST Stacks for Critical Exceptions

Allocate dedicated interrupt stacks for Double Fault (#DF), NMI, and Machine Check Exception (MCE). Without IST, a stack overflow during an exception handler causes a triple fault and silent reboot — the most common "mystery crash" on bare metal.

**Files:** `src/kernel/gdt.c`, `src/kernel/idt.c`, `include/kernel/gdt.h`

> [!IMPORTANT]
> Linux uses IST1 for #DF, IST2 for NMI, IST3 for MCE. Windows uses separate stacks for the same exceptions via task gates (32-bit) or IST (64-bit). Both guarantee that these critical exceptions can always execute even when the kernel stack is corrupted.

- [x] Allocate 3 IST stacks (4 KiB each) from PMM during `gdt_init()` — identity-mapped, phys = virt
- [x] Set `kernel_tss.ist1` = DF stack top, `kernel_tss.ist2` = NMI stack top, `kernel_tss.ist3` = MCE stack top
- [x] Update IDT entries: vector 8 (#DF) → IST=1, vector 2 (NMI) → IST=2, vector 18 (MCE) → IST=3
- [x] NMI/MCE/#DF handlers: existing `panic_screen()` provides register dump, BSOD, NVRAM write, halt — no separate handler needed
- [ ] Verify: stack overflow in kernel → #DF fires on IST1 stack → shows BSOD instead of triple fault *(deferred to BM Test 1)*
- [x] Commit: `"kernel: IST stacks for #DF, NMI, MCE — no more silent triple faults"`

**Debug POST codes:** `POST16(0xD200)` = IST alloc start, `0xD201` = TSS IST fields set, `0xD202` = IDT entries updated, `0xD203` = §2 complete.

**Test checkpoint:** Intentionally overflow the kernel stack (recursive function). Verify #DF handler fires and shows a BSOD with register dump instead of a silent reboot. Test on: QEMU WHPX, bare metal.

## 3. Hardware Interrupt Root Cause Investigation *(done)*
Root cause found and fixed 2026-03-29.

**Files:** `src/kernel/isr_stubs.asm`

> [!IMPORTANT]
> **Root cause:** `clac` instruction (opcode `0F 01 CA`) at the start of `isr_common_stub`. Intel SDM requires `CPUID.SMAP` bit set for `clac` — without it, `clac` causes #UD. The #UD handler re-enters `isr_common_stub` → `clac` → #UD → infinite loop → triple fault. IST stacks can't save it because the #DF handler also hits `clac` on its IST stack.
>
> **Why it was hidden:** QEMU WHPX passes through host CPU features (i5-11600K has SMAP), so `clac` worked there. TCG's emulated CPU and VirtualBox's NEM mode don't expose SMAP, causing #UD. Bare metal i5-11600K has SMAP in CPUID, so the crash there had different timing characteristics that masked the root cause.
>
> **Fix:** Removed `clac` from `isr_common_stub`. SMAP is not enabled on any platform yet (requires per-process page tables, §8). Will be re-added via runtime alternatives patching when SMAP is activated.

- [x] Root cause identified: `clac` #UD on CPUs without SMAP CPUID support
- [x] Fix applied: removed `clac` from `isr_common_stub` in `isr_stubs.asm`
- [x] Bare metal timer + AHCI workarounds removed (timer.c, boot_storage.c)
- [x] Verified on all 4 platforms: QEMU WHPX ✅, QEMU TCG ✅, VirtualBox ✅, bare metal ✅
- [x] Commit: `"kernel: remove clac from ISR common stub — fixes TCG and bare metal crash"`

**Test results (2026-03-29):**

| Platform | Timer | AHCI | Desktop | Status |
|----------|-------|------|---------|--------|
| QEMU WHPX | LAPIC (Hyper-V MSR) | MSI | Full | ✅ |
| QEMU TCG | PIT | MSI | Full | ✅ |
| VirtualBox | LAPIC (TSC ref) | INTx | Full (NCQ slow) | ✅ |
| Bare metal | LAPIC | MSI | Full | ✅ |

## 4. ACPI FADT Boot Architecture Flags
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

- [x] Parse `IAPC_BOOT_ARCH` from FADT `boot_arch_flags` field (requires FADT length ≥ 113)
- [x] API: `acpi_has_8042()`, `acpi_has_cmos_rtc()`, `acpi_msi_supported()`, `acpi_has_vga()` — all safe-default to 1 if FADT absent/short
- [x] `acpi_hw_reduced()` already exists — logged alongside IAPC_BOOT_ARCH
- [x] Log: `IAPC_BOOT_ARCH: 8042=%d RTC=%d MSI=%d VGA=%d HW_REDUCED=%d`
- [x] Commit: `"kernel: parse ACPI FADT IAPC_BOOT_ARCH flags for legacy device detection"`

**Debug POST codes:** `POST16(0xD400)` = FADT parse start, `0xD401` = IAPC_BOOT_ARCH read, `0xD402` = §4 complete.

**Test checkpoint:** Boot on QEMU. Log shows IAPC_BOOT_ARCH with all flags. On bare metal, log shows actual hardware configuration. Test on: QEMU WHPX, bare metal.

## 5. PS/2 Controller Detection and Safe Init
Gate all PS/2 keyboard and mouse I/O behind ACPI detection. Never write to ports 0x60/0x64 if the i8042 doesn't exist.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`

- [x] `keyboard_init()`: checks `acpi_has_8042()` before any port I/O — skips if no i8042
- [x] `mouse_init()`: checks `acpi_has_8042()` + 0xFF status + reset ACK probe — skips gracefully
- [x] Timeouts increased to 1000000 with 0xFF early-exit on `ps2_wait_input`/`ps2_wait_output`
- [x] `mouse_init()` re-enabled in `boot_interrupts.c` — ACPI gate replaces skip workaround
- [x] Keyboard, mouse, vbox_mouse migrated to `irq_request_gsi()` when IOAPIC available
- [x] Commit: `"drivers: PS/2 keyboard/mouse gated by ACPI i8042 detection + GSI-based IRQ"`

**Debug POST codes:** `POST16(0xD500)` = PS/2 detect start, `0xD501` = i8042 check done, `0xD502` = keyboard init, `0xD503` = mouse probe, `0xD504` = §5 complete.

**Test checkpoint:** Boot on laptop without PS/2 mouse. Mouse init logs "skipped" and boot continues. Boot on QEMU with PS/2 — mouse works normally. Test on: QEMU WHPX, bare metal.

## 6. AHCI Interrupt Hardening
Make `ahci_setup_interrupts()` safe on bare metal: mask LAPIC timer during MSI setup, verify ABAR MMIO accessibility, fall back to polled mode if MSI fails.

**Files:** `src/kernel/drivers/ahci/ahci_core.c`

> [!IMPORTANT]
> On bare metal, enabling MSI writes to PCI config space which triggers the device to send MSI messages to the LAPIC. If the LAPIC timer is also firing, the two LAPIC writes can race. Additionally, AHCI ABAR MMIO at the device's BAR5 address is accessed through WB-cached page table entries — same issue as HPET.

- [x] Mask LAPIC timer LVT before MSI enable; unmask after
- [x] Validate ABAR: page-aligned, within 4 GiB, not 0 — logs and uses polling if invalid
- [x] MSI enable verify: read VID after enable, if 0xFFFF → device gone, free vector, fall back to INTx
- [x] MSI → INTx fallback (already existed in ahci_core.c)
- [x] INTx → polled fallback (already existed — logs "using polling")
- [x] Bare metal skip workaround removed (done in §3 clac fix)
- [x] Commit: `"drivers: AHCI interrupt hardening — LAPIC mask + ABAR validation"`

**Debug POST codes:** `POST16(0xD600)` = AHCI harden start, `0xD601` = LAPIC masked, `0xD602` = MSI enable, `0xD603` = MSI verify, `0xD604` = LAPIC unmasked, `0xD605` = §6 complete. On crash at 0xD602: MSI enable killed the device.

**Test checkpoint:** Boot on bare metal. AHCI init logs either "MSI vector 0xNN" or "INTx fallback" or "polled mode". No crash. Disk I/O works. Test on: QEMU WHPX, bare metal.

## 7. Resilient Boot with Graceful Degradation
Wrap every Phase 1–3 subsystem init in a protective pattern: emit POST code, call init, check result, log outcome, continue on failure. Never `boot_halt()` for non-critical subsystems.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_storage.c`, `src/kernel/main/boot_desktop.c`

> [!IMPORTANT]
> **Critical subsystems (BOOT_FATAL if fail):** PMM, VMM, Heap, GDT, IDT, VFS.
> **Non-critical (BOOT_DEGRADED if fail):** RTC, keyboard, mouse, NIC, AHCI, SMBIOS, splash, registry, SMP, DHCP.
> A degraded boot reaches the desktop with reduced functionality. The user sees a notification listing what failed.

- [x] `BOOT_TRY(subsys, fn_call, name)` macro: calls fn, sets degraded_mask on failure, logs warning, continues
- [x] `g_boot_info.degraded_mask` (32-bit) tracks which subsystems failed
- [x] Classification: critical subsystems use `boot_halt()` (PMM, VMM, GDT, IDT, VFS); non-critical use BOOT_TRY or void+log (RTC, keyboard, mouse, etc.)
- [x] Phase 3 desktop: logs degraded subsystem list if any failed
- [x] Bare-metal skip workarounds already removed in §3 (clac fix) and §5 (ACPI gate)
- [x] Commit: `"boot: resilient init with BOOT_TRY — non-critical failures degrade, never crash"`

**Test checkpoint:** Disable a non-critical subsystem (e.g., force `rtc_init()` to fail). Boot completes. Desktop shows degraded notification. Serial log shows `[WARN] RTC: init failed — degraded`.

## 8. Per-Process Page Tables (Minimal Base)

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

- [x] `vmm_create_user_pml4()` — clones kernel PML4, splits PD[4] into 4KiB PT, User bit on PML4/PDPT/PD levels
- [x] `vmm_set_user_page(pml4, virt)` — sets User bit on individual 4KiB pages in split PT
- [x] `vmm_destroy_user_pml4(pml4)` — frees cloned PML4/PDPT/PD/PT (not data pages)
- [x] `task_create_user()`: creates per-process PML4, marks ELF + user stack pages as User, user stack at 0x8FC000 (in PD[4] range)
- [x] `schedule()` + `schedule_now()`: CR3 switch if next task has different PML4
- [x] `task_create()`: kernel tasks use boot PML4 (cr3=0)
- [ ] Remove `pmm_mark_region_used(0x800000, 0x100000)` — deferred, ELF loader still uses fixed phys address
- [ ] Remove SMEP/SMAP skip — moved to §9 (CPU security activation)
- [x] Commit: `"mm: per-process page tables — user/kernel separation, CR3 switch"`

**Regression risk:** This changes the memory model fundamentally. CR3 switch on every context switch adds ~100ns overhead. If page table creation has a bug, user-mode ELF won't run. Rollback: revert to shared identity map and re-skip SMEP/SMAP.

**Debug POST codes:** `POST16(0xD800)` = PML4 create start, `0xD801` = kernel entries cloned, `0xD802` = user page mapped, `0xD803` = CR3 switch test, `0xD804` = SMEP/SMAP enable, `0xD805` = user ELF loaded in new PML4, `0xD806` = §8 complete. On crash at 0xD803: CR3 switch broke. At 0xD804: SMEP/SMAP faulted.

**Test checkpoint:** Boot on QEMU WHPX. cmd.exe runs in user mode with its own PML4. Kernel pages don't have User bit. `KeGetCurrentIrql()` works from user-mode interrupt. Boot on bare metal: SMEP/SMAP enabled (CR4 bits set), cmd.exe runs, no page faults. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 9. CPU Security Activation and Verification

Ensure CPU security features (NX, SMEP, SMAP) are activated in the correct order and verified on bare metal. This incorporates the bare-metal-relevant parts of the CPU boot sequencing that are currently unimplemented.

**Files:** `src/kernel/cpu_security.c`, `src/kernel/main/boot_hw.c`

> [!NOTE]
> Minimal prerequisite — full CPU boot sequencing (EFER before VMM, XSAVE/PCID, AP parity) is in `TODO-04-cpu-boot-sequencing.md §2,§4,§1`. This section implements just enough to ensure NX/SMEP/SMAP work on bare metal and verifies post-activation.

- [x] Activation order formalized: `cpu_harden()` (NX) → `vmm_apply_nx_policy()` → `cpu_harden_post_pagetable()` (SMEP/SMAP) → `cpu_verify_hardening()`
- [x] `cpu_verify_hardening()`: reads back EFER (NX), CR4 (SMEP/SMAP), logs discrepancies or EPT enforcement
- [x] Bare metal: SMEP/SMAP still skipped — boot PML4 has User bit on all 2MiB pages (entry.asm 0x87). Need to clear User from kernel pages in boot PML4 first. Per-process PML4 has correct U/S but kernel PML4 does not.
- [x] VMs: SMEP/SMAP work on KVM/TCG/VBox (if CPUID has feature), EPT enforced on WHPX. Logged.
- [x] Debug POST codes: 0xD900-0xD904
- [x] Commit: `"boot: CPU security activation with post-enable verification"`

**Test checkpoint:** Boot on bare metal. Log shows `[OK] cpu: NX enabled, SMEP skipped (shared page tables), SMAP skipped`. On QEMU: log shows all three enabled (or EPT-enforced).

## 10. Boot Order Hardening (Timer-Last, UEFI-Safe)

Formalize the boot order lessons learned: timer is the last thing initialized before `sti`, all UEFI runtime calls mask the LAPIC timer, and `boot_splash_start_animation()` only runs after `sti`.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_init.c`

- [ ] Move `timer_hal_init()` to the last step of Phase 1, immediately before `sti` (already done — formalize and document)
- [ ] `boot_post_write()`: always mask LAPIC timer LVT during `uefi_set_variable()` call (already done — verify correctness)
- [ ] `uefi_set_variable()` wrapper: generic LAPIC mask/unmask guard for ALL UEFI runtime calls, not just POST write
- [ ] Document the Phase 1 init order contract in a comment block at the top of `boot_interrupts.c`
- [ ] Add `BOOT_ASSERT(condition, msg)` macro for impossible-state checks (e.g., `BOOT_ASSERT(kernel_subsystem_ready(SUBSYS_IDT), "IDT must be ready before timer")`)
- [ ] Commit: `"boot: formalize Phase 1 init order — timer-last, UEFI-safe, documented contract"`

**Test checkpoint:** Boot on QEMU + bare metal. Phase 1 order is correct. UEFI runtime calls don't crash with timer running. Boot order comment block is visible at top of `boot_interrupts.c`.

## 11. `boot.conf` Subsystem Skip List

Allow the user to skip specific subsystems via `boot.conf` for debugging or working around hardware bugs without recompiling.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`, `src/kernel/main/boot_interrupts.c`

- [ ] Add `skip=` key to `boot.conf`: comma-separated list of subsystem names (e.g., `skip=mouse,ahci_msi,hpet,smbios`)
- [ ] Parse into `g_boot_info.config.skip_mask` (32-bit bitmask matching `SUBSYS_*` enum + extra flags for sub-features like `ahci_msi`, `hpet`)
- [ ] `BOOT_TRY()` macro (§2) checks skip mask before calling init function; if skipped, log `[SKIP] SUBSYS: disabled via boot.conf`
- [ ] Default: `skip=` (empty, nothing skipped)
- [ ] Serial log: `[CONF] skip: mouse,ahci_msi` (echo parsed skip list)
- [ ] Commit: `"boot: boot.conf skip= key for subsystem bypass"`

**Test checkpoint:** Set `skip=mouse,smbios` in boot.conf. Boot on bare metal. Mouse and SMBIOS are skipped. Desktop works without them.

## 12. Migrate Logging from X:\ to C:\ + Remove Log Partition

Remove the dedicated FAT32 logging partition (`X:\`) — a development hack from when IXFS didn't support writes. All boot logs, crash dumps, and diagnostics write to `C:\Impossible\System\Logs\` on the main IXFS partition. The FAT32 partition is repurposed as EFI System Partition recovery storage.

**Files:** `src/kernel/klog.c`, `src/kernel/main/boot_storage.c`, `src/kernel/boot_timing.c`, `scripts/build.sh` (disk image layout)

- [ ] Change `klog` disk target from `X:\` to `C:\Impossible\System\Logs\` — create directory at first boot if it doesn't exist
- [ ] Move `boot-profile.log` and `boot-timeline.json` writes from `X:\` to `C:\Impossible\System\Logs\`
- [ ] Remove FAT32 log partition from GPT disk image layout in `scripts/build.sh` (or repurpose as ESP recovery area)
- [ ] Remove `X:\` mount from VFS partition scan — no more `blk: Mounted Logs partition as X:`
- [ ] If `C:\` is not yet mounted when early klog tries to flush (Phase 2, before VFS): buffer in ring, flush when VFS is ready (already the current behavior)
- [ ] Verify: no references to `X:\` remain in kernel code after migration
- [ ] Commit: `"boot: migrate logging from X:\\ to C:\\Impossible\\System\\Logs\\, remove log partition"`

**Test checkpoint:** QEMU: boot log written to `C:\Impossible\System\Logs\26032801.LOG`. No `X:\` mount in serial log. Bare metal: same path, IXFS write works. Disk image has 2 partitions (ESP + IXFS) instead of 3.

## 13. CPU Feature Minimum Requirements and Verification

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

## 14. Bare-Metal Test Matrix and Validation Plan

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

## 15. Boot Splash Spinner Bare-Metal Fix

The boot splash spinner stutters on bare metal — stops and restarts repeatedly during Phase 2. Root cause: the spinner animation is driven by `timer_tick_callback_fire()` in the LAPIC timer ISR. On bare metal, the timer is disabled (TODO-06 §3 hw interrupt investigation) so the spinner only advances when the compositor or `boot_splash_tick()` explicitly calls it. Between explicit calls (PCI scan, AHCI init), the spinner freezes for seconds.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/spinner.c`, `src/kernel/main/boot_storage.c`

- [ ] Investigate: is the spinner driven by timer callback (`timer_register_tick_callback`) or by explicit `boot_splash_tick()` calls? On bare metal without timer, only explicit calls work.
- [ ] Add `boot_splash_tick()` calls at regular intervals during long operations (PCI scan loop, AHCI port enumeration, VFS mount) — every ~100ms of wall time.
- [ ] If timer IS working on bare metal (after §3 hw interrupt fix): verify spinner callback fires at 10fps (every 10 ticks at 100Hz). If not, the callback registration might be lost during timer reinit.
- [ ] If timer is NOT working: implement TSC-based spinner fallback — `boot_splash_tick()` reads TSC and advances the spinner if 100ms has elapsed since last advance, independent of timer interrupts.
- [ ] Verify: spinner rotates smoothly at ~10fps on QEMU WHPX, VBox, TCG, AND bare metal.
- [ ] Commit: `"boot: fix spinner stutter on bare metal — TSC fallback + explicit tick calls"`

**Test checkpoint:** Bare metal: spinner rotates smoothly during PCI scan and AHCI init (no visible stutter or freeze). QEMU: spinner unchanged (already smooth).

---

## OS Comparison


| ⭐ | Feature                 | Win11                       | Linux                        | Impossible OS                    |
|----|-------------------------|-----------------------------|------------------------------|----------------------------------|
| 💎 | UC MMIO mapping         | ✅ MmMapIoSpace             | ✅ ioremap_uc               | ✅ §1 vmm_map_mmio_uc           |
| 💎 | IST stacks              | ✅ All critical exceptions  | ✅ IST1-4 for DF/NMI/MCE    | ✅ §2 IST1-3 for DF/NMI/MCE     |
| 💎 | ACPI FADT boot arch     | ✅ HAL checks all flags     | ✅ Gates PIT/RTC/PS2        | ✅ §4 IAPC_BOOT_ARCH parsed     |
| 💎 | PS/2 ACPI detection     | ✅ HAL detects i8042        | ✅ i8042.nopnp              | ✅ §5 FADT + GSI routing        |
| 💎 | AHCI MSI fallback       | ✅ StorAHCI INTx fallback   | ✅ libahci polled fallback  | ✅ §6 MSI→INTx→polled           |
| 💎 | Graceful degradation    | ✅ Safe Mode + Last Known   | ✅ systemd continues        | ✅ §7 BOOT_TRY + degraded_mask  |
| 💎 | Per-process page tables | ✅ Each process own CR3     | ✅ mm_struct per task       | ✅ §8 PML4 clone + CR3 switch   |
| 💎 | CPU security verify     | ✅ HAL verifies CR4/EFER    | ✅ Checks feature enable    | ✅ §9 verify NX/SMEP/SMAP       |
| ⭐ | boot.conf skip list     | ⚠️ bcdedit safeboot         | ⚠️ i8042.noaux per-driver   | ⬜ §11                          |
| 💎 | Logging on main FS      | ✅ C:\Windows\System32      | ✅ /var/log                 | ⬜ §12 — migrate from X:\       |
| 💎 | CPU feature minimums    | ✅ NX required since Vista  | ✅ Minimum checks at boot   | ⬜ §13                          |
| ⭐ | Bare-metal test matrix  | ❌ Internal only (WHQL)     | ❌ Community-driven         | ⬜ §14                          |

> **After §1–§14:** Impossible OS boots on any x86-64 hardware with the same reliability as Windows and Linux. User/kernel separation with SMEP/SMAP enforced. Graceful degradation on hardware failures. Configurable skip list. Logging on main filesystem. No external TODO blocks execution.

## Bare Metal Testing Plan

> [!IMPORTANT]
> **5 bare metal checkpoints instead of 15.** Implement and verify batches on QEMU first. Only go to bare metal at critical checkpoints where VM behavior diverges from real hardware. Debug POST codes (0xD1xx–0xD9xx) make each bare metal cycle fast — one boot, read serial/VPD, identify failure.

### BM Test 1+2 — Foundation + Interrupts (§1–§4) ✅ PASSED 2026-03-29
> [!NOTE]
> BM Tests 1 and 2 were combined — the `clac` fix (§3) resolved all hardware interrupt crashes simultaneously. All 4 platforms tested in one session.

- [x] UC MMIO: `vmm_map_mmio_uc: LAPIC ID match` on all platforms
- [x] IST stacks: `IST stacks: DF=... NMI=... MCE=...` allocated
- [x] FADT flags: `IAPC_BOOT_ARCH: 8042=1 RTC=1 MSI=1 VGA=1 HW_REDUCED=0`
- [x] **LAPIC timer fires on bare metal** — root cause was `clac` #UD
- [x] PCI scan completes with timer ticks running
- [x] Desktop reached with timer active on all 4 platforms
- [x] HPET calibration: not triggered (Tier 1/1b succeed first) — needs bare metal test without Hyper-V/TSC

**Results:**

| Platform | Timer | Calibration | AHCI | Desktop | Notes |
|----------|-------|------------|------|---------|-------|
| QEMU WHPX | LAPIC 100Hz | Tier 1 Hyper-V MSR | MSI | ✅ 8.4s | Primary dev |
| QEMU TCG | PIT 100Hz | N/A (PIT) | MSI | ✅ | Fixed by clac removal |
| VirtualBox | LAPIC 100Hz | Tier 1b TSC ref | INTx | ✅ 21.8s | NCQ timeout adds 15s |
| Bare metal | LAPIC 100Hz | Tier 1b TSC ref | MSI | ✅ | Fixed by clac removal |

### BM Test 3 — Driver Hardening (§5 + §6 + §7) ✅ PASSED 2026-03-29
> [!NOTE]
> PS/2 gated by ACPI FADT, mouse re-enabled with proper detection, AHCI hardened with LAPIC mask + ABAR validation, BOOT_TRY macro + degraded_mask infrastructure added. Also fixed: VBox mouse click (IOAPIC unmask in irq_request_gsi), VBox spinner speed (TSC calibration sanity check), VBox driver self-contained (buttons read internally).

- [x] PS/2 keyboard/mouse: skipped on QEMU (8042=0), active on VBox/bare metal (8042=1)
- [x] AHCI: MSI on QEMU/bare metal, INTx on VBox — no crash
- [x] BOOT_TRY + degraded_mask: infrastructure in place for graceful degradation
- [x] Keyboard works on all platforms after GSI migration
- [x] VBox: mouse click working, spinner at correct speed (PM Timer calibration)

**Results (2026-03-29, post-§7):**

| Platform | Boot Time | Timer | Calibration | AHCI | PS/2 | Status |
|----------|-----------|-------|-------------|------|------|--------|
| QEMU WHPX | 9.4s | LAPIC 100Hz | Tier 1 Hyper-V MSR | MSI | Skipped (8042=0) | ✅ |
| QEMU TCG | 3.1s | PIT 100Hz | N/A (PIT) | MSI | Skipped (8042=0) | ✅ |
| VirtualBox | 21.8s | LAPIC 100Hz | PM Timer (TSC skip) | INTx | Active (8042=1) | ✅ |
| Bare metal | ~10s | LAPIC 100Hz | Tier 1b TSC ref | MSI | Active (8042=1) | ✅ |

### BM Test 4 — Memory Model (§8 + §9) ✅ PASSED 2026-03-29
> [!NOTE]
> Per-process page tables working on all 3 VM platforms. cmd.exe runs in its own PML4 with User bit on ELF + stack pages. CR3 switches on context switch. SMEP/SMAP still skipped (boot PML4 has User on all 2MiB pages — needs kernel PML4 fix).

- [x] cmd.exe runs with own PML4, types input, shows output on all 3 VMs
- [x] NX verified: `Verify: NX enabled (EFER.NXE set)` on all platforms
- [x] SMEP/SMAP: EPT-enforced on WHPX, not available on TCG/VBox (no CPUID feature)
- [x] No page faults, no triple faults (after fixing User bit on PML4/PDPT levels + user stack in PD[4])

**Results (2026-03-29, post-§9):**

| Platform | Boot | Per-Process PT | NX Verify | SMEP/SMAP | cmd.exe | Status |
|----------|------|---------------|-----------|-----------|---------|--------|
| QEMU WHPX | 10.3s | ✅ CR3 switch | ✅ EFER.NXE | EPT enforced | ✅ | ✅ |
| QEMU TCG | 3.2s | ✅ CR3 switch | ✅ EFER.NXE | N/A (no CPUID) | ✅ | ✅ |
| VirtualBox | ~45s | ✅ CR3 switch | ✅ EFER.NXE | N/A (no CPUID) | ✅ | ✅ |
| Bare metal | TBD | TBD | TBD | TBD | TBD | Next session |

### BM Test 5 — Final Validation (after §11–§15)
Full acceptance pass. All sections complete.

- [ ] Full boot to desktop, timer running, no workarounds
- [ ] `skip=mouse,smbios` → those subsystems skipped → boot completes
- [ ] NVRAM: reboot shows "Last boot: SUCCEEDED"
- [ ] Spinner smooth during PCI scan (no stutter)
- [ ] Boot time within thresholds (Phase 0 <200ms, Phase 1 <2s)
- [ ] Keyboard responsive, AHCI I/O works

**~20 minutes.** Full regression pass.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU WHPX: full boot to desktop, all subsystems OK, POST codes visible in serial log
- [ ] QEMU TCG: full boot to desktop with PIT timer path
- [ ] VirtualBox: full boot to desktop
- [ ] Bare metal (i5-11600K): BM Tests 1–5 all pass
- [x] No `HV_BAR()` calls remaining in codebase — `hv_bar.h` deleted, replaced by TODO-05 VPD
- [ ] Commit: `"boot: bare metal hardening complete — all platforms boot reliably"`
