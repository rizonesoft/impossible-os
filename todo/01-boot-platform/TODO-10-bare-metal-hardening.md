---
schema_version: 1
id: bare-metal-hardening
domain: 01-boot-platform
status: active
title: "TODO-10 -- Bare Metal Boot Hardening"
---

# TODO-10 -- Bare Metal Boot Hardening

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Make the kernel boot reliably on any x86-64 bare-metal hardware. After this TODO, the boot sequence is robust against absent hardware, misconfigured firmware, and platform-specific quirks through correct gating, fallback behavior, and bare-metal-first validation. Rich POST/VPD diagnostics stay owned by the dedicated diagnostics TODOs referenced below.

> [!IMPORTANT]
> **Current state (2026-04-11):** `clac` is removed from `isr_common_stub` (`isr_stubs.asm`). UC MMIO uses `vmm_map_mmio_uc` / `vmm_unmap_mmio` (`vmm.c`). IST stacks and TSS fields are allocated in `gdt.c` with IDT IST wiring in `idt.c`. FADT `IAPC_BOOT_ARCH` gates legacy devices (`acpi.c`). AHCI MSI setup masks the LAPIC timer around PCI MSI writes (`ahci_core.c`). `BOOT_TRY` + `g_boot_info.degraded_mask` live in `boot_init.h` with Phase 1--3 call sites. Per-process PML4 + CR3 switch live in `vmm.c` / `task.c`. Early boot reads prior POST from UEFI NVRAM and logs last-boot outcome (`boot_hw.c`, `boot_init.c`). **Still open:** §1 `hpet_read_ns()` (owner TODO-11 §1 UTS), §7 deliberate kernel stack-overflow BSOD test, §4 `pmm_mark_region_used` cleanup for fixed ELF phys + boot PML4 User-bit blocker for SMEP/SMAP on bare metal, `src/kernel/test/test_bare_metal.c` + `test_register_bare_metal()`, BM Test 5, Verification checklist. Dense per-function POST16 instrumentation beyond the `0xD1xx` bring-up set is owned by `TODO-15-visual-post-display.md`. **Gap-audit 2026-06-06 demotions:** §4 -> `[/]` (ACPI capability APIs ignore `acpi_hw_reduced()`; hw-reduced platforms wrongly report legacy devices present), §8 -> `[/]` (per-process PML4 ships, but "directly unblocks SMEP/SMAP" was false -- the boot PML4 still has User on kernel pages; real unblock owned by `D02 T10 §3-§6` KPTI). **Newly named gaps:** nested-NMI safety on shared IST2 (blocking prereq for `TODO-23 §1` watchdog), and corrected/recoverable machine-check (RAS/WHEA) recovery -- currently every MCE panics; no owner exists, a dedicated RAS TODO is recommended.

> [!IMPORTANT]
> **Origin (2026-03-28) -- historical context:** A full day of bare-metal debugging on an i5-11600K laptop exposed fundamental gaps (SMEP page tables, HPET WB MMIO, GS_BASE clobbered by GDT reload, TSS RSP0 uninitialized, heartbeat ISR reentrancy, RTC hang, calibration timeouts). The **original mystery symptom** was hardware interrupts (LAPIC timer / PIT) faulting on bare metal while software `INT 0x81` through the same ISR path worked -- **root cause fixed 2026-03-29** (`clac` #UD in `isr_common_stub`, §3). Workarounds listed here (mouse/AHCI/timer disabled) were removed as the real fixes landed; keep this block so future readers know why the TODO exists.

> [!NOTE]
> **Design principle:** Every subsystem init must be independently skippable. If `mouse_init()` crashes, boot continues without a mouse. If AHCI MSI fails, fall back to polled I/O. If HPET MMIO faults, skip to PM Timer. The boot sequence must be **unbreakable** -- degrade gracefully, never crash.

> [!CAUTION]
> **Scope boundary -- this TODO does NOT own:**
> - CPU security feature implementation (NX, SMEP, SMAP, CET, Spectre) → owned by `TODO-10-kernel-security-hardening.md`
> - CPU activation sequencing (EFER before VMM, CR4 order) → owned by `TODO-09-cpu-boot-sequencing.md`
> - Timer HAL architecture and calibration waterfall design → owned by `TODO-11-interrupt-timer-arch.md`
> - Visual boot progress display (VPD) → owned by `TODO-15-visual-post-display.md`
> - Panic forensic evidence struct and cross-boot persistence → owned by `TODO-14-boot-diagnostics.md §6`
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
- -> XREF: `02-kernel-core/TODO-20-eif-full-implementation.md §9` -- EIF per-process dispatch-table isolation consumes the per-process PML4 base from §8
- -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md §6` -- higher-half kernel relocation builds the kernel-high/user-low shared-PML4 split on the §8 per-process page-table base
- [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h) -- `BOOT_TRY`, `g_boot_info.degraded_mask`, subsystem IDs
- → XREF: `TODO-14-boot-diagnostics.md §6` -- panic forensic evidence struct (deferred; this TODO validates it works on bare metal when implemented)
- → XREF: `TODO-11-interrupt-timer-arch.md` -- UTS / LAPIC calibration / `hpet_read_ns()` owner; bare-metal ISR path fixed in this file §5 (`clac` removal)
- → XREF: `TODO-09-cpu-boot-sequencing.md §2,§5` -- CPU hardening activation order (deferred there; minimal version in §10 here)
- → XREF: `TODO-15-visual-post-display.md` §1 §4 -- 4-digit POST16 system and per-function instrumentation (moved from this TODO to TODO-15)
- → XREF: `TODO-15-visual-post-display.md` §7 -- Tier 1 VPD raw VRAM; follows bare-metal interrupt and page-flip constraints in this file §6
- → XREF: `TODO-24-blackbox-service-partition.md` §5-§8 -- authoritative owner for log, boot timeline, crash, perf, and diagnostic storage paths
- → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md §2` (SMEP/SMAP CR4 activation) + `§3-§6` (KPTI clean kernel PML4 -- the actual unblock for bare-metal SMEP/SMAP, since the boot PML4 has User on all kernel 2 MiB pages) -- this TODO does NOT reimplement; `§8`/`§9` here observe + verify the bare-metal shared-page-table quirk once that owner clears kernel U/S bits
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md §1` -- boot_progress() infrastructure (this TODO consumes it)
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §11` -- full `vmm_map_mmio()` / UC MMIO; this TODO §1 keeps minimal `vmm_map_mmio_uc()` only
- → XREF: `TODO-03-bootloader-error-recovery.md §6` -- serial port probe and COM2 fallback; §8 here provides graceful degradation when serial is absent, §6 there detects serial presence in the bootloader
- → XREF: `TODO-07-boot-entry-store-menu-policy.md §5` -- graceful-degradation flags from §7 here map to safe-mode entry flags in the boot entry store

## Outcome

- Boot path emits POST16 codes to I/O port 0x80, UEFI NVRAM (`boot_post_nvram_write16`), and serial; on-screen 4-digit VPD coverage is coordinated with `TODO-15-visual-post-display.md` / `TODO-14-boot-diagnostics.md` (not every subsystem is VPD-instrumented yet).
- Next boot: early serial shows last-boot outcome (`[BOOT] Last boot succeeded` / `failed` in `boot_hw.c`) plus prior POST16 read from NVRAM -- full WinRE-style recovery UI is `TODO-03` / `TODO-22`, not this file.
- Hardware interrupts work reliably on bare metal -- IST stacks, correct LAPIC delivery, verified ISR frame layout.
- PS/2, RTC, AHCI, and all legacy subsystems are gated by ACPI capability flags -- never touch hardware that doesn't exist.
- Any subsystem failure degrades gracefully with a log message instead of crashing.
- Non-critical failures use `BOOT_TRY` (§7) plus `degraded_mask` -- no silent `boot.conf` skip list.

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On | Status |
| --- | :---: | -------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Minimal UC MMIO mapping (`vmm_map_mmio_uc`)        | --         |  [x]   |
| 💎  |   2   | IST stacks for critical exceptions                 | --         |  [/]   |
| 💎  |   3   | Hardware interrupt root cause investigation        | §2         |  [x]   |
| 💎  |   4   | ACPI FADT boot architecture flags                  | --         |  [x]   |
| 💎  |   5   | PS/2 controller detection and safe init            | §4         |  [x]   |
| 💎  |   6   | AHCI interrupt hardening                           | §1, §3     |  [x]   |
| 💎  |   7   | Resilient boot with graceful degradation           | --         |  [/]   |
| 💎  |   8   | Per-process page tables (minimal base)             | §1         |  [/]   |
| 💎  |   9   | CPU security activation and verification           | §4, §8     |  [x]   |
| 💎  |  10   | Boot order hardening (timer-last, UEFI-safe)       | §3         |  [x]   |
| ⭐  |  11   | ~~`boot.conf` subsystem skip list~~                | --         |  [x]   |
| 💎  |  12   | Logging and diagnostic storage moved to TODO-24    | §7         |  [x]   |
| 💎  |  13   | CPU feature minimum requirements and verification  | §4, §9     |  [x]   |
| 💎  |  14   | Bare-metal test matrix and validation plan         | §3         |  [x]   |
| 💎  |  15   | Boot splash spinner bare-metal fix                 | §3, §10    |  [x]   |
| 💎  |  16   | Post-ship follow-up backfill (2026-07-31 cohort)   | --         |  [x]   |
| 💎  |  17   | Emergency-writer robustness residuals (§16 review) | §16        |  [x]   |
| 💎  |  18   | Panic-path cross-CPU ownership residuals           | §16, §17   |  [ ]   |
| 💎  |  19   | Panic-path caller-string snapshot (§17 review)     | §17        |  [ ]   |

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
- [/] `hpet_read_ns()` -- deferred to TODO-11 §1 (UTS / HPET standalone driver); HPET calibration works without it
- [x] Commit: `"mm: minimal vmm_map_mmio_uc + HPET re-enabled with UC mapping"`

**Test checkpoint:** QEMU: HPET calibration succeeds (`Tier 2: HPET calibration -> N ticks/ms`). Bare metal: HPET mapped via UC, no MCE, calibration succeeds. `hpet_read_ns()` is deferred (TODO-11 §1); when implemented, returns monotonic ns. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | 88 kernel suites, 0 failures

> **Notes:**
> - Shipped `vmm_map_mmio_uc()` / `vmm_map_mmio_wc()` / `vmm_unmap_mmio()` in `vmm.c`: 4 KiB PTEs, UC via PCD+PWT+NX (PAT index 3), bump allocator over a dedicated kernel VA window.
> - Consumers: LAPIC UC self-test (`boot_hw.c`), HPET calibration (`lapic.c` `cal_try_hpet`), and post-boot NVMe/xHCI/HPET/framebuffer BAR mappings.
> - Review hardening (2026-06-07): one `mmio_map_range()` helper with `s_mmio_lock` on VA reservation, uint64 page math, mid-loop rollback, MMIO window moved to 9-10 GiB (disjoint from mmap + PE-image windows).
> - Full `vmm_map_mmio()` (cache-type selection, central VA allocator, VA reclaim) is owned downstream by `D03 T01 §11`.
> - Scope boundary: `hpet_read_ns()` monotonic-time wrapper deferred to `D01 T11 §1` (UTS / HPET standalone driver).
> **Verified:** 2026-06-07 | commit `bc4a72b8` | 5/6 items | build OK | mm 88/88 PASS
> **Accepted:** [M] MMIO bump allocator never reclaims unmapped VA + windows hand-picked disjoint (no central kernel VA allocator) -> XREF: 03-memory-concurrency/TODO-01 §11 (item: "Implement `vmm_map_mmio(phys_base, size)` ... central kernel VA allocator with reserved non-overlapping ranges")
> **Accepted:** [H] `get_or_create_table()` unlocked -- concurrent post-SMP MMIO maps into a fresh `kernel_pml4` subtree race (pre-existing VMM-wide; original §1 held no map lock at all) -> XREF: 03-memory-concurrency/TODO-01 §11 (item: "VMM-wide kernel page-table-creation lock")
> **Accepted:** [H] `pe_load()` accepts file-controlled `ImageBase` that can overlap the MMIO window (pre-existing VA-layout gap) -> XREF: 03-memory-concurrency/TODO-01 §11 (item: "PE loader rejects reserved kernel VA ranges")
> **Quality reviewed:** 2026-06-07 | Codex 7x (adversarial, consistency, perf, re-adversarial) | 6H+2M fixed, 2H+1M accepted-XREF | scope: kernel-code-quality

---

## 2. IST Stacks for Critical Exceptions

Allocate dedicated interrupt stacks for Double Fault (#DF), NMI, and Machine Check Exception (MCE). Without IST, a stack overflow during an exception handler causes a triple fault and silent reboot -- the most common "mystery crash" on bare metal.

**Files:** `src/kernel/gdt.c`, `src/kernel/idt.c`, `include/kernel/gdt.h`

> [!IMPORTANT]
> Linux uses IST1 for #DF, IST2 for NMI, IST3 for MCE. Windows uses separate stacks for the same exceptions via task gates (32-bit) or IST (64-bit). Both guarantee that these critical exceptions can always execute even when the kernel stack is corrupted.
> **Debug POST (§2):** `POST16(0xD200)` = IST alloc start, `0xD201` = TSS IST fields, `0xD202` = IDT IST vectors, `0xD203` = §2 done.

> [!WARNING]
> **Nested-NMI gap:** the shared IST2 NMI stack does NOT nest safely -- a second NMI arriving after the first unblocks (post-`iret`) reuses IST2 and corrupts the prior frame. Linux carries an explicit `repeat_nmi`/`nested_nmi` latch-and-replay path (`arch/x86/entry/entry_64.S`). Today only fatal NMIs (panic_screen) run, so the path is dormant -- but `TODO-23 §1` (LAPIC-NMI boot watchdog) makes it live. Per-CPU nested-NMI latch/replay (or drop) semantics must land before that watchdog ships. → XREF: `TODO-23 §1`.

- [x] Allocate 3 IST stacks (`IST_STACK_PAGES`=2, 8 KiB usable + 4 KiB guard page each) from PMM during `gdt_init()` via `ist_alloc()`; identity-mapped, `boot_halt` on alloc/guard failure
- [x] Set `kernel_tss.ist1` = DF stack top, `kernel_tss.ist2` = NMI stack top, `kernel_tss.ist3` = MCE stack top
- [x] Update IDT entries: vector 8 (#DF) → IST=1, vector 2 (NMI) → IST=2, vector 18 (MCE) → IST=3
- [x] NMI/MCE/#DF handlers: existing `panic_screen()` provides register dump, BSOD, NVRAM write, halt -- no separate handler needed for the fatal-fault path (corrected/recoverable MCE recovery is a separate RAS scope, not yet owned -- see gap report)
- [/] Verify: stack overflow in kernel → #DF fires on IST1 stack → shows BSOD instead of triple fault *(deferred to BM Test 1)*
- [x] Commit: `"kernel: IST stacks for #DF, NMI, MCE -- no more silent triple faults"`

**Test checkpoint:** Intentionally overflow the kernel stack (recursive function). Verify #DF handler fires and shows a BSOD with register dump instead of a silent reboot. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1723 kernel suites, 0 failures

> **Notes:**
> - Shipped: IST1/2/3 for #DF/NMI/MCE allocated in `gdt.c` `gdt_init()` via `ist_alloc()` (8 KiB usable + guard page each); IDT IST indices wired in `idt.c` (vec 8->1, 2->2, 18->3).
> - Runs: #DF/NMI/MCE with no registered handler fall through to `panic_screen()` (BSOD + NVRAM + halt); a stack overflow lands on the fresh IST stack, and overflowing the IST itself hits the labeled guard page.
> - Review hardening (2026-06-07): `boot_halt` on IST alloc/guard failure (was a silent NULL-IST arm -> triple fault), IST size 4->8 KiB (panic path puts a 2 KiB buffer on stack), and the 3 inline allocs consolidated into `ist_alloc()`.
> - Scope boundary: BSP TSS only -- per-CPU TSS/IST for APs is owned by `D01 T09 §10`; corrected/recoverable MCE (RAS) recovery is unowned (recommended new TODO).
> - Test gap: IST-allocated and stack-overflow-#DF assertions are deferred to `test_bare_metal.c` (Unit Tests section) + BM Test 1.
> **Verified:** 2026-06-07 | commit `2d9bfbf3` | 4/6 items | build OK | boot 1723 PASS
> **Accepted:** [Critical] AP per-CPU TSS/IST not configured -- all CPUs share one `kernel_tss`/IST1-3 (no AP `ltr`), so AP #DF/NMI/MCE IST delivery is not SMP-safe -> XREF: 01-boot-platform/TODO-09 §10 (item: "Per-CPU TSS + IST")
> **Quality reviewed:** 2026-06-07 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M+1L fixed, 1Crit accepted-XREF | scope: kernel-code-quality

---

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

| Platform   | Timer               | AHCI | Desktop         | Status |
| ---------- | ------------------- | ---- | --------------- | ------ |
| QEMU WHPX  | LAPIC (Hyper-V MSR) | MSI  | Full            | ✅     |
| QEMU TCG   | PIT                 | MSI  | Full            | ✅     |
| VirtualBox | LAPIC (TSC ref)     | INTx | Full (NCQ slow) | ✅     |
| Bare metal | LAPIC               | MSI  | Full            | ✅     |

**Test checkpoint:** Table rows match serial on each platform; no triple fault after `sti` with LAPIC timer ticking; `clac` absent from `isr_common_stub` (`isr_stubs.asm`). QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1723 kernel suites, 0 failures

> **Notes:**
> - Shipped: removed the unconditional `clac` from `isr_common_stub` (`isr_stubs.asm:23`) -- it was the only CPUID-gated opcode in the ISR entry path; nothing else (`stac`/`xsave`/`fsgsbase`/AVX/`rdrand`) lives there.
> - Root cause: `clac` ran before any `CPUID.SMAP` check; on TCG/VBox (no SMAP) it raised #UD, the #UD handler re-entered `isr_common_stub` -> `clac` -> #UD -> triple fault.
> - Downstream: the bare-metal timer skip (`timer.c`) and AHCI skip workarounds were removed once the real ISR-path fix landed; hardware interrupts work on all 4 platforms.
> - Scope boundary: re-adding `clac` (CPUID-gated / alternatives-patched, not an unconditional opcode) when SMAP is actually enabled is owned by `D02 T10 §2`.
> - Canonical: `isr_stubs.asm:23` NOTE + the `D02 T10 §2` re-add item.
> **Verified:** 2026-06-07 | commit `93f997c6` | 4/4 items | build OK | boot 1723 PASS
> **Quality reviewed:** 2026-06-07 | Codex 3x (adversarial, consistency, perf) | 0 findings (re-adversarial skipped: confirmation-only, no code change) | scope: kernel-code-quality

---

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
- [x] Hardware-reduced override: `acpi_has_8042()`/`acpi_has_cmos_rtc()`/`acpi_has_vga()` return 0 when `acpi_hw_reduced()` is set; FADT-absent/short keeps default-present for legacy PCs (FADT `flags`/`pm_timer_block` reads now length-guarded)
- [x] Commit: `"kernel: parse ACPI FADT IAPC_BOOT_ARCH flags for legacy device detection"`

**Test checkpoint:** Boot on QEMU. Log shows IAPC_BOOT_ARCH with all flags. On bare metal, log shows actual hardware configuration. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1723 kernel suites, 0 failures

> **Notes:**
> - Shipped: FADT `IAPC_BOOT_ARCH` parse (8042/RTC/MSI/VGA) + `acpi_has_8042/cmos_rtc/vga/msi_supported` + `acpi_hw_reduced` in `acpi.c`, gated behind `header.length` checks since the field is firmware-supplied.
> - Integrates: `keyboard_init`/`mouse_init` (§5) and LAPIC PIT-calibration gate on these before touching legacy 0x60/0x64/CMOS ports.
> - Review hardening (2026-06-07): hw-reduced override; FADT `flags`/`pm_timer_block` reads + the `acpi_init` parse floor + the `acpi_reboot` reset-reg all length-guard vs short-table overread; `rtc_init` gated on `acpi_has_cmos_rtc()`.
> - Scope boundary: FADT-absent/short keeps default-present so legacy PCs still probe; the §5 driver-side `0xFF`/reset-ACK probe is the access guard. MSI is intentionally NOT hw-reduced-overridden.
> - Test gap: short-FADT-length fixtures (112/113/115/116) need a `fadt_ptr` injection hook -> `test_bare_metal.c` (Unit Tests section).
> **Verified:** 2026-06-07 | commit `b959ad2a` | 5/5 items | build OK | boot 1723 PASS, smoke PASS (KVM 2.5s)
> **Resolved:** [H] runtime CMOS consumers (wall_clock RTC fallback, klog_disk, compositor clock, rtc_get_*) no longer touch 0x70/0x71 on hw-reduced -- 02-kernel-core/TODO-08 §5 added the `cmos_read()` hard gate + fail-closed `s_rtc_available` latch (2026-06-27), so the single port-I/O site refuses access when absent and every consumer is covered.
> **Quality reviewed:** 2026-06-07 | Codex 7x (adversarial, adversarial-impl, consistency, perf, re-adversarial) | 4H+1M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 5. PS/2 Controller Detection and Safe Init
Gate all PS/2 keyboard and mouse I/O behind ACPI detection. Never write to ports 0x60/0x64 if the i8042 doesn't exist.

**Files:** `src/kernel/drivers/keyboard.c`, `src/kernel/drivers/mouse.c`

> [!NOTE]
> **Debug POST (§5):** `POST16(0xD500)` = PS/2 detect start, `0xD501` = i8042 check done, `0xD502` = keyboard init, `0xD503` = mouse probe, `0xD504` = §5 complete.

- [x] `keyboard_init()`/`mouse_init()`: `acpi_hw_reduced()` hard-skip, then an unconditional `inb(0x64)==0xFF` probe before any 0x60/0x64 I/O (FADT 8042 bit is advisory only -- unreliable on WHPX)
- [x] `mouse_init()`: reset->0xFA ACK->BAT 0xAA->defaults/sample/res/F4 sequence; IRQ12 enabled only after the device is confirmed (aborted init never leaves IRQ12 on)
- [x] PS/2 waits are bounded (`PS2_WAIT_SHORT`/`PS2_WAIT_LONG`, 1024-iter flush) with `0xFF` floating-bus early-exit
- [x] `mouse_init()`/`vbox_mouse_init()` run from `boot_storage.c` deferred-input init -- ACPI gate replaces the old skip workaround
- [x] Keyboard, mouse, vbox_mouse use `irq_request_gsi()` when IOAPIC available; PIC fallback unmasks the line (`pic_unmask_irq`)
- [x] Commit: `"drivers: PS/2 keyboard/mouse gated by ACPI i8042 detection + GSI-based IRQ"`

**Test checkpoint:** Boot on laptop without PS/2 mouse. Mouse init logs "skipped" and boot continues. Boot on QEMU with PS/2 -- mouse works normally. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1723 kernel suites, 0 failures

> **Notes:**
> - Shipped: `keyboard_init`/`mouse_init`/`vbox_mouse_init` gate PS/2 access on ACPI + a direct port-0x64 probe; IRQ routing via `irq_request_gsi()` (IOAPIC) with a `pic_unmask_irq` PIC fallback.
> - Integrates: run from `boot_storage.c` deferred-input init; IRQ callbacks unchanged; bounded waits (`PS2_WAIT_*`, 1024-iter flush) with 0xFF floating-bus early-exit.
> - Review hardening (2026-06-07): `acpi_hw_reduced()` hard-skip + unconditional 0xFF probe (never write an i8042-less box); mouse IRQ12 deferred until the device ACKs (cached-config write, no read-after-reporting); vbox PIC fallback now unmasks.
> - Scope boundary: resolves the §4-deferred hw-reduced PS/2 hard-skip; this owns the i8042 path only.
> - Test gap: PS/2 init is live-port I/O (no pure unit-test surface) -- validated by boot suite + smoke + multi-platform boot.
> **Verified:** 2026-06-07 | commit `ea51dc78` | 5/5 items | build OK | boot 1723 PASS, smoke PASS (KVM 2.7s)
> **Quality reviewed:** 2026-06-07 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+3M fixed | scope: kernel-code-quality

---

## 6. AHCI Interrupt Hardening
Make `ahci_setup_interrupts()` safe on bare metal: mask LAPIC timer during MSI setup, verify ABAR MMIO accessibility, fall back to polled mode if MSI fails.

**Files:** `src/kernel/drivers/ahci/ahci_core.c`

> [!IMPORTANT]
> On bare metal, enabling MSI writes to PCI config space which triggers the device to send MSI messages to the LAPIC. If the LAPIC timer is also firing, the two LAPIC writes can race (the LVT-timer mask closes this). AHCI ABAR MMIO at the device's BAR5 is now mapped UC via `vmm_map_mmio_uc()` (§1) -- accessing it through the boot WB identity map caused stale reads / MCE, same hazard as HPET.
> **Observability:** `ahci_setup_interrupts()` runs in Phase 2 with `klog` fully up (logs "MSI vector 0xNN" / "INTx IRQ" / "using polling" and "ABAR phys 0xX UC-mapped at 0xY"), so it uses klog rather than POST16.

- [x] Mask LAPIC timer LVT before MSI enable; unmask after
- [x] Validate ABAR: page-aligned, within 4 GiB, not 0 -- logs and uses polling if invalid
- [x] MSI enable verify: read VID after enable, if 0xFFFF → device gone, free vector, fall back to INTx
- [x] MSI → INTx fallback (already existed in ahci_core.c)
- [x] INTx → polled fallback (already existed -- logs "using polling")
- [x] Bare metal skip workaround removed (done in §3 clac fix)
- [x] Commit: `"drivers: AHCI interrupt hardening -- LAPIC mask + ABAR validation"`

**Test checkpoint:** Boot on bare metal. AHCI init logs either "MSI vector 0xNN" or "INTx fallback" or "polled mode". No crash. Disk I/O works. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1723 kernel suites, 0 failures

> **Notes:**
> - Shipped: `ahci_setup_interrupts()` -- LAPIC-timer mask during MSI program, MSI->INTx(IOAPIC)->polled fallback chain, device-present re-verify after MSI enable.
> - Runs in Phase 2 with klog outcomes; ABAR mapped UC via `vmm_map_mmio_uc()` (§1) so HBA/port registers are never read through the WB identity map.
> - Review hardening (2026-06-07): ABAR now genuinely UC (old `ahci_map_mmio` left it WB; deleted); MSI dest = real BSP `lapic_id()` (was hardcoded 0); INTx fallback now `ioapic_unmask_irq`; both PCI cap walks TTL-bounded.
> - Scope boundary: consumes §1 `vmm_map_mmio_uc`; AHCI data-path (NCQ/rw) owned by the storage driver, not this section.
> - Test gap: AHCI is live PCI/MMIO (no pure unit-test surface) -- validated by smoke (log shows "ABAR phys 0xX UC-mapped" + NCQ) + boot suite.
> **Verified:** 2026-06-07 | commit `417a1294` | 6/6 items | build OK | boot 1723 PASS, smoke PASS (KVM 2.5s)
> **Quality reviewed:** 2026-06-07 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1Crit+3H fixed | scope: kernel-code-quality

---

## 7. Resilient Boot with Graceful Degradation
Wrap every Phase 1--3 subsystem init in a protective pattern: emit POST code, call init, check result, log outcome, continue on failure. Never `boot_halt()` for non-critical subsystems.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_storage.c`, `src/kernel/main/boot_desktop.c`

> [!IMPORTANT]
> **Critical subsystems (BOOT_FATAL if fail):** PMM, VMM, Heap, GDT, IDT, VFS.
> **Non-critical (BOOT_DEGRADED if fail):** RTC, keyboard, mouse, NIC, AHCI, SMBIOS, splash, registry, SMP, DHCP.
> A degraded boot reaches the desktop with reduced functionality. The user sees a notification listing what failed.

- [x] `BOOT_TRY(subsys, fn_call, name)` macro: delegates ready/degraded_mask to `kernel_subsystem_apply_result()` (single source of truth, bounds-checked), logs warning on any non-OK, continues
- [x] `g_boot_info.degraded_mask` (32-bit) tracks which subsystems failed; `_Static_assert(SUBSYS_COUNT <= 32)` guards the `1u<<subsys` width
- [x] Classification: critical subsystems `boot_halt()` (PMM, VMM, GDT, IDT, VFS); the wired non-critical paths (UEFI vars/time, SecureBoot, TPM, XSAVE, PCID) use `apply_result` -> degraded_mask
- [x] Phase 3 desktop: logs degraded subsystem list (bounded by `SUBSYS_COUNT`, names from single source)
- [x] Bare-metal skip workarounds already removed in §3 (clac fix) and §5 (ACPI gate)
- [ ] Add `SUBSYS_*` slots + `s_subsys_names[]` for the non-critical subsystems the callout lists but the enum lacks (keyboard, mouse, AHCI, NIC, SMBIOS, splash, DHCP) so degraded_mask can represent them
- [ ] Wire Phase 1-3 non-critical inits through `apply_result`: `rtc/keyboard/mouse/fb_init` are void and `ahci_init` return is ignored, so failures never set degraded_mask -- make them return `boot_result_t` and wrap
- [ ] Forced-failure regression: force one non-critical init to fail -> boot reaches Phase 3 -> degraded summary lists it (the Test checkpoint below is non-functional until then)
- [ ] Harden boot_phase0 handoff (Codex §2 review, defense-in-depth): pin pointer to canonical `BOOT_INFO_PHYS_ADDR`; validate `g_boot_info.fb` geometry before pre-`fb_init` VRAM writes; bulk-copy `boot_info`. -> XREF: 02-kernel-core/TODO-01 §2
- [x] Commit: `"boot: resilient init with BOOT_TRY -- non-critical failures degrade, never crash"`

**Test checkpoint:** Disable a non-critical subsystem (e.g., force `rtc_init()` to fail). Boot completes. Desktop shows degraded notification. Serial log shows `[WARN] RTC: init failed -- degraded`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 1723 kernel suites, 0 failures

> **Notes:**
> - Infrastructure shipped: `BOOT_TRY` macro, `kernel_subsystem_apply_result()`, `g_boot_info.degraded_mask`, and the Phase 3 degraded-subsystem summary (bounded by `SUBSYS_COUNT`).
> - Critical subsystems (PMM/VMM/Heap/GDT/IDT/VFS) `boot_halt()`; the wired non-critical paths (UEFI vars/time, SecureBoot, TPM, XSAVE, PCID) record degraded_mask via `apply_result`.
> - Review hardening (2026-06-07): `BOOT_TRY` now delegates to `apply_result` (was OK-only ready + unbounded shift); added `_Static_assert(SUBSYS_COUNT <= 32)` guarding the uint32 degraded_mask shift.
> - Downgraded to `[/]`: most non-critical inits (`rtc/keyboard/mouse/fb` void, `ahci_init` return ignored) mark ready unconditionally and several lack a `SUBSYS_` slot, so their failures never reach degraded_mask. 3 concrete `[ ]` items filed.
> - Scope boundary: this section owns the degradation INFRASTRUCTURE; per-driver result-returning signatures + the forced-failure test are the filed follow-ups.
> **Verified:** 2026-06-07 | commit `2340c34e` | 5/8 items | build OK | boot 1723 PASS, smoke PASS (KVM 3.0s)
> **Deferred:** [H] resilient wrapping not wired for most non-critical subsystems (void/ignored-return inits + missing enum slots) -- Test checkpoint non-functional until then -> XREF: 01-boot-platform/TODO-10 §7 (item: "Wire Phase 1-3 non-critical inits through `apply_result`")
> **Quality reviewed:** 2026-06-07 | Codex 3x (adversarial, consistency, perf) | 2M fixed, 1H deferred (re-adversarial skipped: header-only fix, no faultable region) | scope: kernel-code-quality

---

## 8. Per-Process Page Tables (Minimal Base)

Implement the minimal per-process page table infrastructure so each task has its own PML4. Kernel pages are supervisor-only, user pages have the User bit. CR3 switches on context switch. This eliminates the user-stacks-in-kernel-heap hack and is a prerequisite step toward SMEP/SMAP on bare metal -- but does NOT by itself enable them: the boot / kernel-task PML4 (`entry.asm`) still carries the User bit on all kernel 2 MiB pages, so the actual unblock is the KPTI clean kernel PML4 owned by `D02 T10 §3-§6` (see §9).

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`, `src/kernel/sched/task.c`, `include/kernel/sched/task.h`

> [!NOTE]
> Minimal prerequisite -- full per-process VM (COW fork, mmap, demand paging) is in `03-memory-concurrency/TODO-05-advanced-virtual-memory.md`. This section implements just enough to have separate user/kernel address spaces with correct U/S bits.

> [!IMPORTANT]
> **What this changes:**
> - Each task gets its own PML4 (cloned from kernel PML4 at task creation)
> - Kernel mappings (0x0--0x7FFFFF, 0x900000+): Present + Writable, NO User bit
> - User mappings (0x800000--0x8FFFFF for ELF, plus user stack): Present + Writable + User bit
> - User stacks allocated from PMM in the user address range, NOT from `kmalloc`
> - `schedule()` / `schedule_now()`: load new task's CR3 before `iretq`
> - Boot task (PID 0): continues using the identity-mapped PML4 (kernel-only task)
> **Regression risk:** CR3 switch adds ~100ns per switch; bad PT breaks user ELF. Rollback: shared identity map, re-skip SMEP/SMAP.
> **Debug POST (§8):** none. The PML4 create / CR3 switch / per-process mark paths run per-task at runtime (after Phase 3), where klog is fully functional; per kernel-code-quality Gate 4, POST16 is reserved for pre-`sti` boot-path triple-fault diagnostics and is intentionally NOT used here. Validation is via klog (`Task %u: per-process PML4 failed` on OOM) plus the boot/smoke serial log. The originally-planned 0xD800-0xD806 codes were not wired (would have spammed port 0x80 on every process spawn).

- [x] `vmm_create_user_pml4()` -- clones kernel PML4, splits PD[4] into 4KiB PT, User bit on PML4/PDPT/PD levels
- [x] `vmm_set_user_page(pml4, virt)` -- sets User bit on individual 4KiB pages in split PT
- [x] `vmm_destroy_user_pml4(pml4)` -- frees cloned PML4/PDPT/PD/PT (not data pages)
- [x] `task_create_user()`: creates per-process PML4, marks ELF + user stack pages as User, user stack at 0x8FC000 (in PD[4] range)
- [x] `schedule()` + `schedule_now()`: CR3 switch if next task has different PML4
- [x] `task_create()`: kernel tasks use boot PML4 (cr3=0)
- [/] Remove `pmm_mark_region_used(USER_ELF_BASE, USER_ELF_SIZE)` (`user_range.h`) -- deferred, ELF loader still uses fixed phys address
- [/] Bare-metal SMEP/SMAP unblock is external: boot-PML4 User-bit clearing / clean kernel PML4 owned by `D02 T10 §3-§6` (KPTI); §9 here only verifies once it lands
- [x] Commit: `"mm: per-process page tables -- user/kernel separation, CR3 switch"`

**Test checkpoint:** Boot on QEMU WHPX. cmd.exe runs in user mode with its own PML4. Cloned per-process kernel pages don't have the User bit. `KeGetCurrentIrql()` works from user-mode interrupt. Boot on bare metal: cmd.exe runs in its own PML4, no page faults. SMEP/SMAP remain skipped on bare metal (boot-PML4 User-bit blocker, unblock owned by `D02 T10 §3-§6`); do NOT expect CR4.SMEP/SMAP set here. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) -- `test_vmm.c` adds `test_vmm_user_pml4_create_destroy` (frame-neutral create+destroy, cloned-kernel-PD-no-User, split-PT checks) and `test_vmm_set_user_page_sets_bit`; 14 assertions across 2 tests.

> **Notes:**
> - Per-process PML4 minimal base: each user task clones the kernel PML4, splits PD[4] into 4 KiB PTEs, and carries User only on PML4[0]/PDPT[0]/PD[4]; cloned kernel 2 MiB pages stay supervisor-only.
> - `schedule()`/`schedule_now()` load the task CR3 (cr3=0 -> kernel PML4); `task_cleanup` now calls `vmm_destroy_user_pml4` to reclaim the full PML4 tree + PAGE_OWNED frames on exit (unwired-destroy gap fixed this review).
> - SMEP/SMAP stay skipped on bare metal: the boot PML4 (`entry.asm`) still has User on all kernel pages; the real unblock is the KPTI clean kernel PML4 (`02-kernel-core/TODO-10 §3-§6`).
> - Full per-process VM (COW fork, mmap, arbitrary-VA user mapping) is out of scope here -- owned by `03-memory-concurrency/TODO-05` and `TODO-01 §12`.
> - Status `[/]`: 2 items deferred (remove `pmm_mark_region_used`; external bare-metal SMEP/SMAP unblock).
> **Verified:** 2026-06-08 | commit `0ee5aa32` | 6/8 items | build OK | mm 102+16 PASS (14 new §8 assertions), smoke PASS (KVM 2.4s)
> **Accepted:** [Critical] `map_user_page_impl` splits the shared kernel PD (PDPT[1]) for `uthread_create` stacks, breaking per-process isolation -> XREF: 03-memory-concurrency/TODO-01 §12 (item: "Privatize cloned kernel PDs before splitting a huge page outside PD[4]" at line 272)
> **Accepted:** [Critical] `task_exec` continues into `exec_load` after partial OOM remap, corrupting the parent image -> XREF: 03-memory-concurrency/TODO-01 §12 (item: "Make `task_exec` fork+exec private-frame remap atomic under OOM" at line 273)
> **Accepted:** [M] scheduler reads CR3 on every context switch instead of a per-CPU cache -> XREF: 02-kernel-core/TODO-10 §6 (item: "Context switch (`task_switch`): update `smp_this_cpu()->user_cr3`" at line 182)
> **Accepted:** [M] `task_exec` remap zeros 1 MiB scalar + per-page INVLPG -> XREF: 03-memory-concurrency/TODO-01 §12 (item: "In the `task_exec` private-frame remap loop, zero via `zero_page`" at line 274)
> **Accepted:** [H] `task_cleanup` PML4 teardown only guards the local CPU CR3 -- an SMP reaper on another CPU could free a still-loaded page table (pre-existing reap assumption, same as the per-thread unmap path; not reachable on today's single-CPU scheduler) -> XREF: 03-memory-concurrency/TODO-07 §3 (item: "task_cleanup reap barrier: prove a TASK_DEAD task is off-CPU on ALL CPUs" at line 122)
> **Quality reviewed:** 2026-06-08 | Codex 5x (adversarial x2, consistency, perf, re-adversarial) | 1H+1M fixed, 2Crit+1H+2M accepted-XREF | scope: kernel-code-quality

---

## 9. CPU Security Activation and Verification

Ensure CPU security features (NX, SMEP, SMAP) are activated in the correct order and verified on bare metal. This incorporates the bare-metal-relevant parts of the CPU boot sequencing that are currently unimplemented.

**Files:** `src/kernel/cpu_security.c`, `src/kernel/main/boot_hw.c`

> [!NOTE]
> Minimal prerequisite -- full CPU boot sequencing (EFER before VMM, XSAVE/PCID, AP parity) is in `TODO-09-cpu-boot-sequencing.md §2,§5,§1`. This section implements just enough to ensure NX/SMEP/SMAP work on bare metal and verifies post-activation.

- [x] Activation order formalized: `cpu_harden()` (NX) → `vmm_apply_nx_policy()` → `cpu_harden_post_pagetable()` (SMEP/SMAP) → `cpu_verify_hardening()`
- [x] `cpu_verify_hardening()`: reads back EFER (NX), CR4 (SMEP/SMAP/UMIP/PKU), logs enabled / FAILED / skipped (SMEP/SMAP skipped on every platform until KPTI clears User from kernel pages)
- [x] All platforms (bare metal AND VMs): SMEP/SMAP skipped -- boot PML4 (entry.asm 0x87) has User on all kernel 2MiB pages so CR4.SMEP would #PF; `hv_supports_cr4_smep_smap()` returns 0 (commit 5cc120f3 dropped the earlier VM-only enable path).
- [x] `cpu_verify_hardening()` logs honest skip on every platform incl. WHPX; the false `SMEP enforced via EPT` branch was removed this review. Unblock is external: clean kernel PML4 (KPTI, `D02 T10 §3-§6`).
- [x] Debug POST codes: 0xD900-0xD904
- [x] Commit: `"boot: CPU security activation with post-enable verification"`

**Test checkpoint:** Boot on any platform. Log shows `Verify: NX enabled (EFER.NXE set)` plus `Verify: SMEP skipped (kernel PTE User bit -- needs KPTI)` and the same for SMAP -- SMEP/SMAP are skipped on ALL platforms (bare metal AND QEMU/WHPX/VBox) until the KPTI clean kernel PML4 clears User from kernel pages; do NOT expect CR4.SMEP/SMAP set or an `EPT-enforced` line. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) -- `test_cpu_security.c` covers NX EFER.NXE set + SMEP/SMAP CR4 accessibility + activation-state helper. The verify path is logs-only (void), so no direct unit surface; validated by serial log on boot.

> **Notes:**
> - Activation order (BSP boot_phase0): `cpu_harden()` NX/UMIP/PKU -> `vmm_apply_nx_policy()` -> `cpu_harden_post_pagetable()` SMEP/SMAP -> `cpu_verify_hardening()` reads back EFER/CR4 and logs enabled/skipped/FAILED.
> - SMEP/SMAP are skipped on EVERY platform (not just bare metal): `hv_supports_cr4_smep_smap()` returns 0 because the boot PML4 keeps User on kernel pages; the unblock is the KPTI clean kernel PML4 (`02-kernel-core/TODO-10`).
> - This review removed the false `SMEP enforced via EPT (Hyper-V)` verify log and corrected boot_hw.c comments that wrongly claimed `vmm_apply_nx_policy()` clears the User bit (it applies NX only).
> - NX policy now hard-fails (LOG_ERROR + `boot_halt`) instead of silently leaving a text-overlapping huge page executable on a split OOM -- NX is REQUIRED and verify only reads EFER.
> - Verified on KVM serial: `NX enabled`, `SMEP/SMAP skipped (needs KPTI)`, `UMIP enabled`, CR4=0x40e68 (SMEP/SMAP bits clear).
> **Verified:** 2026-06-08 | commit `e194e646` | 5/5 items | build OK | security 79+16 PASS, smoke PASS (KVM 2.6s; NX enabled + SMEP/SMAP skipped confirmed in serial)
> **Accepted:** [M] RESOLVED 2026-08-13 -- `cpu_enable_pku` published per-CPU PKU as global `pku_enabled`, so an AP lacking PKU/XCR0.9 could run PKRU without CR4.PKE. Closed by the online-CPU CR4.PKE intersection in `cpu_features_finalize_global()`; the item moved into the backfill cohort en route -> XREF: 01-boot-platform/TODO-09 §11 (item: "PKU global skew closed", was §6)
> **Quality reviewed:** 2026-06-08 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M+1L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 10. Boot Order Hardening (Timer-Last, UEFI-Safe)

Formalize the boot order lessons learned: timer is the last thing initialized before `sti`, all UEFI runtime calls mask the LAPIC timer, and `boot_splash_start_animation()` only runs after `sti`.

**Files:** `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_init.c`

- [x] Timer last before sti (already in place, documented in init order contract)
- [x] LAPIC timer masked during ALL UEFI runtime calls via `rt_call_enter`/`rt_call_exit` (+ emergency variants) in uefi_runtime.c. PIT-backend (TCG) masking is backend-aware UTS work -> `01-boot-platform/TODO-11 §6`.
- [x] Phase 1 init order contract documented at top of boot_interrupts.c (14-step sequence, matches actual code: DPC-before-sti, input-after-sti)
- [x] `BOOT_ASSERT(cond, msg)` macro added -- used for IDT/GDT checks before timer init
- [x] Commit: `"boot: formalize Phase 1 init order -- timer-last, UEFI-safe, documented contract"`

**Test checkpoint:** Phase 1 order is correct. UEFI runtime calls don't crash with timer running. Boot order comment block is visible at top of `boot_interrupts.c`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** No dedicated kernel test surface -- BOOT_ASSERT halts via `boot_halt`, and `rt_call_enter`/`rt_call_exit` are static helpers touching the live LAPIC + firmware mutex (cannot be unit-tested without forbidden live-boot calls). Validated by `bash scripts/test-smoke.sh` (boot to userspace) + serial-log inspection on each platform.

> **Notes:**
> - Timer-last + UEFI-safe formalized: timer is armed last before `sti`; UEFI runtime calls mask the LAPIC timer via `rt_call_enter`/`rt_call_exit`; a 14-step INIT ORDER CONTRACT heads `boot_interrupts.c`; `BOOT_ASSERT` guards IDT/GDT-before-timer.
> - This review fixed the `klog(LOG_FATAL)`-makes-`boot_halt`-unreachable bug in `BOOT_ASSERT` and the `boot_hw.c` CPU-minimum check (LOG_ERROR + boot_halt so the styled halt screen / POST16_BOOT_FAILED actually render).
> - Emergency reset (`uefi_reset`) now masks the timer regardless of firmware-mutex contention; the init-order contract + `uefi_runtime.c` header were corrected to match the real code (DPC-before-sti, input-after-sti, sleepable mutex).
> - Scope boundary: PIT-backend (TCG) timer masking is backend-aware UTS work owned by `01-boot-platform/TODO-11 §6`; LAPIC-backend (bare metal / WHPX) is masked here.
> **Verified:** 2026-06-08 | commit `483ff06c` | 4/4 items | build OK | boot 1723+16 PASS, smoke PASS (KVM 2.57s)
> **Accepted:** [L] panic/emergency reset keeps LAPIC-only masking on the PIT backend (reason: ioapic_lock is panic-unsafe, deliberate) -- the normal rt_call PIT gap is CLOSED by `timer_hal_quiesce()` -> XREF: 01-boot-platform/TODO-11 §6 (item: "Backend-aware `timer_hal_quiesce()`/`resume()`" at line 253)
> **Quality reviewed:** 2026-06-08 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+2M+2L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 11. ~~`boot.conf` Subsystem Skip List~~ *(removed)*

> [!NOTE]
> **Removed 2026-03-29.** A skip list encourages hiding problems instead of fixing them. Errors can go unnoticed indefinitely. The correct approach is `BOOT_TRY` (§7) -- subsystems degrade gracefully with a logged warning, not silent bypass. If a subsystem crashes, fix the root cause.

- [x] Commit: "(shipped) removed boot.conf skip list -- BOOT_TRY + degraded_mask only"

**Test checkpoint:** N/A -- `boot.conf` skip list removed 2026-03-29; use `BOOT_TRY` / `degraded_mask` (§7) for intentional subsystem failure tests instead.

> **Deferred:** [L] `boot.conf` subsystem skip-list removed 2026-03-29 (code deleted, absent from `src/`), superseded by `BOOT_TRY` / `degraded_mask` -> XREF: 01-boot-platform/TODO-10 §7 (item: "`BOOT_TRY(subsys, fn_call, name)` macro" at line 325)

---

## 12. Logging and Diagnostic Storage Moved to TODO-24

> [!IMPORTANT]
> The temporary log-path reshuffle that once lived here is no longer owned by the bare-metal hardening roadmap. `TODO-24-blackbox-service-partition.md` is now the single owner for `X:\` BlackBox layout, `klog_dir`, boot timeline output, crash persistence, and perf/diag file paths.

- [x] Bare-metal constraints from this TODO fed the final storage design, but the authoritative checklist now lives in `TODO-24 §5-§8`
- [x] `klog_dir` / BlackBox / crash path work shipped in tree (X:\ path), treated here as consumed prerequisites. Remaining multi-platform verification + the §5 C:\-fallback reentrancy follow-up stay owned by `TODO-24` (§5 is `[/]`).
- [x] Commit: "(doc) move log and diagnostic storage ownership to TODO-24"

**Test checkpoint:** Use `TODO-24 §5-§8` verification; this file only depends on those outputs existing on bare metal.

**Test runner:** No kernel test surface -- §12 is an ownership pointer; verification of `klog_dir`/BlackBox/crash paths is owned by `01-boot-platform/TODO-24 §5-§8` and exercised by that TODO's bare-metal checkpoints.

> **Notes:**
> - Ownership transfer only: logging + diagnostic storage (X:\ BlackBox layout, `klog_dir`, boot timeline, crash persistence, perf/diag) is owned by `01-boot-platform/TODO-24 §5-§8`; reciprocal back-XREF in TODO-24 §15.
> - Transfer verified sound: TODO-24 §5-§8 exist + shipped; `klog_disk.c` resolves `klog_dir` to `X:\Logs\` (BlackBox) / `C:\` fallback; no orphaned scope.
> - Cross-TODO consistency fixes this review: TODO-24 §5 status `[x]`->`[/]` (open reentrancy item), TODO-24 §6 test-checkpoint serial-path contradiction, removed a duplicate `klog_dir` assertion from TODO-10's Unit Tests.
> - re-adversarial skipped: docs-only TODO consistency fixes, no code/faultable region.
> **Verified:** 2026-06-08 | commit `f32157aa` | 2/2 items | build OK | docs-only ownership transfer (klog_dir X:\ shipped per TODO-24 §5-§8; lint 0 err, todo-graph 8/8)
> **Accepted:** [H] latent `klog_disk_flush` re-entrancy on the C:\ fallback path (BlackBox unmounted) -- §12 "shipped" covers the X:\ path only -> XREF: 01-boot-platform/TODO-24 §5 (item: "klog_disk_flush re-entrancy guard when C:\ fallback is active" at line 187)
> **Quality reviewed:** 2026-06-08 | Codex 3x (adversarial, consistency, perf) | 1H+2M fixed (cross-TODO ownership consistency), 1H accepted-XREF | scope: N/A (docs-only ownership pointer)

---

## 13. CPU Feature Minimum Requirements and Verification

Define the minimum CPU feature set required to boot, verify features are actually enabled after activation, and provide clear diagnostics when features are missing or fail to enable.

**Files:** `src/kernel/main/boot_hw.c`, `src/kernel/cpu_security.c`, `include/kernel/cpuid.h`

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md` -- owns the IMPLEMENTATION of NX/SMEP/SMAP/CET. This section owns VERIFICATION that features actually took effect on real hardware, and DIAGNOSIS when they don't.

- [x] Minimum: NX + SSE2 + LM + SYSCALL required (= `CPU_FEATURES_REQUIRED_MASK`, same baseline AP validation bug-checks against) -- boot halts with clear per-feature message if missing
- [x] Recommended: SMEP, SMAP, RDRAND -- warns if missing, continues
- [x] `cpu_verify_hardening()`: reads EFER/CR4, logs discrepancies (implemented in §9); NX readback failure HALTS boot (NX is a boot minimum, the readback is the last chance to catch a trapped/ignored EFER.NXE write)
- [x] Known quirks documented in CLAUDE.md bare metal gotchas section
- [x] Commit: `"boot: CPU feature minimum requirements + post-activation verification"`

**Test checkpoint:** Boot on CPU without SMAP. Log shows `[WARN] cpu: RECOMMENDED: SMAP not available`. Boot continues. On every platform today: `Verify: SMAP skipped (kernel PTE User bit -- needs KPTI)` -- do NOT expect CR4.SMAP set until the KPTI clean kernel PML4 lands (see §9 checkpoint). On a CPU missing any required feature: `MINIMUM: <feature> ...` then halt `CPU does not meet minimum requirements (NX + SSE2 + LM + SYSCALL)`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) -- `test_cpu_security.c` required-mask trio (BSP flags contain REQUIRED_MASK, REQUIRED subset of AP_PROBE, global mask honors required); `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) -- NX EFER.NXE + SMEP/SMAP CR4 state. The halt paths (minimum gate, NX verify fail) are boot_halt-terminal -- no unit surface; validated by serial log.

> **Notes:**
> - BSP minimum gate (boot_hw.c) is generated from `CPU_FEATURES_REQUIRED_LIST` (cpuid.h X-macro), the same list that generates `CPU_FEATURES_REQUIRED_MASK` for AP validation -- gate table and mask cannot drift; halt names NX+SSE2+LM+SYSCALL.
> - `cpu_verify_hardening()` now HALTS (LOG_ERROR + boot_halt) when CPUID reports NX but EFER.NXE reads back clear -- last chance to catch a trapped/ignored WRMSR; BSP-only (single caller boot_phase0).
> - Recommended features (SMEP/SMAP/RDRAND) warn-and-continue unchanged; SMEP/SMAP remain skipped on every platform until KPTI (§9).
> - This review corrected the stale test checkpoint (CR4.SMAP-set claim), qualified the cpu_security.h blanket BSP/AP-safe claim, and added the BM Test 4 EPT-claim retraction note.
> - Scope boundary: NX/SMEP/SMAP/CET ENABLE implementation is owned by `02-kernel-core/TODO-10`; this section owns the boot minimum gate + post-activation verification/diagnosis.
> **Verified:** 2026-06-10 | commit `c843ca25` | 4/4 items | build OK | security 79+16 + x86 152+16 PASS, smoke PASS (KVM 2.48s)
> **Quality reviewed:** 2026-06-10 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 2H+3M fixed | scope: kernel-code-quality

---

## 14. Bare-Metal Test Matrix and Validation Plan

Define the hardware platforms to test on, expected boot timings per phase, and a regression detection framework so bare-metal issues are caught early.

**Files:** `todo/01-boot-platform/TODO-10-bare-metal-hardening.md` (this file -- Bare Metal Testing Plan + matrix tables)

- [x] Test platforms documented in BM Testing Plan section above (BM Tests 1-4 with results tables)
- [x] Platform matrix:

| Platform   | Timer | Calibration | AHCI | PS/2             | Per-Process PT  | Boot Time  |
| ---------- | ----- | ----------- | ---- | ---------------- | --------------- | ---------- |
| QEMU WHPX  | LAPIC | Hyper-V MSR | MSI  | Skipped (8042=0) | ✅ CR3 switch   | ~10s       |
| QEMU TCG   | PIT   | N/A         | MSI  | Skipped (8042=0) | ✅ CR3 switch   | ~3s        |
| VirtualBox | LAPIC | PM Timer    | INTx | Active (8042=1)  | ✅ CR3 switch   | ~45s (NCQ) |
| Bare metal | LAPIC | TSC ref     | MSI  | Active (8042=1)  | TBD (BM Test 5) | ~10s       |

- [x] Phase acceptance bands documented (P0 <200ms, P1 <2s, P2 <10s except VBox NCQ, P3 <15s) -- manual per-platform bands from BM records; per-phase bare-metal record owned by BM Test 5
- [x] Regression detection: automated = `boot_perf_budget.c` per-step + 4s-total WARN/ERR (stricter than the bands by design) + `boot_trend.c` drift alarms; policy = CLAUDE.md "Bare Metal First" + validate-todo-file check 11
- [x] BM Tests 1-4 cover the recorded matrix cells; bare-metal per-process PT + per-phase timing are owned by BM Test 5 items below
- [x] Commit: "(shipped) bare-metal test matrix inline in TODO-10"

**Test checkpoint:** Acceptance is BM Test 5 on the i5-11600K: boot matches the checklist with every phase inside the bands (capture the `PERF` step-duration table + any `BOOT-BUDGET` WARN/ERR serial lines). Recorded so far: QEMU WHPX, QEMU TCG, VirtualBox match the matrix rows above; the bare-metal per-process PT cell is TBD until BM Test 5 runs.

**Test runner:** No kernel test surface -- §14 is a documentation matrix + validation plan. Runtime artifacts come from BM Test 5 serial captures (`PERF` table + `BOOT-BUDGET` lines); the budget machinery itself is tested by `01-boot-platform/TODO-29 §1` (SUITE=boot).

> **Notes:**
> - §14 documents the 4-platform matrix, phase acceptance bands, and regression layers; execution of the missing bare-metal cells is owned by BM Test 5 `[ ]` items.
> - This review fixed false completeness: bare-metal Per-Process PT cell -> `TBD (BM Test 5)`, BM Test 4 retracted EPT row struck through, Test checkpoint reworded from asserted-pass to acceptance criteria.
> - Threshold layering clarified: phase bands = manual per-platform acceptance; `boot_perf_budget.c` per-step + 4s-total WARN/ERR = stricter automated layer; `boot_trend.c` = drift alarms.
> - BM Test 5 extended with the exact serial artifact strings and a bare-metal per-process PT closure item.
> - Scope boundary: budget/trend machinery cost findings filed in `01-boot-platform/TODO-29 §1/§3` (both reopened `[/]`).
> - re-adversarial skipped: docs-only fixes, no C/H lines changed.
> **Verified:** 2026-06-10 | commit `d524d572` | 5/5 items | build OK | docs-only (lint 0 err, todo-graph 8/8)
> **Accepted:** [H] `boot_trend_publish_json` cJSON RMW + sync VFS I/O runs pre-userland, unbudgeted boot cost -> XREF: 01-boot-platform/TODO-29 §3 (item: "Defer `boot_trend_publish_json()` ... to a post-DESKTOP_READY work item" at line 156)
> **Accepted:** [M] full `PERF`/timeline serial dump runs pre-cmd.exe outside the `boot_perf_total_check` window -> XREF: 01-boot-platform/TODO-29 §1 (item: "Gate the full `PERF`/timeline serial tables behind debug/test builds" at line 464)
> **Deferred:** [H] bare-metal per-process PT run never recorded (BM Test 4 bare-metal row TBD) -> XREF: 01-boot-platform/TODO-10 BM Test 5 (item: "Per-process PT on bare metal" at line 804)
> **Deferred:** [M] per-phase bare-metal timing artifact not captured -> XREF: 01-boot-platform/TODO-10 BM Test 5 (item: "Boot time within thresholds for ALL phases" at line 803)
> **Quality reviewed:** 2026-06-10 | Codex 3x (adversarial, consistency, perf) | 4H+3M fixed, 1H+1M accepted-XREF | scope: N/A (docs-only)

---

## 15. Boot Splash Spinner Bare-Metal Fix

The boot splash spinner stutters on bare metal -- stops and restarts repeatedly during Phase 2. **Historical note (pre-2026-03-29):** the LAPIC timer ISR was effectively broken on bare metal/TCG due to `clac` #UD (§3), so the spinner only advanced when the compositor or `boot_splash_tick()` explicitly called it. **After the §3 fix**, the timer ISR runs; remaining visible stutter on VirtualBox is dominated by NCQ timeout (15s I/O), not a dead timer.

**Files:** `src/kernel/boot_splash.c`, `src/kernel/spinner.c`, `src/kernel/timer.c`, `src/kernel/main/boot_interrupts.c`, `src/kernel/main/boot_storage.c`

- [x] Spinner driven by `timer_register_tick_callback(spinner_advance, 10)` -- ~10fps from the active timer ISR (LAPIC; PIT backend on TCG)
- [x] Explicit `boot_splash_tick()` keep-alive calls present during long boot phases (PCI scan, partition/filesystem mount, BlackBox/registry, desktop init, test phase)
- [x] Timer IS working on bare metal (§3 fixed clac). Spinner callback fires correctly.
- [x] VBox stutter: caused by NCQ timeout (15s blocking I/O), not a spinner bug. Timer ISR fires during NCQ wait (`event_wait_timeout`), spinner advances. Display may lag due to VBox VGA emulation under heavy I/O -- cosmetic, not a kernel issue.
- [x] Verified: QEMU WHPX ✅, TCG ✅, bare metal ✅. VBox: minor stutter during NCQ timeout only.
- [x] §3 (clac fix) resolved the original stutter root cause; section-15 review (2026-06-11) then fixed drift: removed stale `boot_splash_tick@5` override in `boot_interrupts.c` that overwrote the `@10` registration (ran animation at 2x Fluent speed)
- [x] Tick-callback slot hardening: atomic acquire/release publication + divisor-0 guard in `timer.c`, reentrancy guard in `spinner_advance` (ISR vs thread-context callers), unit tests in `test_timer_tick_cb.c`

**Test checkpoint:** Bare metal: spinner rotates smoothly during PCI scan and AHCI init (no visible stutter or freeze). QEMU: spinner unchanged (already smooth). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts/debug/kernel/run-boot-tests.bat` (TEST_CAT_BOOT) -- `test_timer_tick_cb.c`: 2 tests / 3 assertions on the singleton tick-callback slot

> **Notes:**
> - Diagnostic section; review found real drift: stale `boot_splash_tick@5` registration in `boot_interrupts.c` silently overwrote the spinner's `@10` slot, running the animation at 2x Fluent design speed -- removed, `spinner_start()` is the single owner
> - Tick-callback slot hardened in `timer.c`: release/acquire fn publication, ISR snapshot, divisor-0 guard; `spinner_advance` gained a reentrancy guard (BSP ISR vs thread-context keep-alive calls)
> - `test_timer_tick_cb.c` (TEST_CAT_BOOT) covers divisor counting, unregister retraction, divisor-0 guard; saves/restores the live spinner around the slot tests
> - NCQ tag-state race surfaced by the adversarial pass is owned by the AHCI NCQ section in 05-storage TODO-01 (Accepted below)

> **Verified:** 2026-06-11 | commit `62112f00` | 8/8 items | build OK | smoke PASS (KVM 2.400s)
> **Accepted:** [M] NCQ tag state races AHCI ISR in `ncq_sync_rw` timeout path (reason: scope) -> XREF: 05-storage-filesystems/TODO-01 §3 (item: "Guard NCQ tag state vs AHCI ISR" at line 95)
> **Quality reviewed:** 2026-06-11 | Codex 7x (adversarial x2, consistency, perf, re-adversarial x3) | 2H+6M+2L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 16. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 2:
- [/] AP per-CPU TSS/IST: BSP-only today -- APs share one `kernel_tss`/IST (no AP `ltr`), so AP #DF/NMI/MCE IST delivery is not SMP-safe. Owned by `D01 T09 §10` (item: "Per-CPU TSS + IST").
  - Blocker: needs a per-CPU GDT so each AP has its own TSS descriptor slot, which the shared-BSP-GDT trampoline does not provide. Parked HERE as a consumer; the owner carries the same blocker and already names this item as a re-open trigger -> XREF: `01-boot-platform/TODO-09 §10` (item: "Per-CPU TSS + IST (PARKED)"). Not operator-gated: ordinary kernel work awaiting a scoped owner.
- [x] Abort-safe serial: `serial_write_emergency`/`serial_putchar_emergency` try-lock `g_serial_lock` with a bounded UART wait (`serial.c`). Closes the #DF/#MC/NMI panic self-deadlock -> XREF: 02-kernel-core/TODO-23 §12
  - Proceeds UNLOCKED when the try-lock fails (interleaved bytes beat no crash evidence) and re-asserts LCR 8N1 first, because `serial_init` re-purposes the base port to the divisor latch across its DLAB window; `serial_init` now holds the lock across that window too, closing the cross-CPU half.
  - Bounded on both axes: 65536 LSR polls per byte, then a shared wedged-transmitter budget (8 waits, monotonic within an epoch) drops later bytes to one status probe, so a dead UART costs one budget before arming and one after (~1M polls; a stale pre-arm return can add a third -- §17) rather than unbounded millions. A 1024-char cap bounds the string walk, checked BEFORE the dereference.
  - `serial_write`/`serial_putchar` re-route once the latch is armed, covering indirect emitters (klog's serial sink, subsystem/transition-ring/quota dumps) without opting in. Armed at exactly two terminal sites: `panic.c` after `panic_try_claim_owner()`, and `boot_halt.c`.
  - Panic reason + register dump (incl. `serial_write_hex`) call the emergency writers DIRECTLY, before the latch, so they survive on the async-fault path where the system RECOVERS and must keep its ordinary locked serial.
  - Consumers closed: `except.c` WER-ordering note, `idt.c` unhandled-vector + both frame-integrity branches (the GS-invalid one could never have used `serial_write`, which reads `gs:0` via `spin_lock_irqsave`), `wer.c` `WerpReportFault`.
- [x] NMI crash routed to `KeBugCheckExFrame` (`panic.c`): it discarded a live frame and took the `persist_registry=1` + POST16 path documented as fault-unsafe; now skips both and reports faulting RIP/registers.
- [x] Async fault isolation publishes `async_result`/`async_done` BEFORE its diagnostic, which is now the bounded lock-free emitter not `klog` -- a worker faulting inside `klog` used to hang the BSP.

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes with 5 `serial_emergency` acquire/release-policy tests registered under `TEST_CAT_BOOT`; the held-lock test proves acquisition RETURNS rather than spinning, which is the property the panic path depends on. Boot unaffected across all four smoke-matrix legs (KVM/TCG x 1/2 CPU). The wedged-UART and self-deadlock paths are not unit-testable (real UART state plus a one-way global latch), so they rest on the bounds above plus the matrix showing no boot regression.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 11 added `serial_emergency` suites (lock policy, budget arithmetic, rerouting), 3334 boot-suite tests pass, 0 failures

> **Notes:**
> - Shipped: abort-safe serial (`serial_write_emergency`/`serial_putchar_emergency`, emergency latch, bounded UART wait, LCR re-assert) in `serial.c`/`serial.h`, plus the panic/idt/boot_halt/wer call-site conversions and NMI frame-aware routing.
> - How it runs: try-lock `g_serial_lock` and proceed unlocked, with `local_irq_save`/`local_irq_restore` rather than `spin_lock_irqsave` -- `spin_trylock` raises no IRQL, so the release side must lower none. State, owner and wedged-UART budget are packed in one word so an epoch publishes atomically.
> - Downstream effects: closes TODO-23 §12's Accepted [H] self-deadlock and [M] UART-timeout findings plus the hazards `idt.c` and `except.c` documented in prose; the lock/budget policy surface lives in `serial_emergency.h` so `serial.h` does not widen the arch include closure.
> - Scope boundary: bounds the SERIAL layer only -- `klog_emit` takes `s_klog_lock` above it, so post-claim dumpers still stall if the panic interrupted logging -> XREF: `02-kernel-core/TODO-27 §7`.
> - Canonical doc: this section + the emergency block in `include/kernel/drivers/serial.h`.

> **Verified:** 2026-08-13 | 4/5 items | build OK | 28418 kernel + 17 user PASS | smoke matrix 4/4 (KVM/TCG x 1/2 CPU) | lint 0 errors
> **Accepted:** [M] post-claim panic dumpers still block on `s_klog_lock`, which sits ABOVE the serial layer this section bounds (reason: klog hot path, pre-existing and repo-wide) -> XREF: 02-kernel-core/TODO-27 §7 (item: "`dump_emit_raw(str)` -- panic-safe emitter replacing `klog` in panic-path dumpers" at line 258)
> **Accepted (RESOLVED by §17):** [H] a corrupt caller string can still fault the emergency writer; the walk is length-capped and taken with no lock held, but surviving the fault needs a kernel-range fault-suppressed read (reason: new primitive, different subsystem) -> XREF: 01-boot-platform/TODO-10 §17 (item: "Fault-suppressed kernel reads"). Shipped `__kread_u8`; the "USER range only" premise was stale (`__kstack_read_u64` already had no CR2 gate), and the first dereference turned out to be `pe_copy`, which is now guarded too.
> **Accepted:** [H] no cross-CPU freeze on panic entry -- Win11 IPI-freezes and Linux `smp_send_stop`s every other CPU; healthy CPUs here keep mutating the state the dump captures (reason: SMP design decision, own section) -> XREF: 01-boot-platform/TODO-10 §18 (item: "Freeze other CPUs on panic entry")
> **Accepted:** [M] the async-isolation park leaves `is_online` set, so later `boot_async_group` calls assign work to a dead CPU (reason: pre-existing, async-boot surface) -> XREF: 01-boot-platform/TODO-10 §18 (item: "Clear `is_online` when the async-isolation branch parks a faulting AP")
> **Accepted:** [H] an AP that faults while HOLDING `g_serial_lock` parks still owning it, so the surviving BSP hangs on its next ordinary serial write; §16 publishes `async_done` first so the fallback is reached, but real ownership needs lock poisoning/handoff (reason: SMP-ownership design call) -> XREF: 01-boot-platform/TODO-10 §18 (item: "Async-isolation lock ownership")
> **Accepted (RESOLVED by §17):** [M] the wedged-UART charge is not attributable per reservation, so a stale return or the async refund can absorb a concurrent epoch's charge (reason: changes the unit-tested accounting API; bounded and errs toward more waits) -> XREF: 01-boot-platform/TODO-10 §17 (item: "Epoch-tokened, attributable wedged-UART accounting"). Shipped as a slot bitmap plus epoch generation with a per-CPU slot ledger.
> **Deferred:** [H] AP per-CPU TSS/IST leaves AP #DF/NMI/MCE IST delivery non-SMP-safe; needs a per-CPU GDT -> XREF: 01-boot-platform/TODO-09 §10 (item: "Per-CPU TSS + IST (PARKED)")
> **Quality reviewed:** 2026-08-13 | Codex 38x (design, adversarial x12, consistency x6, perf x6, re-adversarial x13) + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst | 21H+18M+1L fixed, 3H+2M accepted-XREF | scope: kernel-code-quality

---

## 17. Emergency-Writer Robustness Residuals (from the §16 review)

> **Spawned-by:** §16 (review)
> **User impact:** A bare-metal crash caused by memory corruption can still produce NO usable evidence -- if the corruption also reached the panic description pointer, the emergency writer faults and the machine triple-faults instead of printing why it died, which is the exact failure §16 was written to remove.

Three residuals the §16 adversarial and kernel-quality reviews raised against the emergency SERIAL WRITER itself -- surviving a faulting source pointer, attributable wedged-UART accounting, and a check-to-emit race in the recoverable writer. The cross-CPU ownership residuals from the same review are §18; the seam is that everything here is a property of the writer primitive and its accounting, while §18 is about which CPUs are running while it writes.

- [x] Fault-suppressed kernel reads: `__kread_u8` (`cpu_security.c`) guards the caller-string walk in BOTH `serial_emergency_write_str` and `pe_copy`, so a corrupt description truncates with a marker instead of faulting.
  - The filed premise was STALE and the design review caught it: the fixup table was never user-range-only. `__kstack_read_u64` (shipped by `02-kernel-core/TODO-23 §7`) is matched by RIP and read direction with NO `CR2` gate, so kernel-range recovery already existed; `__kread_u8` adds the byte-load label pair beside it, and `kread_u8_fixup_lookup` factors the routing decision out of `page_fault_handler` so the wiring is unit-testable.
  - The writer was NOT the first dereference. `panic_collect_evidence` runs before any serial output and `pe_copy` raw-loaded the same pointer, so guarding only the writer would have protected nothing -- the fault landed earlier and re-entered the panic path. Guarding both is what delivers the section's user impact.
  - NMI is deliberately EXCLUDED, not overlooked: the fixup returns via `IRETQ`, which re-arms NMI delivery while the outer NMI still owns IST2. Callers declare `PANIC_CTX_NORMAL`/`NMI`/`UNKNOWN` (`serial.h`) from `frame->int_no`; the predicate is opt-in so an undeclared context degrades to the plain load. Nested-NMI latch/replay stays with §2 -> XREF: `01-boot-platform/TODO-10 §2` (item: "AP per-CPU TSS/IST: BSP-only today").
- [x] Epoch-tokened, attributable wedged-UART accounting: the anonymous charge COUNT became a slot BITMAP plus an epoch generation, both packed in the existing latch word.
  - `serial_emerg_reserve` claims the lowest free slot and returns a token naming that slot and generation; `serial_emerg_return` releases only that slot, and only while the generation still matches, so a pre-arm reservation returned post-arm no longer credits the new epoch. Tokens are SINGLE-USE by contract -- a freed slot is reissued immediately, and per-slot incarnations do not fit the 32-bit word.
  - The async-isolation refund is now attributable: a per-CPU slot mask (`s_emerg_slot_mask`, indexed by the 8-bit APIC id so it stays GS-independent) replaces the two-snapshot delta against a global counter, which could not tell this CPU's charge from a concurrent panic's. `serial_emerg_refund_self` clears its whole selected set in ONE generation-validated compare-exchange instead of one per slot.
  - Residual, bounded and documented at `serial.c`: if that CAS loses every attempt the charges stay outstanding, and the next arming does NOT clear them because the pre-arbitration dump runs before arming. Cost is degraded evidence on a wedged UART, never a hang or a lost panic; eliminating it needs a two-word latch.
- [x] Recoverable writer check-to-emit race: `serial_emergency_emit` re-tests ownership before every PHYSICAL byte for recoverable output, bounding the residual to one byte.
  - Per-chunk was not enough (a chunk is 128 bytes), and per-input-character was not either -- a `\n` expands to `\r` plus `\n`, so a single test let two bytes through after an arm. The remaining one-byte window is the load-to-`outb` gap, which no lock can close: `serial_enter_emergency` never takes `g_serial_lock` and the emergency emitter proceeds when the try-lock fails.
- [x] Commit: `"kernel: emergency-writer robustness residuals -- fault-suppressed kernel reads, epoch-tokened UART reservation"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes with the epoch-token, per-CPU attribution, context-predicate and fixup-routing suites registered under `TEST_CAT_BOOT`; a token minted before an epoch reset cannot spend the new epoch's charge, and one CPU's refund leaves another CPU's charge standing. The live corrupt-pointer recovery needs a real #PF and stays serial-validated -- the routing DECISION is unit-tested instead, and that gap is deliberate, not claimed as covered.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 19 `serial_emergency` suites (11 pre-existing + 8 added), 28477 kernel + 17 user tests pass, 0 failures

> **Notes:**
> - Shipped: `__kread_u8` + `kread_u8_fixup_lookup`, the `PANIC_CTX_*` declared-context API, guarded walks in the emergency writer and `pe_copy`, slot-bitmap + generation latch accounting with a per-CPU ledger, per-byte recoverable ownership retest.
> - How it runs: the guarded read reuses the RIP-keyed table beside `__kstack_read_u64`; callers declare context from `frame->int_no` rather than probing state a panic may have corrupted; the latch keeps its single-word atomic publish.
> - Downstream effects: closes §16's Accepted [H] corrupt-caller-string and [M] non-attributable-charge findings; `serial_emerg_reserve` changed signature (`int` -> `uint32_t` token) and every caller moved with it.
> - Scope boundary: the crash REASON path only -- the terminal renderers downstream still walk the originals -> XREF: §19. Cross-CPU panic ownership is §18; nested-NMI latch/replay is §2; the panic-safe emitter stays `02-kernel-core/TODO-27 §7`.
> - Canonical doc: the emergency accounting block in `src/kernel/drivers/serial.c` plus the `PANIC_CTX_*` block in `include/kernel/drivers/serial.h`.

---

## 18. Panic-Path Cross-CPU Ownership Residuals (from the §16 review)

> **Spawned-by:** §17 (split)
> **User impact:** One isolated async-init fault currently costs every LATER async group a full 10-second barrier timeout, turning a recovered boot into a visibly stalled one; and if that AP faulted while holding the serial lock, the surviving BSP hangs on its next ordinary log write -- a silent hang instead of a boot. Meanwhile a crash dump can be captured while other CPUs are still mutating the very state it describes, so the evidence a user sends in may not reflect the machine at the moment it died.

The three §16 review residuals that are about WHICH CPUs are running and who owns what while the panic path writes -- split out of §17 before implementation because they are SMP-ownership calls on `panic.c` / `smp.c`, not changes to the serial writer primitive (§17). §16 rewrote the async-isolation block these sit inside but deliberately did not change CPU ownership.

- [ ] Clear `is_online` when the async-isolation branch parks a faulting AP (`panic.c`), so later `boot_async_group` calls stop assigning work to a dead CPU and eating the full 10s barrier timeout per group.
  - Pre-existing (the park predates §16), but it sits inside the block §16 rewrote and is the direct consequence of the isolation design: `boot_async_group` selects workers purely on `is_online`, and the parked AP has interrupts masked so it can never take the IPI.
- [ ] Freeze other CPUs on panic entry: Win11 (`KiFreezeTargetExecution` IPI) and Linux (`smp_send_stop`) both halt every other CPU; we elect a panic owner but never stop healthy CPUs, which keep mutating the state the dump is capturing.
  - `panic_try_claim_owner` only arbitrates among CPUs that are THEMSELVES panicking, and the async-isolation branch parks only an AP that itself faulted; nothing stops a healthy CPU. §16's non-owner drop protects the serial byte stream, not the system state that stream describes -- a running CPU can corrupt heap/PMM/quota state mid-`write_crash_dump`, or take a second unrelated fault racing the first one's dump.
  - The primitive already exists: `lapic_send_ipi_all_but_self()` plus the registered-handler pattern `IPI_VECTOR_CR_VERIFY` established (`cpu_security.c`). Run it immediately after ownership is claimed, before `panic_capture_fpu_state()`. Relates to the "unified owner-token design" note at `panic.c` (bugcheck emission comment).
- [ ] Async-isolation lock ownership: an AP that faults while HOLDING `g_serial_lock` publishes `async_done` and parks forever still owning it, so the surviving BSP hangs on its next ordinary serial write.
  - §16 IMPROVED this (the diagnostic is try-locked and bounded, and `async_done` is now published before it, so the BSP at least reaches the sequential fallback) but cannot close it: the parked AP never unwinds the interrupted `serial_write`. Arming the latch would stop the BSP blocking, but the BSP is then a non-owner and its output is dropped -- so the fix is real ownership, not routing.
  - Needs an owner-tracked poison/handoff on `g_serial_lock`, or a decision to escalate a lock-holding async fault to the system-terminal path. That is an SMP-ownership design call, not a serial-primitive change. Test by faulting an AP while it holds the lock and asserting the BSP reaches sequential fallback AND still logs.
- [ ] Commit: `"kernel: panic-path cross-CPU ownership -- async-park is_online, panic CPU freeze, serial-lock ownership"`

**Test checkpoint:** After an injected async-init fault, a subsequent `boot_async_group` completes without a 10s stall and does not report a spurious FATAL for an unrelated step. A panic on one CPU leaves every other CPU halted before `write_crash_dump` runs, and an AP faulting while holding `g_serial_lock` still lets the BSP log through the sequential fallback.

---

## 19. Panic-Path Caller-String Snapshot (from the §17 review)

> **Spawned-by:** §17 (review)
> **User impact:** A crash whose corruption reached the panic description now reports its reason on serial (§17), but the machine can still die before finishing the job: the BSOD never paints and the disk crash dump never lands, because the terminal renderers walk the original pointer again. The user sees the reason scroll past on a serial cable and gets no on-screen bugcheck and no dump file to send.

§17 guarded the three sites that produce the crash REASON (`pe_copy`, the emergency serial walk, the async diagnostic). The §17 review then found the renderers DOWNSTREAM of that still dereference the originals, which is the same defect one layer later: fixing individual call sites does not make a pointer safe, only routing every consumer through one safe copy does.

- [ ] Take ONE bounded, guarded snapshot of `description` and `file` at panic entry; every later consumer reads the snapshot, never the caller pointer (`panic.c` serial, framebuffer and `write_crash_dump` paths).
  - The snapshot largely exists already: `panic_collect_evidence` writes guarded copies to `ev->message` / `ev->file` in the evidence record. The work is rewiring consumers to it, not building a new mechanism.
  - The design subtlety that makes this a section rather than a patch: `panic_collect_evidence` is guarded by a one-shot atomic claim (`s_evidence_collected`), so on an SMP double panic the LOSING CPU returns without populating the record. A consumer that naively reads it would render the winner's strings as its own. Decide explicitly whether a loser renders the winner's evidence, its own locally-snapshotted copy, or a placeholder.
  - Contexts where the guarded read is unavailable (`PANIC_CTX_NMI`, `PANIC_CTX_UNKNOWN`) must emit a fixed placeholder rather than walking the original -- the rerouted ordinary path is exactly where an unguarded walk survives today.
- [ ] Commit: `"kernel: single guarded panic-string snapshot for every terminal renderer"`

**Test checkpoint:** With a deliberately unmapped panic description, the BSOD paints with a truncation placeholder and `write_crash_dump` completes, on QEMU TCG at 1 and 2 CPUs. No renderer dereferences `description` or `file` directly -- verified by grep at review time.

---

## OS Comparison

| ⭐  | Feature                     | 🪟 Win11                          | 🐧 Linux                        | 🚀 Impossible OS                             |
| --- | --------------------------- | --------------------------------- | ------------------------------- | -------------------------------------------- |
| 💎  | UC MMIO mapping             | ✅ MmMapIoSpace                   | ✅ ioremap_uc                   | ✅ §1 vmm_map_mmio_uc                        |
| 💎  | IST stacks                  | ✅ All critical exceptions        | ✅ IST1-4 DF/NMI/MCE            | ⚠️ §2 BSP IST1-3 (AP: T09 §10)               |
| 💎  | ACPI FADT boot arch         | ✅ HAL checks all flags           | ✅ Gates PIT/RTC/PS2            | ✅ §4 IAPC_BOOT_ARCH parsed                  |
| 💎  | PS/2 ACPI detection         | ✅ HAL detects i8042              | ✅ i8042.nopnp                  | ✅ §5 FADT + GSI routing                     |
| 💎  | AHCI MSI fallback           | ✅ StorAHCI INTx fallback         | ✅ libahci polled fallback      | ✅ §6 MSI→INTx→polled                        |
| 💎  | Graceful degradation        | ✅ Safe Mode + Last Known         | ✅ systemd continues            | ✅ §7 BOOT_TRY + degraded_mask               |
| 💎  | Per-process page tables     | ✅ Each process own CR3           | ✅ mm_struct per task           | ✅ §8 PML4 clone + CR3 switch                |
| 💎  | CPU security verify         | ✅ HAL verifies CR4/EFER          | ✅ Checks feature enable        | ✅ §9 verify NX/SMEP/SMAP                    |
| 💎  | Boot order / UEFI-safe      | ✅ Ordered HAL + RT serialize     | ✅ setup_arch + efi_call wrap   | ✅ §10 timer-last + rt_call mask             |
| 💎  | Logging on main FS          | ✅ C:\Windows\System32            | ✅ /var/log                     | ✅ §12 KLOG_DIR X:\ (T24)                    |
| 💎  | CPU feature minimums        | ✅ NX required since Vista        | ✅ verify_cpu required mask     | ✅ §13 NX+SSE2+LM+SYSCALL mask               |
| ⭐  | Bare-metal test matrix      | ❌ Internal only (WHQL)           | ❌ Community-driven             | ✅ §14 4-platform matrix                     |
| ⭐  | Boot spinner liveness       | ✅ ISR-driven ring                | ⚠️ plymouth (optional)          | ✅ §15 timer ISR @10fps Fluent               |
| 💎  | Abort-safe panic serial     | ✅ IPI-freezes CPUs before output | ✅ trylock UART + smp_send_stop | ⚠️ §16 try-lock + bounded UART (freeze: §18) |
| 💎  | Panic-string fault recovery | ✅ Probes before dereferencing    | ✅ probe_kernel_read fixups     | ✅ §17 `__kread_u8` RIP-keyed fixup          |

> **After §1--§15:** Impossible OS boots on any x86-64 hardware with the same reliability as Windows and Linux. User/kernel separation with per-process PML4; SMEP/SMAP where CPU and page tables allow (see §8--§9). Graceful degradation via `BOOT_TRY` (§7). Logging on BlackBox `X:\` (§12). External CPU sequencing remains in `TODO-09-cpu-boot-sequencing.md`.

---

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

| Platform   | Timer       | Calibration        | AHCI | Desktop  | Notes                 |
| ---------- | ----------- | ------------------ | ---- | -------- | --------------------- |
| QEMU WHPX  | LAPIC 100Hz | Tier 1 Hyper-V MSR | MSI  | ✅ 8.4s  | Primary dev           |
| QEMU TCG   | PIT 100Hz   | N/A (PIT)          | MSI  | ✅       | Fixed by clac removal |
| VirtualBox | LAPIC 100Hz | Tier 1b TSC ref    | INTx | ✅ 21.8s | NCQ timeout adds 15s  |
| Bare metal | LAPIC 100Hz | Tier 1b TSC ref    | MSI  | ✅       | Fixed by clac removal |

### BM Test 3 -- Driver Hardening (§5 + §6 + §7) ✅ PASSED 2026-03-29
> [!NOTE]
> PS/2 gated by ACPI FADT, mouse re-enabled with proper detection, AHCI hardened with LAPIC mask + ABAR validation, BOOT_TRY macro + degraded_mask infrastructure added. Also fixed: VBox mouse click (IOAPIC unmask in irq_request_gsi), VBox spinner speed (TSC calibration sanity check), VBox driver self-contained (buttons read internally).

- [x] PS/2 keyboard/mouse: skipped on QEMU (8042=0), active on VBox/bare metal (8042=1)
- [x] AHCI: MSI on QEMU/bare metal, INTx on VBox -- no crash
- [x] BOOT_TRY + degraded_mask: infrastructure in place for graceful degradation
- [x] Keyboard works on all platforms after GSI migration
- [x] VBox: mouse click working, spinner at correct speed (PM Timer calibration)

**Results (2026-03-29, post-§7):**

| Platform   | Boot Time | Timer       | Calibration         | AHCI | PS/2             | Status |
| ---------- | --------- | ----------- | ------------------- | ---- | ---------------- | ------ |
| QEMU WHPX  | 9.4s      | LAPIC 100Hz | Tier 1 Hyper-V MSR  | MSI  | Skipped (8042=0) | ✅     |
| QEMU TCG   | 3.1s      | PIT 100Hz   | N/A (PIT)           | MSI  | Skipped (8042=0) | ✅     |
| VirtualBox | 21.8s     | LAPIC 100Hz | PM Timer (TSC skip) | INTx | Active (8042=1)  | ✅     |
| Bare metal | ~10s      | LAPIC 100Hz | Tier 1b TSC ref     | MSI  | Active (8042=1)  | ✅     |

### BM Test 4 -- Memory Model (§8 + §9) ✅ PASSED 2026-03-29
> [!NOTE]
> Per-process page tables working on all 3 VM platforms. cmd.exe runs in its own PML4 with User bit on ELF + stack pages. CR3 switches on context switch. SMEP/SMAP still skipped (boot PML4 has User on all 2MiB pages -- needs kernel PML4 fix).
> **Retraction (2026-06-08, §9 review):** the "SMEP/SMAP: EPT-enforced on WHPX" rows below recorded a verify log line that was later removed as a FALSE security signal (EPT does not provide SMEP semantics). Historical record kept as-run; current behavior is `Verify: SMEP/SMAP skipped (kernel PTE User bit -- needs KPTI)` on every platform.

- [x] cmd.exe runs with own PML4, types input, shows output on all 3 VMs
- [x] NX verified: `Verify: NX enabled (EFER.NXE set)` on all platforms
- [x] ~~SMEP/SMAP: EPT-enforced on WHPX~~ RETRACTED (see note above) -- actual outcome: SMEP/SMAP not enabled on any platform; TCG/VBox lack the CPUID feature, WHPX log line was a false signal
- [x] No page faults, no triple faults (after fixing User bit on PML4/PDPT levels + user stack in PD[4])

**Results (2026-03-29, post-§9):**

| Platform   | Boot  | Per-Process PT | NX Verify   | SMEP/SMAP      | cmd.exe | Status       |
| ---------- | ----- | -------------- | ----------- | -------------- | ------- | ------------ |
| QEMU WHPX  | 10.3s | ✅ CR3 switch  | ✅ EFER.NXE | EPT enforced   | ✅      | ✅           |
| QEMU TCG   | 3.2s  | ✅ CR3 switch  | ✅ EFER.NXE | N/A (no CPUID) | ✅      | ✅           |
| VirtualBox | ~45s  | ✅ CR3 switch  | ✅ EFER.NXE | N/A (no CPUID) | ✅      | ✅           |
| Bare metal | TBD   | TBD            | TBD         | TBD            | TBD     | Next session |

### BM Test 5 -- Final Validation (after §11--§15)
Full acceptance pass. All sections complete.

- [ ] Full boot to desktop, timer running, no workarounds
- [ ] Force a non-critical init failure via `BOOT_TRY` path (§7); boot completes with degraded notification (no `boot.conf` skip list)
- [ ] Second boot: serial shows `[BOOT] Last boot succeeded` after a clean prior shutdown (`boot_hw.c` NVRAM path)
- [ ] Spinner smooth during PCI scan (no stutter)
- [ ] Boot time within thresholds for ALL phases (P0 <200ms, P1 <2s, P2 <10s, P3 <15s) -- capture the serial `PERF: --- Boot step durations (sorted by time) ---` table + any `BOOT-BUDGET` WARN/ERR lines as the per-phase artifact
- [ ] Per-process PT on bare metal: cmd.exe in own PML4, CR3 switch, NX verify -- closes the §14 matrix `TBD (BM Test 5)` cell and the BM Test 4 bare-metal TBD row
- [ ] Keyboard responsive, AHCI I/O works

**~20 minutes.** Full regression pass.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bare_metal()` (see [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) and `include/kernel/test/test.h`; same pattern as `02-kernel-core/TODO-11-peb-teb-user-abi.md` Unit Tests).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> Bare-metal hardening is primarily verified by multi-platform boot (QEMU WHPX/TCG, VBox, bare metal). Kernel unit tests cover the infrastructure APIs; full validation requires `scripts/test-smoke.sh` on each platform.
> `g_boot_info.degraded_mask` / `kernel_subsystem_apply_result` paths already have assertions in [`src/kernel/test/test_boot_init.c`](../../src/kernel/test/test_boot_init.c) -- extend `test_bare_metal.c` with MMIO/IST/ACPI APIs below; do not duplicate those mask tests unless a gap is found.

- [ ] Create `src/kernel/test/test_bare_metal.c` with:
  - `vmm_map_mmio_uc(0xFEE00000, 0x1000)` returns non-NULL; LAPIC ID read via returned pointer matches identity-mapped read
  - `vmm_unmap_mmio()` on the mapped region does not crash; subsequent access would fault (not tested, just unmap)
  - IST stacks allocated: `kernel_tss.ist1 != 0`, `kernel_tss.ist2 != 0`, `kernel_tss.ist3 != 0`
  - ACPI FADT flags: `acpi_has_8042()` returns 0 or 1; `acpi_has_cmos_rtc()` returns 0 or 1 (never crashes)
  - `acpi_msi_supported()` returns 0 or 1 (consistent with FADT)
  - hw-reduced override (§4): when `acpi_hw_reduced()` is set, `acpi_has_8042()`/`acpi_has_cmos_rtc()`/`acpi_has_vga()` all return 0 regardless of `boot_arch_flags`
  - `g_boot_info.degraded_mask == 0` on a clean boot (no subsystems failed)
  - Per-process PML4: `vmm_create_user_pml4()` returns non-NULL; `vmm_destroy_user_pml4()` frees without crash
  - `vmm_set_user_page()` on a valid PML4+virt succeeds (User bit is set in PTE)
  - CPU verification: `cpu_verify_hardening()` does not crash; logs NX/SMEP/SMAP status
- [ ] Add to `scripts/test-smoke.sh`:
  - Grep serial for `IAPC_BOOT_ARCH:` (FADT flags parsed)
  - Grep serial for `IST stacks:` (IST allocated)
  - Grep serial for `NX enabled` (CPU hardening ran)
  - Boot completes to desktop on QEMU TCG (PIT path) and QEMU WHPX (LAPIC path)
- [ ] Register in `test_runner_init()`: `test_register_bare_metal()`
- [ ] Commit: `"test: add bare_metal test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes after `test_bare_metal.c` and `test_register_bare_metal()` land; smoke script greps succeed on reference QEMU boot. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU WHPX: full boot to desktop, all subsystems OK, POST codes visible in serial log
- [ ] QEMU TCG: full boot to desktop with PIT timer path
- [ ] VirtualBox: full boot to desktop
- [ ] Bare metal (i5-11600K): BM Tests 1--5 all pass
- [x] No `HV_BAR()` calls remaining in codebase -- `hv_bar.h` deleted, replaced by TODO-15 VPD
- [ ] Commit: `"boot: bare metal hardening complete -- all platforms boot reliably"`

**Test checkpoint:** All Verification bullets pass on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal; BM Test 5 items complete; `tail -1 build/build.log` shows `=== BUILD OK ===`.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)
