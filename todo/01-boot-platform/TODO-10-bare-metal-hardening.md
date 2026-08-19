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
> **Current state (2026-04-11):** `clac` is removed from `isr_common_stub` (`isr_stubs.asm`). UC MMIO uses `vmm_map_mmio_uc` / `vmm_unmap_mmio` (`vmm.c`). IST stacks and TSS fields are allocated in `gdt.c` with IDT IST wiring in `idt.c`. FADT `IAPC_BOOT_ARCH` gates legacy devices (`acpi.c`). AHCI MSI setup masks the LAPIC timer around PCI MSI writes (`ahci_core.c`). `BOOT_TRY` + `g_boot_info.degraded_mask` live in `boot_init.h` with Phase 1--3 call sites. Per-process PML4 + CR3 switch live in `vmm.c` / `task.c`. Early boot reads prior POST from UEFI NVRAM and logs last-boot outcome (`boot_hw.c`, `boot_init.c`). **Still open (refreshed 2026-08-15 at file close-out):** §1 `hpet_read_ns()` (owner TODO-11 §1 UTS), §2 deliberate kernel stack-overflow BSOD test (the item is at §2, NOT §7 as this line said until 2026-08-15; §7 has no such item), §8 `pmm_mark_region_used` cleanup for fixed ELF phys + boot PML4 User-bit blocker for SMEP/SMAP on bare metal (§8, NOT §4 as this line said until 2026-08-15), BM Test 5, Verification checklist. **Closed since:** `src/kernel/test/test_bare_metal.c` + `test_register_bare_metal()` shipped 2026-08-15 (12 suites / 64 assertions, wired at `test_runner.c:985` and `:1114`). Dense per-function POST16 instrumentation beyond the `0xD1xx` bring-up set is owned by `TODO-15-visual-post-display.md`. **Gap-audit 2026-06-06 demotions:** §4 -> `[/]` (ACPI capability APIs ignore `acpi_hw_reduced()`; hw-reduced platforms wrongly report legacy devices present) -- RESOLVED 2026-06-27, §4 is back to `[x]`: `02-kernel-core/TODO-08` §5 added the `cmos_read()` hard gate and the fail-closed `s_rtc_available` latch, and §4 carries its own **Resolved:** stamp; §8 -> `[/]` (per-process PML4 ships, but "directly unblocks SMEP/SMAP" was false -- the boot PML4 still has User on kernel pages; real unblock owned by `D02 T10 §3-§6` KPTI) -- still open. **Newly named gaps:** nested-NMI safety on shared IST2 (blocking prereq for `TODO-23 §1` watchdog), and corrected/recoverable machine-check (RAS/WHEA) recovery -- currently every MCE panics; no owner exists, a dedicated RAS TODO is recommended.

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
- → XREF: `TODO-03-bootloader-error-recovery.md §6` -- serial port probe and COM2 fallback; §6 there detects serial presence in the bootloader, and what this file does with a reported absence is OPEN: the driver keeps COM1 when the bootloader reports 0, so there is no absent-serial degradation today -> XREF: `01-boot-platform/TODO-10 §30` (item: "Decide whether a bootloader-reported serial port of 0 means \"no UART\" and make the code agree with whichever answer is chosen." at line 1205)
- → XREF: `TODO-07-boot-entry-store-menu-policy.md §5` -- graceful-degradation flags from §7 here map to safe-mode entry flags in the boot entry store

## Outcome

- Boot path emits POST16 codes to I/O port 0x80, UEFI NVRAM (`boot_post_nvram_write16`), and serial; on-screen 4-digit VPD coverage is coordinated with `TODO-15-visual-post-display.md` / `TODO-14-boot-diagnostics.md` (not every subsystem is VPD-instrumented yet).
- Next boot: early serial shows last-boot outcome (`[BOOT] Last boot succeeded` / `failed` in `boot_hw.c`) plus prior POST16 read from NVRAM -- full WinRE-style recovery UI is `TODO-03` / `TODO-22`, not this file.
- Hardware interrupts work reliably on bare metal -- IST stacks, correct LAPIC delivery, verified ISR frame layout.
- PS/2, RTC, AHCI, and all legacy subsystems are gated by ACPI capability flags -- never touch hardware that doesn't exist.
- Any subsystem failure degrades gracefully with a log message instead of crashing.
- Non-critical failures use `BOOT_TRY` (§7) plus `degraded_mask` -- no silent `boot.conf` skip list.

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On | Status |
| --- | :---: | --------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Minimal UC MMIO mapping (`vmm_map_mmio_uc`)         | --         |  [x]   |
| 💎  |   2   | IST stacks for critical exceptions                  | --         |  [/]   |
| 💎  |   3   | Hardware interrupt root cause investigation         | §2         |  [x]   |
| 💎  |   4   | ACPI FADT boot architecture flags                   | --         |  [x]   |
| 💎  |   5   | PS/2 controller detection and safe init             | §4         |  [x]   |
| 💎  |   6   | AHCI interrupt hardening                            | §1, §3     |  [x]   |
| 💎  |   7   | Resilient boot with graceful degradation            | --         |  [/]   |
| 💎  |   8   | Per-process page tables (minimal base)              | §1         |  [/]   |
| 💎  |   9   | CPU security activation and verification            | §4, §8     |  [x]   |
| 💎  |  10   | Boot order hardening (timer-last, UEFI-safe)        | §3         |  [x]   |
| ⭐  |  11   | ~~`boot.conf` subsystem skip list~~                 | --         |  [x]   |
| 💎  |  12   | Logging and diagnostic storage moved to TODO-24     | §7         |  [x]   |
| 💎  |  13   | CPU feature minimum requirements and verification   | §4, §9     |  [x]   |
| 💎  |  14   | Bare-metal test matrix and validation plan          | §3         |  [x]   |
| 💎  |  15   | Boot splash spinner bare-metal fix                  | §3, §10    |  [x]   |
| 💎  |  16   | Post-ship follow-up backfill (2026-07-31 cohort)    | --         |  [x]   |
| 💎  |  17   | Emergency-writer robustness residuals (§16 review)  | §16        |  [x]   |
| 💎  |  18   | Panic-path cross-CPU ownership residuals            | §16, §17   |  [/]   |
| 💎  |  19   | Panic-path caller-string snapshot (§17 review)      | §17        |  [x]   |
| 💎  |  20   | Panic-path serial ownership atomicity (§18 review)  | §18        |  [x]   |
| 💎  |  21   | Live CPU online lifecycle (split from §20)          | §18        |  [x]   |
| 💎  |  22   | Dedicated NMI entry stub (split from §20)           | §18        |  [x]   |
| 💎  |  23   | Cross-boot evidence lifecycle + epoch (from §20)    | §19        |  [x]   |
| 💎  |  24   | Panic collector publication latency (from §20)      | §19, §23   |  [x]   |
| 💎  |  25   | GS-validated per-CPU identity cache (from §20)      | §20        |  [/]   |
| 💎  |  26   | Attributable emergency-ledger claim (from §20)      | §17, §20   |  [x]   |
| 💎  |  27   | Async worker quiescence + terminal bringup (§21)    | §21        |  [x]   |
| 💎  |  28   | Cross-boot evidence durability across a reset (§23) | §23        |  [x]   |
| 💎  |  29   | Abandoned-reservation reclamation on park (§26)     | §26        |  [x]   |
| 💎  |  30   | Nested abort after the async branch stops as async  | §27, §29   |  [x]   |
| 💎  |  31   | Guard page under the BSP boot stack                 | §2         |  [ ]   |

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
- [/] Verify: stack overflow in kernel → #DF fires on IST1 stack → shows BSOD instead of triple fault -- `operator-gated`, re-pointed at BM Test 5 (2026-08-15)
  - Re-pointed because its original target went stale under it: this was parked "deferred to BM Test 1", and BM Test 1 closed `✅ PASSED 2026-03-29` with no stack-overflow assertion in its checklist, so the park named a target that could no longer clear it. `test_bare_metal.c` (2026-08-15) does not cover it either, and deliberately so.
  - Not automatable from any host: proving it requires actually overflowing the kernel stack and observing a BSOD rather than a reset. A unit test cannot do that (it would take the test-runner boot down, and the test policy forbids reaching `panic`/halt paths), and no emulator distinguishes the outcomes usefully -- a triple fault under QEMU is a silent reset, which is exactly what a missing IST also looks like.
  - What IS covered mechanically: the IST stacks are allocated, page-aligned, mutually distinct and correctly index-mapped (`test_bare_metal.c`), and the guard page below each is installed by `ist_alloc` (`src/kernel/gdt.c:104`). Those establish the preconditions; only a real overflow establishes the outcome.
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
- [/] Add `SUBSYS_*` slots + `s_subsys_names[]` for the non-critical subsystems the enum lacks (keyboard, mouse, AHCI, NIC, SMBIOS, splash, DHCP) so degraded_mask can represent them -- parked with this section
  - Blocker: this is step 1 of the wrapping chain the section's `Deferred:` stamp names, and lands as one unit with it. Adding slots alone widens `degraded_mask` with bits nothing can ever set, which reads as coverage that does not exist.
- [/] Wire Phase 1-3 non-critical inits through `apply_result`: `rtc/keyboard/mouse/fb_init` are void and `ahci_init` return is ignored, so failures never set degraded_mask -- blocked, this section's named blocker
  - Blocker: this is the exact item the section's `Deferred:` stamp cites. It changes the signature of several Phase 1-3 init functions, so it is an all-callers change across the boot path rather than a local edit. Depends on the `SUBSYS_*` slots above.
- [/] Forced-failure regression: force one non-critical init to fail -> boot reaches Phase 3 -> degraded summary lists it -- blocked by the `apply_result` wiring above
  - Blocker: there is nothing to force until a non-critical init can report failure at all. The section's Test checkpoint stays non-functional until then, which is why this cannot be demonstrated in isolation.
- [/] Harden boot_phase0 handoff (Codex §2 review, defense-in-depth): pin pointer to canonical `BOOT_INFO_PHYS_ADDR`; validate `g_boot_info.fb` geometry before pre-`fb_init` VRAM writes; bulk-copy `boot_info` -> XREF: 02-kernel-core/TODO-01 §2
  - Blocker: owned by `02-kernel-core/TODO-01` §2, which holds the boot_phase0 handoff surface. Parked here rather than implemented, so the owner-side sweep can re-open it when that section ships.
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
> **Accepted:** [Critical] `map_user_page_impl` splits the shared kernel PD (PDPT[1]) for `uthread_create` stacks, breaking per-process isolation -> XREF: 03-memory-concurrency/TODO-01 §12 (item: "Privatize cloned kernel PDs before splitting a huge page outside PD[4]" at line 273)
> **Accepted:** [Critical] `task_exec` continues into `exec_load` after partial OOM remap, corrupting the parent image -> XREF: 03-memory-concurrency/TODO-01 §12 (item: "Make `task_exec` fork+exec private-frame remap atomic under OOM" at line 274)
> **Accepted:** [M] scheduler reads CR3 on every context switch instead of a per-CPU cache -> XREF: 02-kernel-core/TODO-10 §6 (item: "Context switch (`task_switch`): update `smp_this_cpu()->user_cr3`" at line 182)
> **Accepted:** [M] `task_exec` remap zeros 1 MiB scalar + per-page INVLPG -> XREF: 03-memory-concurrency/TODO-01 §12 (item: "In the `task_exec` private-frame remap loop, zero via `zero_page`" at line 275)
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

> **Deferred:** [L] `boot.conf` subsystem skip-list removed 2026-03-29 (code deleted, absent from `src/`), superseded by `BOOT_TRY` / `degraded_mask` -> XREF: 01-boot-platform/TODO-10 §7 (item: "`BOOT_TRY(subsys, fn_call, name)` macro" at line 340)

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
> **Deferred:** [H] bare-metal per-process PT run never recorded (BM Test 4 bare-metal row TBD) -> XREF: 01-boot-platform/TODO-10 BM Test 5 (item: "Per-process PT on bare metal" at line 1371)
> **Deferred:** [M] per-phase bare-metal timing artifact not captured -> XREF: 01-boot-platform/TODO-10 BM Test 5 (item: "Boot time within thresholds for ALL phases" at line 1370)
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
> **Accepted:** [M] the async-isolation park leaves `is_online` set, so later `boot_async_group` calls assign work to a dead CPU (reason: pre-existing, async-boot surface) (RESOLVED 2026-08-15 verification: §18 shipped the named item `[x]`, clearing `is_online` as a release store ordered before the `async_result`/`in_async_work`/`async_done` publish; §21 then replaced that store with the `smp_retract_*` online-mask path) -> XREF: 01-boot-platform/TODO-10 §18 (item: "Clear `is_online` when the async-isolation branch parks a faulting AP")
> **Accepted:** [H] an AP that faults while HOLDING `g_serial_lock` parks still owning it, so the surviving BSP hangs on its next ordinary serial write; §16 publishes `async_done` first so the fallback is reached, but real ownership needs lock poisoning/handoff (reason: SMP-ownership design call) (RESOLVED 2026-08-15 verification: §18 NARROWED but did not close this -- see its own residual note -- and §20 closed it fully with the fused owner-encoded atomic lock word, where §20's copy of the same finding is annotated RESOLVED 2026-08-14. This stamp pointed only as far as §18 and is now carried through to the real closure) -> XREF: 01-boot-platform/TODO-10 §18 (item: "Async-isolation lock ownership")
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
  - SUPERSEDED TWICE since, and recorded here rather than rewritten because this is what §17 shipped: §26 replaced the slot mask with the composite claim word, and §29 replaced `serial_emerg_refund_self` with `serial_emerg_reclaim_self` and deleted it. Neither symbol exists today -> XREF: `01-boot-platform/TODO-10 §29` (item: "Reclaim the abandoned reservations of every panic invocation on a CPU that is about to park forever, instead of only what the innermost dump spent.").
  - Residual, bounded and documented at `serial.c`: if that CAS loses every attempt the charges stay outstanding, and the next arming does NOT clear them because the pre-arbitration dump runs before arming. Cost is degraded evidence on a wedged UART, never a hang or a lost panic; eliminating it needs a two-word latch.
- [x] Recoverable writer check-to-emit race: `serial_emergency_emit` re-tests ownership before every PHYSICAL byte for recoverable output, bounding the residual to one byte.
  - Per-chunk was not enough (a chunk is 128 bytes), and per-input-character was not either -- a `\n` expands to `\r` plus `\n`, so a single test let two bytes through after an arm. The remaining one-byte window is the load-to-`outb` gap, which no lock can close: `serial_enter_emergency` never takes `g_serial_lock` and the emergency emitter proceeds when the try-lock fails.
- [x] Commit: `"kernel: emergency-writer robustness residuals -- fault-suppressed kernel reads, epoch-tokened UART reservation"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes with the epoch-token, per-CPU attribution, context-predicate and fixup-routing suites registered under `TEST_CAT_BOOT`; a token minted before an epoch reset cannot spend the new epoch's charge, and one CPU's refund leaves another CPU's charge standing. The live corrupt-pointer recovery needs a real #PF and stays serial-validated -- the routing DECISION is unit-tested instead, and that gap is deliberate, not claimed as covered.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 23 `serial_emergency` suites (11 pre-existing + 12 added), 28511 kernel + 17 user tests pass, 0 failures

> **Notes:**
> - Shipped: `__kread_u8` + `kread_u8_fixup_lookup`, the `PANIC_CTX_*` declared-context API, guarded walks in the emergency writer and `pe_copy`, slot-bitmap + generation latch accounting with a per-CPU ledger, per-byte recoverable ownership retest.
> - How it runs: the guarded read reuses the RIP-keyed table beside `__kstack_read_u64`; callers declare context from `frame->int_no` rather than probing state a panic may have corrupted; the latch keeps its single-word atomic publish.
> - Downstream effects: closes §16's Accepted [H] corrupt-caller-string and [M] non-attributable-charge findings; `serial_emerg_reserve` changed signature (`int` -> `uint32_t` token) and every caller moved with it.
> - Scope boundary: the crash REASON path only -- the terminal renderers downstream were rerouted onto one guarded snapshot by §19 (shipped). Cross-CPU panic ownership is §18; nested-NMI latch/replay is §2; the panic-safe emitter stays `02-kernel-core/TODO-27 §7`.
> - Canonical doc: the emergency accounting block in `src/kernel/drivers/serial.c` plus the `PANIC_CTX_*` block in `include/kernel/drivers/serial.h`.

> **Verified:** 2026-08-13 | commit `98dbbb632` + review fixes | 4/4 items | build OK | 28511 kernel + 17 user PASS | smoke matrix 4/4 (KVM/TCG x 1/2 CPU) | release flavor compiles | lint 0 errors
> **Accepted:** [H] terminal renderers (BSOD, `printk`, `write_crash_dump`) still walk the ORIGINAL `description`/`file`, so a corrupt pointer can still kill the dump after the reason is out -- RESOLVED 2026-08-13 by §19 (`panic_snapshot_str` + the stack-local snapshot; the double-panic decision is "each CPU renders its own copy") -> XREF: 01-boot-platform/TODO-10 §19 (item: "Take ONE bounded, guarded snapshot of `description` and `file` at panic entry")
> **Accepted:** [H] nested-abort context: `frame->int_no` names only the innermost vector, so a fault inside the NMI handler re-enables the guarded read while the outer NMI owns IST2 (reason: needs a per-CPU NMI-depth or RSP-in-IST2 signal, shares the per-CPU TSS/IST blocker) -> XREF: 01-boot-platform/TODO-10 §18 (item: "Nested-abort panic context")
> **Accepted:** [H] `serial_emerg_reserve` publishes the global slot before its per-CPU ledger entry, so an abort in that window leaves one unattributable charge (reason: the reviewer-proposed ledger-first order was implemented and proved strictly worse -- it lets a nested refund clear ANOTHER CPU's live charge; irreducible without per-slot owner records, which do not fit the 32-bit latch) -> XREF: 01-boot-platform/TODO-10 §26 (item: "Composite owner-plus-generation slot claim") -- re-pointed 2026-08-14: §18 shipped SERIAL-LOCK ownership, a different word from the reservation latch, and the §20 design review showed an owner-only entry cannot carry the token generation, so the fix is its own state machine
> **Accepted:** [M] the panic frame-chain walk still dereferences `frame_ptr[0]`/`[1]` raw behind a hardcoded `rbp` window, in the same abort context this section hardened for strings (reason: adjacent and pre-existing; `__kstack_read_u64` already exists for it) -> XREF: 01-boot-platform/TODO-10 §19 (item: "Guard the panic frame-chain walk the same way")
> **Accepted:** [M] the guarded caller-string walk pays a `noinline` protected-read call per byte (reason: a span-copy primitive trades away byte-precise fault position and adds a second guarded-read mechanism; the 1024-char cap already bounds the cost) -> XREF: 01-boot-platform/TODO-10 §19 (item: "Guard the panic frame-chain walk the same way")
> **Quality reviewed:** 2026-08-13 | Codex 11x (design, adversarial x2, test-coverage x2, re-adversarial x4, consistency, perf) + kernel-quality-auditor + concurrency-evidence-mapper | 9H+11M+2L fixed, 3H+2M accepted-XREF | scope: kernel-code-quality

---

## 18. Panic-Path Cross-CPU Ownership Residuals (from the §16 review)

> **Spawned-by:** §17 (split)
> **User impact:** One isolated async-init fault currently costs every LATER async group a full 10-second barrier timeout, turning a recovered boot into a visibly stalled one; and if that AP faulted while holding the serial lock, the surviving BSP hangs on its next ordinary log write -- a silent hang instead of a boot. Meanwhile a crash dump can be captured while other CPUs are still mutating the very state it describes, so the evidence a user sends in may not reflect the machine at the moment it died.

The three §16 review residuals that are about WHICH CPUs are running and who owns what while the panic path writes -- split out of §17 before implementation because they are SMP-ownership calls on `panic.c` / `smp.c`, not changes to the serial writer primitive (§17). §16 rewrote the async-isolation block these sit inside but deliberately did not change CPU ownership.

- [x] Clear `is_online` when the async-isolation branch parks a faulting AP (`panic.c`), so later `boot_async_group` calls stop assigning work to a dead CPU and eating the full 10s barrier timeout per group.
  - Shipped as a RELEASE store ordered BEFORE the `async_result`/`in_async_work`/`async_done` publish, so a BSP that observes completion cannot then observe this CPU as still online and hand it the next group's work. §21 replaced that store with `smp_retract_cpu_online()`, which additionally clears the live online-mask bit and parks the CPU's async claim.
  - Scope, stated because the review probed it: this fixed async DISPATCH only, and `smp_cpu_count()` remained a one-time boot snapshot that a parked AP already contradicted -- a CPU halted forever was still counted active. CLOSED by §21, which made the count the population of a live mask -> XREF: `01-boot-platform/TODO-10 §21` (item: "Live online-CPU mask distinct from the boot discovery snapshot").
- [/] Freeze other CPUs on panic entry: Win11 (`KiFreezeTargetExecution` IPI) and Linux (`smp_send_stop`) both halt every other CPU; we elect a panic owner but never stop healthy CPUs, which keep mutating the state the dump is capturing.
  - DEFERRED, and the design review is what stopped it: freezing today makes the panic path strictly WORSE. `write_crash_dump` (`panic.c:874-893`) does live VFS work -- `vfs_open`/`vfs_create`/`vfs_write`/`vfs_flush` -- and `fat32_write` (`fat32_ops.c:216-220`) both `kmalloc`s and takes `spin_lock(&vol->lock)`. A freeze IPI can halt a CPU holding that lock or the heap lock, after which the owner blocks forever on a lock that would otherwise have been released, and the dump never lands. Today's behaviour merely RACES the dump; the freeze would DEADLOCK it.
  - The blocker is a lock-free, allocation-free dump pipeline, which is owned and unbuilt -> XREF: `02-kernel-core/TODO-27 §7` (item: "`dump_sink_write(buf, size)` -- streaming write engine"). Re-open once the raw-partition sink bypasses VFS; the send primitive (`lapic_send_ipi_all_but_self`, `lapic.c:306`) and the registered-handler pattern (`IPI_VECTOR_CR_VERIFY`, `cpu_security.c:1656`) already exist, so what is missing is the safety precondition, not the mechanism.
  - Deliberately NOT an NMI-based freeze when it does land: per-CPU TSS/IST is parked (`gdt.c:121-125`, BSP-only IST1-3), so an NMI to an AP has no guaranteed IST stack -> XREF: `01-boot-platform/TODO-10 §2` (item: "AP per-CPU TSS/IST: BSP-only today").
- [x] Async-isolation lock ownership: an AP that faults while HOLDING `g_serial_lock` publishes `async_done` and parks forever still owning it, so the surviving BSP hangs on its next ordinary serial write.
  - Shipped as owner tracking, not routing, exactly as the item required: `g_serial_lock_owner` (0 = free, 8-bit initial APIC ID + 1 = holder) is paired with the lock across all FOUR acquisition paths -- the three ordinary `serial_lock_acquire`/`serial_lock_release` sites and the emergency try-lock in `serial_emergency_emit`, which the first design missed and the adversarial review caught. The park calls `serial_lock_release_if_owner()` before `hlt`, compare-exchanging on its own id so it can never take the lock from a live holder.
  - Identity is the CPUID-derived APIC id, not `smp_this_cpu()`, so the panic path does not depend on a GS base it cannot trust -- the same rule §17's per-CPU ledger follows.
  - Residual, NARROWED not closed: the owner store is a separate instruction from the flag CAS, and `cli` does not mask NMI or `#MC`, so an abort inside that two-instruction window still strands the lock. Was a guaranteed hang before, is now a two-instruction window -> XREF: `01-boot-platform/TODO-10 §20` (item: "Fuse `g_serial_lock` ownership and lock state into ONE atomic transition").
- [x] Nested-abort panic context: `frame->int_no` names only the INNERMOST vector, so a fault INSIDE the NMI handler re-enters panic classified `PANIC_CTX_NORMAL` and re-enables the guarded read on IST2.
  - Shipped as a per-CPU NMI-depth counter, NOT the RSP-within-IST2 range test the item offered as an alternative: IST2 is shared across CPUs while per-CPU TSS is parked, so an address-range test would be wrong on SMP for the same reason the freeze cannot use NMI. The depth signal is independent of that blocker, which is why this closed and the freeze did not.
  - `s_nmi_depth[256]` in `idt.c`, indexed by the CPUID APIC id so the GS-independent panic emitters can read it. `isr_handler` raises it as its FIRST action -- before the frame-integrity and GS self-pointer checks, which can themselves fault -- and lowers it as the last C statement before `return`, NOT at `irql_restore`, which is still followed by frame dereferences, a `klog` and a transition record. Both boundaries moved there because the adversarial review showed the obvious placements left live windows.
  - `panic_declared_ctx(frame)` now replaces all three vector-only classification sites and returns `PANIC_CTX_NMI` on `int_no == VECTOR_NMI` OR `idt_in_nmi()`. Unbalanced exits saturate at zero rather than wrapping, so a stray lower cannot pin a CPU in permanent NMI context.
- [x] Commit: `"kernel: panic-path cross-CPU ownership -- async-park is_online, serial-lock ownership, NMI-depth panic context"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes with the ownership-handoff and NMI-depth suites registered under `TEST_CAT_BOOT`: the recorded owner releases the fixture lock, a foreign owner releases nothing and leaves the holder intact, the NMI depth nests and saturates at zero, and `panic_declared_ctx` classifies a page-fault frame as `PANIC_CTX_NMI` while the depth is nonzero. The CPU-freeze half of this section is deferred, so it is deliberately NOT part of this checkpoint. The live cases -- a real AP faulting while holding `g_serial_lock`, and a real fault taken inside a real NMI handler -- need a second CPU and a real abort, and stay boot-matrix and bare-metal validated rather than claimed here.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 7 new suites (39 assertions), 28550 kernel + 17 user tests pass, 0 failures

> **Notes:**
> - Shipped: async-park `is_online` release-store, `g_serial_lock_owner` paired across all four acquisition paths plus `serial_lock_release_if_owner()`, and `s_nmi_depth[256]` feeding a new `panic_declared_ctx()`.
> - How it runs: ownership, NMI depth and the crash-evidence cpu_id all key off ONE shared `cpu_panic_safe_apic_id()` (`cpu_security.h`), so they provably mean the same CPU without depending on a GS base a panic cannot trust.
> - Downstream effects: closes §17's Accepted [H] nested-abort finding; adds `panic_declared_ctx` and `serial_lock_try_release_owned` as public surfaces.
> - Scope boundary: 3 of 4 items shipped -- the panic CPU freeze is deferred behind a lock-free dump pipeline, and five narrowed-but-open residuals are owned by §20--§22 (§20 serial lock, §21 CPU lifecycle, §22 NMI stub). The NMI depth covered the faultable BODY of `isr_handler` only, not the asm prologue or epilogue -- the boundary AS §18 CLOSED IT; §22 has since moved the marker into a dedicated vector-2 stub, and the current boundary is stated there.
> - Canonical doc: the ownership block beside `g_serial_lock` in `src/kernel/drivers/serial.c` and the NMI-depth block in `src/kernel/idt.c`.

> **Deferred:** [item 2] Freeze other CPUs on panic entry -- shipping it today DEADLOCKS the crash dump rather than protecting it: `write_crash_dump` (`panic.c:874-893`) does live VFS work and `fat32_write` (`fat32_ops.c:216-220`) `kmalloc`s and takes `spin_lock(&vol->lock)`, so a frozen lock holder blocks the panic owner forever (reason: needs a lock-free, allocation-free dump path; the IPI mechanism already exists) -> XREF: 02-kernel-core/TODO-27 §7 (item: "`dump_sink_write(buf, size)` -- streaming write engine")
> **Accepted:** [M] `g_serial_lock` ownership and lock state are two ordered stores, not one atomic transition, so an NMI or `#MC` between them still strands the lock unattributed (reason: fusing them means an owner-encoded lock word plus a reimplementation of `spin_lock_irqsave`'s IRQL raise/lower, a rewrite of the panic path's most critical primitive) -> XREF: 01-boot-platform/TODO-10 §20 (item: "Fuse `g_serial_lock` ownership and lock state into ONE atomic transition") -- RESOLVED 2026-08-14: §20 shipped `serial_lock_t`, so the two ordered stores are one CAS and the window is closed
> **Accepted:** [M] the `is_online` release store cannot retract a `boot_async_group` worker scan that already completed, so a group overlapping a timed-out worker can still dispatch to a parked AP (reason: needs an atomic worker lifecycle claim shared by dispatch, timeout, completion and panic-offline) -> XREF: 01-boot-platform/TODO-10 §21 (item: "Replace `boot_async_group`'s one-time `is_online` snapshot with an atomic per-worker lifecycle claim")
> **Accepted:** [M] `smp_cpu_count()` still reports a parked CPU as active, so NT processor reporting and topology disagree with the live online set (reason: pre-existing -- a halted AP was already counted; converting the active-CPU consumers to a live mask is separate work) -> XREF: 01-boot-platform/TODO-10 §21 (item: "Live online-CPU mask distinct from the boot discovery snapshot")
> **RESOLVED 2026-08-14 by §22** (was Accepted [H]): the depth is now raised by a dedicated vector-2 stub ahead of its own error-code push, so the shared prologue after the marker and the `frame->int_no` load are covered, and the NMI copy of the stub body lowers it past the register pops and the frame pop. Two residuals remain and are stated at §22: four pre-marker stack writes, and the whole return block -- one register restore, the CS test, the conditional VERW block, the `swapgs` and `IRETQ` -- which runs with the depth down so that `VERW` stays the last instruction to touch memory before returning. Original text: the NMI depth is raised inside `isr_handler`, so the asm stub prologue and the `frame->int_no` load ahead of it stay untracked and a fault there still classifies as ordinary (reason: vector 2 shares the generic stub, so closing it needs a dedicated NMI entry stub; a per-vector test in the shared stub would be paid by every interrupt on the machine) -> XREF: 01-boot-platform/TODO-10 §22 (item: "Dedicated NMI entry stub that raises the NMI depth before ANY common faultable work")
> **Accepted:** [M] every ordinary serial acquisition executes a serializing CPUID inside `g_serial_lock` (reason: the proposed `smp_this_cpu()->lapic_id` substitution was REJECTED -- that value comes from the LAPIC ID register and the ACPI MADT, a different derivation, and mixing sources makes the park-time compare-exchange silently never match; the cache must be fed from the same helper) -> XREF: 01-boot-platform/TODO-10 §25 (item: "GS-validated no-fallback per-CPU accessor") -- re-pointed 2026-08-14: §20 closed the stated harm by hoisting the CPUID out of the lock-held region; the CACHE moved to §25 because reading it through `smp_this_cpu()` would let an AP record the BSP as owner
> **Verified:** 2026-08-13 | commit `228ff0d7c` + review fixes | 3/4 items shipped, 1 deferred | build OK | 28550 kernel + 17 user PASS | smoke matrix 4/4 (KVM/TCG x 1/2 CPU) | lint 0 errors
> **Quality reviewed:** 2026-08-13 | Codex 7x (design, adversarial x2, test-coverage, consistency, perf, re-adversarial x2) + kernel-quality-auditor + concurrency-evidence-mapper | 3H+3M+6L fixed, 1H+3M accepted-XREF, 1 deferred | scope: kernel-code-quality

---

## 19. Panic-Path Caller-String Snapshot (from the §17 review)

> **Spawned-by:** §17 (review)
> **User impact:** A crash whose corruption reached the panic description now reports its reason on serial (§17), but the machine can still die before finishing the job: the BSOD never paints and the disk crash dump never lands, because the terminal renderers walk the original pointer again. The user sees the reason scroll past on a serial cable and gets no on-screen bugcheck and no dump file to send.

§17 guarded the three sites that produce the crash REASON (`pe_copy`, the emergency serial walk, the async diagnostic). The §17 review then found the renderers DOWNSTREAM of that still dereference the originals, which is the same defect one layer later: fixing individual call sites does not make a pointer safe, only routing every consumer through one safe copy does.

- [x] Take ONE bounded, guarded snapshot of `description` and `file` at panic entry; every later consumer reads the snapshot, never the caller pointer (`panic.c` serial, framebuffer and `write_crash_dump` paths).
  - `panic_snapshot_str()` (`panic.c:787`, exported in `panic.h`) copies each caller string once into stack-local `desc_snap[256]` / `file_snap[64]` in `panic_screen_impl`; the parameters were renamed `description_in` / `file_in` and now appear at only those two call sites, so "no renderer walks the original" is greppable rather than asserted.
  - Rewired consumers: the evidence record, the emergency serial dump, the async-isolation diagnostic, the no-framebuffer serial fallback, the BSOD `printk`, and `write_crash_dump`.
  - Storage is stack-local, not shared: on an SMP double panic the CPU that loses the record still renders its OWN reason instead of the winner's, and per-invocation storage needs neither GS nor a lock, which the pre-arbitration dump requires.
  - `PANIC_CTX_NMI` / `PANIC_CTX_UNKNOWN` render `PANIC_STR_NO_GUARD` without touching the pointer; a pointer unreadable from its first byte renders `PANIC_STR_UNREADABLE`; a NULL source renders the caller fallback, or an empty string when that is NULL too (never a walk of address zero).
- [x] Guard the panic frame-chain walk the same way: it still dereferences `frame_ptr[0]`/`[1]` raw, gated only by a hardcoded `rbp` window, in the identical abort context §17 hardened for caller strings.
  - `panic_capture_frames()` (`panic.c:843`) replaces BOTH raw walks (BSOD and `write_crash_dump`) with `__kstack_read_u64` plus the `rtl_capture_stack_from_context` validity rules -- 8-aligned, at/above the interrupted RSP with the whole slot pair inside `rsp + PANIC_STACK_SPAN`, strictly climbing, canonical. The `0x100000`/`0x200000` window is deleted.
  - It returns 0 without touching memory when the context forbids the guarded read, because the walk is the route by which a nested #PF re-arms NMI delivery (`panic_declared_ctx`); both renderers then print `PANIC_TRACE_NO_GUARD` rather than an empty trace a reader would misread as a corrupt chain.
- [x] Evidence ownership reworked so the snapshot cannot cost the record: the reserve helper claimed it in ONE compare-and-swap at panic entry. SUPERSEDED by §23 -> XREF: `01-boot-platform/TODO-10 §23`.
  - Completion was the PAIR of a published flag and the page magic. §23 moved the claim to terminal arbitration and collapsed completion into one epoch-keyed publication word, so both the helper and the pair are gone.
  - Order is flag FIRST, magic LAST (`panic.c:1367-1368`), and the magic is zeroed at the start of population, so no instruction boundary between the two stores lets a nested abort erase a valid record. The magic is the final publication instruction the pair-gate observes.
  - A nested abort that interrupted an unfinished record writes a complete one of its own; a finished record is never overwritten. Replaces the entry-claimed `s_evidence_collected` one-shot, which made population irrevocable before anything durable existed.
- [/] PARKED: prove a frame slot is RAM-backed kernel stack before reading it -- a corrupt RSP puts the whole search span inside mapped MMIO, where a read can have side effects `__kstack_read_u64` does not recover.
  - Co-owned, because the exposure is identical in the repo reference walker: `rtl_capture_stack_from_context` (`src/kernel/rtl/unwind.c:1208-1239`) applies the same span-and-canonical rules -> XREF: `02-kernel-core/TODO-23 §7` (item: "`src/kernel/rtl/unwind.c` -- `RtlCaptureStackBackTrace` + `rtl_capture_stack_from_context`: bounds-checked RBP walk").
  - NOT repaired by restoring the deleted `0x100000`/`0x200000` window: that window rejects every real AP, IST and kernel-task stack, which is why the old walk usually produced nothing. The fix needs a lock-free, GS-independent stack-residency test that does not exist in the tree yet.
- [/] PARKED: end-to-end renderer verification under an injected corrupt-description panic -- operator-gated, since it needs a deliberate panic on a live boot rather than a unit test.
  - The unit suite proves the guarded helpers recover a real `#PF` (`Crash: snapshot recovers a real fault`, `Crash: frame walk recovers a real fault`), but nothing invokes `panic_screen_impl`, the framebuffer render, or `write_crash_dump`, so "the BSOD paints and the dump completes" is reasoned from the rewiring rather than observed.
  - Belongs with the bare-metal checkpoints below; the same boot can confirm the evidence reserve/publish state machine, which is likewise unreachable from a unit test.
- [/] PARKED: live double-panic checkpoint for the evidence reserve/publish state machine -- operator-gated, for the same reason.
  - `panic_evidence_take()` (the entry-time reserve helper it replaced in §23) mutates boot-global ownership, so calling it from a test would lock a later real panic out of the record, and the pair-gate alone reduces to a tautological constant test. The reachable cases (first-owner CAS, a losing CPU writing nothing, nested re-entry completing an unfinished record) all need two real panics.
  - The same boot also verifies the lifecycle rework this section's review scoped out, which SHIPPED in §23 -> XREF: `01-boot-platform/TODO-10 §23` (item: "Rework the cross-boot evidence lifecycle so ownership is decided at TERMINAL arbitration").
  - It is now also the only way to exercise §23's remaining residual -> XREF: `01-boot-platform/TODO-10 §23` (item: "PARKED, operator-gated: a terminal takeover that cannot prove writer quiescence").
- [x] Commit: `"kernel: single guarded panic-string snapshot for every terminal renderer"`

**Test checkpoint:** `panic_snapshot_str` renders the fault placeholder after a real `#PF` on a canonical-unmapped source and the no-guard placeholder for the SAME address in NMI context; `panic_capture_frames` records a synthetic chain, clamps to `PANIC_MAX_STACK_DEPTH`, and walks nothing in a forbidden context; the full suite and the 4-leg smoke matrix (TCG and KVM at 1 and 2 CPUs) stay green. No renderer dereferences `description` or `file` directly -- verified by grep at review time (`description_in`/`file_in` reach only the two snapshot calls). End-to-end BSOD-and-dump behaviour under an injected corrupt description is parked above, not claimed here.

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- NEW `src/kernel/test/test_panic.c` (`test_register_panic`, 24 `Crash: ...` cases under `TEST_CAT_BOOT`), split out of `test_crashdump.c` so the tests bind to the source they cover; full suite 28591 kernel + 17 user-mode, 0 skipped; smoke matrix 4/4 legs.

> **Notes:**
> - Shipped: `panic_snapshot_str()` and `panic_capture_frames()` in `panic.c` (both exported in `panic.h`), plus the evidence reserve/publish split -- one guarded read of each caller string per panic, one fault-safe frame walker for both renderers.
> - Integrates by rebinding: `panic_screen_impl`'s parameters are `description_in`/`file_in` and reach only the two snapshot calls, so every consumer downstream is structurally unable to walk the original.
> - Downstream: the BSOD and disk crash dump now survive a corrupt description pointer that previously killed them after the serial reason had already printed; `PANIC_CTX_NMI` trades the reason for not re-arming NMI over live IST frames.
> - Canonical doc: `panic.h` carries the contracts (placeholder set, context gating, ownership/publication rules); `docs/infrastructure/bare-metal-gotchas.md` owns the panic-path rules this obeys.
> - Scope boundary: the guarded-read primitives themselves are §17 and `cpu_security.h`; cross-CPU panic ownership is §18 and its residuals §20--§24; the panic-safe emitter stays `02-kernel-core/TODO-27 §7`. Three residuals are parked in the checklist above with named owners: stack-slot residency (shared with `TODO-23 §7`) and two operator-gated live-panic checkpoints.

> **Verified:** 2026-08-13 | commit `418604911` + review fixes | 4/7 items (3 parked) | build OK | 28591 kernel + 17 user PASS, 0 skipped | smoke matrix 4/4 (KVM/TCG x 1/2 CPU) | lint 0 errors
> **Accepted:** [H] a recovered async fault publishes a surviving cross-boot record, so the next boot reports a crash that never happened -- and while it holds the slot it refuses the record to the crash that does kill the machine (reason: pre-existing, not introduced here; two repairs were attempted during this review and BOTH raced, so it needs an evidence-lifecycle design rather than a patch) -> XREF: 01-boot-platform/TODO-10 §23 (item: "Rework the cross-boot evidence lifecycle so ownership is decided at TERMINAL arbitration" at line 927)
> **Accepted:** [H] the evidence page has no epoch, so consume/restore/collect each check identity ad hoc and none is atomic against a concurrent panic (reason: pre-existing and shared with the lifecycle rework; four independent patches would not fix it) -> XREF: 01-boot-platform/TODO-10 §23 (item: "Make the evidence page's readers and writers agree on an epoch" at line 935)
> **Accepted:** [H] the collector spends ~170k instructions of bitwise CRC plus ~2,900 guarded byte-read calls before the record is durable, while a table-driven CRC already linked in this kernel would cut ~88% of the first part (reason: the checksum must be proven byte-identical to Phase-0 restore first, and the byte-read cost needs a new guarded primitive) -> XREF: 01-boot-platform/TODO-10 §24 (item: "Cut the pre-publication latency of the panic collector" at line 974)
> **Quality reviewed:** 2026-08-13 | Codex 8x (adversarial, consistency x2, perf x2, re-adversarial x3) | 3H+6M+3L fixed, 3 open | scope: kernel-code-quality

---

## 20. Panic-Path Serial Ownership Atomicity (from the §18 review)

> **Spawned-by:** §18 (review)
> **User impact:** Each item here is a NARROWED version of a bug §18 fixed, not a new one, so the user-visible cost is a rare repeat of a failure that used to be routine: a crash that lands inside a two-instruction window can still hang the machine on serial instead of logging why it died. Short of that, every ordinary log line pays a serializing CPUID inside the UART lock -- a VM exit under KVM and WHPX -- while other CPUs spin waiting for it, and an emergency reservation aborted mid-publish leaves a wedged-UART charge that can never be attributed or refunded.

The SERIAL-PATH residuals §18 narrowed but could not close atomically, plus the reservation-latch item §17 parked against §18 that §18 did not cover. All three are one defect shape in one subsystem: ownership state published in two steps, where the abort entries this code exists to serve (NMI, `#MC`) pierce `cli` and can land between them. Each needs a primitive §18 deliberately did not rewrite -- an atomic state transition where §18 has two ordered stores. Filed as its own section rather than as items in §18 because §18 is shipped and stamped.

Split 2026-08-13 (9 work items, over the one-worker-context threshold): the worker-lifecycle, NMI-stub, evidence-lifecycle and collector-latency residuals moved to §21--§24 respectively. They shared an origin review with these three, not a subsystem or a primitive.

- [x] Fuse `g_serial_lock` ownership and lock state into ONE atomic transition, so an abort between the flag CAS and the owner store cannot strand the UART lock unattributed.
  - `serial_lock_t` (`serial_emergency.h`) replaces the `spinlock_t` + separate `g_serial_lock_owner` pair with ONE word: `SERIAL_LOCK_FREE` = free, `SERIAL_LOCK_OWNER_OF(id)` = held. All four transitions are single atomics -- acquire CAS 0 -> owner, release-store 0, non-blocking try-acquire, force-release CAS exact-owner -> 0 -- so no instruction boundary exists at which the word says held-by-nobody. `serial_lock_note_owner`/`serial_lock_clear_owner` are deleted, and the two-store window with them.
  - A DISTINCT type, not a re-encoded `spinlock_t`: the latter would still compile against every generic `spin_lock`/`spin_trylock` in the tree, each storing a bare 1 and silently recording "owned by APIC id 0". `spin_lock_irqsave` could not be reused, so its IRQL discipline is reimplemented verbatim in `serial_lock_acquire`/`serial_lock_release` (RFLAGS save, `cli`, previous KIRQL packed into bits 56-63, raise to `DISPATCH_LEVEL`, restore on release); the try-acquire path lowers no IRQL it never raised -> XREF: `01-boot-platform/TODO-10 §18` (item: "Async-isolation lock ownership").
  - Identity is the raw CPUID-derived `cpu_panic_safe_apic_id()`, never `smp_this_cpu()`, and the design review made that load-bearing rather than stylistic: `smp_this_cpu()` falls back to `&cpu_data[0]` when GS is unset (`smp.c:591`), so a GS-less AP would record the BSP as owner and the real BSP could then force-release the AP's LIVE lock. GS is consulted only for the IRQL, exactly as the generic primitive already did.
  - Layer-1 asserts pin the encoding to one value across three headers (`SERIAL_LOCK_ID_MASK` == `SERIAL_EMERG_CPU` == `CPU_PANIC_SAFE_ID_MASK`, and no id encodes to FREE); a divergence would fail no build and no test, it would just stop the compare-exchange matching above the narrower mask and restore the hang silently.
- [/] PARKED: cache the panic-safe APIC id per CPU. The harm named here -- CPUID inside `g_serial_lock` -- is CLOSED: the derivation is hoisted before the CAS loop. Residual: one CPUID per acquisition, now outside the lock.
  - The per-CPU cache itself is deliberately NOT shipped. Reading it needs `smp_this_cpu()`, whose unset-GS fallback to `&cpu_data[0]` would let an AP record the BSP as lock owner and hand the BSP a successful force-release of the AP's live lock -- strictly worse than the CPUID cost it saves. Closing it needs a no-fallback accessor that validates the real GS base, plus a guarantee the cache is populated BEFORE GS becomes observable on every BSP and AP path -> XREF: `01-boot-platform/TODO-10 §25` (item: "GS-validated no-fallback per-CPU accessor").
- [x] DELIVERED BY §26: per-slot owner records for the emergency terminal-charge ledger, so a reservation aborted between the global slot publish and the per-CPU ledger entry stays attributable.
  - Parked by §17 against §18, and §18 did not cover it: §18's item was the SERIAL LOCK, a different word from the reservation latch. `serial_emerg_reserve` published the slot bitmap before recording the charge in the per-CPU ledger, so an abort in that window left one unattributable charge.
  - The design review showed the obvious repair does not hold: an owner-only per-slot entry cannot carry the token generation, which is read from the exact latch word the winning CAS installs, and the OFF->ARMED publication clears the latch bitmap in that same CAS while a separate array would keep pre-arm claims alive into the fresh epoch. It needs a composite owner-plus-generation entry with claim, return, refund, stale reclamation and epoch rollover reviewed as ONE state machine -> XREF: `01-boot-platform/TODO-10 §26` (item: "Composite owner-plus-generation slot claim").
  - Closed 2026-08-14: §26 made the composite claim word AUTHORITATIVE rather than publishing a latch bit behind it, so the window this item names no longer exists in either order. The two-word ledger is deleted.
- [x] Commit: `"kernel: owner-encoded serial lock word -- ownership and lock state in one transition"`

**Test checkpoint:** Fixture tests over a caller-supplied `serial_lock_t` prove the acquire records its owner in the same word that takes the lock, that a non-owner's force-release leaves a live holder byte-identical, that an already-free lock is not claimed by a release, that `SERIAL_LOCK_FREE` is refused as an identity on BOTH the acquire and release sides, and that the encoding is injective across every id the mask admits. Same-owner re-entry is REFUSED rather than treated as reentrant success (the panic/NMI shape, where granting it would let the inner release free the outer live lock), and the handoff wrapper's raw-id-to-encoded step is exercised through `serial_lock_release_if_owner_for` at raw ids 0 and the top of the mask -- the one conversion every encoded-owner fixture would otherwise miss. The full suite 29132 kernel + 17 user-mode passes, and the 4-leg smoke matrix (KVM and TCG at 1 and 2 CPUs) is green -- the last is load-bearing here, because this rewrites the lock every Phase-0 `klog` line takes. The APIC-id cache and the ledger per-slot records are parked above with owners, so their checkpoints belong to §25 and §26, not here.

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- `src/kernel/test/test_serial_emergency.c` (`TEST_CAT_BOOT`), 5 new `serial_lock:` cases plus 4 existing `serial_emergency:` lock cases converted to the fused word; full suite 29132 kernel + 17 user-mode, 0 failed.

> **Notes:**
> - Shipped: `serial_lock_t` in `serial_emergency.h` and its four single-atomic transitions in `serial.c` -- one word holding both lock state and owner, replacing the `spinlock_t` + `g_serial_lock_owner` pair whose two stores left an abort window.
> - Integrates by type change: `serial_emergency_acquire` takes the lock plus an ENCODED owner, `serial_lock_try_acquire_owned`/`try_release_owned` are the testable policy, and `serial_lock_release_if_owner_for` is the fixture-safe raw-to-encoded seam; `note_owner`/`clear_owner` are gone.
> - Downstream: `spin_lock_irqsave` is no longer on the serial path, so its IRQL pack/raise/restore is reimplemented in `serial_lock_acquire`/`release`; the CPUID identity derivation moved out of the lock-held region as a side effect.
> - Canonical doc: `serial_emergency.h` carries the encoding contract and the raw-versus-encoded rule; `smp.c:591` is why identity never comes from `smp_this_cpu()`.
> - Scope boundary: the SERIAL LOCK only -- the per-CPU identity cache is §25 and the emergency-ledger claim state machine is §26, both parked above with reciprocal XREFs.
> - Review additions: the acquire spin became PAUSE-backed test-and-test-and-set and the lock was given its own cache line (the latch shared it, measured on the linked artifact).

> **Verified:** 2026-08-14 | commit `9ab784b35` + review fixes | 1/3 items (2 parked with owners) | build OK | 29132 kernel + 17 user PASS, 0 failed | smoke matrix 4/4 (KVM/TCG x 1/2 CPU) | lint exit 0
> **Accepted:** [H] the per-CPU panic-safe id cache is not shipped -- reading it via `smp_this_cpu()` would let a GS-less AP record the BSP as owner, so the BSP could force-release the AP's live lock (reason: needs a GS-validated no-fallback accessor and a populate-before-GS-observable ordering proof; the stated harm, CPUID inside the lock, is closed here by hoisting it out of the held region) -> XREF: 01-boot-platform/TODO-10 §25 (item: "GS-validated no-fallback per-CPU accessor" at line 1007)
> **Accepted:** [H] a reservation aborted between the emergency slot publish and the per-CPU ledger entry is still unattributable (reason: an owner-only per-slot entry cannot carry the token generation, which is read from the latch word the winning CAS installs, and ARMED publication clears the bitmap but would not clear a separate array; needs one reviewed state machine) -> XREF: 01-boot-platform/TODO-10 §26 (item: "Composite owner-plus-generation slot claim" at line 1043)
> **Accepted:** [H] no test can observe the single-transition property -- post-state assertions pass equally against a two-mutation implementation (reason: the reviewer's mutation-hook alternative needs test scaffolding inside the production atomic path on the panic path, which Gate 10 forbids; an object-code gate is the right shape and has no tooling owner here) -> XREF: 01-boot-platform/TODO-10 §26 (item: "Build gate over the generated object proving a claim is ONE lock-prefixed transition" at line 1050)
> **Quality reviewed:** 2026-08-14 | Codex 8x (design, adversarial x2, test-coverage, consistency, perf, re-adversarial x3) | 3H+5M fixed, 3 accepted-XREF | scope: kernel-code-quality

---

## 21. Live CPU Online Lifecycle (split from §20)

> **Spawned-by:** §20 (split)
> **User impact:** A boot whose async groups overlap a dying worker can still pay a 10-second stall before the desktop appears, and the work dispatched into that stall lands on a CPU that will never run it. Separately, a machine that parks a CPU keeps reporting it to applications as an active processor, so a program sizing its thread pool from the processor count over-subscribes a core that is gone.

Both residuals are the same disagreement between a snapshot taken once and a set that changes underneath it. §18 made the online set mutable (it clears `is_online` when a CPU parks) without converting either consumer that still reads a boot-time snapshot: `boot_async_group`'s per-group scan, and `smp_cpu_count()`. The fix is a live mask plus a lifecycle claim, which is worker and SMP-topology work rather than the panic-path work §20 keeps.

- [x] Replace `boot_async_group`'s one-time `is_online` snapshot with an atomic per-worker lifecycle claim, so a CPU that goes offline mid-scan cannot still be dispatched work.
  - `per_cpu_data.async_claim` (`smp.h`) is ONE word: state in bits [1:0] (OFFLINE / IDLE / RESERVED / BUSY) and a generation in bits [31:2]. `boot_async_group` no longer snapshots anything -- it CASes each candidate slot IDLE(g) -> RESERVED(g+1), and a slot that parked is refused by the same instruction that would have selected it. OFFLINE encodes to 0, so a slot that never came online is undispatchable without an initialiser.
  - Dispatch is TWO-PHASE because the design review's single-transition version had a hole: arming before the payload was written let a delayed or misdelivered async IPI run the PREVIOUS group's function under the new generation, retire the new group's claim, and report a step complete that never ran. Reserve, write `async_fn`/`async_name`, fence, then `smp_async_claim_arm` RESERVED(g) -> BUSY(g); the AP handler runs work only when its own slot reads BUSY.
  - Completion is generation-exact (BUSY(g) -> IDLE(g)), so a worker the BSP already timed out cannot retire a later dispatch. A timed-out worker's slot stays BUSY and is therefore un-reusable until that worker retires it -- the property the section exists for, not a leak.
  - The barrier now also breaks on a claimed slot reading OFFLINE at its own generation and records that as a BSP-local forced `BOOT_FATAL`, never from `ap->async_result`: `panic.c` retracts membership BEFORE publishing the result, so reading the shared field there would have collected the PREVIOUS group's `BOOT_OK`. That removes the 10s stall the item names -> XREF: `01-boot-platform/TODO-10 §18` (item: "Async-isolation lock ownership").
- [x] Live online-CPU mask distinct from the boot discovery snapshot, so a parked CPU stops being reported as an active processor.
  - `smp_cpu_count()` is now the population of a live `online_mask`; `smp_cpu_present_count()` keeps the discovery snapshot (1 + APs enumerated). `smp_online_mask()` and `smp_cpu_is_online()` are the membership API, and `smp_publish_cpu_online`/`smp_retract_cpu_online` are the only transitions -- publish writes the mask bit LAST, retract clears it FIRST, so the mask is always a subset of `is_online` and can never count a CPU that has stopped answering.
  - The mask bit is the PUBLICATION POINT, and BSP bringup waits on it rather than on `is_online`. The adversarial review showed the other order let an AP stalled by an SMI mid-publication be observed as complete: `smp_init` would finish, `topology_init` would cache the CPU as permanently offline, and the first async group would omit a CPU that then went live.
  - Consumers converted by what they actually ask: `NtQuerySystemInformation` derives `NumberOfProcessors` and `ActiveProcessorsAffinityMask` from ONE mask snapshot (two reads could disagree, and the old `(1 << n) - 1` claimed a parked low CPU and omitted the live high one), `MaximumProcessors` and `topology_init`'s slot bound take the present count, `boot_timing` stamps the present count as machine configuration, and `irq_set_affinity` bounds on `MAX_CPUS` plus a mask test instead of a count that rejected valid sparse slots.
- [x] Commit: `"kernel: live online-CPU mask and atomic async worker lifecycle claim"`

**Test checkpoint:** 20 `smp_lifecycle:` cases over caller-supplied words prove the claim state machine: a zero word is OFFLINE and undispatchable, dispatch reserves rather than arms, only an armed slot reads as carrying work, a BUSY or RESERVED slot refuses a second dispatch and refuses to be republished as IDLE, completion and arming are generation-exact (an IDLE slot at the matching generation cannot complete twice), park preserves the generation and blocks both completion and arming, the generation wraps without aliasing OFFLINE, and every NULL / optional-output branch answers rather than faults. The mask helpers prove the sparse case the section exists for -- clearing CPU1 leaves CPU2 live and the count at 2, so a count can never be a slot bound -- and the live queries prove the count floor, BSP membership, count-equals-population, present >= live, and that no slot at or above the present count is online. Full suite 29295 kernel + 17 user-mode passes and the 4-leg smoke matrix is green; the 2-CPU legs are load-bearing, because AP bringup now completes on the mask bit rather than `is_online`, and they report `2 of 2 CPUs online`.

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- `src/kernel/test/test_smp_lifecycle.c` (`TEST_CAT_BOOT`), 20 `smp_lifecycle:` cases / 163 assertions; full suite 29295 kernel + 17 user-mode, 0 failed.

> **Notes:**
> - Shipped: `per_cpu_data.async_claim` plus its six single-atomic transitions in `smp.c`, and a live `online_mask` behind `smp_cpu_count` / `smp_cpu_present_count` / `smp_online_mask` / `smp_cpu_is_online`.
> - Integrates by replacing two snapshots: `boot_async_group` claims each worker instead of reading `is_online` once, and AP bringup waits on the mask bit, which `smp_publish_cpu_online` writes last.
> - Downstream: `smp_cpu_count()` can now FALL, so consumers were split by intent -- live for NT processor reporting and `NUMBER_OF_PROCESSORS`, present for slot bounds, maximums and machine-configuration stamps.
> - Canonical doc: `include/kernel/smp.h` carries the claim encoding, the two-phase rule, and why the claim is distinct from `in_async_work`.
> - Scope boundary: no CPU-hotplug re-online path is added -- a parked CPU stays parked; `nt_process.c` thread affinity remains the stub it was.
> - Review additions: the two-phase RESERVED state, the mask-last publication order, the publish-idle refusal to overwrite an in-flight slot, and the PEB processor-count conversion all came from review rounds.

> **Verified:** 2026-08-14 | commit `8ba6495b7` + review fixes | 3/3 items | build OK | 29296 kernel + 17 user PASS, 0 failed | smoke matrix 4/4 (KVM/TCG x 1/2 CPU), 2-CPU legs report `2 of 2 CPUs online` | lint exit 0
> **Accepted:** [M] a worker that overruns the 10-second async deadline is declared complete while still running, and the BSP then re-runs every storage initializer sequentially, so two CPUs can reset the same controller (reason: pre-existing since the barrier was written; the repair is an ownership transfer or cooperative cancellation agreed with the driver owners, not a change to the claim word this section shipped) -> XREF: 01-boot-platform/TODO-10 §27 (item: "Make the async timeout an ownership TRANSFER rather than a fabricated completion, so the BSP never re-runs an initializer a worker is still inside" at line 1087)
> **Accepted:** [L] an AP that commits to going live and is then delayed past the bounded bringup wait joins the live set after `smp_init` has finalized, with default topology and outside the consistency verdict (reason: pre-existing in the TODO-09 handshake; closing it needs a decision between a fatal expiry and an AP rollback path, which is a handshake redesign) -> XREF: 01-boot-platform/TODO-10 §27 (item: "Make the AP bringup handshake terminal, so no discovered AP can join the live set after `smp_init` returns" at line 1091)
> **Accepted:** [L] no automated leg boots a configuration where the live count differs from the present count, so the two-count split has coverage only over synthetic words (reason: the smoke matrix cannot produce an abandoned AP or a parked CPU on demand; it needs a fault-injection hook) -> XREF: 01-boot-platform/TODO-10 §27 (item: "Cover the degraded configurations no automated leg reaches today, so live-versus-present divergence is tested rather than reasoned about" at line 1094)
> **Quality reviewed:** 2026-08-14 | Codex 7x (design, adversarial x2, test-coverage, re-adversarial, consistency, perf) + kernel-quality-auditor + concurrency-evidence-mapper | 3H+5M+4L fixed, 3 accepted-XREF | scope: kernel-code-quality

---

## 22. Dedicated NMI Entry Stub (split from §20)

> **Spawned-by:** §20 (split)
> **User impact:** A fault in the NMI stub prologue or epilogue still classifies as an ordinary panic, which re-enables the guarded stack read over a live IST2 frame -- the nested-abort hang §18 removed, returning at a much lower rate. The user sees the machine freeze with no bugcheck rather than a BSOD naming the fault.

§18 closed the faultable BODY of the NMI path and said so honestly; the asm entry and exit are what it could not reach without a per-vector stub. Split from §20 because this is assembly and IDT-wiring work with a whole-machine interrupt-cost constraint, sharing no primitive with the serial-lock residuals.

- [x] Dedicated vector-2 entry stub that raises the NMI depth ahead of the shared prologue and lowers it past the register pops, closing the entry and exit windows §18 left open, with two residuals stated rather than claimed away.
  - `isr2` no longer comes from the generic `ISR_NOERRCODE` macro. It raises the depth as early as it can be raised -- ahead of its own error-code and vector pushes -- so all 15 register pushes, the conditional `swapgs`, the LFENCE and every C statement in `isr_handler` are covered. It is not the first instruction: four stack writes precede it, per RESIDUAL 1 below. The index is derived the way `cpu_panic_safe_apic_id()` derives it (CPUID leaf 1, EBX[31:24]), which is the only source needing neither GS nor a memory operand -- load-bearing here, because the raise runs before the `swapgs` that would make GS trustworthy.
  - The epilogue is reached by making the stub body a build-time-parameterized NASM macro instantiated TWICE: `isr_common_stub` for every ordinary vector and `isr_nmi_stub` for vector 2, whose copy lowers the depth after the register pops and the frame pop, ahead of the return block. §18 rejected closing this with a per-vector runtime test in the shared stub, which every interrupt on the machine would pay for a counter only the NMI path reads; two build-time copies charge the shared path nothing, which is asserted rather than assumed.
  - RESIDUAL 1, four stack writes precede the marker: the CPUID clobber set the raise must save to index itself. That is the floor, not an oversight -- a per-CPU counter update needs either a scratch register (so, a save) or GS, and this counter is deliberately GS-independent. RDPID would need one register instead of four but would introduce a SECOND derivation of CPU identity, the exact silent-drift failure `cpu_panic_safe_apic_id()` exists to prevent, and although `IA32_TSC_AUX` IS programmed (BSP and APs, gated on a probe), it holds a LOGICAL CPU INDEX while this counter is keyed by APIC ID -- different identity spaces, not substitutable until made to converge (the boot log records the probe failing).
  - RESIDUAL 2, the return block runs with the depth already lowered: one register restore, the CS test, the VERW block, the `swapgs` and `IRETQ`. The lower sits BEFORE that block rather than last, and the post-ship kernel audit is why -- lowering last refilled the store and fill buffers AFTER the MDS `VERW` had cleared them (breaking the contract §19 established) and put a serializing CPUID inside the post-`swapgs` user-GS window, where an `#MC` would run with the user GS base. Neither is reachable while every NMI handler is terminal, which is exactly why they were easy to miss. The trade is right: those instructions perform no guarded reads, while the buffers `VERW` clears and the GS window are live security properties. It is still ONE register restore rather than four because the lower folds the index into `rcx` -- the symmetric shape put all four inside the window, which the adversarial review caught, and it is why the two macros are deliberately asymmetric.
  - The depth now has exactly ONE production owner: the `vec == VECTOR_NMI` raise/lower pair is gone from `isr_handler`. The C helpers remain as the arithmetic a unit test can drive, and three `_Static_assert`s in `idt.c` pin what the assembly assumes about `g_nmi_depth` (id mask 0xFF for `shr ebx, 24`, 4-byte elements, full id-space coverage) because NASM cannot see the header -> XREF: `01-boot-platform/TODO-10 §18` (item: "Nested-abort panic context").
- [/] PARKED, blocked on a returning NMI handler existing: close the last return-tail residual so no instruction after the lower runs with the depth clear.
  - Today the park costs nothing observable, and that is the reason it is a park rather than work. Every NMI on this machine is terminal -- `nmi_crash_handler` (`panic.c:317`) calls `KeBugCheckExFrame` and never returns, and an NMI arriving before `bugcheck_init` falls to `panic_screen`, which also never returns -- so the entire epilogue, including this residual, is unreachable on the production path.
  - TWO concrete triggers, not one, and not hypothetical. `01-boot-platform/TODO-23 §1` plans a LAPIC NMI watchdog at roughly 1 Hz -> XREF: `01-boot-platform/TODO-23 §1` (item: "Nested-NMI safety prerequisite: per-CPU latch/replay (or drop) so a watchdog NMI during an existing NMI/MCE path cannot corrupt the shared IST2 stack" at line 69). `02-kernel-core/TODO-09 §12` plans AMD IBS sampling whose handler explicitly re-arms and RETURNS, at a ~100K-op sample rate -> XREF: `02-kernel-core/TODO-09 §12` (item: "IBS NMI handler: read all IBS MSRs; pack into a ring buffer of `ibs_sample_t` structs (256 entries per CPU, static allocation); re-arm counter; return from NMI").
  - The rate difference is the whole point, and this section's first reading of it was WRONG: a search for the sampling consumer checked two of four matches and concluded none existed, so the cost below was parked against the 1 Hz watchdog alone. At 1 Hz two CPUIDs are unmeasurable; at IBS sample rates they are not, and the packed counter's cache-line sharing becomes material per core.
  - Two costs to weigh before enabling either consumer, both nil today because every NMI is terminal: the raise and lower each execute a serializing CPUID (a VM exit under KVM/WHPX), and each does a `lock`-prefixed RMW whose prefix buys nothing under the owner-exclusive invariant.
  - An RDPID fast path IS available, correcting a second wrong claim here: `IA32_TSC_AUX` is programmed on both the BSP (`boot_hw.c`, writes 0) and the APs (`cpu_security.c`, writes the logical CPU id), each ONLY where the MSR probe succeeded -- so it is available, not guaranteed. A fast path needs a gate proving every ONLINE-OR-COMMITTED-ONLINE CPU both supports RDPID and has a RECORDED SUCCESSFUL TSC_AUX write, published BEFORE its `AP_BRINGUP_ONLINE` CAS. Online-only is not sufficient: §21 established that an AP commits to coming online before it publishes its mask bit, and the bounded BSP wait can finish while a committed AP is still unpublished, so a latch reduced over the online mask alone can go true and then admit a CPU lacking either property -- use `cpu_slot_committed_online()`, which exists for exactly this race. Three existing signals are each insufficient on their own: `g_tsc_aux_available` is set by the BSP probe alone, `per_cpu_data.tsc_aux` is recorded as the cpu id even when the write was skipped, and RDPID is absent from `CPU_FEATURES_AP_PROBE_MASK` so `cpu_has(CPU_FEATURE_RDPID)` proves only the BSP. Without such a latch, keep the CPUID-derived path -- the earlier note that the probe fails generalised one host's boot log into a property of the kernel. The real constraint is different and sharper: TSC_AUX holds a LOGICAL CPU INDEX while this counter is keyed by APIC ID, so switching to it means making the two identities converge, not just swapping the instruction -- and whatever it derives must match the C helper, which is the drift the leaf/subleaf/shift asserts now pin.
  - The fix, if it is ever wanted, is a per-CPU return tail holding a hardcoded absolute operand so the lower needs no live register, at roughly 2.8 KB of `.text` for the 256-entry id space. Rejected here as disproportionate against a dormant window, not as wrong.
- [/] PARKED, blocked on a receipt-surface change the unattended run may not make: prove the shared stub never touches `g_nmi_depth` by RELOCATION rather than by recognising instruction encodings.
  - The `nmi_stub: shared stub pays nothing for the depth` case scans for RIP-relative LEAs in two shapes: the 6-byte `8D /r <rel32>` and the 7-byte `4{8..F} 8D /r <rel32>`. REX.W=0 forms such as `lea r8d` (prefix `0x44`) are caught as well, because the no-REX shape matches one byte past the prefix and the displacement end coincides, so the target resolves correctly -- verified by control rather than assumed. What it still cannot see is a direct RIP-relative read-modify-write such as `lock inc dword [rel g_nmi_depth]`, which references the counter with no LEA at all.
  - The complete fix is not a wider byte scan: it is a build-time assertion over `isr_stubs.o` permitting relocations against `g_nmi_depth` only at the two dedicated NMI sites, which is encoding-independent and cannot drift. It needs a build-step change, and `Makefile*` plus the build scripts are the RECEIPT SURFACE the unattended run is forbidden to edit (`receipt_surface_guard.py`), so it is parked for an operator rather than half-attempted here.
  - The measured case FOR doing it this way, which is the part worth carrying: this is a NEGATIVE claim, and a negative claim cannot be soundly byte-matched over a variable-length instruction set. Four consecutive review rounds each found another encoding the scan missed -- only `lea rcx`, then r8-r15 via REX.R, then redundant REX.X/B forms, then the no-REX 32-bit form (a valid pointer here because the kernel lives below 4 GiB). Every hole was real and every widening was correct, and the SEQUENCE is the finding: the instrument is wrong for the job, so widening it again is not the fix.
  - Not a live hazard, and bounded: the realistic regression is depth code re-entering the shared body, which comes from the `NMI_DEPTH_*` macros and emits an ordinary LEA the scan does catch, negative-controlled in that shape. The park closes the encoding-independent remainder.
- [x] Commit: `"kernel: dedicated NMI entry stub raising NMI depth before any faultable work"`
  - That subject is reproduced exactly as committed and it OVERCLAIMS, which is recorded here rather than quietly corrected: the marker does not precede all faultable work. Four CPUID-clobber stack writes run before it, per RESIDUAL 1 above. The subject was written before the review established the exact boundary, and the commit cannot be rewritten now that it is pushed.

**Test checkpoint:** The claim is an ORDERING property, so it is verified against the EMITTED machine code rather than by executing the vector. Four `nmi_stub:` cases in a new `src/kernel/test/test_idt.c`, bounded by exported `_end` symbols and matching CONTIGUOUS instruction templates rather than byte landmarks -- only the two RIP-relative displacements are holes, each resolved against the symbol it must reach. `isr2` is exactly 40 bytes that raise the depth and only then push the error code and vector, indexing `g_nmi_depth` itself; it jumps to `isr_nmi_stub` and not the shared body; the shared stub never addresses the counter at all; the lower is 45 contiguous bytes from its first register save through the opening of the RETURN BLOCK that must follow it, pinning the id derivation, the indexing scale, the zero comparison, its branch and -- because the return block is included -- the lower's PLACEMENT, so it cannot drift back below `VERW`; and the LOADED vector-2 descriptor, decoded from the live IDTR, must target `isr2` through the ring-0 selector as a present DPL-0 64-bit interrupt gate on IST2 -- the integration boundary the other three cannot see, since they all start AT the symbol. That last one is BSP-SCOPED and named so: it proves the gate on the CPU running the suite, not that an AP NMI reaches the stub, which today it could not, because `gdt.c:130-134` configures the BSP TSS only. SEVENTEEN negative controls, each failing only its own assertions: late raise; jump redirected to `isr_common_stub`; four-restore epilogue; `lock inc` for `lock dec`; `cmp $1` for `cmp $0`; an inserted `nop`; a wrong index scale; vector 2 redirected to another stub; `shr ebx, 23` in the lower; a null gate selector; the lower moved back below `swapgs`, which must fail the placement assertion; five shared-stub counter references that must each be detected (`lea rax`, `lea r8`, a redundant-REX `0x49` form, an unprefixed 32-bit `lea eax`, and a REX.W=0 `lea r8d`); and a C-side CPUID-leaf change, which must break the BUILD via the derivation asserts. An eighteenth attempt was DISCARDED as an invalid control rather than counted: it put its probe instruction ahead of the register saves, clobbering the interrupted RAX on every interrupt, so it broke the boot instead of isolating the assertion. Three earlier probe designs were discarded as unsound rather than tuned: a fixed 64-byte window over a 40-byte stub read into `isr0` where a stray `push` byte satisfied the ordering; a raw `0xF0` scan matched the `and $0xfffffffffffffff0, %rsp` alignment immediate; and an unbounded displacement resolver would have overread on exactly the malformed-symbol regression it exists to catch. Running those controls also surfaced a harness defect that is not this section's to fix: a failing assertion whose message is long enough to overrun the klog entry wedges the boot instead of reporting, filed with its reproduction -> XREF: `00-infrastructure/TODO-03 §10` (item: "A failing `TEST_ASSERT` whose composed klog line exceeds the 256-byte message buffer wedges the boot"). An executing handler was rejected outright: it observes only the aggregate depth, and `int $2` would displace `nmi_crash_handler` on every CPU through the global handler table while setting no NMI blocking.

> **Test runner:** `bash scripts/test.sh SUITE=boot` -- new `src/kernel/test/test_idt.c` (`TEST_CAT_BOOT`, `test_register_idt`), 4 `nmi_stub:` cases / 18 assertions in `test_idt.c`; the C-mirror `nmi_depth:` pair stays in `test_serial_emergency.c`; suite 4230 kernel + 17 user-mode, 0 failed.

> **Notes:**
> - Shipped: a dedicated `isr2` raising the NMI depth before its own error-code push, and an `ISR_STUB_BODY` macro instantiated as `isr_common_stub` and `isr_nmi_stub`, the latter lowering the depth after the frame pop and ahead of the return block.
> - Integrates by removing the `vec == VECTOR_NMI` raise/lower pair from `isr_handler`, leaving the assembly as the counter's single production owner and the C helpers as the test-drivable mirror.
> - Downstream: `g_nmi_depth` is no longer static, so three `_Static_assert`s in `idt.c` pin the id mask, element size and id-space coverage that NASM indexes but cannot see.
> - Canonical doc: `include/kernel/idt.h` states which windows are covered and names both residuals; each is argued at its own site in `isr_stubs.asm`.
> - Scope boundary: the four pre-marker stack writes stay uncovered by construction, the return-tail residual is parked above against a named trigger, reachability is proven BSP-only (AP per-CPU TSS/IST is §2's accepted Critical, owned by TODO-09 §10), and no hardware-NMI injection harness is added.
> - Review additions: the asymmetric raise/lower macros, the exported `_end` symbols, contiguous-template assertions bound to the counter's relocated address, the loaded-descriptor integration test, moving the lower ahead of the VERW/swapgs return block, and pinning the CPUID leaf/subleaf/shift all came from review rounds.

> **Verified:** 2026-08-14 | commit `68496891f` + review fixes | 2/3 items shipped, 1 parked | build OK | 29314 kernel + 17 user PASS, 0 failed | smoke matrix 4/4 (KVM/TCG x 1/2 CPU) + WHPX boots to `C:\>` with 0 CRIT | lint exit 0 | emitted-code order verified by disassembly: `lock decl` then `pop rcx` then the CS test, VERW and `swapgs`
> **Accepted:** [H] vector 2 still decides `swapgs` from the interrupted CS RPL, so an NMI landing in the CPL0 window after an ordinary interrupt's exit `swapgs` but before its `iretq` sees a ring-0 CS, skips `swapgs`, and halts on `isr_handler`'s GS self-pointer check instead of reaching `nmi_crash_handler` (reason: pre-existing and unchanged here -- the only swapgs-related diff in this section is label renaming and the decision logic is byte-identical; the fix is MSR-based paranoid entry, already stamped `Deferred` as operator-reserved under CLAUDE.md's ABI-change/large-refactor rule. This section moves that fix CLOSER: the dedicated vector-2 stub its item asks for now exists, leaving only the GS detection, and the depth raise is GS-independent so the marker is already up if the halt does occur) -> XREF: 02-kernel-core/TODO-11 §18 (item: "Route NMI/#DF/#MCE to dedicated paranoid stubs instead of the plain `isr_common_stub` CS-RPL path; keep the IST stack assignment" at line 542)
> **Quality reviewed:** 2026-08-14 | Codex 22x (design, adversarial x5, re-adversarial x12, test-coverage x3, consistency, perf) + kernel-quality-auditor + concurrency-evidence-mapper | 3H+9M+3L fixed, 1H accepted-XREF, 1 parked | 17 negative controls, each firing only its own assertion | scope: kernel-code-quality

---

## 23. Cross-Boot Evidence Lifecycle and Epoch (from §20)

> **Spawned-by:** §20 (split)
> **User impact:** The next boot's `last-panic.txt` is misleading in both directions: a fault the machine SURVIVED is reported as an unexpected shutdown that never happened, and while that record holds the only slot it refuses the record to the crash that actually killed the machine -- so when the user finally does hit a fatal bug, the report explains a different fault.

Two §19-review findings and one §19-consistency finding share one root: the evidence page has a single slot, an entry-time claim, and no epoch, so every reader invents its own ad-hoc identity check and none is atomic against a concurrent panic. Split from §20 because this needs a lifecycle DESIGN before any code -- two patches were attempted during the §19 review and both raced -- and because the primitive it needs is a page epoch, not a lock word.

- [x] Rework the cross-boot evidence lifecycle so ownership is decided at TERMINAL arbitration, not at panic entry -- one slot plus an entry-time claim cannot express "this fault was survivable".
  - Two defects share this root and neither is fixable without it. (1) A recovered async fault publishes a complete, CRC-valid record on its way to parking; the machine then boots fine, `panic_evidence_restore_early` accepts the record, writes `last-panic.txt` and reports an unexpected shutdown that never happened. (2) A survivable fault holding the slot refuses it to the crash that DOES kill the machine, so the next boot explains the wrong fault.
  - Both were verified during the §19 review, and both PREDATE §19: `panic_collect_evidence` has always been called unconditionally at panic entry behind an entry-claimed one-shot, so a survivable async fault has always published a surviving record. §19 changed the claim's shape, not this behaviour.
  - Two repairs were attempted during that review and BOTH were reverted, which is why this needs design rather than a patch. Releasing the claim on the async park path races: the terminal CPU fails its ownership check before the release lands and never retries, so the boot ends with NEITHER record. Taking the record at terminal arbitration races the other way: the prior owner may still be inside `panic_collect_evidence`, so two CPUs zero, populate and CRC the same page and the same `s_panic_klog_scratch` concurrently.
  - SHIPPED as demote-on-survival, which is the polarity that keeps the early capture: a record is restorable from the instant it is published, and the async park path is the one place that KNOWS the machine survives, so it is the only place that revokes (`panic_evidence_abandon`, `panic.c`). Promote-on-terminal was rejected because a machine that triple-faults between collection and arbitration would then leave nothing, which is the property the early placement exists to provide.
  - Ownership moved from an APIC-id claim taken at entry to a per-INVOCATION token in one 64-bit word `{cpu:32, token:32}` with bit 63 as ACTIVE. The token is the identity because `panic_try_claim_owner` (`panic.c:399-409`) is same-CPU RE-ENTRANT, so an APIC id names a CPU and never the invocation that published a record; the CPU half answers the separate question of CONCURRENCY (an invocation on this CPU interrupted us and cannot be running in parallel).
  - ACTIVE lives IN the owner word, not beside it: held apart there is a window between acquiring ownership and declaring the write in which a terminal CPU reads a fresh owner as quiescent, takes the page, and populates it alongside the first owner. Two CPUs on one page and one shared klog scratch is corruption rather than a wrong record. Found by adversarial review round 1, fixed by fusing both into the CAS.
  - Verification is a live double panic, not a unit test: `panic_evidence_take` mutates boot-global ownership, so calling it from a test would lock a later real panic out of the record -> XREF: `01-boot-platform/TODO-10 §19` (item: "PARKED: live double-panic checkpoint for the evidence reserve/publish state machine").
- [x] Make the evidence page's readers and writers agree on an epoch: consume, restore and collect each assume the page is quiescent while they touch it, and none of them is atomic against a concurrent panic.
  - `panic_evidence_consume` (`panic.c:1445-1452`) already knows about this: it clears the magic only when `boot_seq` and `crc32` still match the record it restored, precisely so a fresh crash from another CPU is not erased. The compare and the clear are still separate operations, so a panic publishing between them loses its record anyway. Pre-existing; found by the §19 consistency review.
  - SHIPPED as page format v3: `magic` and a new `epoch` are adjacent at offset 0 and form ONE naturally-aligned 64-bit PUBLICATION WORD, pinned by `_Static_assert`s on both offsets and on the page alignment. Publish is a single release store; revoke and consume are single CAS, so each transition is atomic instead of a compare followed by a separate clear.
  - `panic_evidence_consume` is now one CAS keyed by the restored epoch, so a record published between the check and the clear carries a different epoch and survives. The epoch is an equality-only generation token, NOT an arithmetic sequence: `panic_evidence_next_epoch` walks until the candidate is nonzero and equal to neither the standing nor the restored epoch, because the standing value is untrusted cross-boot RAM and a plain max()+1 wraps (standing UINT32_MAX with restored 1 lands back on 1).
  - `panic_evidence_restore`'s contract and code now agree: it RETAINS the page and is repeatable, and the header says so. Every rejection drops the record with a CAS on the exact publication word sampled before validation, so a rejection decided about an old record cannot erase a newer one republished underneath it. Epoch 0 is rejected on restore because publish refuses it and consume is inert for it, so accepting it would re-report the same crash on every boot forever.
  - Reads are epoch-guarded rather than assumed quiescent: `panic_evidence_restore_at` samples the publication word before and after the copy and reports a record that moved as NO record, bounded at three attempts so a survivor cannot hang behind a panicking CPU.
- [/] PARKED, operator-gated: a terminal takeover that cannot prove writer quiescence declines to write, so a terminal record can be lost -- narrowed, not closed -> XREF: `01-boot-platform/TODO-10 §19` (item: "PARKED: live double-panic checkpoint").
  - The obligation is recorded against the terminal invocation's TOKEN (not a shared flag, which any later panic would clear) and retried at all three terminal exits, by which point the whole BSOD render has run. An unrecoverable case prints `cross-boot evidence NOT recorded` on serial rather than failing silently.
  - Not closable within this section's contract: a bounded wait cannot PROVE another live CPU is quiescent, and writing anyway puts two CPUs on one page and one shared klog scratch, which is corruption in place of a wrong record. The real closures are a stop-and-ack handoff or a second page, and both change the fixed 0x80000 page contract.
  - The SAME live double panic also exercises the other end of this window: a nested abort landing between the terminal-ownership claim and the evidence terminal promotion can still replace the first fatal record. Narrowed to two adjacent compare-and-swaps by moving the promotion to the FIRST statement after `panic_try_claim_owner()` succeeds, which takes `serial_enter_emergency` and the whole call transition out of it.
  - Closing that end entirely means fusing two ownership words keyed by DIFFERENT CPU-id spaces -- `panic_try_claim_owner` uses the logical `cpu_id`, the evidence state uses the panic-safe APIC id -- a design change this section does not own.
- [/] PARKED: the interleaving where a republish lands INSIDE `panic_evidence_restore_at` is unasserted -- blocked on a deterministic fixture hook that does not exist -> XREF: `01-boot-platform/TODO-10 §19` (item: "PARKED: live double-panic").
  - What IS asserted deterministically: the generation-conditional drop (a revoke aimed at a superseded generation is refused and the newer record survives with its payload), which is the property an unconditional clear would fail. What is not is the post-copy recheck and the 3-attempt retry bound INSIDE restore, because reaching them needs the page to change mid-call.
  - A concurrent kthread racing the fixture page was proposed by the coverage review and REJECTED: its assertion would depend on scheduling, which is the test-accepts-two-answers shape this repo forbids, and it would make the boot suite intermittently red. The honest closure is a fixture hook that republishes at a chosen point, or the live double panic.
- [x] Commit: `"kernel: terminal-arbitrated cross-boot evidence ownership with a page epoch"`

**Test checkpoint:** A recovered async fault leaves NO surviving cross-boot record while a terminal fault in the same boot does publish one, and a terminal fault that follows a survivable one takes the slot rather than being refused it; the epoch makes consume/restore/collect each fail closed against a record that changed underneath them, asserted over a fixture page. The two-real-panics cases stay operator-gated on a live double panic -> XREF: `01-boot-platform/TODO-10 §19` (item: "PARKED: live double-panic checkpoint for the evidence reserve/publish state machine").

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `boot: panic evidence epoch lifecycle` added alongside `boot: panic forensic evidence`; 29534 kernel + 17 user tests pass, 0 failures

> **Notes:**
> - Shipped page format v3: `magic`+`epoch` at offset 0 form one aligned 64-bit publication word, so publish is a single store and revoke/consume/drop are single CAS keyed by generation.
> - Ownership moved from an entry-time APIC-id claim to a per-invocation token word with the ACTIVE bit fused in, arbitrated at terminal declaration; the survivable async park path now revokes instead of holding the slot forever.
> - Downstream: `panic_collect_evidence` gained a token + terminal flag, and `PANIC_EVIDENCE_VERSION` is 3 so a v2 record is rejected by the existing version/size gate.
> - Canonical contract: [`include/kernel/panic.h`](../../include/kernel/panic.h) "cross-boot evidence lifecycle" block.
> - Scope boundary: does NOT close the bounded-quiescence residual (parked above, operator-gated), and collector publication latency stays with §24.
> **Verified:** 2026-08-14 | commit `3a1723c3d` + review fixes | 3/5 items (2 parked) | build OK | 29534 kernel + 17 user PASS, 0 failures | smoke matrix 4/4 (KVM/TCG x 1/2 CPU) | lint 0 errors
> **Accepted:** [M] RESOLVED 2026-08-15 in §28: publication was a plain store into the WB-mapped page and an x86 RESET invalidates caches without writing back, so the record could never reach DRAM on real hardware. §28 kept the page WB and made every lifecycle edge persist through `cache_writeback_range`; the bare-metal proof stays parked there -> XREF: `01-boot-platform/TODO-10` §28 (item: "Decide and implement the memory-type policy for the evidence page so a published record survives a hardware reset on real silicon")
> **Quality reviewed:** 2026-08-14 | Codex 7x (adversarial, consistency, perf, re-adversarial x4) | 6H+6M+5L fixed, 1 open | scope: kernel-code-quality

---

## 24. Panic Collector Publication Latency (from §20)

> **Spawned-by:** §20 (split)
> **User impact:** On a real crash the machine spends roughly 170k instructions of bitwise CRC plus ~2,900 guarded per-byte calls before the record is durable or the first serial byte leaves the UART. A machine that faults again inside that window -- which is exactly the failure mode a panic path exists to survive -- leaves the user nothing at all: no serial reason, no record for the next boot.

The three costs the §19 perf review measured, kept together because they are all paid in the same pre-publication window and two of them share the record layout that §23 reworks. Ordered after §23 deliberately: changing the checksum and the record layout in either order alone would churn the other.

- [x] Cut the pre-publication latency of the panic collector, measured at roughly 170k instructions of CRC plus ~2,900 guarded byte-read calls before the record is durable or the first serial byte is out.
  - CRC: the bit-at-a-time `panic_crc32` is RETIRED and all three collector/restore sites call `kcrc32` (`src/kernel/kchecksum.c:99`) instead. Byte-identity is asserted rather than reasoned: `test_panic_crc_table_matches_bitwise` (`src/kernel/test/test_boot_diag.c`) keeps the retired algorithm as its oracle and checks the full record, the Phase-0 restore path over a record checksummed by the RETIRED routine, and that a single flipped body bit is still rejected.
  - Same defect, same fix, one file over: `klog.c`'s private `crash_crc32` also ran bit-at-a-time in panic context, over up to 164 KB of serialized ring, and now calls `kcrc32`. Byte-identical, so a crash region written by an older kernel still validates on the next boot.
  - CONTEXT: `panic_collect_evidence` takes `ctx` derived once at panic entry; the internal `panic_declared_ctx` call is gone. Counted, not asserted -- `panic_declared_ctx_calls()` (KERNEL_TESTS only) proves population re-derives it ZERO times, and a separate case proves the copies still HONOUR that context (NMI and UNKNOWN never enter the guarded reader).
  - GUARDED COPY: `__kstr_read_guarded` (`src/kernel/cpu_security.c`) is ONE protected loop with `__kstr_read_fault`/`__kstr_read_fixup` routed in `page_fault_handler`, replacing the per-byte `__kread_u8` walks in `pe_copy`, `panic_snapshot_str`, `panic_append_guarded` and the serial emergency chunk walk. The index advances only after a successful store, so the fixup inherits exactly the readable prefix -- proven by a real mid-copy #PF over a guard-page fixture, not by inspection.
  - Design review made the clip reportable rather than silent: a walk that runs into the canonical hole returns `KSTR_STOP_NONCANON`, so a corrupt pointer can never be rendered as a complete string the way a silent clip would have.
  - Record population split into `panic_evidence_populate` over a caller-supplied record, which is what gave the collector's content a test surface at all; arbitration, epoch, zeroing and the publication word all stay in the collector.
- [x] Commit: `"kernel: table-driven panic CRC, single identity derivation, bounded guarded string copy"`

**Test checkpoint:** `kcrc32` and the retired `panic_crc32` produce byte-identical output over the full record and over the Phase-0 restore path, asserted directly rather than reasoned; a normal panic derives identity and context ONCE, verified by a call counter on the fixture; the bounded guarded string primitive preserves the exact readable prefix and reports how many bytes it stored, plus a stop reason distinguishing an ordinary truncation from an unreadable source, matching what the per-byte loop produced.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 9 suites added (3 boot-diag, 5 serial_emergency, 1 crash), 29612 kernel + 17 user tests pass, 0 failures

> **Notes:**
> - Shipped `__kstr_read_guarded` in cpu_security.c, retired `panic_crc32` and klog's `crash_crc32` for `kcrc32`, and gave `panic_collect_evidence` a `ctx` parameter.
> - Runs through a third RIP-keyed guarded-load block in `page_fault_handler`, ahead of the pager, serving the four converted string walks in panic.c and serial.c.
> - `panic_evidence_populate` split out of the collector, which is what gave record population a test surface; arbitration, epoch and the publication word stay behind.
> - On-page CRC and crash-region formats are unchanged, so older records still restore; the collector's signature changed and all four call sites moved with it.
> - Canonical doc: the guarded-read context table in `include/kernel/cpu_security.h` now covers the bounded loop as well as the byte load.
> - Scope boundary: the emergency writer's chunk-boundary and marker orchestration has no output sink and is not tested here; filed in §26.
> **Verified:** 2026-08-14 | ship `cf359460e` (+ this review commit) | 2/2 items | build OK | tests 29612 kernel + 17 user, 0 failures | lint 0 errors | smoke matrix 4/4 (KVM+TCG at 1+2 CPUs)
> **Accepted (RESOLVED by §26):** [M] the emergency writer's chunk boundaries and unreadable-marker decision have no test surface -- `SERIAL_EMERG_CHUNK`/`MAX_CHARS` are file-local and every byte leaves through the UART (reason: needs a test output sink, which is infrastructure for the emergency-ledger section) -> XREF: `01-boot-platform/TODO-10 §26` (item: "Output sink for the emergency writer, so `serial_emergency_write_str`'s chunk arithmetic and unreadable-marker decision are testable at all."). Shipped 2026-08-14 as a walk taking its output as a PARAMETER rather than an installable sink, with the three constants moved to `serial_emergency.h`.
> **Quality reviewed:** 2026-08-14 | Codex 6x (design + adversarial + test-coverage + consistency + perf + re-adversarial x2) | 2H+5M+4L fixed, 1M accepted-XREF, 4L rejected | scope: kernel-code-quality

---

## 25. GS-Validated Per-CPU Identity Cache (split from §20)

> **Spawned-by:** §20 (split)
> **User impact:** None today, and that is the honest answer: §20 already moved the serializing CPUID out of the UART lock's held region, so no CPU spins waiting through another's VM exit. What remains is one CPUID per serial acquisition -- a real cost under KVM and WHPX on a chatty boot, but paid by the writer alone.

The per-CPU cache §20 designed and then deliberately did not ship. It is filed rather than dropped because the perf finding behind it is real, and filed SEPARATELY because the design review showed the obvious implementation is a correctness regression, not a speedup.

- [/] BLOCKED on the authentication latch below: GS-validated no-fallback per-CPU accessor, returning NULL rather than silently standing in for CPU 0, so a cached identity can never be attributed to the wrong CPU.
  - `smp_this_cpu()` reads `gs:0` and falls back to `&cpu_data[0]` when it is NULL (`smp.c:890`; the `smp.c:591` this item originally cited is stale, the function moved). Any cache read through it would let an AP with unset GS record the BSP as the serial-lock owner -- after which the real BSP's raw-CPUID force-release matches and frees the AP's LIVE lock. That is strictly worse than the CPUID cost the cache saves, which is why §20 ships the raw derivation instead.
  - The accessor must validate the actual GS base or per-CPU slot rather than trusting a non-NULL pointer, because `&cpu_data[0]` is non-NULL and is exactly the wrong answer -> XREF: `01-boot-platform/TODO-10 §20` (item: "PARKED: cache the panic-safe APIC id per CPU").
  - BUILT AND WITHDRAWN 2026-08-14, and the reason narrows the item rather than repeating it. A structural predicate (in-array slot boundary, `self == p`, `p == &cpu_data[p->cpu_id]`) was implemented and fixture-tested, and it is NOT sufficient on its own: a slot belonging to ANOTHER CPU satisfies every clause. That is harmless for the CPU-LOCAL bookkeeping `smp_this_cpu()` was written for -- a wrong IRQL harms only its own CPU -- and becomes a defect the moment the answer is published as a GLOBAL lock owner, because a missed `swapgs` reads `gs:0` out of the ring-3 TEB, where user-shaped memory can name a real foreign slot. Structural validation alone therefore reinstates §20's exact failure through a different door, so the accessor is only half of this item and must ship WITH the latch below.
- [/] BLOCKED on the authentication latch below: populate the cache FROM `cpu_panic_safe_apic_id()` BEFORE GS becomes observable, on every BSP and AP bring-up path, so no window exists in which the cache reads zero to an NMI or `#MC`.
  - Ordering is the whole correctness argument: a cache published after GS is observable is readable, and wrong, for the interval between the two. The identity must also stay ONE value -- populated from this helper and no other. `smp_this_cpu()->lapic_id` was REJECTED and must stay rejected: it comes from the LAPIC ID register on the BSP and the ACPI MADT on APs (`smp.c:264,288,434`), a different derivation, so mixing sources makes the park-time compare-exchange silently never match.
  - Only then convert the ordinary serial acquisition to the cache; the panic and emergency paths keep the raw CPUID permanently, because they cannot trust GS by construction.
  - AND §30 PUT A SECOND IDENTITY ON THE SAME ORDERING PROBLEM, which is why it is filed here rather than re-derived later. `smp_publish_panic_safe_id` claims the CPUID-derived id into a per-id owner table by compare-exchange, and it is called from `ap_entry` AFTER the live-LAPIC guard for exactly the reason this item gives. That leaves a window an adversarial round found and this section owns: an AP that faults BEFORE it claims -- reachable because the BSP abandons a timed-out AP and sends the next SIPI while the late one may still be in `ap_entry` (`smp.c:571-597`) -- resolves through `smp_cpu_by_apic_id` to whichever CPU DID claim that id, so the panic park path could retract a live CPU and publish failure into its async slot. Closing it needs the claim moved ahead of faultable AP init, which contradicts the after-the-guard rule above, or abandoned APs proven quiesced before async dispatch. Both are this item's design decision, not §30's -> XREF: `01-boot-platform/TODO-10 §30` (item: "Decide and implement what a nested abort should do once the async branch has stopped identifying itself as async: it takes the SAME recovery tail, resumed at the first unfinished step, rather than the terminal path.").
  - Bounded today by the same premise the rest of this surface rests on: it requires two CPUs reporting the SAME CPUID leaf-1 initial APIC id, which `cpu_security.h` states happens only above 255 logical CPUs, and such x2APIC systems are unsupported repo-wide.
  - CORRECTED BY THE ATTEMPT: "before GS becomes observable" is right on the BSP and WRONG on an AP. `ap_entry` writes `self`, `cpu_id` and both CR3 fields and then the GS base (`smp.c:160-177`) BEFORE the live-LAPIC identity guard (`smp.c:202-209`), and that guard exists because a slow AP can latch a reassigned `cpu_index` out of the shared AP_DATA page. Every pre-guard write today is value-IDENTICAL whichever AP makes it, which is why that aliasing is currently benign; a cached id would be the FIRST per-CPU value that differs by writer, so a pre-guard publish lets an impostor stamp its own id into a slot another CPU is already live on. The AP publish belongs AFTER the guard, with the BSP clearing the slot to UNSET before the SIPI -- and the resulting UNSET window is safe precisely because UNSET must route the reader to the raw derivation.
- [/] GS-independent armed latch proving every online CPU can authenticate its own per-CPU slot, without which no read path may consult the cache -- blocked by this section's three recorded blockers
  - Blocker: named in the section's 2026-08-14 `Deferred:` stamp, which records an end-to-end attempt that was reverted to a clean tree with nothing shipped. Each blocker was confirmed at `file:line` by an adversarial round; the latch cannot be built until they clear.
  - Authentication cannot come from GS (that is the thing under test) and must be cheaper than the serializing CPUID it replaces, or the cache is pointless. `IA32_TSC_AUX` read via `RDTSCP` is the right SHAPE and was implemented: the kernel already programs it to the logical cpu id (`boot_hw.c:606` BSP, `cpu_security.c:1366` AP) and RDTSCP reads it from the CPU with no memory operand, so a pointer forged at another slot fails the compare.
  - What made it unshippable is the GATE, not the mechanism. Gating on `cpu_has(CPU_FEATURE_RDTSCP)` reads the BSP's mask. `cpu_feature_local()` exists because features are AP-skewed and the TSC_AUX profile write is already gated on it (`cpu_security.c:1357`, shipped as `01-boot-platform/TODO-09 §10`), so an AP lacking RDTSCP correctly gets NO tag while the global mask still reads true -- and the first ordinary serial acquisition on that AP would execute `rdtscp` and take a `#UD`. The write side is AP-local; the READ side has no published per-CPU fact to consult, which is the actual missing piece.
  - Needed: a latch the BSP arms once after bringup, only if EVERY online CPU both supports RDTSCP and actually received its TSC_AUX tag; unarmed means no cache anywhere, on any CPU. It extends the per-CPU capability publication that already exists -> XREF: `01-boot-platform/TODO-09 §6` (item: "Two-phase ordering: each AP publishes its mask into `cpu_data[ap_id].features` before `cpu_harden()`'s optional CR4 enables" at line 257).
  - VALIDATION GAP, to be accepted deliberately rather than discovered later: QEMU advertises RDTSCP in CPUID while trapping the TSC_AUX MSR, so `g_tsc_aux_available` stays 0 and the boot log carries `RDTSCP: TSC_AUX MSR probe failed; CPU ID tagging disabled` (`boot_hw.c:610`). The fast path is inert on KVM, TCG and WHPX alike and only bare metal can exercise it, so the conversion must land with pure-helper fixtures covering the cache-HIT composition (matching id at 0 and 255, foreign id, UNSET, NULL out-param, out-param untouched on every refusal) -- a platform-conditional live test alone would pass an implementation that always refuses.
- [/] Commit: `"kernel: GS-validated per-CPU panic-safe identity cache"` -- blocked until this section's own items clear
  - Blocker: nothing to commit. The 2026-08-14 attempt was reverted to a clean tree, so this line ships only when the armed latch above does.

**Test checkpoint:** The accessor returns NULL for an unset or invalid GS rather than `&cpu_data[0]`, asserted against a fixture rather than reasoned; a structurally valid FOREIGN slot is rejected as well, since that is the case structural validation alone cannot decide; the cached id is byte-identical to `cpu_panic_safe_apic_id()` on every online CPU; and an ordinary serial acquisition executes no CPUID once the cache is live, verified by a call counter rather than by inspection. The cache-HIT composition needs pure-helper fixtures rather than a live case, because no available emulator can authenticate a slot.

> **Deferred:** 2026-08-14 | attempted end-to-end, reverted to a clean tree, nothing shipped | the design is now specified rather than sketched, and the three blockers below are what the attempt established, each confirmed at `file:line` by an adversarial round.
> - Structural GS validation is only half an accessor: a foreign per-CPU slot passes every structural clause, which is safe for CPU-LOCAL bookkeeping and unsafe the moment the answer becomes a GLOBAL lock owner.
> - `IA32_TSC_AUX` via `RDTSCP` authenticates the slot GS-independently and cheaply, but gating it on the BSP-global `cpu_has()` would execute `rdtscp` on a feature-skewed AP and take a `#UD` -- the write side is already AP-local (`cpu_security.c:1357`), the read side has no per-CPU fact published to consult.
> - The fast path cannot be exercised on any available platform: QEMU traps the TSC_AUX MSR, so every local leg runs the raw fallback only.
> - Cost of leaving it parked is unchanged from §20 and stated in this section's own User impact: one CPUID per ordinary serial acquisition, paid by the writer alone, with no user-visible effect today -> XREF: `01-boot-platform/TODO-10 §20` (item: "PARKED: cache the panic-safe APIC id per CPU" at line 798).

---

## 26. Attributable Emergency-Ledger Claim (split from §20)

> **Spawned-by:** §20 (split)
> **User impact:** A wedged UART can spend slightly more than its advertised stall budget: a reservation aborted mid-publish leaves one full-length wait charged to nobody, so it is never refunded and the ceiling is one wait tighter for the rest of the pre-arm phase. Bounded and in the safe direction, but it means the budget the panic path reasons about is not the budget it actually gets.

The per-slot owner records §17 parked against §18 and §18 did not cover, kept as their own section because the §20 design review showed the obvious repair races and the real fix is a state machine rather than a patch.

- [x] Composite owner-plus-generation slot claim, so claiming a wedged-UART allowance and recording who owns it is ONE atomic transition.
  - Allowances left the latch word entirely. Each one is now a composite claim word of its own in `s_emerg_claim[SERIAL_EMERG_STUCK_BYTES]` (`serial.c:344`), encoded `VALID | generation | owner`, and `serial_emerg_claim_slot` (`serial.c:1062`) takes the allowance and records its owner in the SAME compare-exchange. There is no second step, so no abort window: a charge exists if and only if a live claim exists.
  - The per-CPU advisory ledger this replaces (`s_emerg_slot_mask` + `s_emerg_charge_gen`) is DELETED, not kept alongside. It was the second word an NMI or `#MC` could land in front of, and `serial_emerg_waits` / `charges_self` / the refund path now read the claims directly instead of reconstructing a count from two sources that could disagree (the refund itself became `serial_emerg_reclaim_self` in §29) -> XREF: `01-boot-platform/TODO-10 §17` (item: "Epoch-tokened, attributable wedged-UART accounting").
- [x] Define claim, publish, return, refund, stale reclamation and epoch rollover as ONE reviewed state machine rather than six independent edits.
  - SHIPPED WITHOUT A PUBLISH STEP, which is a deliberate change from the drafted claim-then-publish design: the Codex design review showed that publishing a latch bit after a composite claim only converts an anonymous charge into an equally uncounted CLAIM leak, since an abort in the new window leaves a same-generation claim no contender can tell from a live publisher. Two words and one abort window, in either order. Making the per-slot word authoritative removes the window instead of narrowing it.
  - Epoch rollover is the generation bump alone: `serial_emerg_publish_word` advances it in the same compare-exchange that publishes ARMED, which invalidates every outstanding claim at once with no second pass and no interval where two epochs coexist. Stale claims are reclaimed only by exact CAS against the observed word, so reclaiming can never overwrite a live holder. `serial_emerg_return` validates generation AND owner before releasing, so a token that crossed CPUs releases nothing -> XREF: `01-boot-platform/TODO-10 §20` (item: "PARKED: per-slot owner records for the emergency terminal-charge ledger").
  - `serial_emerg_reserve` re-reads the generation after its claim lands and releases the claim if an epoch was published in between. Adversarial review caught this as a regression against the bitmap shape, where the reservation CAS targeted the same word the publication did and so simply failed; without it a caller could be granted a full-length wait that no budget counted.
- [x] Build gate over the generated object proving a claim is ONE lock-prefixed transition, so the single-transition property this section and §20 both rest on is checkable rather than asserted.
  - `tools/atomic-claim-check/check.sh` disassembles the built `serial.o` and requires each audited helper to carry exactly one `lock` prefix, bound to that same instruction, on a 32-bit `cmpxchgl`, with NO other memory write in the function. The last clause is the one that matters: one locked cmpxchg alone is satisfied by a plain store of the owner followed by an unrelated compare-exchange, which is the two-step mutation the gate exists to reject.
  - Wired into `scripts/test-tooling.sh` (pre-push) and, in `--required` mode that refuses a missing or stale object, into `build.yml` AFTER the clean build. Adversarial review caught that the tooling pack runs before the build in CI, so the gate could only ever have skipped there. `--selftest` carries nine negative controls, including both shapes a naive two-grep gate accepts.
  - Target-word verification was considered and REJECTED on evidence: two audited helpers compare-exchange through a caller-supplied pointer, and the other three relocate as `R_X86_64_32S .bss+<offset>` with no symbol name to compare -> XREF: `01-boot-platform/TODO-10 §20` (item: "Fuse `g_serial_lock` ownership and lock state into ONE atomic transition").
- [x] Output sink for the emergency writer, so `serial_emergency_write_str`'s chunk arithmetic and unreadable-marker decision are testable at all.
  - `serial_emergency_walk(str, ctx, emit, sink)` takes its output as a PARAMETER, reached by tests through `serial_emerg_walk_for_test`. Not an installable global: a writable function pointer consulted on the panic path is mutable control-flow state that could be swapped between the calls making up one report, and would turn the diagnostic into a second fault if corrupted. Production passes one constant callee.
  - `SERIAL_EMERG_CHUNK`, `SERIAL_EMERG_MAX_CHARS` and `SERIAL_EMERG_STUCK_BYTES` moved to `serial_emergency.h` so fixtures assert against the real numbers rather than copies. Covered: chunk-edge cutting, continuation past a capacity stop, the ceiling at exactly `MAX_CHARS` and at `MAX_CHARS + 1`, the marker on an unreadable source, identical chunking in NMI context, and NULL operands -> XREF: `01-boot-platform/TODO-10 §24` (item: "Cut the pre-publication latency of the panic collector, measured at roughly 170k instructions of CRC plus ~2,900 guarded byte-read calls before the record is durable or the first serial byte is out.")
  - Marker-AFTER-partial-text ordering is structural (the marker is emitted after the loop) and its fault half stays serial-validated, matching this file's existing discipline for guarded-read recovery: a real `#PF` fixup is not unit-drivable, so the non-canonical stop is what the fixture uses.
- [x] Commit: `"kernel: composite owner-plus-generation emergency ledger claim"`

**Test checkpoint:** The fixtures the drafted design called for -- an abort between the owner claim and the bitmap publish, a stale pre-arm claim still held at ARMED publication -- describe windows the shipped design does not have, because there is no publish step to abort inside. What is asserted instead is the property they were reaching for: a charge is attributable to its owner or absent, never outstanding and anonymous. Claim and release are exact against the observed word; a token from a dead epoch, a foreign owner's token, a nonzero token without VALID, and an out-of-range slot each release nothing; one generation bump releases every allowance and the whole budget is reclaimable in the fresh epoch; a held allowance costs exactly one, not the rest; and a granted token always names the live epoch. Single-transition atomicity is not assertable from a fixture at all and is checked against the disassembly by `tools/atomic-claim-check/check.sh`, whose own matcher carries nine negative controls.

> **Test runner:** `scripts/debug/kernel/run-boot-tests.bat` (or `make test-boot`) -- expect 0 failures; the emergency-ledger and walk fixtures are the `serial_emergency:` cases under `TEST_CAT_BOOT`. The object gate runs separately: `bash tools/atomic-claim-check/check.sh --required` after a build, and `--selftest` for the matcher's own controls.

> **Notes:**
> - Allowances moved out of the latch into one composite `VALID | generation | owner` word each (`s_emerg_claim[]`), so taking an allowance and recording its owner are one compare-exchange; the per-CPU advisory ledger they replaced is deleted.
> - Epoch publication invalidates every outstanding claim by advancing one generation field, replacing an explicit slot clear that only worked while the allowances lived in the word being published.
> - `serial_emerg_waits` / `charges_self` / the refund path (`serial_emerg_reclaim_self` since §29) read the claims directly, and `serial_emerg_return` now checks owner as well as generation, so a token cannot release another CPU's charge; that identity is hoisted once per chunk rather than resolved per byte, because it costs a serializing CPUID.
> - `serial_emergency_write_str` split into `serial_emergency_walk` with the output as a parameter; `SERIAL_EMERG_CHUNK` / `MAX_CHARS` / `STUCK_BYTES` moved to `serial_emergency.h`.
> - Canonical doc: `include/kernel/drivers/serial_emergency.h` carries the allowance contract, the single-transition rule, and the walk seam.
> - Scope boundary: the compare-exchange TARGET word is not verified by the object gate (two helpers take it by pointer, two relocate section-relative with no symbol name); that exactness is carried by the signatures and the fixtures.
> - Scope boundary: "attributable" is WITHIN an epoch. A wait admitted while its epoch was live runs to completion after a publication, uncounted by the new epoch -- the unchanged two-phase contract at `serial.c:80`, not something this section introduced or claims to close.
> **Verified:** 2026-08-14 | ship `8a0af976b` (+ this review commit) | 5/5 items | build OK | tests 29673 kernel + 17 user, 0 failures | lint 0 errors | smoke matrix 4/4 (KVM+TCG at 1+2 CPUs) | atomic-claim gate 4/4 transitions, 10/10 matcher controls
> **Accepted:** [H] a nested abort inside an async-isolation dump carries the outer dump's claims into its own refund baseline, so a CPU that parks forever strands them for the rest of the pre-arm epoch (reason: pre-existing -- the previous per-CPU ledger had identical delta semantics; the obvious repair erases genuine timeouts and breaks the monotonic-in-timeouts ceiling, so it needs claim state the encoding does not carry) -> XREF: `01-boot-platform/TODO-10 §29` (item: "Distinguish a reservation that TIMED OUT from one that was merely taken, so a parking CPU can return the second without erasing the first.")
> **Quality reviewed:** 2026-08-14 | Codex 9x (design + adversarial x3 + test-coverage + re-adversarial + consistency + perf x2) + kernel-quality-auditor + concurrency-evidence-mapper | 3H+11M+4L fixed, 1H accepted-XREF, 2 rejected with code evidence | scope: kernel-code-quality

---

## 27. Async Worker Quiescence and Terminal Bringup Handshake (from the §21 review)

> **Spawned-by:** §21 (review)
> **User impact:** Two boots that look recovered are not. A storage driver that overruns the 10-second async deadline is declared failed and the BSP immediately re-runs every storage initializer sequentially, so if the original worker is still inside AHCI or NVMe init, two CPUs reset the same controller and program the same DMA and MMIO registers concurrently: a hung boot, or a disk that comes up with a half-programmed controller. Separately, an AP that commits to going live and is then delayed past the bringup timeout joins the running system after `smp_init` has already finalized -- with default topology, outside the register-consistency verdict, and never audited.

**Continuation waiver (review-spawn at depth 3, limit 3, accepted 2026-08-14).** `user_impact`: stated above -- concurrent controller re-initialization is data-path damage, not a diagnostic wart. `not_parkable`: §21 is the section being stamped in this same pass and the fixpoint loop never revisits a DONE section, so a `- [/]` park there is stranded; §22--§26 are open but each owns a specific panic-path primitive (NMI stub, evidence lifecycle, collector latency, identity cache, ledger claim) and none owns worker ownership or the bringup handshake. `severity_trend`: the post-ship round produced 1 medium + 1 low in this class, on a DISTINCT surface (driver ownership and bringup finalization) rather than a deepening of the claim-word surface §21 closed. `surface`: `src/kernel/main/boot_init.c`, `src/kernel/main/boot_storage.c`, `src/kernel/smp/smp.c`.

Both defects predate §21 and neither is created by it: the async timeout has fabricated completion since the barrier was written, and the bringup wait has been bounded-then-continue since the handshake shipped in TODO-09. §21 is what makes them nameable -- the lifecycle claim now records that a timed-out worker still owns its slot, so the BSP can finally tell "this work is finished" from "this work is abandoned", which is the distinction both fixes need.

- [x] Make the async timeout an ownership TRANSFER rather than a fabricated completion, so the BSP never re-runs an initializer a worker is still inside.
  - `boot_async_group` marks a timed-out worker's `async_done` and forces `BOOT_FATAL` (`boot_init.c`), and `boot_storage_init` treats a FATAL group as licence to re-run `ata_init`/`ahci_init`/`nvme_init`/`virtio_blk_init` sequentially on the BSP (`boot_storage.c:333-340`). Nothing joins or stops the original worker, so both CPUs can be inside the same controller reset.
  - The claim word already carries the fact needed to fix this: a timed-out slot stays BUSY at its dispatch generation and only the worker itself can retire it. Gate the sequential fallback on the claim returning to IDLE (or on the CPU having parked), and where it has not, degrade that driver rather than re-entering it -> XREF: `01-boot-platform/TODO-10 §21` (item: "Replace `boot_async_group`'s one-time `is_online` snapshot with an atomic per-worker lifecycle claim").
  - Cooperative cancellation is the alternative and is a bigger change: a per-worker cancel flag each long initializer polls at a safe point, plus an acknowledgement the BSP can wait on. Decide between the two with the driver owners, not in the barrier.
- [x] Make the AP bringup handshake terminal, so no discovered AP can join the live set after `smp_init` returns.
  - When the BSP loses the STARTING->ABANDONED race the AP is committed to going live, and the BSP waits a bounded 100 ms for the publication (`smp.c`). If that wait expires the BSP continues anyway: `cpu_features_finalize_global`, `cpu_audit_consistency_check` and `topology_init` all run over a set the AP is not in, and the AP then publishes its mask bit and starts taking IPIs.
  - Needs a decision the review could not make unattended: treat the expiry as fatal (bug-check, since a committed AP that cannot publish is a broken machine), or give the AP a rollback path so it can park itself dark after committing. Both are handshake redesigns -> XREF: `01-boot-platform/TODO-10 §21` (item: "Live online-CPU mask distinct from the boot discovery snapshot").
- [x] Cover the degraded configurations no automated leg reaches today, so live-versus-present divergence is tested rather than reasoned about.
  - Three test-only injection keys, all reached through `cmdline=` and all compiled out at `KERNEL_TESTS=off`: `test_abandon_ap` (abandon an AP at bringup), `test_hold_async_cpu` (hold a worker inside its async step), `test_park_cpu` (park an online CPU after `topology_init`).
  - The hold is a BSP-RELEASED HANDSHAKE, not a duration. A fixed hold measured from the AP's own start could expire before the BSP even created its barrier deadline, so the injected scenario would silently not happen and a healthy boot would be reported as coverage.
  - `smp_test_park_cpu` (`smp.c`) checks the four relations itself and klogs each violation: live count falls by exactly one, present count holds, `g_topo_cpu_count` holds, the mask bit clears, and `popcount(mask) == smp_cpu_count()` -- which is what makes the NT-facing counts agree, since PEB `NumberOfProcessors` and the `NUMBER_OF_PROCESSORS` registry/env value all read `smp_cpu_count()`.
  - It parks the BOOKKEEPING, not the CPU: the target stays in `sti; hlt` and keeps taking interrupts, so it drops out of CR-pin re-verification for that boot. Documented at the function; a true park needs a halt IPI.
- [x] Commit: `"kernel: async worker quiescence and terminal AP bringup handshake"`

**Test checkpoint:** A worker stalled inside a storage initializer past the 10-second deadline leaves that driver degraded and NOT re-entered on the BSP, proven by a fault-injection hook that holds the worker and asserts the initializer's entry count stays at one; an AP stalled between its ONLINE CAS and its mask publication either fails the boot deterministically or parks dark, and never appears in the live set after `smp_init` returns; and with a CPU parked by injection, `smp_cpu_count()` falls while `smp_cpu_present_count()`, `g_topo_cpu_count` and `MaximumProcessors` hold.

> **Verified:** 2026-08-14 | 3/3 items | build OK | tests 29727+17 PASS | smoke matrix 4/4 (kvm 1+2 cpu, tcg 1+2 cpu) | live degraded-boot evidence
> Held-worker boot (`async_init=1 cmdline="test_hold_async_cpu=1"`, 2 cpu): barrier `TIMEOUT: AHCI on CPU1 did not complete in 10000ms -- worker still owns its slot`; fallback kept ATA/NVMe/VirtIO and refused AHCI (`NOT re-run on the BSP`); quiescence spent its full 2000 ms; `mask=0x2` suppressed AHCI interrupt setup and its block registration; the released worker completed at 12150 ms. AHCI's initializer was entered exactly once and the boot reached userspace in 13.27 s with no panic.
> Parked-CPU boot (`cmdline="test_park_cpu=1"`, 4 cpu): `[S27] parked CPU1: live 4->3, present 4 (held), topo 4 (held), mask 0xf->0xd -- consistent`.
> Every injection is fail-loud, proven by four negative controls that each fired their marker: `test_park_cpu=3` on 2 cpu -> `park REJECTED: CPU3 is not online`; `test_hold_async_cpu=1` on 1 cpu -> `requested but storage init took the SEQUENTIAL path`; `test_hold_async_cpu=7` on 2 cpu -> `was never dispatched a storage step`; and a deliberately inverted assertion in `test_smp_lifecycle.c` failed the suite (`FAIL: 1 of 4637`) before being reverted. An injection that does not happen can no longer read as coverage.
> **Deferred:** [H] exclusion is not containment -- suppressing registration and interrupt setup cannot undo hardware side effects an initializer already performed (PCI bus master, MSI-X), so a still-running or poisoned worker can leave a device able to DMA or interrupt -> XREF: 02-kernel-core/TODO-01 §13 (item: "Contain a degraded storage driver at the HARDWARE, not just in bookkeeping" at line 544)
> **Accepted:** [M] no per-DRIVER degraded state survives boot -- `boot_health.c` has no `SUBSYS_AHCI`/`SUBSYS_NVME`, so nothing can answer "is AHCI excluded right now" the way NT's `CM_PROB_FAILED_START` devnode property does -> XREF: 01-boot-platform/TODO-29 §19 (item: "Per-DRIVER degraded state for storage, not just a per-subsystem bit" at line 471)
> **Accepted:** [M] an AP abandoned at bringup can never rejoin -- terminal by design here, while Linux retries via `cpuN/online` and Windows supports ACPI processor hot-add -> XREF: 03-memory-concurrency/TODO-07 §7 (item: "`cpu_up(id)` must also cover an AP ABANDONED at bringup" at line 196)
> **Quality reviewed:** 2026-08-14 | Codex 7x (design, adversarial x2, consistency, perf, test-coverage, re-adversarial x3) + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst | 2H+6M+6L fixed, 1H+2M owned elsewhere | scope: kernel-code-quality

> **Notes:** The two defects this section names both predate §21 and neither is created by it; §21 is what made them nameable, by recording that a timed-out worker still owns its slot. Three things turned out to be load-bearing beyond the checklist. (1) "Released" is not "finished": a claim word reaches the same `OFFLINE(gen)` whether the worker completed and its CPU parked afterwards or the worker was cut down mid-initializer, and only the `async_retired` witness separates them -- reading `async_result` without it credits a driver that never initialized, because panic.c parks the claim BEFORE publishing the result. (2) A fault injection measured in wall-clock cannot prove anything: the original hold was anchored at the AP's own entry while the BSP creates its barrier deadline later, so on any machine where BSP-side storage work ran long the injected scenario silently did not happen and a healthy boot was reported as coverage. Both the hold and its deadlock backstop are now anchored on BSP-published points. (3) An injection whose failure is silent is worse than none -- hence the negative control in the evidence above.
> The injection keys are reached through `cmdline=`, not as bare `boot.conf` lines: only typed `struct boot_config` fields are projected into the arg table, and an unrecognized bare key is not even reported as unknown. That cost one wasted boot to discover and is recorded at the schema rows in `config.c`.

---

## 28. Cross-Boot Evidence Durability Across a Hardware Reset (from the §23 review)

> **Spawned-by:** §23 (review)
> **User impact:** On real hardware the crash record can be silently lost. Publication is a plain release store into the WB-mapped `0x80000` page, and an x86 RESET invalidates caches WITHOUT writing back modified lines, so the record may never reach DRAM. The user reboots after a crash and finds no `last-panic.txt` and no unexpected-shutdown notice. Invisible under QEMU/KVM/WHPX, which model no cache hierarchy -- so every gate the repo runs today reports success on a mechanism that does not work on the target platform.

> **Continuation waiver (review-chain depth 3):**
> - **not_parkable:** parking would misreport ready work as blocked. The repair (map the page UC, or flush the published line) is implementable now, and what is at stake is the subsystem's own acceptance criterion -- this repo's bar is "Verified on bare metal", and this is the defect that makes cross-boot evidence a no-op on exactly that platform while passing every VM gate.
> - **severity_trend:** falling. The §23 review rounds ran 4 high, then 3 high, then approve; the post-commit wave produced 1 high (collector running with interrupts enabled) plus 5 medium, ALL fixed in place. This is the only finding of that wave not fixable inside §23's surface, because it changes memory-type policy rather than lifecycle logic.
> - **surface:** the publication path in `src/kernel/panic.c`, plus the PMM reservation and VMM mapping of the fixed `0x80000` page. `klog.c` crash persistence shares the assumption and must be decided with it, not after it.

The §23 lifecycle work made a single release store the sole durability point for the whole cross-boot record, which is what turned a long-standing repo-wide assumption into a specific, testable defect. When this section was filed, a `grep` for `clflush`/`wbinvd` across `src/kernel/**/*.c` returned no matches, so nothing on any path forced a writeback before the machine reset. `src/kernel/cache.c` is now the tree's only cache-maintenance code.

- [x] Decide and implement the memory-type policy for the evidence page so a published record survives a hardware reset on real silicon, not just under an emulator.
  - DECIDED: keep the page write-back and make each writer persist its own record, rather than mapping `0x80000` uncached. UC was rejected on four specific grounds: `panic_evidence_restore_early` reads and copies the whole page in Phase 0 on EVERY boot, so UC taxes a path no crash ever touches; §24 exists because collector publication latency is tracked; retyping needs `vmm_split_huge_page` against the bootloader's 2 MiB identity map in Phase 0 before any panic can occur; and it does nothing for `klog_crash_persist`, whose region is allocated fresh each boot, so a second mechanism would be needed anyway.
  - SHIPPED as `cache_line_size` / `cache_writeback_range` / `cache_writeback_all` (`include/kernel/cache.h`, `src/kernel/cache.c`): CLFLUSH bracketed by MFENCE, with a WBINVD fallback when CLFLUSH is unenumerated. CPUID is probed on the EXECUTING CPU every call, with no global state and no init step, so it is callable from an AP and before any feature table exists.
  - The ordering rule is about WHEN THE PUBLICATION WORD IS STORED, not about sequencing flushes: un-publish, flush the whole body while the word reads zero, then store and flush the word. The reviewed-and-discarded alternative (publish, then flush body lines before the publication line) holds only where CLFLUSH gives line granularity and collapses on the WBINVD fallback, which commits many lines with no ordering boundary.
  - A durable un-publish requires an OWNER. The evidence page has arbitration (`panic_evidence_take`) and flushes its clear; the klog region has none, so it stores the clear without flushing, because a nested panic that dies before republishing would otherwise destroy a complete record.
  - Every lifecycle edge persists, not just publication: publish, un-publish, revoke, consume and validation-drop. A consume left in cache lets a reset resurrect the record it retired, so the next boot re-reports a crash the user already saw while that stale record holds the slot against the crash that actually killed the machine.
  - `klog_crash_persist` was split into `klog_crash_serialize_region` so the publication order is reachable from a fixture; `klog_crash_recover` persists the cleared magic before the allocator can reuse that physical run, and the zeroed header of the new region before its address reaches NVRAM.
  - Both reset paths carry a full writeback immediately before the reset write (the panic tail, and all three methods of `acpi_reset_now`), documented as defence in depth ONLY: it runs on the calling CPU, does not quiesce the others, and does not wait for external caches.
  - Also repaired here because the review found it on this surface: recovered crash entries are copied into owned storage and force-terminated before any consumer reads them. They were handed to `serial_write` straight out of untrusted cross-boot memory with no terminator guaranteed, and the previous boot's run is never reserved by this boot, so sanitizing in place would have corrupted live allocations.
- [x] Commit: `"kernel: evidence page memory type -- cross-boot durability across a hardware reset"`

**Test checkpoint:** On bare metal, a forced panic followed by a reset restores the record on the next boot with a valid CRC -- parked above, because only a physical machine can produce that evidence. What IS asserted deterministically is everything the decision rests on: the line-size decoder rejects a missing leaf 1, a clear CLFSH bit, a zero size and a non-power-of-two size; the range planner is exact at a straddled line boundary, at both-ends-unaligned, on a wrapping range, and on the top-of-address-space partial line an end-bounded loop never terminates on; a published record still validates after its own persist and after a republish over a still-valid word; and the klog region publishes its magic last with a CRC covering exactly the entries it clamped to. The flush ORDER itself is not observable from a unit test, and nothing here pretends otherwise.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 7 cases added to `test_boot_diag.c` (line-size contract, decoder rejections, range writeback preserves contents, full writeback preserves contents, planner boundaries, evidence record survives its own persist, klog region publishes magic last); 29775 kernel + 17 user tests pass, 0 failures

> **Notes:**
> - Shipped `src/kernel/cache.c` + `include/kernel/cache.h`, the tree's only cache maintenance: CLFLUSH+MFENCE with a WBINVD fallback, probing CPUID on the executing CPU per call so it is AP-safe and needs no init step.
> - Chose per-writer flushing over mapping `0x80000` UC, because UC taxes every boot's Phase-0 restore and the collector latency §24 tracks, and would not have covered the klog region at all.
> - Publication order is enforced by storing the publication word LAST rather than by sequencing flushes, which is what keeps it correct on the WBINVD fallback where no line-order boundary exists.
> - Downstream: every evidence lifecycle edge now persists, `klog_crash_persist` split into `klog_crash_serialize_region`, `klog_crash_entry_t` moved to `klog.h`, and recovered crash strings are copied and terminated before use.
> - Canonical contract: [`include/kernel/cache.h`](../../include/kernel/cache.h) header block, mirrored in [bare-metal-gotchas.md](../../docs/infrastructure/bare-metal-gotchas.md) "Cross-boot durability".
> - Scope boundary: does NOT close the bare-metal proof, the klog in-place overwrite, the evidence-ownership retiring state, pre-kernel preservation of the page, or the device-durable consume -- each carried by a stamp below with a concrete owner item.
> **Verified:** 2026-08-15 | ship `1ea16cf86` + this review commit | 2/2 items shipped, 5 residues owned elsewhere | build OK | 29774 kernel + 17 user PASS, 0 failures | smoke matrix 4/4 (KVM+TCG x 1+2 CPU) | lint 0 errors | todo-graph 10/10 | WHPX NOT RUN (no Windows interop here)
> **Deferred:** [H] the bare-metal proof is this section's only real acceptance evidence and no emulator can produce it -> XREF: `01-boot-platform/TODO-10` BM Test 5 (item: "Cross-boot evidence survives a hardware reset")
> **Accepted:** [H] nothing preserves the `0x80000` page between the reset and Phase 0, so firmware and bootloader allocations can clobber a record that did reach memory (reason: bootloader-side reservation, not memory-type policy) -> XREF: `01-boot-platform/TODO-14` §5 (item: "`0x80000` kept out of the allocator by the existing pmm_init first-1-MiB reservation")
> **Accepted:** [H] `vfs_flush() == 0` is not a device-level durability predicate, so consume is left non-durable rather than risking loss of both the file and the record (reason: filesystem sync semantics) -> XREF: `05-storage-filesystems/TODO-06` §1 (item: "Flush must reach the DEVICE")
> **Accepted:** [M] the klog crash region overwrites a complete record in place, so a nested panic that dies before republishing can still lose it -- narrowed here, not closed (reason: needs a two-slot or arbitrated publication protocol the region does not have) -> XREF: `02-kernel-core/TODO-04` §8 (item: "PARKED: eviction-independent publication for the crash region")
> **Deferred:** [M] `panic_evidence_abandon` relinquishes ownership before its durable publication-word clear, so a reset in that window leaves a complete CRC-valid record standing and the next boot reports a fault the machine survived (reason: repair is a retiring state in the evidence ownership state machine) -> XREF: `01-boot-platform/TODO-10` §23 (item: "PARKED, operator-gated: a terminal takeover that cannot prove writer quiescence declines to write")
> **Quality reviewed:** 2026-08-15 | Codex 9x (design, adversarial x4, re-adversarial x2, test-coverage, consistency, perf) + kernel-quality-auditor + concurrency-evidence-mapper + kernel-explorer + spec-research-analyst | 7H+11M+4L fixed, 4 parked with owners, 2 rejected with code evidence | scope: kernel-code-quality

---

## 29. Abandoned-Reservation Reclamation on a Parking CPU (from the §26 review)

> **Spawned-by:** §26 (review)
> **User impact:** A machine whose serial port is already failing loses the crash evidence for the panic that matters. An AP that faults during async boot init, takes a nested abort inside its own dump, and parks forever leaves up to the whole eight-wait pre-arm budget charged with no owner alive to return it -- so a LATER real panic emits its reason and register dump through single status probes on a wedged UART, and may emit nothing at all.

**Continuation waiver (review-spawn at depth 3, limit 3, accepted 2026-08-14).** `user_impact`: stated above -- lost crash evidence on the boot after the one that failed, which is the exact outcome the whole emergency-writer chain exists to prevent. `not_parkable`: §26 is the section being stamped in this same pass and the fixpoint loop never revisits a DONE section, so a `- [/]` park there is stranded; §27 owns async worker ownership and AP bringup in `boot_init.c`/`smp.c` and §28 owns evidence-page durability, neither of which is the allowance ledger. `severity_trend`: the post-ship round produced 1 high in this class, on the CONSUMER side (`panic.c`'s refund arithmetic) rather than a deepening of the claim-word surface §26 closed -- §26's own findings converged to codegen cost and comment accuracy. `surface`: `src/kernel/panic.c` async-isolation branch, `src/kernel/drivers/serial.c` allowance accounting.

The defect predates §26 and is not created by it: the refund was already a delta against a count sampled at panic entry, so an outer dump's charges were already inside the baseline. §26 is what makes it nameable -- each allowance now records its owner and epoch, so "this CPU holds a charge" is a fact the code can read rather than infer from two snapshots.

- [x] Distinguish a reservation that TIMED OUT from one that was merely taken, so a parking CPU can return the second without erasing the first.
  - `SERIAL_CLAIM_TIMEDOUT` (bit 16, defined in `serial.c`) rides in the bits the composite claim left free above the generation, so the claim word is not widened and no second location exists for an abort to land in front of. Static asserts pin it clear of VALID/GEN/OWNER and inside that free range.
  - `serial_emerg_mark_timeout` sets it in ONE exact compare-exchange over the word the shared token validator observed; a failed CAS means the exact claim changed (reclaimed, replaced, or a republished epoch), on which this CPU holds nothing and owes nothing -> XREF: `01-boot-platform/TODO-10 §26` (item: "Composite owner-plus-generation slot claim, so claiming a wedged-UART allowance and recording who owns it is ONE atomic transition.").
  - The mark is the timeout's LINEARIZATION POINT and lands BEFORE the byte, in `serial_emerg_timeout_byte`. Write-then-mark was the original design and was rejected in design review: an NMI between the write and the mark enters the nested panic, finds an unmarked claim, and reclaims a timeout that really happened.
- [x] Reclaim the abandoned reservations of every panic invocation on a CPU that is about to park forever, instead of only what the innermost dump spent.
  - `serial_emerg_reclaim_self` frees every live claim this CPU owns that carries no timed-out mark and returns the count; the async-isolation branch calls it in place of the delta (`panic.c`), and `charges_before` is gone rather than left computed-but-unused.
  - `serial_emerg_refund_self` is DELETED, not kept beside it: replacing its sole production caller left a kernel API with no callers, and two refund mechanisms with different bounds is exactly the drift the one-state-machine rule exists to stop -> XREF: `01-boot-platform/TODO-10 §26` (item: "Define claim, publish, return, refund, stale reclamation and epoch rollover as ONE reviewed state machine rather than six independent edits.").
  - Soundness is that every disposition reachable from the call is non-returning, NOT that the park is nearby: the branch parks about a hundred lines later, and across most of that tail a nested abort takes the terminal path rather than re-entering. Stated in both the header contract and the call site.
  - `serial_emerg_return` now REFUSES a charged reservation. The exact compare does not prevent it (the marked word compares exactly as well as an unmarked one), and the earlier draft claimed in a comment that it did -- a property of the single caller written down as a property of the ledger.
- [x] Fixtures for the abort schedules that produce the leak, which no current test drives.
  - Nine `serial_emergency:` cases under `TEST_CAT_BOOT`: untouched reservation reclaimed, charged timeout preserved, nested invocation covered (both reservations, against the entry sample the broken arithmetic was built from), whole-budget sweep at the ceiling with the final slot untouched and then charged, per-CPU isolation, foreign-owner refusal, mark idempotence and ownership, mark cannot reach a later epoch, and return cannot erase a charge.
  - The mark-before-write ORDERING is not fixture-observable, so it is carried by `tools/atomic-claim-check` instead: `serial_emerg_timeout_byte` joins a new ORDERED audit asserting the locked compare-exchange precedes the port write and that none follows it, with negative controls for the reversed order, a mark on both sides of the byte, and the branch-around shape the gate deliberately does NOT reject.
- [x] Commit: `"kernel: reclaim abandoned wedged-UART reservations on a parking CPU"`

**Test checkpoint:** With a claim taken and no timeout recorded, the parking path returns it and `serial_emerg_waits()` falls; with a timeout recorded, the same path leaves that charge standing and the count holds. A nested invocation on one CPU reclaims the outer invocation's abandoned reservation rather than counting it as its own baseline, proven by a fixture that takes a claim, samples `charges_self`, takes a second, and asserts the reclamation covers both.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (or `make test-boot`) -- expect 0 failures; the nine new cases are the `serial_emergency:` reclaim/mark entries under `TEST_CAT_BOOT`. The ordering property runs separately: `bash tools/atomic-claim-check/check.sh --required` after a build, and `--selftest` for the matcher's own 17 controls.

> **Notes:**
> - Shipped `SERIAL_CLAIM_TIMEDOUT` plus `serial_emerg_mark_timeout` / `serial_emerg_reclaim_self` / `serial_emerg_timeout_byte`, and deleted `serial_emerg_refund_self` once its sole production caller moved.
> - The panic async-isolation branch reclaims by reading this CPU's own claims instead of a delta against a panic-entry sample, so a nested abort no longer buries the outer dump's reservations inside its own baseline.
> - A shared token validator now backs both the return and the mark, rejecting a token with the valid bit clear, a non-canonical word, a dead epoch, an out-of-range slot, a dead claim and a foreign owner in one place so the two cannot drift apart.
> - Ordering that no fixture can observe is carried by `tools/atomic-claim-check`'s new ORDERED audit, already wired into `scripts/test-tooling.sh` and the CI build.
> - Boundaries, both deliberate: an INTERRUPTED wait counts as taken and is reclaimed, because committing the charge first leaves a permanent charge on a machine that survives; and with no per-slot incarnation counter, a token is valid only while its reservation is held.
> - Review also bounded the reserve loss re-sweep, which had inferred a spent budget from a single lost compare-exchange, and moved the test-only ledger-identity override behind `KERNEL_TESTS` because one global made every CPU share a ledger identity.
> **Verified:** 2026-08-15 | commit `1504a8969` + review fixes | 4/4 items | build OK | 29836 kernel + 17 user tests, 0 failed; smoke matrix 4/4 legs; KERNEL_TESTS=off builds clean
> **Deferred:** [M] a nested abort arriving after `in_async_work` is cleared fails the async entry test and takes the terminal bugcheck path, in both the pre- and post-publication intervals (reason: needs a boot-recovery semantics decision, and the store predates this section) -> XREF: `01-boot-platform/TODO-10 §30` (item: "Decide and implement what a nested abort should do once the async branch has stopped identifying itself as async, instead of falling through to the terminal path by accident." at line 1200)
> **Deferred:** [M] the documented no-UART state does not exist -- `s_serial_port` is never 0, so every zero-port guard is dead and an emergency byte on a port-less machine still spends a full-length wait (reason: making it real is a boot-behaviour change) -> XREF: `01-boot-platform/TODO-10 §30` (item: "Decide whether a bootloader-reported serial port of 0 means \"no UART\" and make the code agree with whichever answer is chosen." at line 1205)
> **Quality reviewed:** 2026-08-15 | Codex 6x (adversarial, consistency, perf, re-adversarial, consistency, perf) + kernel-quality-auditor + concurrency-evidence-mapper | 0H+10M+4L fixed, 2 open | scope: kernel-code-quality

---

## 30. Panic-Path States That Are Documented But Not Implemented (from the §29 review)

> **Spawned-by:** §29 (review)
> **User impact:** Two states the panic and emergency-serial paths claim to have and do not. A second fault on an async boot worker, arriving any time after that worker stops identifying itself as async -- which is BEFORE it publishes completion, not after -- reboots a machine that was recovering: the AP takes the full terminal bugcheck path and restarts the box, in the earlier interval with the BSP not yet told anything and in the later one with the BSP already running sequential init on the strength of the publication. And on a machine whose bootloader reports no serial port, every emergency byte still pays a full-length wait on a UART that will never drain, spending the whole wedged-UART budget on hardware that has no UART at all.

**Continuation waiver (review-spawn at depth 4, limit 3, accepted 2026-08-15).** `user_impact`: stated above, and it is a machine-terminal outcome rather than a refinement. `not_parkable`: §27 owns async worker quiescence and carries both stamps, so a bare item there is invisible to the triage oracle; §29 is being stamped in this same pass, so a `- [/]` park in it would be stranded by the fixpoint loop; and §25, the only open section, owns per-CPU identity caching rather than fault semantics. `severity_trend`: this round produced one high (an ordering window, rejected as a documented policy boundary) and two mediums in shipped code, both fixed in place -- this is the only finding of the round whose fix needs a design decision rather than an edit. `surface`: `src/kernel/panic.c` async-isolation branch only.

Found by the kernel-quality auditor during the §29 review and CONFIRMED at file:line, but NOT created by §29: `pcpu->in_async_work = 0` predates it (`b37d7d3e5`, the async Phase 2 work), and the §29 commit changed neither that store nor the branch entry. What §29 added was a comment asserting an invariant that turned out to be false, which is how the behaviour came to be looked at; that comment is already corrected.

- [x] Decide and implement what a nested abort should do once the async branch has stopped identifying itself as async: it takes the SAME recovery tail, resumed at the first unfinished step, rather than the terminal path.
  - DECISION, of the three shapes the filing listed: the separate "already parking" marker. Not the `in_async_work` move, which reorders a publication the BSP barrier depends on; not accept-the-terminal-path, which reboots a machine the BSP is already recovering, from a CPU that has retracted itself from the online mask. The entry test is now `panic_async_disposition(in_async_work, park_stage)` in `panic.c`, so BOTH intervals the filing named are covered by one test instead of the later one only.
  - THE MARKER IS GS-INDEPENDENT, which the obvious `per_cpu_data` field would not have been. `smp_this_cpu()` reads `gs:0` and SUBSTITUTES `cpu_data[0]` when that read yields 0 (`smp.c:1157-1164`), never NULL, so on the corrupt-GS AP that most needs the marker a per-CPU flag would be read from and written to the BSP's slot and the CPU would still reach terminal arbitration. `s_async_park_stage[]` is keyed by `cpu_panic_safe_apic_id()`, the same CPUID-derived id the serial owner word, the NMI depth and the evidence record use, carrying the `_Static_assert` pairing `g_nmi_depth` has at `idt.c:269`.
  - AND SO IS THE SLOT THE TAIL MUTATES, which took four rounds to get right. Identity is NOT `lapic_id`: `cpu_security.h:287-289` says a panic-safe id is only ever compared with another panic-safe id, and `lapic_id` carries the MADT/LAPIC-register value, so a cross-domain compare would fail silently and only on firmware that remaps them. Each CPU publishes its own CPUID-derived id, and `smp_retract_may_clear_mask()` now gates the online-mask clear on that same field -- previously it still compared `lapic_id` and would have refused the clear on remapping firmware, halting a CPU that stayed online.
  - OWNERSHIP OF THE ID SPACE IS ATOMIC AND CROSS-SLOT. `smp_cpu_by_apic_id()` is one indexed load into `s_panic_id_owner[]` (value = CPU index plus one, 0 = unclaimed) plus a bounds-checked `smp_get_cpu`, not the linear scan it started as. The scan could not detect two slots claiming one id -- it returned whichever came first -- and the check-then-store behind it rested on a serialisation claim that is FALSE: `smp.c:571-597` abandons a timed-out AP and sends the next SIPI while the late one may still be in `ap_entry`. One compare-exchange now decides ownership, `smp_publish_panic_safe_id()` reports refusal, and `ap_entry` parks an AP that cannot publish -- because a refused CPU does NOT resolve to NULL, it resolves to the CPU that did claim the id, so carrying on would have let it retract a live CPU. The `panic_safe_id_plus1` field remains at the struct tail (`gs:104`-`gs:136` are `_Static_assert`-pinned for the KPTI trampoline, and the build caught the first placement) and is now only the retract gate's input.
  - TWO CONSEQUENCES OF THE IDENTITY MIGRATION had to follow it, both found in round 4. `smp_retract_cpu_online` still gated its online-mask clear on `pcpu->lapic_id`, so on remapping firmware the lookup would succeed and the clear be refused, halting a CPU that stayed online for every later async group to select; it now matches `panic_safe_id_plus1`. And `panic_append_guarded` FELL THROUGH TO A RAW WALK when the context forbids the guarded read (`panic.c:842-844`), which is precisely what the NMI and double-fault park guards enter with -- a corrupt `async_name` would have faulted there, and in a `#DF` context that is a triple fault and a reset before the UART is handed back. It now renders `PANIC_STR_NO_GUARD`.
  - The nested diagnostic also carries the real error code (`frame->err_code`, or the STOP code on a frame-less bugcheck) rather than 0: the tail revokes the durable record, so that one serial line can be the only survivor and an `err=0` would have described a real page fault as having had none.
  - THE STAGE IS TESTED AT EVERY PANIC INGRESS, not just at the branch, which took rounds 2 and 3. The park tail is a function (`panic_async_park`) reached from three guards, all calling the named `panic_async_is_parking()`: the ordinary first-fault site, an early exit right after `ev_cpu` is derived, and the top of `ke_bugcheck_emit`. That last one is not redundant -- it is the common ingress for `KeBugCheckEx`/`KeBugCheckExFrame`, it sits ABOVE the others, and before reaching them it overwrites the shared `g_last_bugcheck` record (clobbering a terminal panic mid-flight on another CPU) and can call into the registry, which takes locks and the heap. Everything those guards skip is faultable work a first fault needs and a nested one must not repeat.
  - ONLY THE UNBOUNDED STEP IS CLAIMED BEFORE IT RUNS (`panic_park_claim_step`), and the first draft claiming ALL of them was a defect the adversarial round caught: a claim taken before a step means an abort in the gap SKIPS that step forever, which would have parked a CPU still holding `g_serial_lock` and hung every survivor on its next write. Exactly one step earns it -- the diagnostic, which walks a caller-supplied string and drives the UART and could otherwise fault in the same place on every entry.
  - EVERYTHING ELSE RE-RUNS UNCONDITIONALLY and is not gated at all: the allowance reclaim, the online-mask retract, the completion stores, the UART hand-back and the evidence revoke. `PANIC_PARK_ENTERED` and `PANIC_PARK_PUBLISHED` are RECORDS of progress read by the entry guards and the publication-owed question, not permissions. The recursion is bounded by that split rather than by a depth counter: the one step that could loop runs at most once, and the only step touching a mapped page rather than BSS (the evidence revoke) relinquishes its owner word before writing, so a re-entry returns at its first test.
  - THE PUBLICATION IS RECORDED AFTER ITS STORES, not before, so the error leans the safe way: an abort in that gap leaves the stage at ENTERED and the nested entry writes the completion again, which is idempotent. The opposite order could leave a completion RECORDED and never WRITTEN, and the BSP would wait out its whole barrier deadline on a signal never sent.
  - The nested entry still REVOKES the cross-boot record. Codex recommended preserving it; rejected against `panic.c:1626-1633`, whose contract already names this case: the machine survives either way, so a standing record is the same false unexpected-shutdown holding the same single slot against the crash that does kill the box. The nested fault's vector, error code and RIP go out on the `[ASYNC] NESTED FAULT` line instead, which spends no durable slot. Codex also recommended treating a nested #MC/#DF as terminal; rejected because the terminal path is a BSOD plus a disk crash dump driven from a CPU already off the online mask, and `hlt` is the containment.
  - The §29 reclamation is unaffected and is now sound for a STRONGER reason: every later abort re-enters this branch at every point of the tail, so the three intervals collapse to two non-returning dispositions -> XREF: `01-boot-platform/TODO-10 §29` (item: "Reclaim the abandoned reservations of every panic invocation on a CPU that is about to park forever, instead of only what the innermost dump spent.").
- [x] Decide whether a bootloader-reported serial port of 0 means "no UART": it is believed for the WAIT BUDGET and refused for the OUTPUT, and the driver's dead zero-port guards are deleted rather than made reachable.
  - DECISION. The bootloader's 0 is a real negative result, not an unfilled field: `bootx64.c:753-757` stores port 0 with `serial_source` NONE only after ACPI SPCR and the scratch-register probe of both COM1 and COM2 have all failed. Codex read the ABI correctly and recommended adopting it wholesale; refused, because adopting it means storing 0 into `s_serial_port`, which is the 2026-04-11 incident verbatim (`boot-code-quality` Gate 8) -- recommended by a review, applied, and it killed kernel serial output. The probe is a scratch-register test, so a 16450 or vendor UART that does not implement that register fails it while transmitting perfectly well.
  - A PRE-EXISTING BUG SURFACED WHILE ANSWERING THIS, and it is the bigger half of what shipped. `serial_init` ran at `boot_hw.c:96` and read `g_boot_info`, which `boot_hw.c:160` does not copy until sixty lines later -- `boot_hw.c:124` states the struct is still zero-initialised there. So `if (g_boot_info.serial_port)` and the SPCR baud arm have NEVER fired, and the kernel has always run on the compiled-in COM1 default. This also re-explains the 2026-04-11 incident: the unconditional assignment stored 0 because the SOURCE STRUCT WAS ZERO, not because a probe had failed on that machine. Gate 8's rule is right; only its recorded reason was wrong.
  - The driver is therefore SPLIT: `serial_init` does early bring-up on the compiled-in default and reads no `boot_info` at all, and a new `serial_adopt_boot_info()` is called from `boot_hw.c` after the validated copy. The reported BASE is adopted there for the first time, bounded by `SERIAL_PORT_BASE_MAX` since SPCR is firmware-supplied and the 16550 block is base..base+7. The reported BAUD is not, because it is the one field that can make a working console worse -> XREF: `02-kernel-core/TODO-29 §1` (item: "Decide whether the bootloader-reported `g_boot_info.serial_baud` is adopted, now that the handoff is readable at all").
  - THE READY-CHECK-FIRST ARGUMENT ONLY COVERED THE FIRST BYTE, which the final round caught and which inverted the whole safety claim: on a false-negatived machine byte 1 finds THRE set and goes out, and byte 2, issued while byte 1 is still draining, finds THRE clear and takes SKIP_WAIT -- a record that emits roughly one character. `serial_emerg_probe_absent_next` now retires the boot verdict permanently on the first observed readiness, so a machine with a real transmitter self-corrects and a genuinely absent one never clears it.
  - THE COSTS ARE NOT SYMMETRIC, which is what splits the answer. Believing a false negative about OUTPUT costs the entire diagnostic channel on the target platform, undetectably from inside the machine. Believing one about the WAIT costs an emergency record that dribbles instead of blocking: the byte is still written, only the full-length wait for a transmitter already shown not to answer is skipped, and the probe-absent test sits AFTER the readiness test so a false-negative machine's byte has already gone out before it is reached.
  - SHIPPED. `serial_select_port(boot_port, boot_source, current)` is now the only writer of `s_serial_port` besides its COM1 initialiser and never returns 0 for a non-zero `current`, so the never-zero invariant is structural rather than observed; `serial_emerg_first_probe(thre_ready, probed_absent)` is the pure SEND_NOW / SKIP_WAIT / MAY_WAIT policy the bounded writer now runs on, with readiness decided FIRST so a false-negative machine's byte is already out before the probe result is consulted; `s_uart_probed_absent` carries the finding and `serial_uart_probed_absent()` lets `boot_hw.c` klog it, so a short crash record is explained rather than deduced.
  - The budget cost the filing named is retired: a port-less machine now spends none of its `SERIAL_EMERG_STUCK_BYTES` ceiling -> XREF: `01-boot-platform/TODO-10 §29` (item: "Distinguish a reservation that TIMED OUT from one that was merely taken, so a parking CPU can return the second without erasing the first.").
  - The five dead `if (!s_serial_port)` tests are deleted (`serial_putchar_raw`, `serial_init`, `serial_trygetchar`, `serial_emergency_restore_lcr`, `serial_putchar_raw_bounded`) and the invariant is stated once at the definition. An unreachable test the optimiser removes is a comment claiming a protection the object does not carry, which is what the §29 review found.
  - `serial_select_port` is an ALLOWLIST rather than an anything-but-NONE test, which rounds 2 and 3 forced in two steps: the field decides which I/O port Phase 0 programs and the panic path writes to forever after, and `boot_info` header validation covers the envelope rather than this byte. PROBE accepts only COM1 or COM2 because the scratch-register probe can report nothing else; SPCR accepts only the four legacy COM bases; every other source byte keeps the working default.
  - A NUMERIC RANGE WAS NOT ENOUGH, which is round 3's finding and the reason SPCR is not simply bounded at `SERIAL_PORT_BASE_MAX`: an in-range base such as 0xCF8 is PCI CONFIG_ADDRESS, and `serial_program_uart` writes base+1 through base+4. Refusing vendor-custom SPCR bases costs real hardware support and is the correct local defence; widening it needs the handoff body to be integrity-checked -> XREF: `01-boot-platform/TODO-01 §23` (item: "Once the body is checked, revisit the consumers that narrowed themselves to compensate, starting with the SPCR base").
- [x] Commit: `"kernel: classify a nested abort taken after the async branch stops identifying as async"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes with the disposition asserted for both intervals separately -- an entry before the completion is recorded resolves to `PANIC_ASYNC_ISOLATE`, one after it resolves to `PANIC_ASYNC_PARK`, and neither reaches `panic_try_claim_owner`. The claim-primitive fixture pre-claims each ladder rung in turn, replays the ladder, and asserts the claimed prefix is refused while the unfinished suffix is granted exactly once and the slot ends at `PANIC_PARK_DIAGNOSED`; the ungated cleanup steps are deliberately NOT modelled, because they re-run on every entry by design. The serial half asserts `serial_select_port` never returns 0 across every source and reported-base pair, that a non-legacy base is refused, and that `serial_emerg_first_probe` sends on a ready transmitter even when the boot probe reported no UART, with `serial_emerg_probe_absent_next` retiring that verdict on the first liveness.

> **Test runner:** `scripts/debug/kernel/run-boot-tests.bat` (or `bash scripts/test.sh SUITE=boot`) -- 29,932 kernel + 17 user-mode tests pass, 0 failed; smoke matrix 4/4 legs.

> **Notes:**
> - Shipped `panic_async_disposition` + `panic_park_claim_step` + the GS-independent `s_async_park_stage[]`, and an atomically-claimed `s_panic_id_owner[]` behind `smp_cpu_by_apic_id()`, so a nested abort on a parking CPU resumes the recovery tail instead of rebooting a machine the BSP is recovering.
> - Integrates through three guards into the extracted `panic_async_park()` (first-fault site, panic entry, `ke_bugcheck_emit` ingress); ONLY the diagnostic is claimed before it runs and every other tail step re-runs idempotently.
> - Downstream: `serial_init` is split from the new POST16-bracketed `serial_adopt_boot_info()`, the first code to read the serial handoff after it exists; the reported base is adopted and the no-UART finding retires the wedged-UART budget spend, itself retired on the first byte a transmitter accepts.
> - A panic entry now derives its panic-safe id ONCE: `ke_bugcheck_emit` passes it to `panic_screen_impl` and on into `panic_declared_ctx_for`, so an ordinary bugcheck no longer pays two serializing CPUIDs before the crash record is durable.
> - Canonical docs: `include/kernel/panic.h` (park ladder + disposition contract), `include/kernel/smp.h` (identity publication and lookup) and `include/kernel/drivers/serial_emergency.h` (port-selection and first-probe policy).
> - Scope boundary: the live branch is exercised by the boot matrix, not by unit tests -- a real nested abort cannot be scheduled into the panic tail from a fixture, so only the decisions are unit-tested; the reported baud is owned by TODO-29 §1.

> **Verified:** 2026-08-15 | commit `aacbdd0e3` | 3/3 items | build OK | 29,932 kernel + 17 user tests, 0 failed | smoke matrix 4/4 legs (TCG+KVM x 1+2 CPUs) | lint 0 errors
> **Accepted:** [M] an AP that faults BEFORE it claims its panic-safe identity resolves to whichever CPU did claim that id, so the park path could retract a live CPU (reason: closing it needs either the claim moved ahead of faultable AP init or abandoned APs proven quiesced, and the first contradicts the after-the-guard rule that section's own analysis established) -> XREF: `01-boot-platform/TODO-10 §25` (item: "BLOCKED on the authentication latch below: populate the cache FROM `cpu_panic_safe_apic_id()` BEFORE GS becomes observable, on every BSP and AP bring-up path, so no window exists in which the cache reads zero to an NMI or `#MC`." at line 1011)
> **Accepted:** [M] only the handoff ENVELOPE is validated, so an SPCR-reported vendor base cannot be trusted and is refused (reason: integrity for the payload body affects every scalar in boot_info, not these two fields) -> XREF: `01-boot-platform/TODO-01 §23` (item: "Once the body is checked, revisit the consumers that narrowed themselves to compensate, starting with the SPCR base" at line 837)
> **Accepted:** [M] the bootloader-reported `serial_baud` is still not adopted, now that the handoff is readable at all (reason: it is the one field that can make a working console worse, and the serial line rate belongs to the KD protocol section) -> XREF: `02-kernel-core/TODO-29 §1` (item: "Decide whether the bootloader-reported `g_boot_info.serial_baud` is adopted, now that the handoff is readable at all" at line 97)
> **Quality reviewed:** 2026-08-15 | Codex 15x (design, test-coverage x2, adversarial x5, consistency x2, perf x2, re-adversarial x3) + kernel-quality-auditor + concurrency-evidence-mapper | 14H+18M+4L fixed, 0 open | scope: kernel-code-quality

---

## 31. Guard Page Under the BSP Boot Stack

> **Spawned-by:** root
> **User impact:** a Phase-0/1 stack overflow on the BSP silently corrupts whatever BSS sits next to the boot stack and the machine continues into a wrong state, instead of stopping at a labelled guard fault. It is the one stack in the system without that net, and it is the one running the code that has no console yet to report anything.

Every other stack in this kernel is guarded. CLAUDE.md's Safety Gates list guard pages at "kernel task stacks (bottom), AP stacks (bottom), IST stacks (DF/NMI/MCE, bottom), heap end, user ELF range end", and §2 of this file allocates each IST stack as 8 KiB usable plus a 4 KiB guard installed by `ist_alloc` (`src/kernel/gdt.c`). The BSP boot stack is the exception: it is a plain `BSP_BOOT_STACK_SIZE` (16 KiB) BSS array in `src/kernel/main/boot_hw.c`, with no page alignment and no PTE cleared below it, so an overflow writes into neighbouring BSS rather than faulting.

VERIFIED 2026-08-19 by the kernel-quality auditor during the TPM headless-authorization review, which grepped the symbol and found only its declaration and the `stack_top` computation -- no `vmm_install_guard_page` call anywhere near it. The finding was raised because that section added ~444 bytes of Phase-1 locals and a headless enrollment chain whose peak was estimated near 4 KiB; nothing there overflows, and that is exactly the point -- the margin is what is protecting the boot today, not a mechanism.

The failure is worse than an ordinary overflow because of WHERE it happens: Phase 0 runs before `sti`, before klog is fully live, and a corrupted neighbour usually surfaces much later as an unrelated symptom. A labelled guard fault ("GUARD: BSP boot stack overflow") turns a silent miscompare into a named line on serial.

- [ ] Page-align the BSP boot stack and install a guard page below it, so an overflow faults with a label instead of corrupting BSS.
  - The allocation is a BSS array today, so it needs either page alignment plus a cleared PTE once paging is live, or a move to the same PMM-backed shape `ist_alloc` uses. Whichever, the guard must be installed at the earliest point the page tables allow, and the window before that must be stated.
  - Register the label in the page-fault handler's guard table beside the existing entries, so the fault names this stack rather than reporting a generic page fault.
- [ ] State the measured Phase-0/1 peak usage against the 16 KiB size, so the margin is a number rather than an assumption.
  - The deepest known chain is the headless enrollment path (`tpm_baseline_enroll_headless` -> `baseline_write_candidate` -> `tpm_baseline_predecessor` plus the NV command/response buffers), estimated near 4 KiB by struct arithmetic and not yet measured with `llvm-objdump`.
- [ ] Do the same audit for any other stack that is a bare array rather than a guarded allocation, so this is closed as a class rather than one instance.
- [ ] Unit-test what is testable: the stack's alignment and the guard entry's presence in the guard table. An actual overflow is a bare-metal validation item, not a unit test.
- [ ] Commit: `"boot: guard page under the BSP boot stack"`

**Test checkpoint:** the BSP boot stack is page-aligned, the page below it is not present, and the guard table carries an entry naming it; a deliberate recursion in a debug build faults with that label on serial rather than continuing. Scope: this section owns the BSP boot stack's guard only; IST stacks are §2, task and AP stacks are already guarded, and the heap and user-range guards are unchanged. Platforms: QEMU for the fault label, bare metal for the acceptance run.

---

## OS Comparison

| ⭐  | Feature                        | 🪟 Win11                           | 🐧 Linux                           | 🚀 Impossible OS                                                     |
| --- | ------------------------------ | ---------------------------------- | ---------------------------------- | -------------------------------------------------------------------- |
| 💎  | UC MMIO mapping                | ✅ MmMapIoSpace                    | ✅ ioremap_uc                      | ✅ §1 vmm_map_mmio_uc                                                |
| 💎  | IST stacks                     | ✅ All critical exceptions         | ✅ IST1-4 DF/NMI/MCE               | ⚠️ §2 BSP IST1-3 (AP: T09 §10)                                       |
| 💎  | ACPI FADT boot arch            | ✅ HAL checks all flags            | ✅ Gates PIT/RTC/PS2               | ✅ §4 IAPC_BOOT_ARCH parsed                                          |
| 💎  | PS/2 ACPI detection            | ✅ HAL detects i8042               | ✅ i8042.nopnp                     | ✅ §5 FADT + GSI routing                                             |
| 💎  | AHCI MSI fallback              | ✅ StorAHCI INTx fallback          | ✅ libahci polled fallback         | ✅ §6 MSI→INTx→polled                                                |
| 💎  | Graceful degradation           | ✅ Safe Mode + Last Known          | ✅ systemd continues               | ✅ §7 BOOT_TRY + degraded_mask                                       |
| 💎  | Per-process page tables        | ✅ Each process own CR3            | ✅ mm_struct per task              | ✅ §8 PML4 clone + CR3 switch                                        |
| 💎  | CPU security verify            | ✅ HAL verifies CR4/EFER           | ✅ Checks feature enable           | ✅ §9 verify NX/SMEP/SMAP                                            |
| 💎  | Boot order / UEFI-safe         | ✅ Ordered HAL + RT serialize      | ✅ setup_arch + efi_call wrap      | ✅ §10 timer-last + rt_call mask                                     |
| 💎  | Logging on main FS             | ✅ C:\Windows\System32             | ✅ /var/log                        | ✅ §12 KLOG_DIR X:\ (T24)                                            |
| 💎  | CPU feature minimums           | ✅ NX required since Vista         | ✅ verify_cpu required mask        | ✅ §13 NX+SSE2+LM+SYSCALL mask                                       |
| ⭐  | Bare-metal test matrix         | ❌ Internal only (WHQL)            | ❌ Community-driven                | ✅ §14 4-platform matrix                                             |
| ⭐  | Boot spinner liveness          | ✅ ISR-driven ring                 | ⚠️ plymouth (optional)             | ✅ §15 timer ISR @10fps Fluent                                       |
| 💎  | Abort-safe panic serial        | ✅ IPI-freezes CPUs before output  | ✅ trylock UART + smp_send_stop    | ⚠️ §16 try-lock + bounded UART; freeze §18 deferred on VFS-free dump |
| 💎  | Panic-string fault recovery    | ✅ Probes before dereferencing     | ✅ probe_kernel_read fixups        | ✅ §17 `__kread_u8` RIP-keyed fixup                                  |
| 💎  | Panic serial-lock handoff      | ✅ Owner-tracked kernel spinlocks  | ✅ Owner in `raw_spinlock` debug   | ✅ §20 owner-encoded word, one atomic transition                     |
| 💎  | Nested-NMI panic context       | ✅ `KiNmiInProgress` per-PRCB      | ✅ `nmi_count` / `in_nmi()`        | ✅ §18 per-CPU NMI depth via `idt_in_nmi()`                          |
| 💎  | NMI entry/exit marker site     | ✅ Dedicated `KiNmiInterrupt` IST  | ✅ `asm_exc_nmi` raises in entry   | ✅ §22 dedicated `isr2`, raised before the shared prologue           |
| 💎  | Offlining a dead CPU           | ✅ Live active-processor mask      | ✅ `cpu_online_mask` cleared       | ✅ §21 live online mask, publication point for bringup and reporting |
| 💎  | Async worker dispatch claim    | ✅ DPC targets checked per-queue   | ✅ `cpu_online()` per work item    | ✅ §21 two-phase generation claim, reserve then arm                  |
| 💎  | Crash-reason snapshot          | ⚠️ STOP text is a static table     | ⚠️ `vsnprintf` copy, no fault net  | ✅ §19 one guarded copy, all renderers                               |
| 💎  | Fault-safe stack unwind        | ✅ Bugchecks on invalid stack      | ✅ `copy_from_kernel_nofault`      | ✅ §19 `__kstack_read_u64`, ctx-gated                                |
| 💎  | Survived fault vs real crash   | ✅ Dump only on KeBugCheck         | ✅ `panic()` only when terminal    | ✅ §23 terminal arbitration; the survivable path revokes its record  |
| 💎  | Crash-record identity          | ⚠️ Single dump slot, overwritten   | ✅ pstore per-record ids           | ✅ §23 64-bit publication word, every transition one atomic op       |
| 💎  | Crash-record checksum          | ✅ Table-driven CRC in ntoskrnl    | ✅ `crc32()` lib, table-driven     | ✅ §24 one shared `kcrc32`, panic and klog copies retired            |
| 💎  | Bounded guarded string read    | ⚠️ Probe-then-copy, per-page       | ✅ `strncpy_from_kernel_nofault`   | ✅ §24 `__kstr_read_guarded`, one protected loop, prefix on fault    |
| ⭐  | Attributable UART stall budget | ⚠️ KD transport retries, per call  | ⚠️ `wait_for_xmitr` per character  | ✅ §26 one composite claim per allowance, owner and epoch in one CAS |
| 💎  | Crash record survives a reset  | ✅ Dump written to disk pre-reset  | ✅ pstore/ramoops region mapped UC | ✅ §28 WB + per-writer CLFLUSH, publication word stored last         |
| 💎  | Cache maintenance primitives   | ✅ `KeInvalidateAllCaches`, WBINVD | ✅ `clflush_cache_range`, `wbinvd` | ✅ §28 `cache_writeback_range`, CPUID-probed per executing CPU       |
| ⭐  | Stall budget returned on park  | ❌ No per-CPU UART stall ledger    | ❌ Budget is per call, not owned   | ✅ §29 parking CPU reclaims untouched allowances, timeouts stand     |
| 💎  | Fault while handling a fault   | ⚠️ Nested bugcheck reboots the box | ⚠️ `panic_on_second` reboots       | ✅ §30 a parking CPU resumes its recovery tail, never re-arbitrates  |
| ⭐  | Cleanup after a nested abort   | ❌ Bugcheck path restarts cleanup  | ❌ No staged panic-tail resumption | ✅ §30 monotonic stage claimed before each step, so none is re-run   |
| 💎  | No-UART transmit budget        | ⚠️ KD retries on an absent port    | ⚠️ `wait_for_xmitr` still spins    | ✅ §30 a negative boot probe skips the wait, output still attempted  |

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
- [ ] Deliberate kernel stack overflow shows a BSOD, not a triple-fault reset -- closes the §2 item at line 168, which was re-pointed here 2026-08-15 after its original target (BM Test 1) closed without covering it
  - Drive a kernel-context recursion deep enough to run past the task stack into its guard page. Expect: #DF delivered on IST1, the panic screen rendered, and the guard label on serial -- NOT a silent reset, which is what a missing or mis-wired IST looks like and is indistinguishable from success under an emulator.
  - Bare metal is the meaningful platform. Record which IST slot serviced it, so a regression that routes #DF to the wrong slot is visible rather than merely "it BSOD'd".
- [ ] Cross-boot evidence survives a hardware reset: force a panic, let it reset, confirm the next boot restores the record with a valid CRC -> XREF: `01-boot-platform/TODO-10` §28 (item: "Decide and implement the memory-type policy").
  - The ONLY acceptance evidence for §28. Every VM passes this sequence whether or not the durability bug exists, because none of them models a cache hierarchy, so a machine with real caches is the only thing that can distinguish a fix from a no-op.
  - Check both records: `X:\Crash\last-panic.txt` from the evidence page, and the `[CRASH-PREV]` serial replay from the klog crash region. They use separate mechanisms and can fail independently.

**~20 minutes.** Full regression pass.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bare_metal()` (see [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c) and `include/kernel/test/test.h`; same pattern as `02-kernel-core/TODO-11-peb-teb-user-abi.md` Unit Tests).
> Boot tests run with `debug=1` or `test=1` in boot.conf.
> Bare-metal hardening is primarily verified by multi-platform boot (QEMU WHPX/TCG, VBox, bare metal). Kernel unit tests cover the infrastructure APIs; full validation requires `scripts/test-smoke.sh` on each platform.
> `g_boot_info.degraded_mask` / `kernel_subsystem_apply_result` paths already have assertions in [`src/kernel/test/test_boot_init.c`](../../src/kernel/test/test_boot_init.c) -- extend `test_bare_metal.c` with MMIO/IST/ACPI APIs below; do not duplicate those mask tests unless a gap is found.

- [x] Create `src/kernel/test/test_bare_metal.c` with: -- 12 suites shipped; four sub-items reconciled against reality, two already covered elsewhere
  - `vmm_map_mmio_uc()` returns non-NULL and the LAPIC ID read through it matches `lapic_id()`; the PTE carries PCD+PWT+NX -- reconciled: the original text compared against an "identity-mapped read", which is a write-back MMIO read, the exact bare-metal hazard this TODO exists to prevent (CLAUDE.md Bare Metal Gotchas / MMU). Value comparison alone also passes through a WB mapping, so the PTE-attribute assertions are what actually prove the map is uncacheable. Base comes from `acpi_get_lapic_base()` rather than a hardcoded `0xFEE00000`, since the APIC base is relocatable.
  - `vmm_unmap_mmio()` on the mapped region clears the present bit (asserted via `vmm_query_flags`)
  - IST stacks allocated, page-aligned and mutually distinct, via the new `gdt_get_ist()` -- reconciled: `kernel_tss` is `static` in `src/kernel/gdt.c:44` with no accessor, so the original `kernel_tss.ist1 != 0` form does not compile from a test. Added read-only `gdt_get_ist()` + pure `tss_get_ist()` (`include/kernel/gdt.h`) rather than exporting a writable TSS pointer, which would let any caller repoint a critical-exception stack.
  - ACPI FADT flags: `acpi_has_8042()` / `acpi_has_cmos_rtc()` / `acpi_msi_supported()` / `acpi_has_vga()` / `acpi_hw_reduced()` each return strictly 0 or 1 on the live table, and each public accessor is pinned to forward to its own pure evaluator
  - hw-reduced override (§4): asserted against synthetic FADTs through new pure evaluators `acpi_fadt_*()` -- reconciled: `fadt_ptr` is `static` and set only inside `acpi_init()` (`src/kernel/acpi.c:628`), which a test may not call, and no platform this runs on publishes a hardware-reduced table, so the override branch was unreachable. The evaluators take the FADT explicitly; the public accessors became one-line wrappers over them (behavior unchanged).
  - Bounds and negative controls the override work exposed: MSI stays authoritative on a hw-reduced table, unrelated `flags` bits do not imply hardware-reduced, and each accessor's own short-table bound is asserted at the exact boundary
  - `g_boot_info.degraded_mask` is well-formed (no bits outside `SUBSYS_COUNT`; every set bit names a subsystem) -- reconciled: the original `== 0` assertion fails on any platform that legitimately degraded a subsystem, which is designed behavior rather than a defect, so it would have reported correct degradation as a test failure. The structural invariant holds on every platform. The dual-channel mask logic stays covered by `test_boot_init.c:364` and is not duplicated, per this section's own note.
  - Per-process PML4 create/destroy and `vmm_set_user_page()` User-bit -- already covered at `src/kernel/test/test_vmm.c:613` and `:671` under `TEST_CAT_MM`; not duplicated
  - CPU verification -- `TEST_SKIP` with the reason recorded in source: `cpu_verify_hardening()` halts the boot on an NX readback failure (`src/kernel/cpu_security.c:886`), so calling it from a test would take the whole test-runner boot down instead of failing one assertion. Its output is asserted end-to-end by the smoke test instead.
- [x] Add to `scripts/test-smoke.sh`: three serial greps added to `BOOT_REQUIRED_STRING_SIGNALS` (failing, not advisory)
  - Grep serial for `IAPC_BOOT_ARCH:` (FADT flags parsed)
  - Grep serial for `IST stacks:` (IST allocated)
  - Grep serial for `Verify: NX enabled` (CPU hardening ran) -- reconciled from the shorter `NX enabled` to the full emitted prefix
  - Boot completes to desktop on QEMU TCG and QEMU KVM at 1 and 2 CPUs -- covered by `scripts/test-smoke-matrix.sh` (4 legs). WHPX is `operator-gated`: it needs native Windows and is not runnable from this host, and it stays with the bare-metal platform validation in BM Test 5.
- [x] Register in `test_runner_init()`: `test_register_bare_metal()` -- `src/kernel/test/test_runner.c:985` (extern) and `:1114` (call)
- [x] Commit: `"test: add bare_metal test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=boot` passes after `test_bare_metal.c` and `test_register_bare_metal()` land; smoke script greps succeed on reference QEMU boot. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Done:** 12 suites, 64 assertions -- registered in `test_runner_init()` (2026-08-15); counts from `scripts/test-coverage.sh --save`, which moved the corpus total from 2943/12160 to 2955/12224. `bash scripts/test.sh SUITE=boot` 4904 kernel + 17 user-mode PASS; full `scripts/test.sh` 30009 kernel + 17 user-mode PASS, 0 failed; `scripts/test-smoke.sh` PASSED with the three new required signals.
> **Reconciled:** 6 items rewritten to match reality (2 untestable as written until a read-only accessor existed, 1 would have introduced a WB MMIO read, 1 would have failed on correct platform degradation, 1 grep string, 1 platform list), 2 items closed by existing coverage rather than new tests, 0 rejected as regressions.
> **Controls run (2026-08-15):** every fixture was proven able to fail before being trusted. A deliberate break in `test_bm_ist_index_bounds` and in the UC-MMIO PCD assertion each produced the expected `[FAIL]` naming the suite and line, then reverted clean; and the ist4/ist5 cases were swapped in the PRODUCTION `src/kernel/gdt.c` mapping, which failed indices 4 and 5 with transposed sentinels, then reverted. A bogus required signal added to `test-smoke.sh` produced `SMOKE TEST FAILED`, then reverted -- so the three real signals are checked by a gate that demonstrably fires.
> **Not proven by these tests:** the short-FADT cases pin the fail-safe DECISION, not memory safety -- the fixture is a full struct whose fields stay addressable after `header.length` is shortened, so an evaluator that read a field before checking the length would still pass. Catching a genuine overread needs a guard-page or sanitizer harness this freestanding target has no equivalent of.

---

## Verification

- [x] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===` -- run 2026-08-15, `=== BUILD OK ===`
- [ ] QEMU WHPX: full boot to desktop, all subsystems OK, POST codes visible in serial log (manual -- `operator-gated`, needs native Windows)
  - Not runnable from this host: `powershell.exe` is absent (WSL Windows interop off), so `scripts/machines/run-qemu.ps1 -Accel whpx` cannot be driven from here at all. That is an environment limit, not a boot failure.
- [x] QEMU TCG: full boot to desktop with PIT timer path -- `scripts/test-smoke-matrix.sh` 2026-08-15: 4/4 legs PASSED in 225s, logs under `build/smoke-matrix/`
  - The matrix supersedes the single-config check this line was written for: it also covers KVM and the 2-CPU SMP case, which is where a boot regression a 1-CPU TCG boot cannot see would land. Legs: tcg 1cpu 55s, tcg 2cpu 57s, kvm 1cpu 56s, kvm 2cpu 57s.
- [ ] VirtualBox: full boot to desktop (manual -- `operator-gated`, needs VirtualBox on the user rig)
- [ ] Bare metal (i5-11600K): BM Tests 1--5 all pass (manual -- `operator-gated`)
  - BM Tests 1--4 passed 2026-03-29. BM Test 5 is the outstanding acceptance pass, and now also carries the §2 stack-overflow BSOD item re-pointed there 2026-08-15.
- [x] No `HV_BAR()` calls remaining in codebase -- `hv_bar.h` deleted, replaced by TODO-15 VPD
- [x] Commit: `"boot: bare metal hardening complete -- all platforms boot reliably"` -- close-out commit 2026-08-15

> **Close-out (2026-08-15):** 3 of 6 Verification items automated PASS (clean build; TCG boot via the 4-leg matrix; `HV_BAR()` absence). 3 remain `operator-gated` and are NOT claimable from this host: QEMU WHPX (no `powershell.exe`), VirtualBox, and bare metal / BM Test 5. Full suite at close-out: 30009 kernel + 17 user-mode PASS, 0 failed; `scripts/lint.sh` clean.

**Test checkpoint:** All Verification bullets pass on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal; BM Test 5 items complete; `tail -1 build/build.log` shows `=== BUILD OK ===`.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot)
