# TODO-05 -- Bare Metal Boot Hardening & POST Diagnostics

> **Goal:** Make the kernel boot reliably on any x86-64 bare-metal hardware with production-grade error reporting. After this TODO, the boot sequence is robust against absent hardware, misconfigured firmware, and platform-specific quirks -- with fine-grained POST codes that pinpoint failures without serial, debugger, or color bars.

> [!IMPORTANT]
> **Current state (2026-04-11):** `clac` is removed from `isr_common_stub` (`isr_stubs.asm`). UC MMIO uses `vmm_map_mmio_uc` / `vmm_unmap_mmio` (`vmm.c`). IST stacks and TSS fields are allocated in `gdt.c` with IDT IST wiring in `idt.c`. FADT `IAPC_BOOT_ARCH` gates legacy devices (`acpi.c`). AHCI MSI setup masks the LAPIC timer around PCI MSI writes (`ahci_core.c`). `BOOT_TRY` + `g_boot_info.degraded_mask` live in `boot_init.h` with Phase 1--3 call sites. Per-process PML4 + CR3 switch live in `vmm.c` / `task.c`. Early boot reads prior POST from UEFI NVRAM and logs last-boot outcome (`boot_hw.c`, `boot_init.c`). **Still open:** §1 `hpet_read_ns()` (owner TODO-06 §6 UTS), §2 deliberate kernel stack-overflow BSOD test, §8 `pmm_mark_region_used` cleanup for fixed ELF phys + boot PML4 User-bit blocker for SMEP/SMAP on bare metal, `src/kernel/test/test_bare_metal.c` + `test_register_bare_metal()`, BM Test 5, Verification checklist. Dense per-function POST16 instrumentation beyond the `0xD1xx` bring-up set is owned by `TODO-08-visual-post-display.md`.

> [!IMPORTANT]
> **Origin (2026-03-28) -- historical context:** A full day of bare-metal debugging on an i5-11600K laptop exposed fundamental gaps (SMEP page tables, HPET WB MMIO, GS_BASE clobbered by GDT reload, TSS RSP0 uninitialized, heartbeat ISR reentrancy, RTC hang, calibration timeouts). The **original mystery symptom** was hardware interrupts (LAPIC timer / PIT) faulting on bare metal while software `INT 0x81` through the same ISR path worked -- **root cause fixed 2026-03-29** (`clac` #UD in `isr_common_stub`, §3). Workarounds listed here (mouse/AHCI/timer disabled) were removed as the real fixes landed; keep this block so future readers know why the TODO exists.

> [!NOTE]
> **Design principle:** Every subsystem init must be independently skippable. If `mouse_init()` crashes, boot continues without a mouse. If AHCI MSI fails, fall back to polled I/O. If HPET MMIO faults, skip to PM Timer. The boot sequence must be **unbreakable** -- degrade gracefully, never crash.

> [!CAUTION]
> **Scope boundary -- this TODO does NOT own:**
> - CPU security feature implementation (NX, SMEP, SMAP, CET, Spectre) → owned by `TODO-17-kernel-security-hardening.md`
> - CPU activation sequencing (EFER before VMM, CR4 order) → owned by `TODO-04-cpu-boot-sequencing.md`
> - Timer HAL architecture and calibration waterfall design → owned by `TODO-06-interrupt-timer-arch.md`
> - Visual boot progress display (VPD) → owned by `TODO-08-visual-post-display.md`
> - Panic forensic evidence struct and cross-boot persistence → owned by `TODO-07-boot-diagnostics.md §5`
>
> **This TODO owns:** making all of the above **work on real hardware** -- diagnostics, detection, fallbacks, IST, ACPI gating, graceful degradation, and the hw interrupt investigation.

## Inputs

- [`src/kernel/main/boot_hw.c`](../../src/kernel/main/boot_hw.c) -- Phase 0
- [`src/kernel/main/boot_interrupts.c`](../../src/kernel/main/boot_interrupts.c) -- Phase 1
- [`src/kernel/main/boot_storage.c`](../../src/kernel/main/boot_storage.c) -- Phase 2
- [`src/kernel/main/boot_desktop.c`](../../src/kernel/main/boot_desktop.c) -- Phase 3
- [`src/kernel/main/boot_init.c`](../../src/kernel/main/boot_init.c) -- `boot_progress()`, `boot_post_write/read()`
- [`src/kernel/main/boot_progress.c`](../../src/kernel/main/boot_progress.c) -- POST display, stage reporting
- [`src/kernel/idt.c`](../../src/kernel/idt.c) -- ISR handler, IRQL tracking
- [`src/kernel/isr_stubs.asm`](../../src/kernel/isr_stubs.asm) -- ISR assembly stubs
- [`src/kernel/gdt.c`](../../src/kernel/gdt.c) -- TSS, RSP0, IST
- [`src/kernel/gdt_asm.asm`](../../src/kernel/gdt_asm.asm) -- GDT reload (GS preservation)
- [`src/kernel/drivers/lapic.c`](../../src/kernel/drivers/lapic.c) -- LAPIC timer, calibration waterfall
- [`src/kernel/drivers/mouse.c`](../../src/kernel/drivers/mouse.c) -- PS/2 mouse init
- [`src/kernel/drivers/ahci/ahci_core.c`](../../src/kernel/drivers/ahci/ahci_core.c) -- AHCI MSI setup
- [`src/kernel/drivers/framebuffer.c`](../../src/kernel/drivers/framebuffer.c) -- `fb_swap`/`fb_swap_rect` cli/sti
- [`src/kernel/acpi.c`](../../src/kernel/acpi.c) -- FADT parsing, MADT, PM Timer
- [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h) -- `BOOT_TRY`, `g_boot_info.degraded_mask`, subsystem IDs
- → XREF: `TODO-07-boot-diagnostics.md §5` -- panic forensic evidence struct (deferred; this TODO validates it works on bare metal when implemented)
- → XREF: `TODO-06-interrupt-timer-arch.md` -- UTS / LAPIC calibration / `hpet_read_ns()` owner; bare-metal ISR path fixed in this file §3 (`clac` removal)
- → XREF: `TODO-04-cpu-boot-sequencing.md §2,§4` -- CPU hardening activation order (deferred there; minimal version in §9 here)
- → XREF: `TODO-08-visual-post-display.md` §1 §2 -- 4-digit POST16 system and per-function instrumentation (moved from this TODO to TODO-08)
- → XREF: `TODO-08-visual-post-display.md` §4 -- Tier 1 VPD raw VRAM; follows bare-metal interrupt and page-flip constraints in this file §3
- → XREF: `02-kernel-core/TODO-17-kernel-security-hardening.md` -- NX/SMEP/SMAP implementation (this TODO does NOT reimplement; handles bare-metal quirks like shared page tables)
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §1` -- boot_progress() infrastructure (this TODO consumes it)
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §11` -- full `vmm_map_mmio()` / UC MMIO; this TODO §1 keeps minimal `vmm_map_mmio_uc()` only
- → XREF: `TODO-02-bootloader-error-recovery.md §4` -- serial port probe and COM2 fallback; §7 here provides graceful degradation when serial is absent, §4 there detects serial presence in the bootloader

## Outcome

- Boot path emits POST16 codes to I/O port 0x80, UEFI NVRAM (`boot_post_nvram_write16`), and serial; on-screen 4-digit VPD coverage is coordinated with `TODO-08-visual-post-display.md` / `TODO-07-boot-diagnostics.md` (not every subsystem is VPD-instrumented yet).
- Next boot: early serial shows last-boot outcome (`[BOOT] Last boot succeeded` / `failed` in `boot_hw.c`) plus prior POST16 read from NVRAM -- full WinRE-style recovery UI is `TODO-02` / `TODO-15`, not this file.
- Hardware interrupts work reliably on bare metal -- IST stacks, correct LAPIC delivery, verified ISR frame layout.
- PS/2, RTC, AHCI, and all legacy subsystems are gated by ACPI capability flags -- never touch hardware that doesn't exist.
- Any subsystem failure degrades gracefully with a log message instead of crashing.
- Non-critical failures use `BOOT_TRY` (§7) plus `degraded_mask` -- no silent `boot.conf` skip list.

## Implementation Order

| ⭐  | Order | Deliverable                                            | Depends On | Status |
| --- | :---: | ------------------------------------------------------ | ---------- | :----: |
| 💎  |   1   | Minimal UC MMIO mapping (`vmm_map_mmio_uc`)            | --          |  [x]   |
| 💎  |   2   | IST stacks for critical exceptions                     | --          |  [x]   |
| 💎  |   3   | Hardware interrupt root cause investigation            | §2         |  [x]   |
| 💎  |   4   | ACPI FADT boot architecture flags                      | --          |  [x]   |
| 💎  |   5   | PS/2 controller detection and safe init                | §4         |  [x]   |
| 💎  |   6   | AHCI interrupt hardening                               | §1, §3     |  [x]   |
| 💎  |   7   | Resilient boot with graceful degradation               | --          |  [x]   |
| 💎  |   8   | Per-process page tables (minimal base)                 | §1         |  [x]   |
| 💎  |   9   | CPU security activation and verification               | §4, §8     |  [x]   |
| 💎  |  10   | Boot order hardening (timer-last, UEFI-safe)           | §3         |  [x]   |
| ⭐  |  11   | ~~`boot.conf` subsystem skip list~~                    | --          |  [x]   |
| 💎  |  12   | Migrate logging from X:\ to C:\ + remove log partition | §7         |  [x]   |
| 💎  |  13   | CPU feature minimum requirements and verification      | §4, §9     |  [x]   |
| 💎  |  14   | Bare-metal test matrix and validation plan             | §3         |  [x]   |
| 💎  |  15   | Boot splash spinner bare-metal fix                     | §3, §10    |  [x]   |

> 💎 = parity -- Windows and Linux both handle bare-metal quirks, IST, ACPI gating, and graceful degradation.
> ⭐ = exclusive -- dense 4-digit POST codes in every boot function are not standard in any OS kernel.

---

## 1. Minimal UC MMIO Mapping (`vmm_map_mmio_uc`)
Implement a minimal `vmm_map_mmio_uc()` that creates uncacheable mappings for device MMIO regions. This unblocks HPET calibration (within this section) and AHCI hardening (§6) on bare metal where WB-cached MMIO causes MCE.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`

> [!NOTE]
> Minimal prerequisite -- full `vmm_map_mmio()` / `MmMapIoSpace()` with cache type selection, HPET quirk table, and driver audit is in `03-memory-concurrency/TODO-01-vmm-memory-protection.md §11`. This section implements just enough to map a device BAR as UC using 4 KiB PTEs.
> **Debug POST (§1):** `POST16(0xD100)` = vmm_map_mmio_uc entry, `0xD101` = PTE alloc, `0xD102` = HPET mapped, `0xD103` = HPET read OK, `0xD104` = §1 done. On crash: last POST on VPD/serial pinpoints failure.

- [x] `vmm_map_mmio_uc(uint64_t phys_base, uint32_t size)` -- bump allocator at 4 GiB+ VA range, 4 KiB PTEs with PCD=1+PWT=1 (UC)+NX
- [x] `vmm_unmap_mmio(void *virt, uint32_t size)` -- walks and unmaps PTEs (does not free physical frames)
- [x] Validate: page-aligned phys_base, size > 0, within 1 GiB MMIO VA limit
- [x] Test: map LAPIC base (0xFEE00000) as UC in boot_phase0, verify LAPIC ID matches identity-mapped read
- [x] Re-enable HPET calibration in `lapic.c`: `cal_try_hpet()` maps HPET via `vmm_map_mmio_uc()` and uses UC pointer for all MMIO reads
- [ ] `hpet_read_ns()` -- deferred to TODO-06 §6 (UTS / HPET standalone driver); HPET calibration works without it
- [x] Commit: `"mm: minimal vmm_map_mmio_uc + HPET re-enabled with UC mapping"`

**Test checkpoint:** QEMU: HPET calibration succeeds (`Tier 2: HPET calibration -> N ticks/ms`). Bare metal: HPET mapped via UC, no MCE, calibration succeeds. `hpet_read_ns()` is deferred (TODO-06 §6); when implemented, returns monotonic ns. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 2. IST Stacks for Critical Exceptions

Allocate dedicated interrupt stacks for Double Fault (#DF), NMI, and Machine Check Exception (MCE). Without IST, a stack overflow during an exception handler causes a triple fault and silent reboot -- the most common "mystery crash" on bare metal.

**Files:** `src/kernel/gdt.c`, `src/kernel/idt.c`, `include/kernel/gdt.h`

> [!IMPORTANT]
> Linux uses IST1 for #DF, IST2 for NMI, IST3 for MCE. Windows uses separate stacks for the same exceptions via task gates (32-bit) or IST (64-bit). Both guarantee that these critical exceptions can always execute even when the kernel stack is corrupted.
> **Debug POST (§2):** `POST16(0xD200)` = IST alloc start, `0xD201` = TSS IST fields, `0xD202` = IDT IST vectors, `0xD203` = §2 done.

- [x] Allocate 3 IST stacks (4 KiB each) from PMM during `gdt_init()` -- identity-mapped, phys = virt
- [x] Set `kernel_tss.ist1` = DF stack top, `kernel_tss.ist2` = NMI stack top, `kernel_tss.ist3` = MCE stack top
- [x] Update IDT entries: vector 8 (#DF) → IST=1, vector 2 (NMI) → IST=2, vector 18 (MCE) → IST=3
- [x] NMI/MCE/#DF handlers: existing `panic_screen()` provides register dump, BSOD, NVRAM write, halt -- no separate handler needed
- [ ] Verify: stack overflow in kernel → #DF fires on IST1 stack → shows BSOD instead of triple fault *(deferred to BM Test 1)*
- [x] Commit: `"kernel: IST stacks for #DF, NMI, MCE -- no more silent triple faults"`

**Test checkpoint:** Intentionally overflow the kernel stack (recursive function). Verify #DF handler fires and shows a BSOD with register dump instead of a silent reboot. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 3. Hardware Interrupt Root Cause Investigation *(done)*
Root cause found and fixed 2026-03-29.

**Files:** `src/kernel/isr_stubs.asm`

> [!IMPORTANT]
> **Root cause:** `clac` instruction (opcode `0F 01 CA`) at the start of `isr_common_stub`. Intel SDM requires `CPUID.SMAP` bit set for `clac` -- without it, `clac` causes #UD. The #UD handler re-enters `isr_common_stub` → `clac` → #UD → infinite loop → triple fault. IST stacks can't save it because the #DF handler also hits `clac` on its IST stack.
> **Why it was hidden:** QEMU WHPX passes through host CPU features (i5-11600K has SMAP), so `clac` worked there. TCG's emulated CPU and VirtualBox's NEM mode don't expose SMAP, causing #UD. Bare metal i5-11600K has SMAP in CPUID, so the crash there had different timing characteristics that masked the root cause.
> **Fix:** Removed `clac` from `isr_common_stub`. SMAP is not enabled on any platform yet (requires per-process page tables, §8). Will be re-added via runtime alternatives patching when SMAP is activated.

- [x] Root cause identified: `clac` #UD on CPUs without SMAP CPUID support
- [x] Fix applied: removed `clac` from `isr_common_stub` in `isr_stubs.asm`
- [x] Bare metal timer + AHCI workarounds removed (timer.c, boot_storage.c)
- [x] Verified on all 4 platforms: QEMU WHPX ✅, QEMU TCG ✅, VirtualBox ✅, bare metal ✅
- [x] Commit: `"kernel: remove clac from ISR common stub -- fixes TCG and bare metal crash"`

**Test results (2026-03-29):**

| Platform | Timer | AHCI | Desktop | Status |
|----------|-------|------|---------|--------|
| QEMU WHPX | LAPIC (Hyper-V MSR) | MSI | Full | ✅ |
| QEMU TCG | PIT | MSI | Full | ✅ |
| VirtualBox | LAPIC (TSC ref) | INTx | Full (NCQ slow) | ✅ |
| Bare metal | LAPIC | MSI | Full | ✅ |

**Test checkpoint:** Table rows match serial on each platform; no triple fault after `sti` with LAPIC timer ticking; `clac` absent from `isr_common_stub` (`isr_stubs.asm`). QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 4. ACPI FADT Boot Architecture Flags
Parse the FADT `IAPC_BOOT_ARCH` and `Flags` fields to know which legacy devices exist before touching any I/O ports. This prevents crashes on platforms without PIT, PS/2 controller, or RTC.

**Files:** `src/kernel/acpi.c`, `include/kernel/acpi.h`

> [!IMPORTANT]
> **FADT `IAPC_BOOT_ARCH` bits (ACPI 6.0, Table 5-11):**
> - Bit 0: LEGACY_DEVICES -- 8042 required for keyboard/mouse
> - Bit 1: 8042 -- i8042 controller present
> - Bit 2: VGA_NOT_PRESENT -- do not probe VGA
> - Bit 3: MSI_NOT_SUPPORTED -- do not enable MSI
> - Bit 4: PCIe_ASPM -- PCIe ASPM must not be disabled
> - Bit 5: CMOS_RTC_NOT_PRESENT -- do not access CMOS RTC ports
> **Debug POST (§4):** `POST16(0xD400)` = FADT parse start, `0xD401` = IAPC_BOOT_ARCH read, `0xD402` = §4 complete.

- [x] Parse `IAPC_BOOT_ARCH` from FADT `boot_arch_flags` field (requires FADT length ≥ 113)
- [x] API: `acpi_has_8042()`, `acpi_has_cmos_rtc()`, `acpi_msi_supported()`, `acpi_has_vga()` -- all safe-default to 1 if FADT absent/short
- [x] `acpi_hw_reduced()` already exists -- logged alongside IAPC_BOOT_ARCH
- [x] Log: `IAPC_BOOT_ARCH: 8042=%d RTC=%d MSI=%d VGA=%d HW_REDUCED=%d`
- [x] Commit: `"kernel: parse ACPI FADT IAPC_BOOT_ARCH flags for legacy device detection"`

**Test checkpoint:** Boot on QEMU. Log shows IAPC_BOOT_ARCH with all flags. On bare metal, log shows actual hardware configuration. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 5. PS/2 Controller Detection and Safe Init
Gate all PS/2 keyboard and mouse I/O behind ACPI detection. Never write to ports 0x60/0x64 if the i8042 doesn't exist.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`

> [!NOTE]
> **Debug POST (§5):** `POST16(0xD500)` = PS/2 detect start, `0xD501` = i8042 check done, `0xD502` = keyboard init, `0xD503` = mouse probe, `0xD504` = §5 complete.

- [x] `keyboard_init()`: checks `acpi_has_8042()` before any port I/O -- skips if no i8042
- [x] `mouse_init()`: checks `acpi_has_8042()` + 0xFF status + reset ACK probe -- skips gracefully
- [x] Timeouts increased to 1000000 with 0xFF early-exit on `ps2_wait_input`/`ps2_wait_output`
- [x] `mouse_init()` re-enabled in `boot_interrupts.c` -- ACPI gate replaces skip workaround
- [x] Keyboard, mouse, vbox_mouse migrated to `irq_request_gsi()` when IOAPIC available
- [x] Commit: `"drivers: PS/2 keyboard/mouse gated by ACPI i8042 detection + GSI-based IRQ"`

**Test checkpoint:** Boot on laptop without PS/2 mouse. Mouse init logs "skipped" and boot continues. Boot on QEMU with PS/2 -- mouse works normally. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 6. AHCI Interrupt Hardening
Make `ahci_setup_interrupts()` safe on bare metal: mask LAPIC timer during MSI setup, verify ABAR MMIO accessibility, fall back to polled mode if MSI fails.

**Files:** `src/kernel/drivers/ahci/ahci_core.c`

> [!IMPORTANT]
> On bare metal, enabling MSI writes to PCI config space which triggers the device to send MSI messages to the LAPIC. If the LAPIC timer is also firing, the two LAPIC writes can race. Additionally, AHCI ABAR MMIO at the device's BAR5 address is accessed through WB-cached page table entries -- same issue as HPET.
> **Debug POST (§6):** `POST16(0xD600)` = AHCI harden start, `0xD601` = LAPIC masked, `0xD602` = MSI enable, `0xD603` = MSI verify, `0xD604` = LAPIC unmasked, `0xD605` = §6 done. Crash at 0xD602: MSI enable faulted device.

- [x] Mask LAPIC timer LVT before MSI enable; unmask after
- [x] Validate ABAR: page-aligned, within 4 GiB, not 0 -- logs and uses polling if invalid
- [x] MSI enable verify: read VID after enable, if 0xFFFF → device gone, free vector, fall back to INTx
- [x] MSI → INTx fallback (already existed in ahci_core.c)
- [x] INTx → polled fallback (already existed -- logs "using polling")
- [x] Bare metal skip workaround removed (done in §3 clac fix)
- [x] Commit: `"drivers: AHCI interrupt hardening -- LAPIC mask + ABAR validation"`

**Test checkpoint:** Boot on bare metal. AHCI init logs either "MSI vector 0xNN" or "INTx fallback" or "polled mode". No crash. Disk I/O works. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 7. Resilient Boot with Graceful Degradation
Wrap every Phase 1--3 subsystem init in a protective pattern: emit POST code, call init, check result, log outcome, continue on failure. Never `boot_halt()` for non-critical subsystems.

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
- [x] Commit: `"boot: resilient init with BOOT_TRY -- non-critical failures degrade, never crash"`

**Test checkpoint:** Disable a non-critical subsystem (e.g., force `rtc_init()` to fail). Boot completes. Desktop shows degraded notification. Serial log shows `[WARN] RTC: init failed -- degraded`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 8. Per-Process Page Tables (Minimal Base)

Implement the minimal per-process page table infrastructure so each task has its own PML4. Kernel pages are supervisor-only, user pages have the User bit. CR3 switches on context switch. This directly unblocks SMEP/SMAP on bare metal and eliminates the user-stacks-in-kernel-heap hack.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`, `src/kernel/sched/task.c`, `include/kernel/sched/task.h`

> [!NOTE]
> Minimal prerequisite -- full per-process VM (COW fork, mmap, demand paging) is in `03-memory-concurrency/TODO-04-advanced-virtual-memory.md`. This section implements just enough to have separate user/kernel address spaces with correct U/S bits.

> [!IMPORTANT]
> **What this changes:**
> - Each task gets its own PML4 (cloned from kernel PML4 at task creation)
> - Kernel mappings (0x0--0x7FFFFF, 0x900000+): Present + Writable, NO User bit
> - User mappings (0x800000--0x8FFFFF for ELF, plus user stack): Present + Writable + User bit
> - User stacks allocated from PMM in the user address range, NOT from `kmalloc`
> - `schedule()` / `schedule_now()`: load new task's CR3 before `iretq`
> - Boot task (PID 0): continues using the identity-mapped PML4 (kernel-only task)
> **Regression risk:** CR3 switch adds ~100ns per switch; bad PT breaks user ELF. Rollback: shared identity map, re-skip SMEP/SMAP.
> **Debug POST (§8):** `POST16(0xD800)` = PML4 create start, `0xD801` = kernel entries cloned, `0xD802` = user page mapped, `0xD803` = CR3 switch test, `0xD804` = SMEP/SMAP enable, `0xD805` = user ELF loaded in new PML4, `0xD806` = §8 complete. On crash at 0xD803: CR3 switch broke. At 0xD804: SMEP/SMAP faulted.

- [x] `vmm_create_user_pml4()` -- clones kernel PML4, splits PD[4] into 4KiB PT, User bit on PML4/PDPT/PD levels
- [x] `vmm_set_user_page(pml4, virt)` -- sets User bit on individual 4KiB pages in split PT
- [x] `vmm_destroy_user_pml4(pml4)` -- frees cloned PML4/PDPT/PD/PT (not data pages)
- [x] `task_create_user()`: creates per-process PML4, marks ELF + user stack pages as User, user stack at 0x8FC000 (in PD[4] range)
- [x] `schedule()` + `schedule_now()`: CR3 switch if next task has different PML4
- [x] `task_create()`: kernel tasks use boot PML4 (cr3=0)
- [ ] Remove `pmm_mark_region_used(USER_ELF_BASE, USER_ELF_SIZE)` (`user_range.h`) -- deferred, ELF loader still uses fixed phys address
- [ ] Remove SMEP/SMAP skip -- moved to §9 (CPU security activation)
- [x] Commit: `"mm: per-process page tables -- user/kernel separation, CR3 switch"`

**Test checkpoint:** Boot on QEMU WHPX. cmd.exe runs in user mode with its own PML4. Kernel pages don't have User bit. `KeGetCurrentIrql()` works from user-mode interrupt. Boot on bare metal: SMEP/SMAP enabled (CR4 bits set), cmd.exe runs, no page faults. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 9. CPU Security Activation and Verification

Ensure CPU security features (NX, SMEP, SMAP) are activated in the correct order and verified on bare metal. This incorporates the bare-metal-relevant parts of the CPU boot sequencing that are currently unimplemented.

**Files:** `src/kernel/cpu_security.c`, `src/kernel/main/boot_hw.c`

> [!NOTE]
> Minimal prerequisite -- full CPU boot sequencing (EFER before VMM, XSAVE/PCID, AP parity) is in `TODO-04-cpu-boot-sequencing.md §2,§4,§1`. This section implements just enough to ensure NX/SMEP/SMAP work on bare metal and verifies post-activation.

- [x] Activation order formalized: `cpu_harden()` (NX) → `vmm_apply_nx_policy()` → `cpu_harden_post_pagetable()` (SMEP/SMAP) → `cpu_verify_hardening()`
- [x] `cpu_verify_hardening()`: reads back EFER (NX), CR4 (SMEP/SMAP), logs discrepancies or EPT enforcement
- [x] Bare metal: SMEP/SMAP still skipped -- boot PML4 has User bit on all 2MiB pages (entry.asm 0x87). Need to clear User from kernel pages in boot PML4 first. Per-process PML4 has correct U/S but kernel PML4 does not.
- [x] VMs: SMEP/SMAP work on KVM/TCG/VBox (if CPUID has feature), EPT enforced on WHPX. Logged.
- [x] Debug POST codes: 0xD900-0xD904
- [x] Commit: `"boot: CPU security activation with post-enable verification"`

**Test checkpoint:** Boot on bare metal. Log shows `[OK] cpu: NX enabled, SMEP skipped (shared page tables), SMAP skipped`. On QEMU: log shows all three enabled (or EPT-enforced). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 10. Boot Order Hardening (Timer-Last, UEFI-Safe)

Formalize the boot order lessons learned: timer is the last thing initialized before `sti`, all UEFI runtime calls mask the LAPIC timer, and `boot_splash_start_animation()` only runs after `sti`.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_init.c`

- [x] Timer last before sti (already in place, documented in init order contract)
- [x] LAPIC timer masked during ALL UEFI runtime calls (rt_mask_timer/rt_unmask_timer in uefi_runtime.c)
- [x] Phase 1 init order contract documented at top of boot_interrupts.c (12-step sequence)
- [x] `BOOT_ASSERT(cond, msg)` macro added -- used for IDT/GDT checks before timer init
- [x] Commit: `"boot: formalize Phase 1 init order -- timer-last, UEFI-safe, documented contract"`

**Test checkpoint:** Phase 1 order is correct. UEFI runtime calls don't crash with timer running. Boot order comment block is visible at top of `boot_interrupts.c`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 11. ~~`boot.conf` Subsystem Skip List~~ *(removed)*

> [!NOTE]
> **Removed 2026-03-29.** A skip list encourages hiding problems instead of fixing them. Errors can go unnoticed indefinitely. The correct approach is `BOOT_TRY` (§7) -- subsystems degrade gracefully with a logged warning, not silent bypass. If a subsystem crashes, fix the root cause.

- [x] Commit: "(shipped) removed boot.conf skip list -- BOOT_TRY + degraded_mask only"

**Test checkpoint:** N/A -- `boot.conf` skip list removed 2026-03-29; use `BOOT_TRY` / `degraded_mask` (§7) for intentional subsystem failure tests instead.

## 12. Migrate Logging from X:\ to C:\ + Remove Log Partition

> **Note:** This section's migration to C:\ was a transitional step. TODO-17 (BlackBox Service Partition) restored X:\ as a 128 MiB FAT32 partition for all logs, crash dumps, and diagnostics. The runtime `klog_dir` global now resolves to `X:\Logs\` (BlackBox) with C:\ fallback.

- [x] Runtime `klog_dir` global in `klog.h` -- resolves to `X:\Logs\` (BlackBox) or `C:\Impossible\System\Logs\` (fallback)
- [x] All log paths built from `klog_dir` at runtime (superseded compile-time `KLOG_DIR` macro)
- [x] BlackBox partition (X:\) mounted by GPT name "BlackBox" in partition.c
- [x] 3-partition GPT: EFI + BlackBox + IXFS (was briefly 2-partition, now 3)
- [x] Commit: "(shipped) klog_dir BlackBox X:\\Logs + GPT BlackBox partition"
- [x] → XREF: `TODO-17-blackbox-service-partition.md` §5 -- full klog migration to X:\

**Test checkpoint:** Serial log shows `"klog: writing to X:\Logs\Serial\..."`. All subsystem logs, boot timeline, crash reports on X:\. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 13. CPU Feature Minimum Requirements and Verification

Define the minimum CPU feature set required to boot, verify features are actually enabled after activation, and provide clear diagnostics when features are missing or fail to enable.

**Files:** `src/kernel/main/boot_hw.c`, `src/kernel/cpu_security.c`, `include/kernel/cpuid.h`

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-17-kernel-security-hardening.md` -- owns the IMPLEMENTATION of NX/SMEP/SMAP/CET. This section owns VERIFICATION that features actually took effect on real hardware, and DIAGNOSIS when they don't.

- [x] Minimum: NX + SSE2 required -- boot halts with clear message if missing
- [x] Recommended: SMEP, SMAP, RDRAND -- warns if missing, continues
- [x] `cpu_verify_hardening()`: reads EFER/CR4, logs discrepancies (implemented in §9)
- [x] Known quirks documented in CLAUDE.md bare metal gotchas section
- [x] Commit: `"boot: CPU feature minimum requirements + post-activation verification"`

**Test checkpoint:** Boot on CPU without SMAP. Log shows `[WARN] cpu: SMAP not available -- skipped`. Boot continues. On CPU with SMAP: verification confirms CR4.SMAP is set. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## 14. Bare-Metal Test Matrix and Validation Plan

Define the hardware platforms to test on, expected boot timings per phase, and a regression detection framework so bare-metal issues are caught early.

**Files:** `todo/01-boot-platform/TODO-05-bare-metal-hardening.md` (this file -- Bare Metal Testing Plan + matrix tables)

- [x] Test platforms documented in BM Testing Plan section above (BM Tests 1-4 with results tables)
- [x] Platform matrix:

| Platform | Timer | Calibration | AHCI | PS/2 | Per-Process PT | Boot Time |
|----------|-------|------------|------|------|----------------|-----------|
| QEMU WHPX | LAPIC | Hyper-V MSR | MSI | Skipped (8042=0) | ✅ CR3 switch | ~10s |
| QEMU TCG | PIT | N/A | MSI | Skipped (8042=0) | ✅ CR3 switch | ~3s |
| VirtualBox | LAPIC | PM Timer | INTx | Active (8042=1) | ✅ CR3 switch | ~45s (NCQ) |
| Bare metal | LAPIC | TSC ref | MSI | Active (8042=1) | ✅ CR3 switch | ~10s |

- [x] Phase thresholds: Phase 0 <200ms ✅, Phase 1 <2s ✅, Phase 2 <10s ✅ (except VBox NCQ), Phase 3 <15s ✅
- [x] Regression policy: documented in CLAUDE.md ("Bare metal first" + validate-todo-file skill §11)
- [x] Checklist covered by BM Tests 1-4 in testing plan above
- [x] Commit: "(shipped) bare-metal test matrix inline in TODO-05"

**Test checkpoint:** Bare-metal boot on i5-11600K matches the checklist. All phases within timing thresholds. VMs: QEMU WHPX, QEMU TCG, VirtualBox match matrix rows above.

## 15. Boot Splash Spinner Bare-Metal Fix

The boot splash spinner stutters on bare metal -- stops and restarts repeatedly during Phase 2. **Historical note (pre-2026-03-29):** the LAPIC timer ISR was effectively broken on bare metal/TCG due to `clac` #UD (§3), so the spinner only advanced when the compositor or `boot_splash_tick()` explicitly called it. **After the §3 fix**, the timer ISR runs; remaining visible stutter on VirtualBox is dominated by NCQ timeout (15s I/O), not a dead timer.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/spinner.c`, `src/kernel/main/boot_storage.c`

- [x] Spinner driven by `timer_register_tick_callback(spinner_advance, 10)` -- fires at 10fps from LAPIC timer ISR
- [x] Explicit `boot_splash_tick()` calls already present during long operations (PCI, AHCI, VFS, desktop)
- [x] Timer IS working on bare metal (§3 fixed clac). Spinner callback fires correctly.
- [x] VBox stutter: caused by NCQ timeout (15s blocking I/O), not a spinner bug. Timer ISR fires during NCQ wait (`event_wait_timeout`), spinner advances. Display may lag due to VBox VGA emulation under heavy I/O -- cosmetic, not a kernel issue.
- [x] Verified: QEMU WHPX ✅, TCG ✅, bare metal ✅. VBox: minor stutter during NCQ timeout only.
- [x] No code change needed -- §3 (clac fix) resolved the root cause.

**Test checkpoint:** Bare metal: spinner rotates smoothly during PCI scan and AHCI init (no visible stutter or freeze). QEMU: spinner unchanged (already smooth). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐   | Feature                 | 🪟 Win11                   | 🐧 Linux                   | 🚀 Impossible OS               |
| --- | ----------------------- | ------------------------- | ------------------------- | ----------------------------- |
| 💎   | UC MMIO mapping         | ✅ MmMapIoSpace            | ✅ ioremap_uc              | ✅ §1 vmm_map_mmio_uc          |
| 💎   | IST stacks              | ✅ All critical exceptions | ✅ IST1-4 DF/NMI/MCE       | ✅ §2 IST1-3 DF/NMI/MCE        |
| 💎   | ACPI FADT boot arch     | ✅ HAL checks all flags    | ✅ Gates PIT/RTC/PS2       | ✅ §4 IAPC_BOOT_ARCH parsed    |
| 💎   | PS/2 ACPI detection     | ✅ HAL detects i8042       | ✅ i8042.nopnp             | ✅ §5 FADT + GSI routing       |
| 💎   | AHCI MSI fallback       | ✅ StorAHCI INTx fallback  | ✅ libahci polled fallback | ✅ §6 MSI→INTx→polled          |
| 💎   | Graceful degradation    | ✅ Safe Mode + Last Known  | ✅ systemd continues       | ✅ §7 BOOT_TRY + degraded_mask |
| 💎   | Per-process page tables | ✅ Each process own CR3    | ✅ mm_struct per task      | ✅ §8 PML4 clone + CR3 switch  |
| 💎   | CPU security verify     | ✅ HAL verifies CR4/EFER   | ✅ Checks feature enable   | ✅ §9 verify NX/SMEP/SMAP      |
| 💎   | Logging on main FS      | ✅ C:\Windows\System32     | ✅ /var/log                | ✅ §12 KLOG_DIR X:\ (T17)      |
| 💎   | CPU feature minimums    | ✅ NX required since Vista | ✅ Minimum checks at boot  | ✅ §13 NX+SSE2 required        |
| ⭐   | Bare-metal test matrix  | ❌ Internal only (WHQL)    | ❌ Community-driven        | ✅ §14 4-platform matrix       |

> **After §1--§14:** Impossible OS boots on any x86-64 hardware with the same reliability as Windows and Linux. User/kernel separation with per-process PML4; SMEP/SMAP where CPU and page tables allow (see §8--§9). Graceful degradation via `BOOT_TRY` (§7). Logging on BlackBox `X:\` (§12). External CPU sequencing remains in `TODO-04-cpu-boot-sequencing.md`.

## Bare Metal Testing Plan

> [!IMPORTANT]
> **5 bare metal checkpoints instead of 15.** Implement and verify batches on QEMU first. Only go to bare metal at critical checkpoints where VM behavior diverges from real hardware. Debug POST codes (0xD1xx--0xD9xx) make each bare metal cycle fast -- one boot, read serial/VPD, identify failure.

### BM Test 1+2 -- Foundation + Interrupts (§1--§4) ✅ PASSED 2026-03-29
> [!NOTE]
> BM Tests 1 and 2 were combined -- the `clac` fix (§3) resolved all hardware interrupt crashes simultaneously. All 4 platforms tested in one session.

- [x] UC MMIO: `vmm_map_mmio_uc: LAPIC ID match` on all platforms
- [x] IST stacks: `IST stacks: DF=... NMI=... MCE=...` allocated
- [x] FADT flags: `IAPC_BOOT_ARCH: 8042=1 RTC=1 MSI=1 VGA=1 HW_REDUCED=0`
- [x] **LAPIC timer fires on bare metal** -- root cause was `clac` #UD
- [x] PCI scan completes with timer ticks running
- [x] Desktop reached with timer active on all 4 platforms
- [x] HPET calibration: not triggered (Tier 1/1b succeed first) -- needs bare metal test without Hyper-V/TSC

**Results:**

| Platform | Timer | Calibration | AHCI | Desktop | Notes |
|----------|-------|------------|------|---------|-------|
| QEMU WHPX | LAPIC 100Hz | Tier 1 Hyper-V MSR | MSI | ✅ 8.4s | Primary dev |
| QEMU TCG | PIT 100Hz | N/A (PIT) | MSI | ✅ | Fixed by clac removal |
| VirtualBox | LAPIC 100Hz | Tier 1b TSC ref | INTx | ✅ 21.8s | NCQ timeout adds 15s |
| Bare metal | LAPIC 100Hz | Tier 1b TSC ref | MSI | ✅ | Fixed by clac removal |

### BM Test 3 -- Driver Hardening (§5 + §6 + §7) ✅ PASSED 2026-03-29
> [!NOTE]
> PS/2 gated by ACPI FADT, mouse re-enabled with proper detection, AHCI hardened with LAPIC mask + ABAR validation, BOOT_TRY macro + degraded_mask infrastructure added. Also fixed: VBox mouse click (IOAPIC unmask in irq_request_gsi), VBox spinner speed (TSC calibration sanity check), VBox driver self-contained (buttons read internally).

- [x] PS/2 keyboard/mouse: skipped on QEMU (8042=0), active on VBox/bare metal (8042=1)
- [x] AHCI: MSI on QEMU/bare metal, INTx on VBox -- no crash
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

### BM Test 4 -- Memory Model (§8 + §9) ✅ PASSED 2026-03-29
> [!NOTE]
> Per-process page tables working on all 3 VM platforms. cmd.exe runs in its own PML4 with User bit on ELF + stack pages. CR3 switches on context switch. SMEP/SMAP still skipped (boot PML4 has User on all 2MiB pages -- needs kernel PML4 fix).

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

### BM Test 5 -- Final Validation (after §11--§15)
Full acceptance pass. All sections complete.

- [ ] Full boot to desktop, timer running, no workarounds
- [ ] Force a non-critical init failure via `BOOT_TRY` path (§7); boot completes with degraded notification (no `boot.conf` skip list)
- [ ] Second boot: serial shows `[BOOT] Last boot succeeded` after a clean prior shutdown (`boot_hw.c` NVRAM path)
- [ ] Spinner smooth during PCI scan (no stutter)
- [ ] Boot time within thresholds (Phase 0 <200ms, Phase 1 <2s)
- [ ] Keyboard responsive, AHCI I/O works

**~20 minutes.** Full regression pass.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bare_metal()` (see [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) and `include/kernel/test/test.h`; same pattern as `02-kernel-core/TODO-04-peb-teb-user-abi.md` Unit Tests).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> Bare-metal hardening is primarily verified by multi-platform boot (QEMU WHPX/TCG, VBox, bare metal). Kernel unit tests cover the infrastructure APIs; full validation requires `scripts/test-smoke.sh` on each platform.
> `g_boot_info.degraded_mask` / `kernel_subsystem_apply_result` paths already have assertions in [`src/kernel/test/test_boot_init.c`](../../src/kernel/test/test_boot_init.c) -- extend `test_bare_metal.c` with MMIO/IST/ACPI APIs below; do not duplicate those mask tests unless a gap is found.

- [ ] Create `src/kernel/test/test_bare_metal.c` with:
  - `vmm_map_mmio_uc(0xFEE00000, 0x1000)` returns non-NULL; LAPIC ID read via returned pointer matches identity-mapped read
  - `vmm_unmap_mmio()` on the mapped region does not crash; subsequent access would fault (not tested, just unmap)
  - IST stacks allocated: `kernel_tss.ist1 != 0`, `kernel_tss.ist2 != 0`, `kernel_tss.ist3 != 0`
  - ACPI FADT flags: `acpi_has_8042()` returns 0 or 1; `acpi_has_cmos_rtc()` returns 0 or 1 (never crashes)
  - `acpi_msi_supported()` returns 0 or 1 (consistent with FADT)
  - `g_boot_info.degraded_mask == 0` on a clean boot (no subsystems failed)
  - Per-process PML4: `vmm_create_user_pml4()` returns non-NULL; `vmm_destroy_user_pml4()` frees without crash
  - `vmm_set_user_page()` on a valid PML4+virt succeeds (User bit is set in PTE)
  - CPU verification: `cpu_verify_hardening()` does not crash; logs NX/SMEP/SMAP status
  - `klog_dir` runtime global resolves to `"X:\\Logs\\"` (BlackBox) or `"C:\\Impossible\\System\\Logs\\"` (fallback)
- [ ] Add to `scripts/test-smoke.sh`:
  - Grep serial for `IAPC_BOOT_ARCH:` (FADT flags parsed)
  - Grep serial for `IST stacks:` (IST allocated)
  - Grep serial for `NX enabled` (CPU hardening ran)
  - Boot completes to desktop on QEMU TCG (PIT path) and QEMU WHPX (LAPIC path)
- [ ] Register in `test_runner_init()`: `test_register_bare_metal()`
- [ ] Commit: `"test: add bare_metal test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes after `test_bare_metal.c` and `test_register_bare_metal()` land; smoke script greps succeed on reference QEMU boot. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU WHPX: full boot to desktop, all subsystems OK, POST codes visible in serial log
- [ ] QEMU TCG: full boot to desktop with PIT timer path
- [ ] VirtualBox: full boot to desktop
- [ ] Bare metal (i5-11600K): BM Tests 1--5 all pass
- [x] No `HV_BAR()` calls remaining in codebase -- `hv_bar.h` deleted, replaced by TODO-08 VPD
- [ ] Commit: `"boot: bare metal hardening complete -- all platforms boot reliably"`

**Test checkpoint:** All Verification bullets pass on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal; BM Test 5 items complete; `tail -1 build/build.log` shows `=== BUILD OK ===`.

**Test runner:** `scripts\debug\run-boot-tests.bat` (SUITE=boot)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-11 | validate | validate-todo-file (pass 2): §1/§2/§6 Debug POST in callouts; §6/§5 checkpoints + platforms; BM Test 5 skip bullet replaced; Unit Tests **Test checkpoint**; Verification `---` before History; runner path fix. Pass 1: en-dash to ASCII; VMM XREF §1 to §11; Debug POST folded into callouts so Commit precedes final Test checkpoint; OS table header + dedupe Graceful row; post-OS summary matches skip-list removal; §11/§12/§14 Commit; Verification test runner; Inputs `boot_hw.c` path still via markdown links. Flags: §1 `hpet_read_ns` deferred; §8 two open items; BM Test 5 open; `test_bare_metal.c` not in tree; `### BM Test` headings remain (named tests, not N.M). |
| 2026-04-11 | validate | validate-todo-file (pass 3): §1/§2/§6 Debug POST in NOTE/IMPORTANT; Outcome + Impl Order footnote match BOOT_TRY; §1/§4/§5/§7/§9/§10/§12/§13/§14/§15 Test checkpoint platform lists; OS table rebuilt aligned; §11 shipped Commit line; TODO-02 XREF arrow; §3 IMPORTANT empty `>` lines removed; Unit Tests intro drops missing TODO-03 path, points at test_runner.c. |
| 2026-04-11 | gap-analysis | gap-analysis-todo: Current state callout; Origin block marked historical + §3 fix; Outcome NVRAM/VPD nuance; §15 stale timer text fixed; Unit Tests note `test_boot_init` degraded_mask overlap; Inputs + `boot_init.h`; TODO-06 §6/§7 XREF TODO-05 §1 (was §3). Research: 6 web + 2 docs fetched (Linux kernel stacks doc; MmMapIoSpaceEx Learn). No new `##` sections; OS table unchanged. |
| 2026-04-11 | validate | validate-todo-file (pass 4): §1 deferral XREF TODO-06 §6 (not §2); Inputs TODO-06 blurb; §3 **Test checkpoint**; §4 IMPORTANT blank `>` removed; §8 `USER_ELF_*`; BM Test 5 NVRAM bullet matches `[BOOT] Last boot succeeded`; History chronological; TODO-06 XREF §3 + parity paragraph. Flags: `### BM Test`; Impl Order `[x]` vs open Verification/BM5. |
| 2026-04-12 | validate | TODO-08 validate-todo-file: Inputs XREF to TODO-08 §1 §2 split from old `§1-§2` line; added back-XREF from TODO-08 §4 Tier 1 VPD to this file §3 (page flip / bare metal). |
