---
schema_version: 1
id: higher-half-kernel-relocation
domain: 02-kernel-core
status: draft
title: "TODO-33 -- Higher-Half Kernel Relocation"
---

# TODO-33 -- Higher-Half Kernel Relocation

> **Validated:** 2026-07-15 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-07-15 | gap-audit + codex-gap-audit; 9 findings filed (6 Codex verified at file:line, 1 rejected as already-owned, 2 parity/edge)

> **Goal:** Move the kernel image out of low memory into the upper canonical half of the 64-bit virtual address space (the Windows/Linux model), and give user space the entire private lower half via per-process page tables. This permanently eliminates the kernel-BSS-vs-`USER_BASE` collision class, retires the hardcoded `0x800000` user ceiling, and is the foundation that unblocks KASLR, a clean SMEP/SMAP split, and KPTI.

> [!IMPORTANT]
> **Current state:** The kernel links and loads **low** -- `readelf -l build/kernel.exe` shows the first `LOAD` at virtual `0x100000` (1 MiB), and the image grows upward. User-mode ELF is **pinned at `0x800000`** (8 MiB) by `include/kernel/mm/user_range.h` (single source of truth; included by `vmm.c`, `pmm.c`, `task.c`), with a guard page at `0x900000` and a manual `user/user.ld` mirror. Because kernel and user share one low layout, kernel static growth (`.bss`) presses up against `0x800000`; `scripts/build.sh` has a hard guard (`✗ BSS COLLISION` when BSS end >= `USER_BASE`). As of 2026-07-15 the ceiling is REACHED: the kernel `.bss` ends at `0x7fee58`, and `__kernel_end` page-aligns to `0x7ff000` -- the LAST page before `USER_BASE`, i.e. **zero pages of headroom**. Measured during TODO-22 s22: adding ~600 bytes of `.text` pushed `.rodata`/`.data`/`.bss` each up one page (they are page-aligned and chain), landing `__kernel_end` exactly on `0x800000` and tripping the guard; that section only shipped by consolidating redundant tests to claw back 609 bytes of `.text`. The next section that adds code WILL fail the guard. This is no longer a medium-term cleanup -- it blocks kernel growth now. **CONFIRMED 2026-07-15: the prediction landed.** TODO-22 §23 (live-environment adoption, ~4 KiB of new `.text` in `env.c`) tripped the guard on its first build -- BSS end exactly `0x800000` -- and is now deferred `[/]` on this TODO; a clean HEAD build immediately before it passed, so the section's own code was the entire delta. §23 is the first section this TODO has actually stalled, and every kernel section behind it is in the same position: this TODO is now the critical path for the kernel queue, not a parallel track. Per-process page tables already exist (`01-boot-platform/TODO-10 §8`: each task has its own PML4, CR3 switches on context switch), but the kernel itself is still mapped low and shared into every address space, so per-process isolation does not relieve the low-memory contention. SMEP/SMAP are blocked because the boot PML4 carries the User bit on all kernel 2 MiB pages.

> [!WARNING]
> **Foundational -- riskiest paging change in the system.** It touches the linker, the early bring-up path (site pinned by §1; there is no `entry.asm` today), boot page-table construction, descriptor tables, the bootloader handoff ABI, the PMM's physical bounds, and the per-process memory model. §1 MUST pass `codex-design-review` before §2 starts. Sequence the sections strictly; each must be independently bootable on KVM/TCG before the next begins, or it is not done.

> [!IMPORTANT]
> **Runner autonomy + bare-metal sign-off policy (applies to EVERY section below).** The overnight runner DOES implement this TODO autonomously. For each section: implement it, then **verify on QEMU KVM + TCG** (`scripts/test-smoke.sh` boots to `C:\>` + the relevant unit tests pass). That KVM/TCG-green result IS the runner's "Verified" -- mark the section `[x]` with the Verified (KVM/TCG) + Quality-reviewed stamps and advance. **Bare-metal verification is a DEFERRED human sign-off, NOT an implementation prerequisite.** Wherever a section's Test checkpoint says "bare metal", that is the deferred sign-off: file it as a one-line item in [`overnight-todo.md`](../../overnight-todo.md) under "Waiting on a human answer" (e.g. "TODO-33 §3 higher-half bring-up: bare-metal SB-chain + early-paging sign-off") and continue -- do not block, defer, or stop the section on the absence of bare metal. **Risk accepted by the operator (2026-06-21):** a real-hardware-only paging bug may pass KVM/TCG and ship as green until the deferred bare-metal pass; the per-section sign-off items are the backstop. If a section cannot reach KVM/TCG-green (not merely bare-metal-unverified), THAT is a real blocker -- defer it `[/]` + Deferred + XREF as usual.

## Inputs

- [`include/kernel/mm/user_range.h`](../../include/kernel/mm/user_range.h) -- the `0x800000`/`0x900000` constants this TODO retires (§7)
- [`user/user.ld`](../../user/user.ld) -- user ELF base, manually mirrored from `user_range.h`
- [`scripts/build.sh`](../../scripts/build.sh) -- BSS-collision guard (`USER_BASE` check) + EIF base
- [`scripts/build-eif.py`](../../scripts/build-eif.py) -- `USER_ELF_BASE = 0x800000`
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- bootloader: loads kernel ELF, sets up identity map, hands off `boot_info`
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) -- handoff ABI (pointers translated across the high-half switch; `BOOT_INFO_VERSION`)
- [`src/boot/linker.ld`](../../src/boot/linker.ld) -- the kernel linker script (`. = 1M` load base, `ENTRY(kernel_main)`); §2 rebases its VMA
- **There is no `entry.asm`** -- the kernel has no assembly entry stub. `bootx64.c` resolves `kernel_main` from the ELF symbol table (`load_kernel()`, `:8520`) and `jump_to_kernel()` (`:10658`) calls it directly as a C function already in Long Mode; the kernel then reads its PML4 back from CR3 (`vmm.c:116`). The bootloader owns the only early page-table build (4 GiB identity map), so §1 must pin WHERE the high-half switch runs before §3 can name a file.
- -> XREF: `01-boot-platform/TODO-10-bare-metal-hardening.md §8` -- per-process page tables (the base §6 builds on; STRUCTURAL dependency)
- -> XREF: `01-boot-platform/TODO-01-boot-protocol-abi-handoff.md §8` -- boot protocol schema changelog (records the high-half handoff ABI change)

## Outcome

- Kernel text/data/bss live in the upper canonical half (Windows-style `0xFFFF800000000000` region / Linux-style `0xffffffff80000000`), mapped shared into every process.
- User space owns the entire private lower half; the `0x800000` ceiling, the `user_range.h` constants, the `0x900000` guard, and the `scripts/build.sh` BSS-collision guard are retired.
- A direct/physical map lets the kernel reach physical memory at a fixed high offset (HHDM-style) without identity-mapping low memory.
- KASLR, SMEP/SMAP, KPTI, and PCID are unblocked; this TODO is their prerequisite. The LIVE owner is `02-kernel-core/TODO-10-kernel-security-hardening` (§2 SMEP/SMAP, §4-§6 KPTI, §7 PCID, §14 KASLR -- all `[/]` + Deferred-stamped on this TODO); `03-memory-concurrency/TODO-02` carries a parallel all-`[ ]` plan for the same features. See the ownership note under §6.

## Implementation Order

| ⭐   | Order | Deliverable                                             | Depends On         | Status |
| --- | :---: | ------------------------------------------------------- | ------------------ | :----: |
| 💎   |   1   | Memory-map design + canonical layout decision           | --                 |  [x]   |
| 💎   |   2   | Linker VMA/LMA split (kernel high virtual base)         | §1                 |  [ ]   |
| 💎   |   3   | Higher-half bring-up + direct map (bootloader site)     | §1, §2             |  [ ]   |
| 💎   |   4   | Descriptor tables + per-CPU at high addresses + AP path | §3                 |  [ ]   |
| 💎   |   5   | Bootloader / `boot_info` / framebuffer high handoff     | §1, §3, D01 T01 §8 |  [ ]   |
| 💎   |   6   | Per-process PML4: kernel high shared, user low private  | §3, D01 T10 §8     |  [ ]   |
| 💎   |   7   | Retire `0x800000` USER_BASE ceiling + BSS guard         | §6                 |  [ ]   |
| ⭐   |   8   | 5-level paging (LA57) support -- exceeds Win11          | §1, §3             |  [ ]   |

> 💎 = parity work -- matches the Windows 11 and Linux memory model.
> ⭐ = exclusive work -- LA57 5-level paging is supported by Linux but **not** Windows; Impossible OS can surpass Win11 here.

---

## 1. Memory-Map Design + Canonical Layout Decision

Pin the target virtual layout BEFORE touching code. This section produces a design doc + constants, not runtime behavior; it is `codex-design-review` gated.

- [x] `KERNEL_VIRT_BASE` = `0xffffffff80000000` (`MM_KERNEL_VIRT_BASE`) -- FORCED by the `-mcmodel=kernel` already in `Makefile:25`, whose `R_X86_64_32S` relocs resolve only in the top 2 GiB; the Windows-style base fails to link
- [x] Bring-up SITE = (a) bootloader: `setup_page_tables()` also installs the high mapping, so the EXISTING call at `bootx64.c:10675` IS the low->high transition -- no far jump / `lretq`. §5 co-designs with §3
- [x] ELF handoff pinned: `load_kernel()` ALREADY copies by `p_paddr` so NO loader change is needed; `linker.ld` gains the `AT()` split, and `jump_to_kernel()` calls a high `st_value` that site (a) maps
- [x] Canonical map defined in `include/kernel/mm/memmap.h`: user / HHDM / MMIO / per-CPU / image windows, each base + extent pinned by `_Static_assert` (canonicality, non-overlap, LA57 invariance, 2 MiB LMA)
- [x] Physical->virtual scheme = fixed-offset HHDM. TWO DISTINCT relations (HHDM vs kernel-image) as separate range-checked APIs; `mm_hhdm_to_phys()` REJECTS an image address instead of returning a wrong one
- [x] Direct-map ALIAS policy: sparse RAM-only from validated UEFI types; NX + writable, never User; MMIO excluded; no writable kernel-image alias (it would defeat `wx.c`)
- [x] Transition plan: the bring-up identity map is TRANSIENT -- shrink to the AP envelope, clear User, retire after all APs ack. Permanent 4 GiB retention REJECTED (writable+User+exec alias of kernel text)
- [x] `docs/infrastructure/kernel-address-space.md` written; `codex-design-review` returned 4 High, all verified at file:line and adopted; follow-ups filed as concrete items in §2/§3/§4
- [x] Commit: `"docs: kernel address-space design -- higher-half layout + direct map"`

**Test checkpoint:** Design doc exists and passed `codex-design-review` with no unresolved High findings (all 4 adopted). `MM_KERNEL_VIRT_BASE` and the HHDM base are canonical -- verified by mutating each layout constant and confirming its `_Static_assert` trips. No runtime change: `__kernel_end` stays byte-identical to baseline at `0x7ff000`. Test on: N/A (design section).

> **Test runner:** `bash tools/memmap-check/check.sh` (host gate, also wired into `scripts/test-tooling.sh`) | 52 checks, 0 failures

> **Notes:**
>
> - Shipped `include/kernel/mm/memmap.h` (5 windows, 22 `_Static_assert`s, HHDM + image translation helpers) and `docs/infrastructure/kernel-address-space.md` (7 pinned decisions with measured evidence).
> - Zero image cost: `vmm.c` includes `memmap.h` so the assert gate compiles every build, but only asserts + unused inlines are emitted -- `__kernel_end` unchanged at `0x7ff000`, which is why §1 ships despite the BSS ceiling.
> - Behavioral gate `tools/memmap-check/check.sh` (52 host checks, wired into `test-tooling.sh`): asserts cannot evaluate a static-inline call, and clang-19 does NOT diagnose the top-of-space wraparound even under `-Weverything` -- host gcc does.
> - The SSOT claim is ENFORCED, not aspirational: `user_range.h` and `smp.h` now include `memmap.h` and assert their sub-ranges sit inside the canonical windows; both assert sets are mutation-tested.
> - Canonical doc: [`docs/infrastructure/kernel-address-space.md`](../../docs/infrastructure/kernel-address-space.md); CLAUDE.md Safety Gates carries the two binding rules (the relations are not interchangeable; never an exclusive image-window bound).
> - Scope boundary: §1 owns the layout decision + constants; §2 the linker split, §3 bring-up + walker conversion + the `PT_PML4` binding, §4 AP-envelope retirement, §7 the `USER_BASE` retirement that unblocks `test_highhalf.c`.

> **Verified:** 2026-07-15 | commit `99d70510` | 8/8 items | build OK | smoke PASS (KVM 3.23s) | tests 21408/21408 PASS | host gate 52/52
> **Deferred:** [M] `PT_PML4` (`bootx64.c:10621`) is not bound to `MM_PML4_PHYS_LIMIT`; the AP's 32-bit CR3 load truncates a >4 GiB root silently (reason: needs bootloader-TU work) -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation.md §3 (item: "Bind `PT_PML4` (`bootx64.c:10621`) to `MM_PML4_PHYS_LIMIT`" at line 129)
> **Deferred:** [M] the in-kernel `test_highhalf.c` suite cannot ship -- it tripped the BSS guard with zero headroom (reason: the ceiling this TODO exists to retire) -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7 (item: "Re-add `src/kernel/test/test_highhalf.c`" at line 200)
> **Quality reviewed:** 2026-07-15 | Codex 7x (design, adversarial, consistency, perf, re-adversarial) | 5H+8M+2L fixed, 0 open | scope: kernel-code-quality

---

## 2. Linker VMA/LMA Split

Relink the kernel at the high virtual base while still loading at the physical address the bootloader places it at.

- [ ] Set the kernel linker script VMA to `MM_KERNEL_VIRT_BASE`; keep LMA at the physical load address (`AT(...)`), so symbols resolve high but the image loads low.
- [ ] Set the kernel LMA to `MM_KERNEL_PHYS_BASE` (`0x200000`, 2 MiB) -- NOT the historical `0x100000`: a `PS=1` PDE needs a 2 MiB-aligned frame, and 1 MiB sets reserved PDE bit 20, faulting before `kernel_main` can report it
- [ ] `-mcmodel=kernel` is ALREADY in `Makefile:25` -- no codegen flag change is needed; the VMA move keeps the same `R_X86_64_32S` relocs valid (they cover the top 2 GiB as well as the low range they resolve to today)
- [ ] Update any tool that reads the kernel ELF (`scripts/build.sh` BSS check) to handle high VMA + low LMA. `load_kernel()` in `bootx64.c` needs NO change -- it already copies by `p_paddr` (`bootx64.c:8506`)
- [ ] Export LMA-derived `__kernel_phys_start` / `__kernel_phys_end` from `linker.ld`: `pmm.c:173` does `kernel_end_phys = (uintptr_t)__kernel_end` and places the PMM bitmap there -- a high VMA turns that into a bogus physical address
- [ ] Repoint PMM bitmap placement, the 1 MiB -> kernel-end reservation, and the overlap checks at the PHYSICAL bounds; reach the bitmap through the direct map. Test the exact reserved physical interval
- [ ] Confirm `readelf -l` shows kernel `LOAD` at the high virtual base with the physical address preserved.
- [ ] Commit: `"build: kernel linker VMA/LMA split -- high virtual base, low load address"`

**Test checkpoint:** `bash scripts/build.sh clean` -> `=== BUILD OK ===`; `readelf -l build/kernel.exe` shows VirtAddr `KERNEL_VIRT_BASE`, PhysAddr at the load address. (Kernel does not boot yet -- §3 establishes the mapping.) Test on: WSL TCG (build only).

> [!WARNING]
> **Regression risk:** After this section the kernel links high but nothing maps it there yet, so it will NOT boot until §3 lands. §2 and §3 must ship together or §2 stays on a branch. Rollback: revert the linker VMA change to the identity base.

---

## 3. Higher-Half Bring-Up + Direct Map

Build the boot page tables that map the kernel high and the physical memory it needs, jump into the high half, then drop the transient identity map. Boot-path; POST16 codes assigned from a free block (verify against `boot_init.h`).

> [!IMPORTANT]
> **The site is §1's decision, not a given.** There is no `entry.asm` in the tree: `bootx64.c` builds the only early page tables (4 GiB identity map) and calls `kernel_main` directly via ELF-symbol lookup, so the switch runs EITHER in the bootloader (before the call) OR in a new `src/kernel/entry.asm` stub the bootloader calls at a low physical address. If §1 picks the bootloader, that section's `boot_info`/framebuffer translation work (§5) partly precedes §3 rather than following it; resequence at §1 time.

- [ ] In `setup_page_tables()` (site (a), pinned by §1), map: (a) the kernel image -> `MM_KERNEL_IMAGE_BASE`, (b) the HHDM window, (c) a **transient** identity map of the bootloader RIP + stack so the `mov cr3` does not fault mid-stream
- [ ] No far jump / `lretq` / CS reload: the EXISTING call at `bootx64.c:10675` IS the transition once CR3 carries both maps (RIP low+identity at the call, `kernel_main`s high `st_value` mapped, CS already a long-mode selector)
- [ ] Map the image window with 4 KiB pages so W^X is per-section: a 2 MiB page forces `.text` and `.rodata` to share permissions. Reserve the extra PDPT/PD/PT pages in the fixed low block BEFORE ExitBootServices
- [ ] Route every page-table walker through `mm_canonical_from_indices()`: the naive `(pml4i << 39) | ...` in `vmm_apply_nx_policy` (`vmm.c:1475`) is non-canonical for `pml4i >= 256`, so it silently skips the high half while reporting NX enabled
- [ ] Convert raw-physical derefs to `mm_phys_to_hhdm()` -- `zero_page` (`vmm.c:178`), `get_or_create_table` (`vmm.c:213`), `vmm_init` CR3 takeover (`vmm.c:1407`); PTE/CR3 values stay physical. A <4 GiB PML4 root does NOT make these valid
- [ ] Replace the hardcoded `kernel_base = 0x100000` NX bound (`vmm.c:1439`) with image-window bounds; add tests asserting `.text` is executable and `.rodata`/`.data`/`.bss` are NX at their real high VAs
- [ ] After the jump, switch the stack to a high-virtual address, then SHRINK the identity map to the AP envelope only and CLEAR the User bit on what remains -- retaining the broad 4 GiB map would keep a writable+User+exec low alias of kernel text
- [ ] Do NOT tear down the AP bootstrap envelope yet: `ap_trampoline.asm` is `[ORG 0x8000]`, `AP_DATA 0x8E00` (`smp.c:305`, SIPI vector 0x08). Keep trampoline + data + temp stack identity-mapped in EVERY bring-up CR3 until all APs ack high entry (§4)
- [ ] Keep the kernel PML4 frame below `MM_PML4_PHYS_LIMIT` (4 GiB) forever: the AP stub loads CR3 with a 32-bit `mov eax, [AP_DATA]; mov cr3, eax` (`ap_trampoline.asm:73`) and cannot express more
- [ ] Bind `PT_PML4` (`bootx64.c:10621`) to `MM_PML4_PHYS_LIMIT` with a compile-time assert: the AP stub loads CR3 with a 32-bit `mov`, so a PML4 root above 4 GiB truncates silently. No gate reaches that constant today
- [ ] The direct map must be live BEFORE any code walks physical memory through it (pmm/vmm init, ACPI/framebuffer reads); order the switch ahead of those consumers
- [ ] Replace any post-switch physical-memory access with direct-map (`phys_to_virt`) accessors; update `vmm` phys<->virt helpers to the fixed offset.
- [ ] Add `POST16` entry/exit codes around CR3-enable, the high-half jump, and identity-map teardown.
- [ ] Commit: `"boot: higher-half page-table bring-up + direct map + high-half jump"`

**Test checkpoint:** Serial shows the high-half POST16 sequence in order; kernel reaches its existing Phase-1 banner from a high virtual RIP (`llvm-addr2line` on a logged RIP resolves to a `KERNEL_VIRT_BASE` address). `=== BUILD OK ===` and the smoke test boots to `C:\>`. Test on: QEMU WHPX + TCG; **bare metal (mandatory -- early paging differs on real CPUs)**.

> [!WARNING]
> **Regression risk:** Highest-risk section. A wrong transient identity map or a non-canonical base triple-faults at the CR3 load with no serial output. Rollback: revert to the identity-mapped low kernel (§2+§3 as a unit). Keep `scripts/test-smoke.sh` green at every step.

---

## 4. Descriptor Tables + Per-CPU at High Addresses + AP Path

Move the GDT, IDT, TSS, and per-CPU/`GS_BASE` state to high virtual addresses, and route SMP AP startup through the high-half transition.

- [ ] Relocate GDT/IDT/TSS pointers (`lgdt`/`lidt`/`ltr`) to high-virtual addresses after the §3 switch; verify the GDT user-segment order (SYSRET) constraint still holds (`gdt.h` static asserts).
- [ ] Set `GS_BASE`/`KERNEL_GS_BASE` and per-CPU data to high-virtual addresses before any interrupt fires (bare-metal-gotcha: GS_BASE before first interrupt).
- [ ] AP trampoline: APs start in the low real/protected-mode trampoline, then load the shared kernel page tables and far-jump into the high half (mirror §3 for each AP).
- [ ] Own the AP low envelope: reserve + identity-map the trampoline (0x8000), shared data (0x8E00), temp stack, and every table an AP reads, in each bring-up CR3; retire it only after all APs ack, with a timeout policy + TLB invalidation
- [ ] Re-point the IST stacks (DF/NMI/MCE) and their guard pages to high-virtual addresses.
- [ ] Add `POST16` codes around AP high-half entry.
- [ ] Commit: `"boot: descriptor tables + per-CPU + AP startup in the higher half"`

**Test checkpoint:** All CPUs reach the scheduler from high-virtual RIPs; an intentional #DF still shows the guard-page label. Serial shows per-AP high-half POST16. Test on: QEMU WHPX (2 CPUs) + TCG; **bare metal**.

---

## 5. Bootloader / `boot_info` / Framebuffer High Handoff

The bootloader runs identity-mapped and hands the kernel physical/low pointers; translate them across the switch and record the ABI change.

> [!WARNING]
> **Ordering hazard: this section is NOT strictly after §3.** If §1 picks bring-up site (a) (the bootloader maps the kernel high and calls `kernel_main` at its high VA), then `kernel_main` executes high from its first instruction, and there is no "low kernel boots, then reinterprets `boot_info` later" phase to defer this work into. The same bootloader code that builds the high mapping must already hand over pointers in their final form, so the pointer audit below is CO-DESIGNED with §3 (ship them together, as §2+§3 already do) rather than following it. If §1 picks site (b), the listed order stands. Resequence at §1 time.

- [ ] Audit every `boot_info` pointer (memory map, ACPI tables, framebuffer base, command line, initrd/UKI sections) for physical vs virtual; access them via the direct map after §3.
- [ ] Decide framebuffer mapping: map the GOP framebuffer into the kernel's high MMIO window (WC) rather than touching its physical address directly.
- [ ] Bump `BOOT_INFO_VERSION` if the handoff contract changes (mirror header + kernel header together per the boot_info ABI rules); update the drift manifest.
- [ ] Record the high-half handoff in the boot protocol schema changelog (`D01 T01 §8`).
- [ ] Commit: `"boot: boot_info + framebuffer handoff translated for higher-half kernel"`

**Test checkpoint:** Desktop renders (framebuffer reachable via the high MMIO window); ACPI tables parse; `boot_info` validation passes. `boot-info-manifest` compare gate passes. Test on: QEMU WHPX + TCG; **bare metal** (real GOP framebuffer).

---

## 6. Per-Process PML4: Kernel High Shared, User Low Private

Layer the high-half kernel onto the existing per-process page tables: every process PML4 shares the kernel's upper-half entries and owns a private lower half.

- [ ] On PML4 creation (`TODO-10 §8` path), copy/alias the kernel's upper-half PML4 entries into every new process PML4 (shared, supervisor-only).
- [ ] Prepopulate + permanently own the kernel upper-half hierarchy so a later kernel mapping never needs a NEW top-level entry propagated into live PML4s; otherwise implement synchronized propagation to every process PML4
- [ ] Pin shared-frame ownership: process PML4 teardown must never free kernel upper-half tables; require a cross-CPU TLB-shootdown test for a kernel mapping added after processes exist
- [ ] User mappings go in the lower half only; remove the assumption that user pages live just above the kernel.
- [ ] Ensure kernel upper-half entries are marked supervisor (no User bit) -- this is the clean split that unblocks SMEP/SMAP and KPTI.
- [ ] Verify TLB/CR3 switch semantics: switching to a user process keeps the kernel mapped (high half) while swapping the lower half.
- [ ] Commit: `"mm: per-process PML4 -- kernel high-half shared, user low-half private"`

**Test checkpoint:** `cmd.exe` and a second user process run in separate PML4s, each with a private lower half and the shared kernel high half; a user pointer in process A is not valid in process B. Test on: QEMU WHPX + TCG; **bare metal**.

> [!NOTE]
> **Unblocks:** with the kernel high (supervisor-only) and user low (user-only), the whole Meltdown/ret2user isolation stack becomes implementable. This section is its structural prerequisite.
> → XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md` -- the LIVE owner, whose sections are Deferred-stamped on this TODO: §2 (item: "Commit: `kernel/security: CR4 SMEP/SMAP live, IDT clac/SMAP entry path`"), §6 (item: "Commit: `\"kernel/security: KPTI dual page tables, user_cr3 allocation, Meltdown isolation active\"`"), §7 (item: "Commit: `\"kernel/security: PCID TLB tagging, NOFLUSH CR3 writes, INVPCID for targeted flush\"`"), §14 (item: "Commit: `\"kernel/security: KASLR: bootloader RDRAND slide, ELF relocation, kaslr_slide in boot_info\"`").
> → XREF: `03-memory-concurrency/TODO-02-memory-security.md §3/§4` (SMEP / SMAP) + `§6` (KPTI) -- a PARALLEL all-`[ ]` plan for the same features, with no cross-reference to D02 T10 in either direction. Ownership is FORKED; resolving it is a cross-domain call filed for the operator (`overnight-todo.md`), not this TODO's to make. PCID is already owned by D02 T10 §7 -- do not re-file it here.

---

## 7. Retire the `0x800000` USER_BASE Ceiling + BSS Guard

With user space owning the lower half, drop the hardcoded ceiling and all the bookkeeping that defended it.

- [ ] Re-add `src/kernel/test/test_highhalf.c` + `test_register_highhalf()` (`TEST_CAT_MM`): authored in §1 but reverted because it tripped the BSS guard with zero headroom. Do this FIRST in §7 -- it is the regression net for the whole relocation
- [ ] Choose the new conventional user load base (e.g. `0x400000`, or PIE/ASLR-randomized once `D03 T02 §2` lands) and update `user/user.ld` + `scripts/build-eif.py`.
- [ ] Reduce `include/kernel/mm/user_range.h` to the new base (or remove it if ASLR makes it dynamic); update the three includers (`vmm.c`, `pmm.c`, `task.c`) and their static asserts.
- [ ] Remove the `scripts/build.sh` BSS-collision guard (no longer meaningful) and the `0x900000` user-range guard page; replace with the relevant lower-half guard if still needed.
- [ ] Confirm the tactical static-pool conversions (see Notes) are no longer load-bearing -- the kernel may grow freely in the high half.
- [ ] Unblocks the kernel sections the ceiling stalled: re-run each deferred section once the guard is gone -> XREF `02-kernel-core/TODO-22` §23 (item: "Commit: `\"ntdll: live-environment adoption over an atomic env exchange\"`")
- [ ] Re-run `02-kernel-core/TODO-22` §24 once the guard is gone (item: "Commit: `\"ntdll: measured expansion budget + lookup cache for Rtl env\"`"); it also carries a latent non-atomic `env_buf_free` SMP fix
- [ ] Re-run `02-kernel-core/TODO-22` §25 once the guard is gone (item: "Commit: `\"ntdll: counted (non-_U) Rtl env read forms over a SIZE_T-safe core\"`")
- [ ] Re-run `02-kernel-core/TODO-23` §1 once the guard is gone (item: "Commit: `\"kernel: add EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS types\"`"); code COMPLETE, parked in `git stash` `todo23-s1-wip` -- apply, do not rewrite
- [ ] Commit: `"mm: retire 0x800000 user-base ceiling -- user owns the lower half"`

**Test checkpoint:** User programs load + run at the new base; `bash scripts/build.sh clean` -> `=== BUILD OK ===` with the BSS guard removed; no `user_range.h` static-assert failures. Test on: QEMU WHPX + TCG; **bare metal**.

> [!NOTE]
> **Interim bridge superseded:** before this TODO lands, the headroom risk was mitigated tactically by converting large kernel static pools (IXFS `volumes` ~1.4 MB, registry `reg_key_pool`/`reg_value_pool` ~1 MB, `klog_ring`, `devices`) from static BSS to `pmm_alloc_contiguous`, and by keeping unit-test fixtures out of the shipped kernel image. Those conversions remain correct (freestanding-kernel rule: no large static), but they stop being *necessary for headroom* once the kernel lives in the high half.

---

## 8. 5-Level Paging (LA57) Support -- Exceeds Win11

Optional competitive edge: support 57-bit virtual addresses on capable hardware. Linux made LA57 unconditional in 6.10; Windows does not support 5-level paging at all.

> [!TIP]
> Supporting LA57 lets Impossible OS address 128 PiB of virtual space and surpass Windows 11, which is stuck at 48-bit (256 TiB). Gate on `CPUID` leaf 7 subleaf 0 ECX bit 16, fall back to 4-level cleanly.

> [!WARNING]
> **`CR4.LA57` CANNOT be set from the running kernel.** x86 forbids modifying `CR4.LA57` while `CR0.PG=1`, and UEFI hands off with paging + Long Mode already enabled (`bootx64.c:8522`), so "detect LA57 then set `CR4.LA57`" implemented literally raises `#GP`, not a 5-level boot. Enablement must happen in a pre-paging window (the bootloader, which builds our page tables, drops to a PML5 root before enabling paging) or via a full architectural transition that exits paging, installs PML5, sets LA57, and re-enters Long Mode.
>
> **UNBLOCKED by §1:** enablement lives in the bootloader, which is the same site §1 already pinned for the high-half switch -- so LA57 rides the existing `setup_page_tables()` + `jump_to_kernel()` window rather than needing a new mechanism. `MM_KERNEL_VIRT_BASE` is canonical under BOTH 4- and 5-level paging (asserted in `memmap.h`), so the kernel image base does NOT move; only `MM_HHDM_BASE` must be re-based (Linux's 5-level base `0xff11000000000000` is not 4-level canonical, which is why consumers never hardcode a literal).

- [ ] Detect LA57 via `CPUID` leaf 7 subleaf 0 ECX bit 16 and (optionally) honor a `boot.conf` opt-in; default to 4-level if absent
- [ ] Place enablement in the bootloader's pre-paging window (pinned by §1 -- the same site as the high-half switch): the paging-mode choice must be made where `CR0.PG` can legally be cleared, not from `kernel_main`
- [ ] Negative tests: unsupported CPU falls back to 4-level cleanly; an already-LA57 firmware handoff is detected rather than re-enabled
- [ ] Re-base `MM_HHDM_BASE` for 5-level (sign extension at bit 56 vs 47) and parameterize the page-table walk depth. `MM_KERNEL_VIRT_BASE` is mode-INVARIANT (asserted canonical under both) and must NOT move
- [ ] Boot identically on 4-level and 5-level hosts; log the active paging mode.
- [ ] Commit: `"mm: optional 5-level paging (LA57) -- 57-bit address space on capable CPUs"`

**Test checkpoint:** On a 4-level host the kernel boots in 4-level mode; on an LA57-capable host (or QEMU `-cpu ...,la57=on`) it boots in 5-level mode. Serial logs the active mode. Test on: QEMU TCG (la57 toggled); **bare metal (Arrow Lake / Zen 5)**.

---

## OS Comparison

| ⭐   | Feature                           | 🪟 Win11                  | 🐧 Linux                         | 🚀 Impossible OS                                 |
| --- | --------------------------------- | ------------------------ | ------------------------------- | ----------------------------------------------- |
| 💎   | Kernel in upper canonical half    | ✅ `0xFFFF800000000000`+  | ✅ `0xffffffff80000000` (-2 GiB) | ⚠️ §1 pins `0xffffffff80000000`; §2-§3 move it  |
| 💎   | 128 TB user / 128 TB kernel split | ✅ 48-bit split           | ✅ 48-bit split                  | ⚠️ §1 defines the split; §7 retires the ceiling |
| 💎   | Per-process address space         | ✅ per-process            | ✅ `mm_struct` per task          | ⚠️ PML4 per task (D01 T10 §8); high-share §6    |
| 💎   | Kernel/user page-table isolation  | ✅ KVA Shadow             | ✅ KPTI                          | ⬜ Unblocked by §6 (D02 T10 §6)                  |
| 💎   | KASLR                             | ✅ kernel ASLR            | ✅ KASLR                         | ⬜ Unblocked by §3 (D02 T10 §14)                 |
| 💎   | SMEP / SMAP clean split           | ✅ enforced               | ✅ enforced                      | ⬜ Unblocked by §6 (D02 T10 §2)                  |
| 💎   | PCID no-flush ring transitions    | ✅ with KVA Shadow        | ✅ with KPTI                     | ⬜ Unblocked by §6 (D02 T10 §7)                  |
| 💎   | No hardcoded user ceiling         | ✅ no low ceiling         | ✅ no low ceiling                | ⬜ §7 retires `0x800000`                         |
| ⭐   | 5-level paging (LA57, 128 PiB)    | ❌ not supported          | ✅ unconditional (6.10+)         | ⬜ Planned -- §8 (surpasses Win11)               |
| ⭐   | Layout as asserted single source  | ⚠️ undocumented publicly | ⚠️ macros + prose, no manifest  | ✅ §1 `memmap.h` + 20-assert gate (live)         |
| ⭐   | Phys<->virt relations type-split  | ⚠️ single blended macro  | ⚠️ single `__pa`/`__va` pair    | ✅ §1 HHDM vs image, range-checked + rejecting   |

> **After §1-§7:** Impossible OS matches the Windows 11 / Linux memory model -- higher-half kernel, private per-process lower half, and the security split that KASLR / SMEP / SMAP / KPTI build on.
> **After §8:** Impossible OS exceeds Windows 11, which has no 5-level paging support.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_highhalf()` -- register in `src/kernel/test/test_runner.c`. Use `TEST_CAT_MM` (memory management).

> [!WARNING]
> **The in-kernel suite is BLOCKED on the BSS ceiling until §7.** `test_highhalf.c` was authored and wired during §1 and tripped the guard on its first build: `BSS COLLISION: kernel BSS end (0x801000) >= user base (0x800000)`. There is ZERO headroom (`__kernel_end` already page-aligns to `0x7ff000`, the last page), so it cannot ship until §7 retires the ceiling -- clawing back bytes is exactly the treadmill this TODO exists to end. Re-add it as the first work item of §7.
> → XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §7 (item: "Re-add `src/kernel/test/test_highhalf.c`")
>
> **Coverage is NOT absent meanwhile.** `tools/memmap-check/check.sh` (48 checks, wired into `scripts/test-tooling.sh`) exercises the same constants and translation helpers on the HOST at zero kernel-image cost. This is not a stopgap: a `_Static_assert` cannot evaluate a static-inline call, so the helpers have no compile-time net in-kernel either -- and clang-19 does not diagnose the top-of-address-space wraparound under `-Wall -Wextra -Werror` or even `-Weverything`, while host gcc catches it via `-Wtype-limits`. The host gate is the only automated net for that bug class and stays after §7.

- [ ] Create `src/kernel/test/test_highhalf.c` (BLOCKED -- see the warning above; re-add in §7) with:
  - `phys_to_virt(p)` / `virt_to_phys(v)` round-trip equals the original for a sample physical page.
  - A kernel symbol address (e.g. `&kmain`) is `>= KERNEL_VIRT_BASE` (canonical higher half).
  - `KERNEL_VIRT_BASE` and the direct-map offset are canonical (bits 48-63 all 1, or bit-56 sign-extended under LA57).
  - A freshly created process PML4 shares the kernel upper-half entries and has an empty lower half (post-§6).
  - The retired `user_range.h` ceiling is gone or set to the new low base (post-§7).
- [ ] Register in `test_runner_init()`: `test_register_highhalf()`
- [ ] Author the matching runner bat: `scripts/debug/kernel/run-mm-tests.bat` (extend the existing MM bat; no new category needed).
- [ ] Commit: `"test: add higher-half kernel address-space test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===` (BSS-collision guard removed after §7).
- [ ] `readelf -l build/kernel.exe` shows kernel `LOAD` VirtAddr at `KERNEL_VIRT_BASE`, PhysAddr at the load address.
- [ ] Boot serial shows the higher-half POST16 sequence in order; a logged kernel RIP resolves (via `llvm-addr2line-19 -e build/kernel.exe`) to a high-virtual address.
- [ ] `scripts/test-smoke.sh` boots to `C:\>` with the kernel running high-half.
- [ ] Two user processes run in separate PML4s with private lower halves + shared kernel high half.
- [ ] `make test-mm` shows all PASS including the new higher-half suite.
- [ ] Runner bar: QEMU KVM + TCG green per section (smoke + unit tests). Bare-metal sign-off is DEFERRED -- file per-section items in `overnight-todo.md` (see the Runner-autonomy policy above).
- [ ] Commit: `"mm: higher-half kernel relocation -- complete"`

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | N suites, 0 failures
