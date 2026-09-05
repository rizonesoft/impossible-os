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
> **Current state:** The kernel links and loads **low** -- `readelf -l build/kernel.exe` shows the first `LOAD` at virtual `0x100000` (1 MiB), and the image grows upward. User-mode ELF is **pinned at `0x800000`** (8 MiB) by `include/kernel/mm/user_range.h` (single source of truth; included by `vmm.c`, `pmm.c`, `task.c`), with a guard page at `0x900000` and a manual `user/user.ld` mirror. Because kernel and user share one low layout, kernel static growth (`.bss`) presses up against `0x800000`; `scripts/build.sh` has a hard guard (`✗ BSS COLLISION` when BSS end >= `USER_BASE`). **HISTORICAL as of 2026-07-17 -- the ceiling is no longer reached.** It WAS: on 2026-07-15 the kernel `.bss` ended at `0x7fee58` and `__kernel_end` page-aligned to `0x7ff000`, the LAST page before `USER_BASE` -- zero pages of headroom. §10 then moved the large static pools to frame-backed storage and the BSS end fell to **`0x6c2000`, ~1272 KiB of headroom**. The layout below is still accurate and §3 still owns the permanent fix; only the zero-headroom emergency is over. Measured during TODO-22 s22: adding ~600 bytes of `.text` pushed `.rodata`/`.data`/`.bss` each up one page (they are page-aligned and chain), landing `__kernel_end` exactly on `0x800000` and tripping the guard; that section only shipped by consolidating redundant tests to claw back 609 bytes of `.text`. The next section that adds code WILL fail the guard. This is no longer a medium-term cleanup -- it blocks kernel growth now. **CONFIRMED 2026-07-15: the prediction landed.** TODO-22 §23 (live-environment adoption, ~4 KiB of new `.text` in `env.c`) tripped the guard on its first build -- BSS end exactly `0x800000` -- and is now deferred `[/]` on this TODO; a clean HEAD build immediately before it passed, so the section's own code was the entire delta. §23 is the first section this TODO has actually stalled, and every kernel section behind it is in the same position: this TODO is now the critical path for the kernel queue, not a parallel track. Per-process page tables already exist (`01-boot-platform/TODO-10 §8`: each task has its own PML4, CR3 switches on context switch), but the kernel itself is still mapped low and shared into every address space, so per-process isolation does not relieve the low-memory contention. SMEP/SMAP are blocked because the boot PML4 carries the User bit on all kernel 2 MiB pages.

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
- [`src/boot/linker.ld`](../../src/boot/linker.ld) -- the kernel linker script (`. = 1M` load base, `ENTRY(kernel_main)`); §3 rebases its VMA
- **There is no `entry.asm`** -- the kernel has no assembly entry stub. `bootx64.c` resolves `kernel_main` from the ELF symbol table (`load_kernel()`, `:8520`) and `jump_to_kernel()` (`:10658`) calls it directly as a C function already in Long Mode; the kernel then reads its PML4 back from CR3 (`vmm.c:116`). The bootloader owns the only early page-table build (4 GiB identity map), so §1 must pin WHERE the high-half switch runs before §3 can name a file.
- -> XREF: `01-boot-platform/TODO-10-bare-metal-hardening.md §8` -- per-process page tables (the base §6 builds on; STRUCTURAL dependency)
- -> XREF: `01-boot-platform/TODO-01-boot-protocol-abi-handoff.md §8` -- boot protocol schema changelog (records the high-half handoff ABI change)

## Outcome

- Kernel text/data/bss live in the upper canonical half (Windows-style `0xFFFF800000000000` region / Linux-style `0xffffffff80000000`), mapped shared into every process.
- User space owns the entire private lower half; the `0x800000` ceiling, the `user_range.h` constants, the `0x900000` guard, and the `scripts/build.sh` BSS-collision guard are retired.
- A direct/physical map lets the kernel reach physical memory at a fixed high offset (HHDM-style) without identity-mapping low memory.
- KASLR, SMEP/SMAP, KPTI, and PCID are unblocked; this TODO is their prerequisite. The LIVE owner is `02-kernel-core/TODO-10-kernel-security-hardening` (§2 SMEP/SMAP, §4-§6 KPTI, §7 PCID, §14 KASLR -- all `[/]` + Deferred-stamped on this TODO); `03-memory-concurrency/TODO-02` carries a parallel all-`[ ]` plan for the same features. See the ownership note under §6.

## Implementation Order

| ⭐  | Order | Deliverable                                                 | Depends On             | Status |
| --- | :---: | ----------------------------------------------------------- | ---------------------- | :----: |
| 🔥  |  10   | Tactical BSS headroom: large static pools -> dynamic        | --                     |  [x]   |
| 🔥  |  11   | Unpark the ceiling-stalled kernel queue (status sweep)      | §10                    |  [/]   |
| 🔥  |  12   | Second tactical BSS pass: reclaim a large static again      | --                     |  [x]   |
| 🔥  |  13   | Third tactical reclamation pass: buy a page of headroom     | --                     |  [x]   |
| 🔥  |  14   | Fourth tactical reclamation pass: reclaim test-only statics | --                     |  [x]   |
| 🔥  |  15   | Fifth tactical reclamation pass: convert the `pipes` pool   | --                     |  [ ]   |
| 💎  |   1   | Memory-map design + canonical layout decision               | --                     |  [x]   |
| 💎  |   2   | Direct map construction (install HHDM; kernel still low)    | §1                     |  [x]   |
| 💎  |   9   | VMM walker conversion -- derefs onto the HHDM helper        | §2                     |  [/]   |
| 💎  |   3   | Linker VMA/LMA split + higher-half jump (one unit)          | §1, §2, §9             |  [/]   |
| 💎  |   4   | Descriptor tables + per-CPU at high addresses + AP path     | §3                     |  [/]   |
| 💎  |   5   | `boot_info` / framebuffer handoff + identity teardown       | §1, §3, §9, D01 T01 §8 |  [/]   |
| 💎  |   6   | Per-process PML4: kernel high shared, user low private      | §3, D01 T10 §8         |  [/]   |
| 💎  |   7   | Retire `0x800000` USER_BASE ceiling + BSS guard             | §6                     |  [/]   |
| ⭐  |   8   | 5-level paging (LA57) support -- exceeds Win11              | §1, §3                 |  [/]   |

> 💎 = parity work -- matches the Windows 11 and Linux memory model.
> ⭐ = exclusive work -- LA57 5-level paging is supported by Linux but **not** Windows; Impossible OS can surpass Win11 here.
> 🔥 = unblocks the stalled kernel queue; depends on nothing and ships FIRST (see §10, then the §11 sweep that collects on it).
> **Ship order note:** the row order above IS the ship order; the number column is the section-heading id. Original §2 (direct map + walker conversion) was SPLIT 2026-07-16 on the design review's SPLIT-CONFIRMED verdict (2+ worker contexts) into §2 (HHDM construction, additive, kernel still walks identity) and a new §9 (walker conversion) appended at file end to keep the §5/§6/§7 external XREF anchors stable. §9 ships between §2 and §3; §3 and §5 both depend on it.

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
> - Scope boundary: §1 owns the layout decision + constants; §2 the direct map + walker conversion + the `PT_PML4` binding (all while the kernel still links low), §3 the linker split + high-half jump as one atomic unit, §4 AP-envelope retirement, §7 the `USER_BASE` retirement that unblocks `test_highhalf.c`.
> - §2/§3 were resequenced 2026-07-16: the original §2 (linker split) could not ship alone -- it links high with nothing mapped there, so it does not boot, and the runner ships+pushes per section. The additive direct-map work was pulled ahead of the flip instead, which `vmm.c:1417-1419` already anticipated ("moves to `mm_phys_to_hhdm()` when the direct map lands"). Each section is now independently bootable, per this TODO's own sequencing rule.

> **Verified:** 2026-07-15 | commit `99d70510` | 8/8 items | build OK | smoke PASS (KVM 3.23s) | tests 21408/21408 PASS | host gate 52/52
> **Deferred:** [M] the in-kernel `test_highhalf.c` suite cannot ship -- it tripped the BSS guard with zero headroom (reason: the ceiling this TODO exists to retire) -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7 (item: "Re-add `src/kernel/test/test_highhalf.c`" at line 250)
> **Quality reviewed:** 2026-07-15 | Codex 7x (design, adversarial, consistency, perf, re-adversarial) | 5H+8M+2L fixed, 0 open | scope: kernel-code-quality

---

## 2. Direct Map Construction (kernel still low)

Build the HHDM direct map and install it in an unused PML4 slot while the kernel still links and boots LOW and still WALKS page tables via the identity map. Purely ADDITIVE: it changes no address the kernel executes from and touches no existing walker, so it is independently bootable. The walker conversion that moves every deref onto this map is split out to §9 (design review SPLIT-CONFIRMED, 2026-07-16, 2+ worker contexts); together §2 + §9 shrink §3 to the flip itself.

> [!IMPORTANT]
> **Why this precedes the linker split.** §1's own bring-up comment pins the order: `vmm.c:1417-1419` says the raw-physical CR3 takeover "moves to `mm_phys_to_hhdm()` when the direct map lands". The HHDM is a NEW window in an unused PML4 slot, so installing it does not move any address the kernel executes from. The kernel-IMAGE mapping is deliberately NOT here: pre-split the image's physical base is still `0x100000`, not `MM_KERNEL_PHYS_BASE` (`0x200000`), so mapping the image window belongs with the split that moves it (§3).

> [!WARNING]
> **HIGH RISK -- the riskiest paging change in the system. Codex design review (2026-07-16, `[review-kind: design]`) returned NO-SHIP on the original 2 MiB-only / fixed-pre-EBS-arena plan; the 3 HIGH findings + Q1-Q4 are adopted into the items below.** (1) [RESOLVED by §9, commit `49883c1f`] the kernel root WAS physical-as-pointer and `vmm_get_kernel_cr3()` returned it raw for CR3, so a blanket HHDM conversion would have triple-faulted at the next kernel-task switch; §9 shipped the split -- `kernel_pml4` is the HHDM WALK pointer and the physical CR3 root is derived via `mm_hhdm_to_phys(kernel_pml4)`, so a walk pointer never reaches CR3. (2) NX HHDM leaves are walked before `cpu_enable_nx()` (`boot_hw.c:493` vs `:612`; `bootx64.c` never touches EFER); with NXE=0 bit 63 is RESERVED, so the first HHDM walk faults on the BSP AND every AP. (3) [H1] 2 MiB-only leaves cannot express the sparse RAM-only map: UEFI usable ranges are 4 KiB-aligned (`bootx64.c:9120`), so a partial 2 MiB bucket at any edge/hole either aliases reserved/MMIO WB (forbidden) or leaves a PMM frame with no leaf -- `mm_phys_to_hhdm()` only range-checks (nonzero, <64 TiB), so the miss is a silent phys-page-0 write (`memmap.h:298-304`), not a fault. (4) [H3] a WRITABLE HHDM alias of the kernel image (loaded into EfiConventionalMemory) defeats `kernel_wx_protect()` (`boot_interrupts.c:195-211`) -- §1 Decision 6 / `kernel-address-space.md:143-145` forbid it.

- [x] INSTALL-SITE = (a) bootloader: `bl_hhdm_reserve_arena()` sizes+allocates the arena PRE-EBS; `setup_page_tables()` installs leaves POST-EBS (memory writes only); it owns PML4 slot 0, 273-400 free
- [x] [F2] `bl_hhdm_collect_usable`: page-aligned, coalesced usable-RAM intervals (the types `pmm_init` frees), clamped to the 64 TiB window; adds the retained fixed PT frames `0x70000-0x75fff`; kernel-image envelope excluded at emit
- [x] [Q1/F2] `bl_hhdm_worstcase_pages()`: occupancy-based PDPT/PD (holes cost nothing) + envelope-aware PT budget; `AllocatePages(AllocateMaxAddress, <4 GiB)`; every arena frame bounds-checked (fail-closed)
- [x] [F1] `bl_hhdm_retag_arena_reserved()`: POST-EBS splits the covering mmap entry, retags the exact arena interval `UEFI_MMAP_RESERVED` (capacity-checked) so `pmm_init` keeps it used; arena re-added to the plan for §9
- [x] [Q1/H1/F3] `bl_hhdm_install_leaves()`: PML4 slots 273+(phys>>39) (273-400, bound-checked); 2 MiB whole-bucket PS=1 PDEs + 4 KiB partial PTs; leaves NX+Writable+supervisor, tables P|RW, never User
- [x] [Q2/Q3] `bl_enable_nxe()`: CPUID.NX-gated EFER.NXE RMW + read-back + FATAL, before any NX leaf is WALKED; NXE persists BSP->kernel, APs enable it in the trampoline
- [x] [H3] kernel-image PT_LOAD envelope `[g_kernel_img_lo,g_kernel_img_hi)` excluded from the writable HHDM alias AND from the arena location (disjointness assert) so the HHDM cannot defeat W^X -- §1 Decision 6
- [x] Bind `PT_PML4` to `MM_PML4_PHYS_LIMIT` via mirrored literal + local `_Static_assert(PT_PML4 < 4 GiB)` in `bootx64.c`. Closes §1 Deferred (AP 32-bit CR3 truncation)
- [x] `POST16` 0xB061-0xB065 around the HHDM steps (PLAN/ARENA_OK pre-EBS, NXE/INSTALL/OK post-EBS), classified in `tools/post16-manifest`; smoke-verified in order
- [ ] Reclaim the unused worst-case arena tail: retag only the `s_hhdm_arena_next` used frames RESERVED (retag after `bl_hhdm_install_leaves`), leaving the ~4-5 MiB over-allocation for pmm to free (review defer; efficiency, not correctness)
- [x] Commit: `"boot: HHDM direct map construction (kernel still walks identity)"`

**Test checkpoint:** `=== BUILD OK ===`; `scripts/test-smoke.sh` boots to `C:\>`; full `scripts/test.sh` green. The kernel still runs LOW from the identity map AND still walks page tables physically -- this section only INSTALLS the HHDM in unused PML4 slots, so behavior is unchanged and the section is independently bootable. The readback proof runs in the BOOTLOADER (`jump_to_kernel`, after CR3 loads the HHDM): a frame read through its HHDM alias must equal the identity read (aliasing) and the walk of an NX leaf proves EFER.NXE landed -- an in-kernel test is BSS-blocked until §7 (same ceiling as `test_highhalf.c`). The walker conversion and its identity-map-disabled test are §9. Test on: QEMU KVM + TCG.

> [!NOTE]
> **Regression risk: MEDIUM (construction only; the HIGH-risk walker conversion is §9).** This section adds a mapping in unused slots and touches no address the kernel executes from or walks through, so its failure modes are contained to the install itself: the design review found a sparse-map hole leaves a PMM frame with no leaf (silent phys-page-0 write on any later HHDM read) and a writable kernel-image alias defeats W^X. Both are construction-time invariants tested here. Rollback: revert the HHDM install (no existing PTE or pointer changes). The repo-wide 13-site deref conversion, the root phys-vs-walk split, and the identity-map-disabled test all live in §9.

> **Test runner:** `bash tools/memmap-check/check.sh` (host, PML4-slot math) + `scripts/test-smoke.sh` (bootloader readback "HHDM: direct map verified" + boot to `C:\>`) | 0 failures. In-kernel suite BSS-blocked -> §7.

> **Notes:**
>
> - Shipped the HHDM constructor `bl_hhdm_*` in `src/boot/uefi/bootx64.c`: pre-EBS arena sizing + `AllocateMaxAddress`, post-EBS `UEFI_MMAP_RESERVED` retag + multi-slot leaf install (2 MiB PS=1 + 4 KiB partial, NX/RW) into PML4 273-400, plus EFER.NXE.
> - Additive only: installs into unused PML4 slots, enables NXE on the BSP, touches no address the kernel executes from; smoke boot unchanged (`C:\>`, POST16 0xB061-0xB065 + `jump_to_kernel` identity-vs-alias readback verified).
> - Root cause fixed: the bootloader does NOT zero `.bss` (firmware poisons it 0xAF), so `g_kernel_img_hi`/`g_hhdm_arena_*` are reset at RUNTIME; without it the disjointness check false-fataled on poison.
> - Added `mm_hhdm_pml4_slot()` + `MM_HHDM_PML4_SLOT`(273)/`_LAST`(400) to `memmap.h` (host-tested in `tools/memmap-check`); §9 reuses them. Codex design + adversarial adoptions in the commit message.
> - Canonical doc: [`docs/infrastructure/kernel-address-space.md`](../../docs/infrastructure/kernel-address-space.md).
> - Scope boundary: §2 owns HHDM CONSTRUCTION only; §9 owns walker conversion + root phys-vs-walk split + identity-map-disabled test; §7 retires the BSS ceiling blocking the in-kernel suite.

> **Verified:** 2026-07-17 | commit `e9c73f8f` | 9/10 items | build OK | smoke PASS (KVM 3.4s) | tests 21408+16 PASS | host gate 52/52
> **Deferred:** [M] worst-case arena tail (~4-5 MiB) reserved-but-unused (reason: efficiency, not correctness) -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation.md §2 (item: "Reclaim the unused worst-case arena tail" at line 124)
> **Quality reviewed:** 2026-07-17 | Codex 5x (design, adversarial, consistency, perf, re-adversarial) | 3H+1M fixed, 1M deferred, 1M rejected | scope: boot-code-quality
> **Design reviewed:** 2026-07-16 | TWO Codex `[review-kind: design]` rounds. Round 1 NO-SHIP -> SPLIT into §2 (HHDM construction) + §9 (walker conversion). Round 2 (post-split, install-site (a) plan) NO-SHIP -> 3 HIGH adopted into the items above and VERIFIED at file:line: F1 the PMM reclaims an EfiLoaderData arena (pmm.c:203-219 frees Loader/BootServices, reserves only 1 MiB+image) -> retag the arena interval `UEFI_MMAP_RESERVED` in the normalized mmap; F2 sizing must consume the FINAL post-carve plan (W^X carve makes a whole bucket partial) + use the loaded ELF `p_paddr` envelope, not kernel symbols; F3 HHDM spans PML4 slots 273-400 (64 TiB), not one slot -> multi-slot install loop. Q3 NXE safe if CPUID-gated RMW + read-back + FATAL; Q4 no TLB hazard (inactive root, slot 0 untouched). **Design DONE + adopted; §2 items are the implementation-ready spec -- a fresh context implements DIRECTLY (skip re-review, cite this stamp).** Install-site RESOLVED to (a) bootloader.

---

## 3. Linker VMA/LMA Split + Higher-Half Jump

Relink the kernel at the high virtual base, map the image there, and make the existing bootloader call the low->high transition. **This section is the atomic unit: the linker split and the jump CANNOT be separated** -- after the split the kernel links high, and nothing maps it there until the jump lands. §2 has already built the direct map and §9 has moved the walkers onto it, so what remains here is the flip itself. (Depends on §9: the flip keeps the identity map live, but the walkers must resolve through the HHDM before §5 retires that map.)

> [!IMPORTANT]
> **Site (a) is pinned by §1: the bootloader.** There is no `entry.asm` in the tree -- `bootx64.c` builds the only early page tables and calls `kernel_main` via ELF-symbol lookup, so `setup_page_tables()` installs the high mapping and the EXISTING call at `bootx64.c:10675` becomes the transition. No far jump, no `lretq`, no CS reload. Consequently §5's `boot_info`/framebuffer translation is CO-DESIGNED with this section rather than following it: `kernel_main` executes high from its first instruction, so there is no "boots low, reinterprets `boot_info` later" phase to defer into.

- [ ] Set the linker VMA to `MM_KERNEL_VIRT_BASE` and keep the LMA at the load address via `AT(...)`, so symbols resolve high but the image still loads low. `linker.ld:25` is `. = 1M` with no `AT()` today
- [ ] Move the LMA to `MM_KERNEL_PHYS_BASE` (`0x200000`) -- NOT the historical `0x100000`: a `PS=1` PDE needs a 2 MiB-aligned frame, and 1 MiB sets reserved PDE bit 20, faulting before `kernel_main` can report it
- [ ] No codegen flag change: `-mcmodel=kernel` is already in `Makefile:25` and its `R_X86_64_32S` relocs resolve in the top 2 GiB
- [ ] Export LMA-derived `__kernel_phys_start`/`__kernel_phys_end` from `linker.ld`, then repoint `pmm.c` kernel_end_phys, bitmap placement, the 1 MiB reservation, and the disjointness checks at PHYSICAL bounds
- [ ] Teach the `scripts/build.sh` BSS check to read high VMA + low LMA. `load_kernel()` needs NO change -- it already copies by `p_paddr`
- [ ] Claim loaded kernel-image pages (`AllocatePages(AllocateAddress)`) right after `load_kernel()`, before any post-load alloc, so none lands on the unclaimed image (pre-existing; §2 guards only its arena). ← XREF: §2
- [ ] Map the kernel image to `MM_KERNEL_IMAGE_BASE` with 4 KiB pages (per-section W^X; a 2 MiB page forces `.text` and `.rodata` to share permissions), plus a TRANSIENT identity map of the bootloader RIP + stack
- [ ] No far jump / `lretq` / CS reload: the existing call at `bootx64.c:10675` IS the transition once CR3 carries both maps and `kernel_main` high `st_value` is mapped (CS is already a long-mode selector)
- [ ] Replace the `kernel_base = 0x100000` NX bound (`vmm.c:1452`) with image-window bounds; assert `.text` executable and `.rodata`/`.data`/`.bss` NX at their real high VAs
- [ ] After the jump switch the stack to a high VA. Do NOT shrink the identity map here: early code still derefs handoff pointers physically (`boot_progress.c:137`), and that translation is §5. Teardown moves to §5
- [ ] Keep the AP envelope: `ap_trampoline.asm` is `[ORG 0x8000]` with `AP_DATA 0x8E00`, and `smp.c:317` hands each AP the BSP live CR3, so trampoline + data + temp stack stay identity-mapped in EVERY bring-up CR3 until all APs ack (§4)
- [ ] Confirm `readelf -l build/kernel.exe` shows VirtAddr `MM_KERNEL_VIRT_BASE` with PhysAddr preserved at the LMA
- [ ] Add `POST16` entry/exit codes around CR3-enable and the high-half jump (teardown codes land with the teardown in §5)
- [ ] Commit: `"boot: kernel linker VMA/LMA split + higher-half jump"`

**Test checkpoint:** Serial shows the high-half POST16 sequence in order; the kernel reaches its existing Phase-1 banner from a high virtual RIP (`llvm-addr2line` on a logged RIP resolves to a `MM_KERNEL_VIRT_BASE` address). `=== BUILD OK ===` and the smoke test boots to `C:\>`. Test on: QEMU KVM + TCG; **bare metal (deferred human sign-off per the policy above -- NOT an implementation prerequisite)**.

> [!WARNING]
> **Regression risk: HIGHEST in the system.** A wrong transient identity map or a non-canonical base triple-faults at the CR3 load with NO serial output. The split and the jump ship as ONE commit -- an intermediate commit that links high without mapping high does not boot, so it must never be pushed alone. Rollback: revert to the identity-mapped low kernel (this whole section as a unit); §2's direct map is independent and stays. Keep `scripts/test-smoke.sh` green at every step.

> **Deferred:** [Critical] 2026-07-17 -- awaiting-operator design decision. `codex-design-review` returned **NO-SHIP** (2 Critical + 2 High, all verified at file:line); §3 cannot ship standalone. **BLOCKER (crit #2): physical collision.** The image is 6.99 MiB (`__kernel_start` 0x100000 .. `__kernel_end` 0x7fe000); at LMA `MM_KERNEL_PHYS_BASE` 0x200000 it ends at phys 0x8fe000, overlapping `USER_PT_WINDOW` [0x800000, 0xa00000]. User exec still writes the launcher ELF + stack to identity-mapped phys 0x800000, overwriting the kernel `.bss` tail + PMM bitmap; PMM reservation cannot stop a direct user write, and the smoke test reaches `cmd.exe` so gating userspace fails the ship checkpoint. A 6.99 MiB image does not fit 2 MiB-aligned below 0x800000 (only 6 MiB free), so the fix is a SEQUENCING decision the runner will not make unattended: (a) relocate user processes to private physical frames BEFORE §3 (pull §6 ahead of the jump), or (b) re-base `MM_KERNEL_PHYS_BASE` above `USER_PT_WINDOW` in `memmap.h` (pinned-SSOT/layout change). Circular dep (§3 <-> §6/§7) the plan did not resolve. Re-attempt corrections captured: crit #1 -- checklist item "Set the linker VMA to `MM_KERNEL_VIRT_BASE`" is WRONG; VMA must be `MM_KERNEL_IMAGE_BASE` (0xffffffff80200000) or `kernel_main`'s `st_value` falls outside the image leaves and triple-faults. high #3 -- `boot_payload.c:152-153` also reads `__kernel_start`/`__kernel_end` as PHYSICAL bounds; after the split they are high VMAs, silently disabling payload-vs-image overlap detection (repoint to `__kernel_phys_start/end`). high #4 -- the post-load reservation must claim the image PLUS the max PMM bitmap extent before ANY post-load Boot Services alloc. Confirmed OK: `AT()` + LMA-derived symbols is valid, and the existing 4 GiB identity map already covers the transition RIP/stack (no second transient map needed). -> XREF: `todo/answers.md` Q3 (higher-half vs user-frame relocation sequencing -- operator decision); implementation owner for option (a) is this file §6 (item: "User mappings go in the lower half only; remove the assumption that user pages live just above the kernel").

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

> **Deferred:** [Critical] 2026-07-17 -- cascade-blocked on §3. This section builds on the higher-half kernel that §3's low->high jump establishes (Depends-On §3), so it cannot start until §3 lands. §3 is itself deferred awaiting an operator sequencing decision (the physical image / user-window collision). -> XREF: this file §3 (item: "Commit: `"boot: kernel linker VMA/LMA split + higher-half jump"`") + `todo/answers.md` Q3.

---

## 5. Bootloader / `boot_info` / Framebuffer High Handoff

The bootloader runs identity-mapped and hands the kernel physical/low pointers; translate them across the switch and record the ABI change.

> [!WARNING]
> **Ordering hazard: site (a) is pinned, so this section is NOT strictly after §3.** `kernel_main` executes high from its first instruction, so there is no "low kernel boots, then reinterprets `boot_info` later" phase to defer this work into: the same bootloader code that builds the high mapping must hand over pointers in their final form. The pointer audit below is CO-DESIGNED with §3.

> [!IMPORTANT]
> **This section OWNS the identity-map teardown** (moved out of §3 by the 2026-07-16 design review). §3 makes the jump but deliberately leaves the identity map live, because early code still derefs handoff pointers physically -- `boot_progress.c:137` reads `g_boot_info.fb.addr` directly. Shrinking the map before those pointers are translated faults early boot with no output. The map staying live through §3/§4 is the status quo (it is already live and User-mapped today), so this is a not-yet-fixed hazard, never a new regression.

- [ ] Audit every `boot_info` pointer (memory map, ACPI tables, framebuffer base, command line, initrd/UKI sections) for physical vs virtual; access them via the direct map after §3.
- [ ] Decide framebuffer mapping: map the GOP framebuffer into the kernel's high MMIO window (WC) rather than touching its physical address directly.
- [ ] Once every handoff pointer above is translated, SHRINK the identity map to the AP envelope and CLEAR the User bit on what remains -- a retained 4 GiB map is a writable+User+exec low alias of kernel text (§1 Decision 6)
- [ ] Teardown lands HERE, not in §3: `boot_progress.c:137` derefs `g_boot_info.fb.addr` physically, so shrinking the map before this section faults early boot. Add the `POST16` teardown codes with it
- [ ] Bump `BOOT_INFO_VERSION` if the handoff contract changes (mirror header + kernel header together per the boot_info ABI rules); update the drift manifest.
- [ ] Record the high-half handoff in the boot protocol schema changelog (`D01 T01 §8`).
- [ ] Commit: `"boot: boot_info + framebuffer handoff translated for higher-half kernel"`

**Test checkpoint:** Desktop renders (framebuffer reachable via the high MMIO window); ACPI tables parse; `boot_info` validation passes; the identity map is gone except the AP envelope, and what remains carries no User bit. `boot-info-manifest` compare gate passes. Test on: QEMU KVM + TCG; **bare metal** (real GOP framebuffer, deferred human sign-off).

> **Deferred:** [Critical] 2026-07-17 -- cascade-blocked on §3. This section builds on the higher-half kernel that §3's low->high jump establishes (Depends-On §3), so it cannot start until §3 lands. §3 is itself deferred awaiting an operator sequencing decision (the physical image / user-window collision). -> XREF: this file §3 (item: "Commit: `"boot: kernel linker VMA/LMA split + higher-half jump"`") + `todo/answers.md` Q3.

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

> **Deferred:** [Critical] 2026-07-17 -- cascade-blocked on §3. This section builds on the higher-half kernel that §3's low->high jump establishes (Depends-On §3), so it cannot start until §3 lands. §3 is itself deferred awaiting an operator sequencing decision (the physical image / user-window collision). -> XREF: this file §3 (item: "Commit: `"boot: kernel linker VMA/LMA split + higher-half jump"`") + `todo/answers.md` Q3.

---

## 7. Retire the `0x800000` USER_BASE Ceiling + BSS Guard

With user space owning the lower half, drop the hardcoded ceiling and all the bookkeeping that defended it.

- [ ] Re-add `src/kernel/test/test_highhalf.c` + `test_register_highhalf()` (`TEST_CAT_MM`): authored in §1 but reverted because it tripped the BSS guard with zero headroom. Do this FIRST in §7 -- it is the regression net for the whole relocation
- [ ] `test_highhalf.c` also covers the §9 identity-map-disabled walker exercise + absent-PML4/PDPT/PD `vmm_set_user_page`/`_remap` no-crash cases → XREF: §9
- [ ] `scripts/lint.sh` check rejecting raw page-table phys-as-pointer casts outside the bootloader + `pt_walk` (§9 design-review F1 hardening) → XREF: §9
- [ ] Choose the new conventional user load base (e.g. `0x400000`, or PIE/ASLR-randomized once `D03 T02 §2` lands) and update `user/user.ld` + `scripts/build-eif.py`.
- [ ] Reduce `include/kernel/mm/user_range.h` to the new base (or remove it if ASLR makes it dynamic); update the three includers (`vmm.c`, `pmm.c`, `task.c`) and their static asserts.
- [ ] Remove the `scripts/build.sh` BSS-collision guard (no longer meaningful) and the `0x900000` user-range guard page; replace with the relevant lower-half guard if still needed.
- [ ] Confirm the §10 static-pool conversions are no longer load-bearing for HEADROOM. KEEP them (freestanding rule: no large static); only the headroom justification retires -> XREF: this file §10 (item: "Convert `reg_value_pool`").
- [ ] Unblocks the kernel sections the ceiling stalled: re-run each deferred section once the guard is gone -> XREF `02-kernel-core/TODO-22` §23 (item: "Commit: `\"ntdll: live-environment adoption over an atomic env exchange\"`")
- [ ] Re-run `02-kernel-core/TODO-22` §24 once the guard is gone (item: "Commit: `\"ntdll: measured expansion budget + lookup cache for Rtl env\"`"); it also carries a latent non-atomic `env_buf_free` SMP fix
- [ ] Re-run `02-kernel-core/TODO-22` §25 once the guard is gone (item: "Commit: `\"ntdll: counted (non-_U) Rtl env read forms over a SIZE_T-safe core\"`")
- [ ] Re-run `02-kernel-core/TODO-23` §1 once the guard is gone (item: "Commit: `\"kernel: add EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS types\"`"); code COMPLETE, parked in `git stash` `todo23-s1-wip` -- apply, do not rewrite
- [ ] Re-run `01-boot-platform/TODO-14` §15 once the guard is gone (item: "Commit: `\"boot: durable record for the anti-rollback terminal give-up\"`")
  - Code COMPLETE and design-reviewed, preserved at `.claude/state/deferred-todo14-s15.diff` -- apply, do not rewrite. Measured 2026-08-30: needs 4,208 bytes against 1,323 available.
- [ ] Re-run `01-boot-platform/TODO-14` §12 once the guard is gone (item: "Granular per-driver `boot_load_record`"); parked purely on size, its three sibling items share the same blocker
- [ ] Re-run `01-boot-platform/TODO-14` §13 once the guard is gone (item: "Move `%d`/`%u`/`%x` to standard C width semantics in `vformat_buf`"); the `-Wformat` attribute and the hand-written width warning retire in the same commit
- [ ] Re-run `01-boot-platform/TODO-14` §17 once the guard is gone (item: "Capture and validate the evidence page at the very start of `kernel_main`, before `boot_phase0()` can panic, using a helper that neither logs nor allocates")
  - Parked 2026-09-03 before any code was written, so the redo starts from the design and not from a patch. The section offers two mechanisms (move the restore ahead of Phase 0, or refuse an overwrite of a prior-boot record until the restore has run) and deliberately leaves the choice to whoever has the image budget.
  - The user impact is the one this whole page is about: a Phase-0 crash on the boot after a Phase-0 crash reports only the second one, so a boot loop is diagnosed from its symptom.
- [ ] Re-run `01-boot-platform/TODO-14` §19 once the guard is gone (item: "Gate the kernel's access to the evidence page on the decoded state, so a boot whose pin failed does not write a page firmware may own")
  - Parked 2026-09-03, unstarted. Its bootloader half costs the kernel image nothing and would land today; it is held back because a `boot_info` field with no consumer is a `BOOT_INFO_VERSION` bump spent on nothing.
  - Carries a `BOOT_INFO_VERSION` 24 -> 25 bump in BOTH headers plus the `dump-fields.inc` and ownership-matrix rows, so schedule it where a protocol bump is acceptable rather than squeezing it in beside another section.
- [ ] Re-run `02-kernel-core/TODO-04` §14's runtime renderer test once the guard is gone (item: "Add the RUNTIME regression test driving the renderer at 7/8/9/10-digit `sec` values")
  - The section's BOUND already shipped and is compile-time verified, so only the runtime test is outstanding here.
  - Code COMPLETE and adversarial-reviewed, preserved at `.claude/state/deferred-TODO-04-s14-renderer-test.patch` -- apply, do not rewrite. It extracts `klog_render_dec_u32()` from `klog_emit()` and adds a guard-byte test around it.
  - MEASURED 2026-09-03: the extraction ALONE moves the raw allocated end `0x7fead5` -> `0x7ffad5`, page-aligning `__kernel_end` from `0x7ff000` to `0x800000` = `USER_BASE`, and the guard refuses the link; with the test it overshoots further. Against a 15-byte `.text` budget, so this one cannot be trimmed to fit.
- [ ] Re-run `02-kernel-core/TODO-23` §2-§16 once the guard is gone -- the whole exception/SEH file is ceiling-parked (each section adds kernel `.text`); un-defer and implement in Implementation-Order once §1's types link
- [ ] Re-run `01-boot-platform/TODO-13` §28 once the guard is gone (item: "Commit: `\"tpm: crash-consistent record pairing and verified-read boot budget\"`"); design review is DONE and the diff is preserved, so the re-attempt is apply-then-re-verify
  - Reverted 2026-08-18 with the tree left green. The code built clean on its own; the section's mandatory unit tests pushed `.rodata` over a 4 KiB page, which cascaded `.data` and `.bss` each up one page and landed `__kernel_end` exactly on `0x800000`.
  - The diff is at `.claude/state/deferred-todo13-s28.patch` (gitignored, survives a session rollover but NOT a fresh clone). If it is lost, the §28 Deferred stamp records every design outcome so the redo is mechanical rather than a re-design.
  - DONE as far as THIS section is concerned, 2026-08-18: §12 cleared the guard tactically without waiting for §7's permanent retirement, and `01-boot-platform/TODO-13` §28 is unparked and back in its own file's queue -> XREF: this file §12 (item: "Convert exactly ONE assessed pool to frame-backed storage").
- [ ] Re-run `01-boot-platform/TODO-13` §33-§35 once the guard is gone (item: "Commit: `\"tpm: report PCR-layer contention as retryable\"`" and the two after it); §33's code is written and preserved, §34-§35 are parked unstarted
  - §33's diff is at `.claude/state/deferred-todo13-s33.patch` and builds clean; the design outcome is recorded in its own Deferred stamp, so the redo is apply-then-re-verify.
  - MEASURED 2026-08-30 and it changes the picture for this whole TODO: §33's production change alone is +96 bytes of `.text` and that was enough to move `__kernel_end` from 0x7ff000 to exactly 0x800000. The image sections are page-aligned and chain, so the tree admits essentially NO new kernel code, not one page of it. `01-boot-platform/TODO-13` is now fully parked on this section.
- [ ] Re-run `01-boot-platform/TODO-13` §32 once the guard is gone (item: "Commit: `\"tpm: authorized-record coverage residue on existing fixtures\"`"); the tests are authored, so the redo is apply-then-re-verify
- [ ] Re-run `01-boot-platform/TODO-14` §12 once the guard is gone (item: "Granular per-driver `boot_load_record`"); the code is written and design-reviewed, so the redo is apply-then-re-verify
- [ ] Re-run `01-boot-platform/TODO-14` §13 once the guard is gone (item: "Move `%d`/`%u`/`%x` to standard C width semantics"); nothing is written, the section is blocked at its FIRST line of work
  - MEASURED 2026-08-30: the `vformat_buf` engine change alone is **+112 bytes of `.text`** against 79 available, so the whole section is unreachable, not merely expensive. The probe was reverted and the tree left green.
  - It is also the largest of the parked sections by far -- 2,731 `%u`, 709 `%x`, 291 `%d`, 249 `%X` conversions and 7,119 `(uint64_t)` casts across `src/kernel`, all of which must land in ONE commit with the engine change. Schedule it accordingly once the ceiling lifts.
  - Preserved at `.claude/state/deferred-todo14-s12.patch` (gitignored, survives a rollover but NOT a fresh clone). The §12 Deferred stamp records all four design-review outcomes, so the redo stays mechanical if the patch is lost.
  - MEASURED 2026-08-30, and it is the sharpest reading of this ceiling so far: the section costs **+1,904 bytes of `.text`** against **79 bytes** of headroom. `.text` ends at `0x427FB1` with its page boundary at `0x428000`, so the binding constraint is not the BSS slack at all -- it is 79 bytes of `.text`, after which `.rodata`, `.data` and `.bss` each move up a full 4 KiB page and `__kernel_end` lands on `0x800000`.
- [ ] Re-run `01-boot-platform/TODO-29` §20 once the guard is gone (item: "C:\ fallback for `boot-trend.json`"); only the C: path construction is parked, the rest of that section shipped
  - `boot-trend.json` is the one Perf artifact with NO C: fallback: `boot-profile` and `boot-timeline` both build their path from `klog_using_blackbox ? "X:\\Perf\\" : klog_dir` (`src/kernel/main/boot_progress.c:387`). Matching them means three runtime-built paths across five call sites in `boot_trend.c`.
  - Not blocked on design: the pattern to copy is shipped and named above. §20 already landed the honest interim -- an explicit BlackBox-absent refusal before the two 16 KiB allocations -- so the parked half is the parity fix alone.
  - Its post-ship review parked two more on the same ceiling, both measured. `boot_trend_compute_growth_pct()` narrows a `uint64_t` quotient, so `prior_median=1` with `newest_median=1073741825` is exactly 25 * 2^32 percent and lands on 0, suppressing the regression alarm for the largest growth representable; two spellings of the clamp were tried and BOTH tripped the BSS guard against 15 bytes of `.text`. And the rebuilt ring is reparsed for six regression samples while the old tree is still live, ~1,461 extra kmalloc calls per boot.
- [ ] Re-run `01-boot-platform/TODO-29` §19 once the guard is gone (item: "Gate the full `PERF`/timeline serial tables behind debug/test builds"); the code is written and design-reviewed, so the redo is apply-then-re-verify
  - Preserved at `.claude/state/deferred-TODO-29-boot-perf-health-observability-s19.patch` (gitignored, survives a rollover but NOT a fresh clone). The §19 Deferred stamp records every design-review outcome, so the redo stays mechanical if the patch is lost.
  - MEASURED 2026-09-03 at `a0a78bde1`: **+2,448 bytes** of `.text` against the **47 bytes** of slack this page already records, landing `__kernel_end` on `0x800000`.
  - It is the second parked section that would REDUCE default serial output rather than add to it: two full per-step tables, 45 lines each, print on every production boot today. The ceiling is holding back a change that makes the boot log smaller.
- [ ] Re-run `01-boot-platform/TODO-29` §21 once the guard is gone (item: "Per-DRIVER degraded state for storage"); nothing is written -- the section was design-reviewed and parked before its first line of code
  - MEASURED 2026-09-03 at `3032a376e`: 15 bytes of `.text` slack, against a smallest-comparable shipped helper in the same consumer file (`boot_health_secureboot_name`) of 30 bytes. No probe patch exists because building one would only reconfirm a section-exact budget.
  - The §21 Deferred stamp carries the full design of record -- the rejected `SUBSYS_*` widening and why it is an ABI change, the accepted registry shape, the confirmed unsafe-versus-degraded contract defect at `src/kernel/main/boot_storage.c:531`, and the `docs/boot/boot-health-schema.md` row the redo owes -- so the redo is implement-then-verify rather than re-design.
- [ ] Re-run `01-boot-platform/TODO-23` §7 once the guard is gone (item: "DEFERRED follow-up: direct Intel iTCO PCI fallback"); the code is written and design-reviewed, so the redo is apply-then-re-verify
  - Preserved at `.claude/state/deferred-todo23-s7.patch` (gitignored, survives a rollover but NOT a fresh clone). The §7 Deferred stamp records the register-layout sources and every design-review outcome, so the redo stays mechanical if the patch is lost.
  - MEASURED 2026-09-03 at `c6b8af000`: `__kernel_end` is `0x7fead5` with **47 bytes** of `.text` slack, and the section costs ~11 KiB across the driver, the header and 10 new fixtures. The measured build landed `__kernel_end` on `0x803000`, three pages over.
  - Trimming does not rescue it and the production half cannot ship alone: the whole value of the change is refusing to arm a watchdog that cannot reset the board, and the refusal paths are exactly what the fixtures cover.
  - It also carries a fix to a LIVE defect in the shipped WDAT path (`gas_usable` ignores GAS `access_size` and `bit_offset` while `gas_read`/`gas_write` size the transaction from `bit_width`), so this parking keeps a real correctness fix out of the tree, not only a new capability.

- [ ] Decide whether the guard's `>=` is off by one before removing it, and record the answer: `__kernel_end` is an EXCLUSIVE end marker, so `__kernel_end == USER_BASE` means the kernel ends where user space begins, adjacent and not overlapping
  - `scripts/build.sh:402` takes the maximum `b`/`B` symbol in `build/kernel.map`, which is `__kernel_end` itself, and refuses on `>=`. For a real BSS OBJECT at `0x800000` that test is right; for the exclusive end marker it costs a full page that is not actually in conflict.
  - That page is worth naming because it is the difference between "79 bytes of headroom" and "4,175 bytes", which is the whole margin several parked sections are waiting on. This is the RECEIPT SURFACE (`scripts/build.sh`), so the unattended run files it rather than changing it.
  - The safe shape if the answer is yes: exclude `__kernel_end` from the max, keep `>=` for real objects, and compare `__kernel_end` with `>`. Do not simply relax the comparison for every symbol.
  - Preserved at `.claude/state/deferred-todo13-s32.patch` (gitignored, survives a rollover but NOT a fresh clone). If it is lost, the §32 Deferred stamp records the three settled design decisions so the redo is still mechanical.
  - Measured 2026-08-30: three of that section's five items cost +5,216 bytes of `.text` against ONE page of headroom, so it is blocked by size alone and not by any missing capability.
- [ ] Re-add the two ceiling-parked warm-update selection tests once the guard is gone
  - They belong in `src/kernel/test/test_boot_reserved.c` -> XREF `01-boot-platform/TODO-01` §29 (item: "Integration tests in `src/kernel/test/test_boot_reserved.c` assert descriptor IDENTITY, not a count")
  - A rejected-to-differently-rejected mutation (empty -> over-contract on the sole candidate), which is the only shape that moves `boot_warm_update_sel.error` alone; the fix it guards IS shipped, the assertion is not.
  - A dedicated whole-range wrap case. The rule shipped and is asserted, but only by taking over the slot the unaligned-base case had, so selection-level alignment coverage was traded away to fit.
  - MEASURED 2026-08-30 at `8fc5b53d3` + review fixes: `.text` had **79 bytes** of slack before `.rodata`, so the two blocks (~350 bytes of `.rodata` and a few hundred of `.text`) cascaded every following page-aligned section up one page. The code fixes themselves fit; only their evidence did not.
- [ ] Re-run `01-boot-platform/TODO-01` §30's three code items once the guard is gone (item: "Apply the same addressability precondition to every payload type the reservation pass admits, not only `BOOT_PAYLOAD_WARM_UPDATE_STATE`")
  - MEASURED 2026-08-30 at `1bb19e115`: HEAD links with `__kernel_end` at `0x7ff000`, **4096 bytes** below the guard. The section's implementation was written, built and measured before being reverted: `+8746` bytes of `.text`/`.rodata` in `src/kernel/test/test_boot_reserved.c` and `+2017` in `src/kernel/mm/boot_reserved.c`, **~10.8 KiB against 4 KiB of slack**, landing `__kernel_end` on `0x802000`.
  - Trimming does not rescue it. The production half alone is ~2.1 KiB and would fit, but shipping the predicate without its refusal tests is false completeness, and the five tests cost 8.7 KiB because each scenario stages a synthetic memory map. A consolidation pass would have to reclaim 20% of an existing 26 KiB test object.
  - The DESIGN is settled and recorded, so the redo is apply-then-verify rather than re-design: replace the `last_frame < pmm_get_total_frames()` ceiling test with full memory-map interval coverage (refuse `UEFI_MMAP_UNUSABLE`/`_MMIO`/`_MMIO_PORT` and type 15 unless the sealed warm extent), fall back to the ceiling test only when `mmap_count == 0` or `mmap_truncated`, and freeze a heap copy of the reservation table in `boot_tests_run()` so `boot_reserved_blackbox_dump()` cannot publish a fixture. The design-review verdict is at `.claude/overnight/reviews/20260830-063215-design.out`.
  - The two policy items the section owned (warm extents do not raise the tracking ceiling; a refused warm extent is held, not reclaimed) SHIPPED at zero image cost as recorded decisions in `src/kernel/mm/pmm.c` and `src/kernel/mm/boot_reserved.c`; only the code items are parked here.
  - -> XREF: `01-boot-platform/TODO-01 §30` (item: "Apply the same addressability precondition to every payload type the reservation pass admits, not only `BOOT_PAYLOAD_WARM_UPDATE_STATE`")
- [ ] The §10 headroom is FULLY CONSUMED again, so the "tactical" reclamation is a recurring need rather than a one-off, and this section should say which it is before §3 lands.
  - MEASURED 2026-08-18 at `d43b73d2b`: `.bss` runs `0x56d000..0x7feea5`, `__kernel_end` page-aligns to `0x7ff000`, leaving **347 bytes** before the guard fires. §11's Notes record `0x6c2000` and ~1272 KiB spare on 2026-07-17; roughly 1.27 MiB was reabsorbed in a month.
  - The failure is cascading rather than proportional, which makes it arrive without warning: shrinking `.rodata` by 859 bytes (or `.text` by 1183) moves every following page-aligned section down one page and clears the guard, so a section either fits with room to spare or fails by a whole page.
  - A second §10-shaped pass would need NEW targets: the three pools this file's conversion table nominated are already converted (`reg_value_pool`, `reg_key_pool` and `s_atoms` are now pointers, 24 bytes apart in `.bss`), so the table is spent and describes finished work.
  - The largest remaining `.bss` consumers, measured from `build/kernel.map`, are `klog_ring` 288000, `devices` 277504, `tasks` 203776, `ctrl_windows` 180736 and `s_recovered` 164000. The table already rules out the first two (pre-PMM, and hot-plug-ISR reachable); the last three are unassessed and are where a second pass would have to look.
  - ANSWERED 2026-08-18: it is recurring, and the tactical route now has its own unblocked owner rather than sitting as a note inside this deferred section -> XREF: this file §12 (item: "Assess the three unassessed candidates against the §10 bar").
- [ ] Re-run `02-kernel-core/TODO-02` §12 once the guard is gone (item: "Privileged production write path for tunables"); nothing is written, the section was parked before its first line of code
  - MEASURED 2026-09-03 at `abf4dc974`: 15 bytes of `.text` slack, against a new `NtSetSystemInformation` set class plus its privilege gate and name marshalling, which is hundreds of bytes. No probe patch exists because building one would only reconfirm a section-exact budget.
  - The user impact is the one that section exists to fix: every `quota.user.<type>` cap stays at its unlimited built-in because no production caller of `kernel_tunable_set` exists, so the quota plane accounts usage without ever refusing on a configured cap.
  - Its Deferred stamp carries the full design of record (set class shape, the `SeSystemProfilePrivilege` gate, and why neither the registry nor a boot-argument route is available), so the redo starts from a design rather than a blank page.
- [ ] Commit: `"mm: retire 0x800000 user-base ceiling -- user owns the lower half"`

**Test checkpoint:** User programs load + run at the new base; `bash scripts/build.sh clean` -> `=== BUILD OK ===` with the BSS guard removed; no `user_range.h` static-assert failures. Test on: QEMU WHPX + TCG; **bare metal**.

> [!NOTE]
> **Interim bridge NOT BUILT -- note corrected 2026-07-17.** This note previously asserted that the headroom risk "was mitigated tactically by converting large kernel static pools ... from static BSS to `pmm_alloc_contiguous`", and that "those conversions remain correct". **Both claims were false against tree**: the pools are still static arrays (`registry.c:61`/`:64`, `klog.c:78`, `blkdev.c:13`, `nt_misc.c:47`) and `git log -S` shows no conversion commit ever landed -- the prose was written aspirationally at this TODO's authoring commit (`26297a49`) and never implemented. The false claim was load-bearing: it made the tactical option look spent, so every ceiling-stalled kernel section was parked behind §3's operator-reserved decision instead. The conversion is now filed as a real, unblocked section -> §10. This section (§7) still owns the PERMANENT retirement; §10 only buys headroom.

> **Deferred:** [Critical] 2026-07-17 -- cascade-blocked on §3. This section builds on the higher-half kernel that §3's low->high jump establishes (Depends-On §3), so it cannot start until §3 lands. §3 is itself deferred awaiting an operator sequencing decision (the physical image / user-window collision). -> XREF: this file §3 (item: "Commit: `"boot: kernel linker VMA/LMA split + higher-half jump"`") + `todo/answers.md` Q3.

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

> **Deferred:** [Critical] 2026-07-17 -- cascade-blocked on §3. This section builds on the higher-half kernel that §3's low->high jump establishes (Depends-On §3), so it cannot start until §3 lands. §3 is itself deferred awaiting an operator sequencing decision (the physical image / user-window collision). -> XREF: this file §3 (item: "Commit: `"boot: kernel linker VMA/LMA split + higher-half jump"`") + `todo/answers.md` Q3.

---

## 9. VMM Walker Conversion -- Route Every Deref Through the HHDM Helper

Move every page-table walker off the identity map and onto the §2 HHDM, while the kernel still links and boots LOW. Additive-behavior: the identity map stays live as the fallback, so this section is independently bootable, but it is the prerequisite that lets §5 retire that map. **Split out of the original §2 on the 2026-07-16 design review's SPLIT-CONFIRMED verdict** (2+ worker contexts); §2 built and installed the HHDM, this section re-expresses the derefs through it. Ships between §2 and §3 in the Implementation Order.

> [!WARNING]
> **HIGH RISK -- the riskiest paging change in the system. SHIPPED in commit `49883c1f`.** Pre-conversion (historical) the kernel root was physical-as-pointer and `vmm_get_kernel_cr3()` returned it raw for CR3, so a blanket HHDM conversion would have triple-faulted at the next kernel-task switch. §9 shipped the split: `kernel_pml4` is now the HHDM WALK pointer and the physical CR3 root is derived on demand via `mm_hhdm_to_phys(kernel_pml4)` in `vmm_get_kernel_cr3()`, so a walk pointer never reaches CR3. The remaining live caution is for §5: an INCOMPLETE walker inventory is silent while the identity map is live and faults the instant §5 retires it -- `mm_phys_to_hhdm()` only range-checks (nonzero, <64 TiB), so a NULL/missed walk is a phys-page-0 read/write (`memmap.h`), not a fault.

- [x] Root split: `kernel_pml4` is now the HHDM WALK pointer; the physical root (for CR3) is derived on demand via `mm_hhdm_to_phys(kernel_pml4)` in `vmm_get_kernel_cr3()`. NO second global -- a static tripped the pre-§7 image ceiling
- [x] Added `pt_walk(phys)` (non-inline, `vmm.h`/`vmm.c`): `mm_phys_to_hhdm` + `KeBugCheckEx(BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION)` on NULL (design review F2: runtime-safe fatal, not `boot_halt`). Non-inline keeps the image under the ceiling
- [x] [H2] Routed EVERY page-table deref through `pt_walk` REPO-WIDE -- 20 functions (18 in `vmm.c`, 2 in `swap.c`): `get_or_create_table`, `zero_page`, the `*_user_page` family, `vmm_promote_to_1g`/`_split_huge_page`/`_apply_nx_policy`
- [x] [H2] Kept `*_phys` for every PTE/CR3 store + `pmm_free_frame()` arg; `vmm_destroy_user_pml4` captures `pdpt_phys`/`pd_phys` separately, freeing physical never the HHDM walk pointer
- [x] `vmm_apply_nx_policy()` VA reconstruction routed through `mm_canonical_from_indices()` at both sites (2 MiB huge + 4 KiB); canonical for `pml4i >= 256`
- [x] Absent-entry safety: `VMM_FLAG_PRESENT` check before every lookup-path `pt_walk` (`vmm_set_user_page`, `vmm_remap_user_page`) -- pt_walk bugchecks on a zero frame (design-review F1)
- [x] Left the `kernel_base = 0x100000` NX bound (`vmm.c`) alone -- correct while the kernel links low; it moves to image-window bounds in §3
- [x] Converted the existing test-side walkers (`test_vmm.c`, `test_cpu_security.c`) to `mm_phys_to_hhdm` + `TEST_ASSERT` (design review F1; not `pt_walk` -- `KeBugCheckEx` banned in test code)
- [/] Dedicated identity-map-disabled runtime test + absent-entry no-crash cases: BSS-ceiling-gated in-kernel -> deferred to §7 → XREF: §7 (item: "`test_highhalf.c` also covers the §9 identity-map-disabled walker exercise")
- [x] Reclaimed a page for the ceiling: `swap_temp_buf` 4 KiB static -> `pmm_alloc_contiguous(1)` via HHDM (`swap.c`, freestanding no-large-static rule); §7 retires the ceiling that forces this
- [x] Added `POST16_HHDM_WALK`/`_OK` (0xDD10/0xDD11) around the `vmm_init` cutover
- [x] Hardened the converted swap.c walkers (adversarial review): reject huge leaves, encode the swap marker before freeing, and verify the leaf still maps the captured frame; swap is dormant, SMP serialization deferred to the pager TODO
- [ ] Lint check rejecting raw phys-as-pointer page-table casts outside the bootloader/`pt_walk` (design review F1 hardening) → XREF: §7 (item: "`scripts/lint.sh` check rejecting raw page-table phys-as-pointer casts")
- [x] Commit: `"boot: VMM walker conversion -- page-table derefs through the HHDM helper"`

**Test checkpoint:** `=== BUILD OK ===`; `scripts/test-smoke.sh` boots to `C:\>`; full `scripts/test.sh` green. The kernel still runs LOW; this section flips the WALK pointer to the HHDM while the identity map stays live as a fallback, so behavior is unchanged and the section is independently bootable. Exercise the converted walkers (remap/unmap/destroy/split/NX-traverse/swap) with the broad identity map disabled (design review requirement) -- either host-side or, if the BSS ceiling blocks the in-kernel suite, deferred to §7's `test_highhalf.c` with a reciprocal XREF. Test on: QEMU KVM + TCG.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) + `run-security-tests.bat` | converted `test_vmm.c`/`test_cpu_security.c` walkers; full suite 21420 kernel + 16 user, 0 failures; smoke boots to `C:\>`.

> **Notes:**
> - **What shipped** -- `pt_walk()` (shared non-inline HHDM walk helper, `vmm.h`/`vmm.c`) + a 20-function repo-wide walker conversion; `kernel_pml4` is now the HHDM walk pointer, the CR3 root derived via `mm_hhdm_to_phys()`.
> - **How it runs** -- cutover in `vmm_init` (POST16 `0xDD10`/`0xDD11`); the identity map stays live as a fallback so the section is independently bootable; verified KVM+TCG smoke + full `test.sh`.
> - **Downstream effects** -- unblocks §3 (linker split) and §5 (identity teardown), both of which depend on §9; Codex design + 2 adversarial adoptions (F1 absent-entry PRESENT guards, F2 `KeBugCheckEx`) in the commit message.
> - **Canonical doc** -- [`docs/infrastructure/kernel-address-space.md`](../../docs/infrastructure/kernel-address-space.md) (HHDM window + phys-vs-walk relation).
> - **Scope boundary** -- §9 owns the WALKER conversion; the dedicated identity-map-disabled test + the raw-cast lint are §7 (BSS-gated); §5 owns identity teardown + data-buffer HHDM migration.

> **Verified:** 2026-07-17 | commit `49883c1f` | 11/13 items | build OK | smoke PASS (KVM 2.9s)
> **Deferred:** [H] `swap_out`/clock PTE transaction is a preemptible TOCTOU -- verify/mark/free/publish is unlocked (reason: not-functional-today -- swap is dormant, no `swap_init`/`swap_out` production caller) -> XREF: 03-memory-concurrency/TODO-04-pager-reclaim-working-set.md §1 (item: "Serialize the `swap_out` + clock PTE transaction" at line 68)
> **Quality reviewed:** 2026-07-17 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 2H+2M+2L fixed, 1H deferred | scope: kernel-code-quality

> [!NOTE]
> **Regression risk: HIGH.** An incomplete deref inventory or a walk-vs-load (`*_phys`) mix-up is silent until §5 retires the identity map or the next kernel-task CR3 load. Rollback: revert to walking via the identity map (the §2 HHDM install is independent and stays). Keep `scripts/test-smoke.sh` green at every step. The repo-wide walker inventory (20 functions) MUST be complete before §5 (identity teardown).

---

## 10. Tactical BSS Headroom -- Large Static Pools to Dynamic Allocation

Buy kernel-image headroom NOW, without the higher-half move. **Depends on nothing**; ships FIRST in the Implementation Order. This is the section that unparks the kernel queue: §3 (the permanent fix) is deferred awaiting an operator sequencing decision, and every ceiling-stalled section behind it is waiting on ~8 KiB. The kernel carries **~3.6 MB of static BSS**, of which ~1.5 MB sits in pools that the freestanding-kernel rule already says should be dynamic (`CLAUDE.md`: `kmalloc()` for <= 4 KB only, `pmm_alloc_contiguous()` for everything larger). Converting the single largest pool buys ~98x the headroom the stalled sections need.

> [!IMPORTANT]
> **Filed 2026-07-17 after the §7 Note was found false.** That Note asserted these conversions had already shipped ("Those conversions remain correct"); they had not -- the pools are static in tree and `git log -S` finds no conversion commit. The stale claim made the tactical path look spent, so the kernel queue was parked behind §3's operator-reserved decision instead. **The proven pattern already exists in-tree** -- §9 converted `swap_temp_buf` (4 KiB static -> `pmm_alloc_contiguous(1)` via HHDM, `swap.c`) exactly this way.

Measured BSS consumers (`build/kernel.map`, 2026-07-17; BSS end `0x7fe000` vs `USER_BASE` `0x800000` = **8 KiB headroom**):

| Symbol           |    Size | Home            | Init phase                               | Safe to convert?      |
| ---------------- | ------: | --------------- | ---------------------------------------- | --------------------- |
| `reg_value_pool` | 784 KiB | `registry.c:64` | `registry_init` (late, `boot_storage.c`) | yes -- PMM is up      |
| `klog_ring`      | 281 KiB | `klog.c:78`     | **pre-PMM** (boot-phase aware)           | NO -- see below       |
| `devices`        | 271 KiB | `xhci_dev.c:31` | `xhci_init` (phase 2) + hot-plug ISR     | not here -- see below |
| `s_atoms`        | 268 KiB | `nt_misc.c:47`  | NT init (phase 3)                        | yes -- PMM is up      |
| `reg_key_pool`   | 228 KiB | `registry.c:61` | `registry_init` (late)                   | yes -- PMM is up      |
| `devices`        | 1.4 KiB | `blkdev.c:13`   | driver registration                      | not worth it          |

- [x] Convert `reg_value_pool` (784 KiB) + `reg_key_pool` (228 KiB) in `registry.c` to frame-backed pools via `pmm_alloc_pages_hhdm` at `registry_init`; pool-index allocator unchanged, fails closed on `BOOT_FATAL` -- cleared the ceiling alone
- [x] Convert `s_atoms` (268 KiB, `nt_misc.c`) to a frame-backed table behind its existing lookup helpers; allocated once at the new `nt_misc_atoms_init()` boundary before handler publication, never lazily and never under `s_atom_lock`
- [x] **Do NOT convert `klog_ring`** (281 KiB) blind -- klog logs BEFORE `pmm_init`. Left static; the early-ring migration is owned elsewhere -> XREF: `01-boot-platform/TODO-04 §1` (item: "Boot-Phase Aware klog Init")
- [x] Re-measure the `scripts/build.sh` BSS gate and record the new headroom in the Notes -- the gate output is the acceptance evidence for this section
- [x] Leave the 1.4 KiB `blkdev.c` `devices` static -- measured `0x580`, far below the large-static bar, so converting it would add a boot-path failure mode to buy nothing (Codex design review concurred)
- [x] Leave the test fixtures (`test_env_value_too_long.big` 32 KiB, `s_bls_fixture` 36 KiB) in the measured image -- at 1280 KiB headroom the 68 KiB is immaterial, and excluding them would measure an image nobody boots
- [x] Commit: `"kernel/mm: convert large static pools to dynamic -- tactical BSS headroom"`

> [!NOTE]
> **The `devices` row was FALSE and is corrected above (2026-07-17).** This section originally attributed the 271 KiB `devices` array to `blkdev.c` and directed the conversion there. Measured from `build/kernel.map` by address delta: `blkdev.c`'s `devices[BLKDEV_MAX=16]` is **1.4 KiB** (`0x580`), while the 271 KiB symbol is a **same-named static in `xhci_dev.c`** (`devices[XHCI_MAX_DEVICES=64]`, sited between `g_usb_ccs_count` and `xhci_hid_start_polling.registered`). Following the original text would have edited the wrong subsystem and reclaimed ~0 -- the same false-claim class as the §7 Note this section was filed to correct. The xHCI table is deliberately NOT converted here: `xhci_enumerate_device()` writes `devices[]` **unlocked from ISR context** on hot-plug (`xhci.c:720-733`, already flagged INTERIM HAZARD there), so making it a pointer adds an ISR-reachable NULL window to a known-hazardous path -> XREF: `04-drivers-hardware/TODO-10-usb-stack.md §1` (item: "Convert the 271 KiB `devices[XHCI_MAX_DEVICES]` static to a frame-backed table")

**Test checkpoint:** `bash scripts/build.sh` prints `✓ BSS check` with a materially lower BSS end (expect >= 700 KiB headroom after the registry pools alone, vs 8 KiB today). Full `scripts/test.sh` green -- registry suites especially, since the pool-index allocator semantics must not change. `scripts/test-smoke.sh` boots to `C:\>`: the registry is on the boot path, so an allocation-failure regression surfaces as a boot hang, not a test failure. Test on: QEMU KVM + TCG; **bare metal**.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | 196 kernel tests, 0 failures (atom fail-closed case: SUITE=abi)

> **Notes:**
>
> - Shipped `pmm_alloc_pages_hhdm()` + `pmm_free_contiguous()` (`pmm.c`) and `mm_phys_extent_in_hhdm()` (`memmap.h`), then moved `reg_value_pool`/`reg_key_pool`, and `s_atoms` off static BSS to frame-backed storage; 9 new MM tests.
> - Allocation happens only at `registry_init` and the new `nt_misc_atoms_init()` (before the atom handlers publish) -- never lazily, never under a spinlock; see the `pmm_alloc_pages_hhdm` caller contract for why that is safe.
> - **Acceptance evidence:** BSS end `0x7fe000` -> `0x6c0000`, headroom **8 KiB -> 1280 KiB** (160x); BUILD OK, 21460 kernel + 16 user tests, smoke boots to `C:\>` in 2.87s. Codex adoptions in the commit message.
> - Unparks the ceiling-stalled kernel queue (TODO-22/23/24); the sweep collecting on it is §11, the only remaining consumer of this headroom.
> - Canonical doc: [docs/infrastructure/kernel-address-space.md](../../docs/infrastructure/kernel-address-space.md).
> - **Scope boundary:** §10 removes large statics only, not address-space layout (§7 owns the `USER_BASE` retirement). `klog_ring` stays static (`01-boot-platform/TODO-04 §1`); the 271 KiB xHCI `devices` table is `04-drivers-hardware/TODO-10 §1`.
> - Adversarial review here found that `-DKERNEL_TESTS` is unconditional (`Makefile:29`), so every test seam ships; a proposed atom-table test seam was dropped and the repo-wide half filed -> `02-kernel-core/TODO-10 §26`.

> **Verified:** 2026-07-17 | commit `000e4745` | 6/6 items | build OK | smoke PASS (KVM 2.90s), 21460 kernel + 16 user tests, BSS `0x6c0000` (1280 KiB headroom, was 8 KiB)
> **Accepted:** [H] PMM bitmap mutates without synchronization; this section adds 2 allocation sites to 10+ existing late callers (reason: pre-existing, repo-wide) -> XREF: `03-memory-concurrency/TODO-03 §1` (item: "**PMM bitmap SMP locking**" at line 103)
> **Accepted:** [H] a timed-out `boot_async_group` worker keeps running and can race a later PMM allocation (reason: degraded-boot-only window; the unlocked bitmap owns it) -> XREF: `03-memory-concurrency/TODO-03 §1` (item: "Close the timed-out-async-worker window" at line 104)
> **Accepted:** [H] `-DKERNEL_TESTS` was unconditional, so every test seam shipped in the release kernel (reason: pre-existing, build-level); the flavor MECHANISM shipped in `TODO-10 §26` 2026-07-17, leaving the proof + the unguarded TUs -> XREF: `02-kernel-core/TODO-10 §27` (item: "Add `scripts/check-release-symbols.sh`" -- the proof the seams are gone) + `02-kernel-core/TODO-10 §28` (item: "Prune `src/kernel/fs/ntfs/ntfs_test.c`, `src/kernel/fs/ixfs/ixfs_test.c`, `src/kernel/main/test_threads.c` from `C_SRCS`")
> **Accepted:** [M] the real 271 KiB `devices` static is `xhci_dev.c`, not `blkdev.c`; converting it adds an ISR-reachable NULL window to a known hot-plug hazard (reason: scope) -> XREF: `04-drivers-hardware/TODO-10 §1` (item: "Convert the 271 KiB `devices[XHCI_MAX_DEVICES]` static" at line 83)
> **Accepted:** [M] `registry_init`'s partial-rollback branch has no deterministic coverage; tests may not call subsystem init (reason: infra) -> XREF: `02-kernel-core/TODO-14 §14` (item: "Extract the `registry_init` two-pool allocation into a pure transaction helper" at line 654)
> **Quality reviewed:** 2026-07-17 | Codex 5x (design + adversarial + consistency + perf + re-adversarial) | 3H+7M+2L fixed, 3H+2M accepted-XREF | scope: kernel-code-quality

---

## 11. Unpark the Ceiling-Stalled Kernel Queue

§10 turned 8 KiB of headroom into 1280 KiB, retiring the blocker that four TODO files are parked behind. This section is the sweep that collects on it. **No kernel code ships here** -- it is a stamp/status sweep plus the re-apply of one already-reviewed stash; each unparked section then returns to its own TODO's normal pipeline.

**This is NOT a blanket un-defer.** Several parked sections carry a SECOND, independent blocker that §10's headroom does not touch; those stay `[/]` with their remaining blocker restated. Verify each stamp before flipping it.

- [x] `TODO-24 §7` (ALPC client token): SHIPPED -- stash re-applied, re-reviewed vs the moved HEAD (4H+4M fixed incl. a same-SID impersonation escalation), 60 IPC tests, stash dropped -> XREF: `02-kernel-core/TODO-24 §7`
- [x] `TODO-23 §1-§16` (exception/SEH): un-deferred -- file blocker cleared, 16 ceiling stamps swept, IO reset to `[ ]`; §1 stash `todo23-s1-wip` = apply-then-re-verify -> XREF: `02-kernel-core/TODO-23 §1`
- [x] `TODO-22 §23` + `§25`: un-deferred -- ceiling-only stamps removed, IO rows `[/]` -> `[ ]`; design settled in-place, so each re-attempt is implementation-only -> XREF: `02-kernel-core/TODO-22 §23`
- [x] `TODO-22 §24`: KEPT `[/]` -- stamp drops the cleared ceiling half; fault-recoverable usercopy remains, sufficient alone -> XREF: `02-kernel-core/TODO-23 §13` (item: "`src/kernel/probe.c` -- implementation; `safe_return_rip` slot")
- [x] `TODO-24 §8`: KEPT `[/]` -- ceiling half dropped; the 6-word `SSDT_HANDLER` transport still cannot carry the 8-11-arg ALPC ABI, and widening it stays operator-reserved -> XREF: `02-kernel-core/TODO-24 §8`
- [x] `TODO-24 §9-§12`: ceiling half dropped per section, residual blockers restated individually -- §9 keeps the SSDT cap + §6 ABI (§7-stash dependency now satisfied); §10/§11 stay cascade-blocked; §12 carries a re-measure caveat
- [/] Re-add `test_highhalf.c`: the ceiling cleared, but it is NOT the only blocker -- 3 of its 5 specced assertions describe a higher-half kernel that does not exist yet -> XREF: this file §7
  - Blocked assertions: a kernel symbol `>= KERNEL_VIRT_BASE` (the kernel still runs LOW -- §3's low->high jump is operator-deferred); the retired `user_range.h` ceiling (§7, cascade-blocked on §3); and a fresh PML4 sharing the kernel UPPER half with an empty lower half (§6 built per-process PML4s, but they clone the LOW identity map, so the assertion as written cannot hold pre-§3). All three would FAIL today
  - The other 2 are already covered: `phys_to_virt`/`virt_to_phys` round-trip + canonical-`KERNEL_VIRT_BASE` are the host `tools/memmap-check` gate (54 checks, wired into `scripts/test-tooling.sh`). `test_vmm.c:278-332` covers per-process PML4 create/destroy + the cloned kernel PD's supervisor-only invariant + a leak net -- it does NOT assert upper-half sharing, so it does not substitute for that assertion
  - This is exactly the second-blocker class this section warns about: §10 bought `.text` headroom, not an address-space move. Re-add it as §7's first work item, where it is already filed
- [ ] Run the status sweep for the THIRD pass (§13) -- not yet run; §13 only named the sections that now fit -> XREF: this file §13 (item: "Unparked what the new headroom actually admits")
  - Candidates: `TODO-04 §14`, `TODO-09 §20`, `TODO-10 §32`, `TODO-11 §28`, `TODO-12 §32`, `TODO-26 §2` (the last also needs its own split decision, independent of headroom)
- [/] Run the status sweep for the FOURTH pass -- blocked until §14 lands its reclaim and names what the new headroom admits -> XREF: this file §14 (item: "Name what the new headroom admits and what it does NOT")
  - The §13 sweep above is still unrun, so these will collect together. §14's own candidate list is the starting set: `02-kernel-core/TODO-26` had all 21 of its remaining sections ceiling-blocked when §14 was filed on 2026-09-05.
- [x] Commit: `"todo: unpark the ceiling-stalled kernel queue -- TODO-22/23/24"`

**Test checkpoint:** `bash scripts/build.sh` -> `=== BUILD OK ===` with the BSS gate still passing after the §7 stash re-applies (that section is what previously tripped it). Full `scripts/test.sh` green including the 9 restored ALPC IPC cases. Test on: QEMU KVM + TCG.

> **Test runner:** `scripts\debug\kernel\run-ipc-tests.bat` (SUITE=ipc) | 326 kernel tests, 0 failures -- the re-applied TODO-24 §7 diff is the only code this section landed

> **Notes:**
>
> - **What shipped** -- a status sweep, plus the one reviewed stash: `TODO-24 §7` re-applied and SHIPPED (`992e2846` + review `abbc46a8`); TODO-23 (16 sections) and TODO-22 §23/§25 un-deferred; TODO-22 §24 and TODO-24 §8-§12 kept `[/]` with their surviving blockers restated.
> - **How it integrates** -- each unparked file now returns to its own Implementation Order; the triage oracle re-picks them because the ceiling `Deferred` stamps are gone and the IO rows reset to `[ ]`.
> - **Downstream effects** -- proves §10's headroom is real: the §7 diff that once pushed BSS to `0x801000` now builds at `0x6c2000` with ~1272 KiB spare. Two stashes remain to apply-and-RE-VERIFY: `todo23-s1-wip` (TODO-23 §1) and `s23-wip-bss-check` (TODO-22 §23, env.c/h).
> - **Not a blanket un-defer** -- `test_highhalf.c` stays `[/]`: the ceiling was only one of its blockers, and 3 of its 5 assertions still need the higher-half kernel §3 never shipped.
> - **Canonical doc:** [docs/infrastructure/kernel-address-space.md](../../docs/infrastructure/kernel-address-space.md).
> - **Scope boundary** -- §11 only changes parked STATUS and re-applies one reviewed stash; implementing each unparked section stays with its owning TODO. It does not touch the address-space layout (§3/§7) and does not re-litigate a second blocker (SSDT transport width, usercopy) the headroom never addressed.

> **Verified:** 2026-07-17 | commit `c374071b` | 7/8 items | build OK | 21504 kernel + 16 user tests | smoke PASS (KVM 2.92s) | BSS end `0x6c2000` (was `0x7fe000`)
> **Deferred:** [M] `test_highhalf.c` not re-added -- the ceiling cleared but 3 of its 5 assertions (kernel symbol `>= KERNEL_VIRT_BASE`, retired `user_range.h`, upper-half PML4 sharing) describe a higher-half kernel §3 has not delivered, so they would FAIL rather than pass (reason: §10 bought `.text` headroom, not an address-space move) -> XREF: this file §11 (item: "Re-add `test_highhalf.c`" at line 403)
> **Accepted:** [H] `TODO-24 §9` is parked section-wide although `NtAlpcCancelMessage` (3 args) and `AlpcBasicInformation` SetInformation (4 args) fit the existing transport and now have headroom; the oracle reads the whole section as DONE (reason: item-level restructure, owner is §9) -> XREF: `02-kernel-core/TODO-24 §9` (item: "**Split the section-level park" at line 452)
> **Accepted:** [M] `TODO-22 §24` likewise parks independent, now-runnable work (measured budget, lookup cache, the documented CAS double-free hardening) behind the usercopy blocker that only three of its items need (reason: item-level restructure, owner is §24) -> XREF: `02-kernel-core/TODO-22 §24` (item: "**Split the section-level park.**" at line 843)
> **Quality reviewed:** 2026-07-17 | Codex 3x (adversarial, consistency, perf) | 5M fixed, 1H+1M accepted-XREF | scope: N/A (TODO-only status sweep, no code)

---

## 12. Second Tactical BSS Reclamation Pass

> **Spawned-by:** root

The `0x800000` ceiling is live again and it is stopping kernel work today. MEASURED 2026-08-18 at `9b9ab5fd6`: `.bss` runs `0x56d000..0x7feea5`, `__kernel_end` page-aligns to `0x7ff000`, and there are **347 bytes** of headroom before `scripts/build.sh` fails the build. §10 bought 1280 KiB on 2026-07-17 and roughly 1.27 MiB was reabsorbed in a month, so the reclamation is a RECURRING need rather than the one-off §10 was written as. §7 owns the permanent retirement and is cascade-blocked on §3, which is operator-deferred; this section is the tactical route that depends on nothing.

**What a user hits if this is not done:** nothing ships. The failure is cascading rather than proportional, so a section either fits with room to spare or fails by a whole page with no warning: 01-boot-platform/TODO-13 §28 was implemented, design-reviewed and building clean, and was reverted purely because its mandatory unit tests moved `.rodata` across a page boundary. Every remaining kernel section in the repo is one page of `.rodata` away from the same revert.

**Why this is not §10 again and not byte-golf.** §10's nomination table is SPENT: `reg_value_pool`, `reg_key_pool` and `s_atoms` are already pointers (24 bytes apart in `.bss`). The alternative route, shortening assertion messages to claw back the ~859 bytes, degrades diagnostics to buy one page and this file already records it as a route that failed on the very next section. A single large static converted to frame-backed storage buys ~500x what the stalled sections need, using the pattern §10 and §9 already proved in tree.

Measured largest remaining `.bss` consumers (`build/kernel.map`, 2026-08-18, by address delta):

| Symbol         |    Size | Home                       | Init phase                    | Assessed?        |
| -------------- | ------: | -------------------------- | ----------------------------- | ---------------- |
| `klog_ring`    | 288,000 | `klog.c:78`                | pre-PMM                       | ruled out by §10 |
| `devices`      | 277,504 | `xhci_dev.c:31`            | phase 2 + hot-plug ISR        | ruled out by §10 |
| `tasks`        | 203,776 | `sched/task.c:69`          | scheduler core                | NO               |
| `ctrl_windows` | 180,736 | `../desktop/controls.c:20` | desktop init, late            | NO               |
| `s_recovered`  | 164,000 | `klog.c:404`               | crash recovery, post-PMM read | NO               |

- [x] Assessed all three candidates against the §10 bar before converting anything; two are RULED OUT and the reasons are recorded here rather than rediscovered.
  - The bar: allocated once after `pmm_init`, never lazily, never under a spinlock, never written from ISR context.
  - `tasks` (`src/kernel/sched/task.c:69`, 203776) is DISQUALIFIED: `schedule()` writes it from the PIT IRQ handler (`task.c:1710`) and `nm_handler` writes `tasks[current_task].fpu_used`/`.xsave_area` from #NM exception context (`task.c:639`). Worse, `sti` happens in Phase 1 (`boot_interrupts.c:473-495`) while `task_init()` runs in Phase 3 (`boot_desktop.c:99-103`), so a pointer would be NULL across every intervening tick. It also relies on BSS-zero state for an APC lock (`task.c:616-618`) and `pmm_alloc_pages_hhdm` does not zero.
  - `ctrl_windows` (`src/desktop/controls.c:20`, 180736) is DISQUALIFIED for now: `ctrl_init()` is defined but has NO caller anywhere in the tree, and static BSS is exactly what makes that survivable. The live path `gallery_open()` (`boot_desktop.c:722-724`) reaches `ctrl_create_*` -> `get_or_create_ctrl_window()` (`controls.c:54-71`), which indexes the array directly, so converting it without first wiring a fallible `ctrl_init()` would fault during boot.
  - `s_recovered` (`src/kernel/klog.c:404`, 164000) is SAFE and was converted: `klog_crash_recover()` is the sole writer, called exactly once from `boot_hw.c:575`, after `pmm_init` (`:515`), `vmm_init` (`:525`) and `heap_init` (`:561`). All reads are bounded by `s_recovered_count`, which is set to 0 at entry, so nothing depended on the array being zero-initialized.
  - Symbol-use audit, mechanical: no `sizeof(s_recovered)`, no whole-array address-of, no static assert naming it, and no compile-time consumer of its address. Only `s_recovered[i].<field>` reads and one bound-checked write, so the pointer conversion has no silent `sizeof` collapse.
- [x] Converted `s_recovered` to frame-backed storage via `pmm_alloc_pages_hhdm`, with an EXPLICIT byte count, zeroed through a local and published last, following the `reg_value_pool` conversion in `registry.c:364-378` as the reference.
  - Failure policy is DEGRADED, not `BOOT_FATAL`, which is a correction the design review forced: crash-log recovery is a diagnostic, the entries still reach serial, and the crash REGION allocation in the same function already degrades this way (`klog.c:681-687`). Halting a bootable kernel over a 41-frame contiguous run would trade a working machine for a log file.
  - The allocation sits AFTER the previous boot's crash region is located and CRC-validated, NOT at function entry. Round 1 of the adversarial review caught the alternative: that region is named only by an NVRAM variable and is not reserved in this boot's PMM bitmap, so allocating and zeroing before reading it can erase the evidence recovery exists to read, most reliably on the first boot after an upgrade.
  - An explicit physical-overlap check sits between the allocation and the zeroing, because `pmm_alloc_contiguous` only sets bitmap bits and writes no memory: an overlapping run is handed back UNTOUCHED. A boot with no prior crash now allocates nothing at all.
  - That overlap is DETERMINISTIC, not a corner case, which the kernel auditor caught: `pmm_alloc_contiguous` is a first-fit scan from frame 0 with no cursor, and the previous boot allocated its own crash region from the same scan at the same boot stage, so a single request comes back with exactly that run on the ordinary crash-then-reboot path. A naive guard would have fired every time and silently retired `crash_recovery.log`. `klog_recovered_pool_acquire` therefore HOLDS the colliding run across a second request, so first-fit steps past it, and only then hands the first one back.
  - Sized by `KLOG_RECOVERED_MAX` (799 entries, 131036 bytes, 32 frames), not by `KLOG_RING_SIZE`. The ring holds 1000 entries in RAM but a 128 KiB crash region minus its header holds 799, so the ring constant over-allocated 9 frames nothing could ever fill; the persist side already clamped to the same arithmetic.
  - The acquire step is bracketed by `POST16_CRASHLOG_POOL` and its degrade reports through `klog` with the previous region's address rather than a bare `serial_write`: it is the kernel's first bulk HHDM data write and it runs while the IDT is still the firmware's, so a fault there would otherwise be a silent hang attributed to the header check.
  - THIS BOOT'S crash region outranks the previous boot's disk copy: the 41-frame pool is taken before the 32-frame crash region, so under fragmentation it could have left the machine with no crash logging armed, which the old static array could not do because it consumed no PMM run. If the region allocation fails, the pool is released and the allocation retried, and that free is safe because it runs in `boot_phase0` single-CPU. The recovered entries have already reached serial by then, so the cost is the log FILE, not the evidence.
  - The pool is RETAINED for the life of the boot rather than freed after the log is written, and that is a decision, not an oversight: releasing it was implemented and reverted because `klog_crash_write_to_disk` runs in Phase 2 after `smp_init` and `pmm_free_contiguous` mutates the bitmap unsynchronized, so it would race a timed-out async storage worker. The array was static BSS before, held on every boot unconditionally, so retaining it on the rare post-crash boot is strictly better than what it replaced -> XREF: `03-memory-concurrency/TODO-03 §1` (item: "**PMM bitmap SMP locking**").
- [x] Two pure public helpers make the degraded contract assertable without calling boot infrastructure: `klog_recovered_at()` and `klog_recovered_set_ok()`.
  - `klog_recovered_at()` refuses a NULL pool, a count past capacity, and an index at or past the count; `klog_recovered_set_ok()` answers whether a set may reach the writer at all.
  - `klog_crash_write_to_disk` now checks coherence BEFORE its truncating open, and treats an in-loop accessor refusal as invariant failure rather than end-of-loop. Without that, a fired guard would fall through to the success path, report every entry as written and clear the count, destroying the evidence at exactly the moment the guard fired.
- [x] Recorded the acceptance evidence, in the §10 shape. Measured from `build/kernel.map` by address delta, same build flavor (`-DKERNEL_TESTS` on, as always).
  - BEFORE, at `9b9ab5fd6`: `.bss` `0x56d000..0x7feea5`, `__kernel_end` `0x7ff000`, headroom **347 bytes**.
  - AFTER: `__kernel_end` `0x7d7000`, headroom **167936 bytes** (41 pages). Reclaimed `0x28000` = 163840 bytes, which is the expected 40 whole pages for a 164000-byte array, so the observed figure matches the prediction rather than merely being larger.
  - Guard output UNCHANGED and unweakened: `scripts/build.sh` still prints `BSS check: kernel BSS end 0x00000000007d7000 < user base 0x800000`. The check was not relaxed by so much as a byte.
- [x] A third pass IS expected, and this is what is left in reserve for it.
  - §10 was written as a one-off and its Notes read as solved, which is why the ceiling arrived again unannounced. Naming the reserve is what makes the next recurrence cheap.
  - Reserve, in the order a third pass should consider them: `ctrl_windows` 180736 (needs a fallible `ctrl_init()` wired before `gallery_open()` first), then `tasks` 203776 (needs allocation before Phase 1 unmasks the timer, plus explicit zeroing), then the two §10 already ruled out, `klog_ring` 288000 (pre-PMM) and the xHCI `devices` 277504 (hot-plug ISR writer).
  - **CORRECTION (§14, 2026-09-05): the `tasks` entry on this line is SUPERSEDED and must not be acted on.** It contradicts this section's own assessment 22 lines above, which DISQUALIFIED the symbol rather than gating it. Treat the disqualification as authoritative. -> XREF: this file §14 (item: "Re-assessed `tasks` against §12's disqualification")
  - Below those: `pipes` 73216, `cpu_data` 63872, `s_ureap_slot` 38208, `ports` 37632, `s_bls_fixture` 36992, `glyph_cache` 36480, `s_iocp_pool` 33152. None was assessed here.
  - Trigger the next pass on HEADROOM, not on a failed build. The failure is cascading rather than proportional, so a section either fits with a page to spare or fails by a whole page with no warning: treat headroom under one page (4096 bytes) as the signal, which is roughly where §28 of `01-boot-platform/TODO-13` was reverted from.
- [x] Unparks the ceiling-stalled work -> XREF: this file §7 (item: "Re-run `01-boot-platform/TODO-13` §28 once the guard is gone").
  - The §28 diff is preserved at `.claude/state/deferred-todo13-s28.patch` and its design review is complete, so that re-attempt is apply-then-re-verify rather than a rewrite -> XREF: `01-boot-platform/TODO-13 §28` (item: "Commit: `\"tpm: crash-consistent record pairing and verified-read boot budget\"`").
  - Re-running each unparked section stays with its owning TODO; this section only removes the constraint.
- [x] Commit: `"kernel/mm: second tactical BSS pass -- reclaim a large static"`

**Test checkpoint:** `bash scripts/build.sh` prints the `BSS check` line with `__kernel_end` at least one page below `0x7ff000`, and the reported headroom is recorded in the Notes as the acceptance evidence. Full `scripts/test.sh` green, with the converted pool's owning suite specifically green rather than only the aggregate. `scripts/test-smoke.sh` boots to `C:\>`, because an allocation-failure regression in a boot-path pool surfaces as a hang and not as a failing assertion. A control proves the guard still fires: the conversion must not be accompanied by any weakening of the `scripts/build.sh` check, which stays exactly as strict as it is today. Scope: this section owns ONE tactical conversion and the headroom measurement. The permanent ceiling retirement is §7, the address-space move is §3, `klog_ring` stays with `01-boot-platform/TODO-04 §1` and the xHCI `devices` table with `04-drivers-hardware/TODO-10 §1`. Platforms: QEMU KVM + TCG; **bare metal**.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 4 new klog suites, 0 failures -- 31341 kernel + 17 user tests overall

> **Notes:**
>
> - **What shipped** -- `s_recovered` (164000 bytes of BSS) moved to a frame-backed pool sized by the crash region at 131036 bytes, plus two pure guards (`klog_recovered_at`, `klog_recovered_set_ok`) and a coherence precondition on the disk writer's truncating open.
> - **How it integrates** -- allocation happens inside `klog_crash_recover` AFTER the previous crash region is validated, never at entry, with a physical-overlap check between the allocation and the zeroing; failure and overlap both degrade to serial-only rather than halting.
> - **Downstream effects** -- `__kernel_end` `0x7ff000` -> `0x7d7000`, headroom 347 bytes -> 167936 (41 pages), which unparks the ceiling-stalled queue; a boot with no prior crash now allocates nothing where it previously spent 40 pages permanently.
> - **Reserve for the next pass** -- `ctrl_windows` 180736 then `tasks` 203776, both needing prerequisite work named in the items above; trigger on headroom under one page rather than on a failed build. **`tasks` is SUPERSEDED: see this section's DISQUALIFIED assessment and the §14 correction.**
> - **Canonical doc:** [docs/infrastructure/kernel-address-space.md](../../docs/infrastructure/kernel-address-space.md).
> - **Scope boundary** -- ONE conversion plus its headroom measurement. The permanent ceiling retirement is §7, the address-space move is §3, and the boot-level degraded-path coverage is filed with the klog owner -> XREF: `02-kernel-core/TODO-04 §15` (item: "Cover the crash-recovery DEGRADED path at boot level").

> **Verified:** 2026-08-18 | commit `ef09a0579` | 6/6 items | build OK | `__kernel_end` 0x7ff000 -> 0x7d7000 (headroom 347 -> 167936 bytes) | 31341 kernel + 17 user tests | smoke matrix 4/4 (kvm 1+2 cpu, tcg 1+2 cpu) | lint 0 errors
> **Accepted:** [M] the boot-level DEGRADED path has no executable coverage -- allocation failure, the overlap route and the crash-region priority fallback all live inside `klog_crash_recover`, which `src/kernel/test/test_*.c` may not call (reason: infra; the `pmm_alloc_fail_next()` injector exists, the reachable CALL SITE does not) -> XREF: `02-kernel-core/TODO-04 §15` (item: "Cover the crash-recovery DEGRADED path at boot level")
> **Accepted:** [M] `pmm_free_contiguous` mutates the frame bitmap and `used_frames` unsynchronized, which is why the pool is not released on the Phase-2 success path (reason: pre-existing, repo-wide) -> XREF: `03-memory-concurrency/TODO-03 §1` (item: "**PMM bitmap SMP locking**" at line 103)
> **Quality reviewed:** 2026-08-18 | Codex 15x (design, test-coverage, adversarial x3, re-adversarial x8, consistency, perf) + kernel-quality-auditor | 5H+14M+3L fixed, 2M accepted-XREF, 1L rejected | scope: kernel-code-quality
---

## 13. Third Tactical Reclamation Pass -- the Trigger Section 12 Named Has Fired

> **Spawned-by:** root

Section 12 wrote down the condition for its own successor and then had nowhere to put it: *"A third pass IS expected, and this is what is left in reserve for it"*, with the trigger stated as *"treat headroom under one page (4096 bytes) as the signal"*. That signal has fired and nothing picks it up, because §12 is stamped `[x]` and scoped to ONE conversion, §10's nomination table is spent, and §7 (the permanent retirement) is cascade-blocked on §3, which is operator-deferred pending `todo/answers.md` Q3. This section is the recurring tactical route, and like §10 and §12 it depends on nothing.

MEASURED 2026-09-03 at `56e11533e`: `__kernel_end` page-aligns to `0x7ff000`, one page below `USER_BASE`, and `scripts/overnight/bss-headroom.py` reports per-section budgets of `.text` **95**, `.rodata` 1826, `.data` 980, `.bss` 1323 bytes. `.text` is the tightest and the constraint is cascading, exactly as §12 described.

**What a user hits if this is not done:** nothing kernel-side ships. On 2026-09-03 alone the ceiling deferred `01-boot-platform/TODO-23` §7, `01-boot-platform/TODO-29` §21, `02-kernel-core/TODO-02` §12, and `02-kernel-core/TODO-04` §14's runtime test plus the whole of its §15, §16 and §17. Each was written or designed and then reverted or parked, so the cost is paid twice: once to build the thing and once to take it back out.

**Reclaiming `.bss` buys `.text` headroom, which is the non-obvious part.** A reader seeing `.text: 95` may conclude that converting a large `.bss` array cannot help. It does: the guard tests `__kernel_end`, and the per-section budgets are all distances to the SAME page boundary, so `.text` is tight only because `.rodata` sits 95 bytes above it and a push there cascades `.data` and `.bss` up a whole page onto `0x800000`. Move a large static out of `.bss` and `__kernel_end` drops a page or more, after which `.text` can grow by that page plus its current 95.

**Reserve, in the order §12 left it** (sizes from `build/kernel.map`, 2026-08-18):

| Symbol         |    Size | Home                       | What must land first                                               |
| -------------- | ------: | -------------------------- | ------------------------------------------------------------------ |
| `ctrl_windows` | 180,736 | `../desktop/controls.c:20` | a fallible `ctrl_init()` wired before `gallery_open()` reaches it  |
| `tasks`        | 203,776 | `sched/task.c:69`          | allocation before Phase 1 unmasks the timer, plus explicit zeroing |
| `klog_ring`    | 288,000 | `klog.c:78`                | it is written pre-PMM; ruled out by §10 on that ground             |
| `devices`      | 277,504 | `xhci_dev.c:31`            | a hot-plug ISR writes it; ruled out by §10 on that ground          |

Below those, none assessed: `pipes` 73216, `cpu_data` 63872, `s_ureap_slot` 38208, `ports` 37632, `s_bls_fixture` 36992, `glyph_cache` 36480, `s_iocp_pool` 33152.

> **Complexity verdict, recorded at filing time so it is not re-derived:** `section-manifest.py` returns `SPLIT-RECOMMENDED` (7 work items, 5 subsystems) with a waiver required. That is a signal, not a verdict on the shape: §12 shipped as ONE section with the same profile, because the items are a single conversion plus the assessment and acceptance evidence that make it reviewable, not five independent pieces of work. Re-run the check before implementing and make the call then; if it still recommends a split, the natural seam is assessment-and-measurement first, conversion second.

- [x] Re-measured the reserve against the CURRENT `build/kernel.map` at `56e11533e` before choosing, rather than acting on the 2026-08-18 sizes.
  - Sizes held: `ctrl_windows` was still 180,736 bytes at `src/desktop/controls.c:20` (`CTRL_MAX_WINDOWS * sizeof(struct ctrl_window)`), still top of the reserve table and still the only candidate whose stated prerequisite ("a fallible `ctrl_init()` wired before `gallery_open()` reaches it") was concretely actionable in one section.
- [x] Assessed `ctrl_windows` against the §10 bar and it PASSES, unlike its §12 disqualification (which was "no caller anywhere in the tree", not an SMP hazard).
  - `ctrl_init()` had a doc comment ("call once at boot") but genuinely zero callers -- confirmed dead code, so there was no pre-existing writer to race. It is now called exactly once, from `boot_desktop.c` Phase 3, after `desktop_init()` and before `gallery_open()`.
  - No ISR or ISR-adjacent writer touches `ctrl_windows`: mouse/keyboard input reaches controls only through `ctrl_handle_mouse()`/`ctrl_handle_key()`, called from the compositor's synchronous event loop, never from an interrupt handler.
  - `pmm_alloc_pages_hhdm` does not zero, so the conversion adds an explicit `memset(cw, 0, bytes)` plus a per-slot `focused_id = -1` init loop (the array's original static-BSS invariant was `focused_id` implicitly 0, but the struct's `int focused_id` field means "no control focused" is `-1`, not `0` -- the old code relied on an explicit reset in `get_or_create_ctrl_window()` rather than the zero value, so this is preserved exactly, not newly introduced).
  - Symbol-use audit, mechanical: no `sizeof(ctrl_windows)`, no whole-array address-of (`&ctrl_windows`), no static assert naming it, no compile-time consumer of its address. Only 6 usage sites, all `ctrl_windows[i].field` / `ctrl_windows[handle]` indexing, so the pointer conversion has no silent `sizeof` collapse.
  - Codex adversarial review (round 1) found `ctrl_init()` itself was NOT concurrency-safe against a hypothetical second caller (no ordering guarantee if two CPUs both raced the dead-code path before boot wiring existed) -- fixed with a 3-state atomic CAS machine (`CTRL_INIT_UNINIT` -> `CTRL_INIT_INITIALIZING` -> `CTRL_INIT_READY` via `atomic_cmpxchg`/`atomic_set`/`atomic_read`), so a second concurrent `ctrl_init()` call now no-ops instead of double-allocating or racing the readiness check.
  - Re-adversarial (round 2, both the re-adversarial and test-coverage dispatches independently converged on the same defect) found the OOM-recovery test's cleanup used `ctrl_destroy()`, which only tombstones a control's TYPE and leaves `cw->active`/`cw->count` set -- the test-created control on window_handle 0 would have survived into production boot with the slot "in use". Fixed: cleanup uses `ctrl_destroy_all(0)` instead, with an assertion that a post-cleanup control gets id 0 (proving the pristine reset). Test-coverage review also found the CAS guard itself had zero direct coverage (both existing tests only exercise UNINIT->degraded->UNINIT->READY, never a repeat call while READY) -- added `test_ctrl_init_idempotent_when_ready`, asserting a repeat `ctrl_init()` call stays READY, allocates no second pool (`pmm_get_used_frames()` unchanged), and leaves an existing control's text intact.
  - Post-ship review pipeline (round 3: adversarial + consistency + perf Codex, plus a `kernel-quality-auditor` pass) found `ctrl_test_reset_for_fault_injection()` freed the pool BEFORE un-publishing `ctrl_init_state` -- the inverse of `ctrl_init()`'s own publish-last order, so a reader between the free and the unpublish would see `READY` over already-freed frames. Fixed: un-publish (NULL the pointer, zero phys/pages, `atomic_set` to `UNINIT`) now runs first, and the free uses captured locals after. Also fixed a klog `%u` argument passed as `uint32_t` where the format consumer reads `va_arg(ap, uint64_t)` (repo convention is an explicit `(uint64_t)` cast, confirmed against `elf.c`'s identical pattern) and added `TEST-SIDE-EFFECT-ALLOWED` markers to the 3 tests that call `ctrl_init()` directly, since it is a desktop-only, idempotent-by-design, degrade-not-halt init outside the hook's enumerated boot-critical list -- a review-judgment call per the policy doc's opt-out, not a hook violation.
- [x] Converted `ctrl_windows` to frame-backed storage via `pmm_alloc_pages_hhdm`, following the `s_recovered` conversion in `klog.c` and the `reg_value_pool` one in `registry.c:364-378`.
  - Explicit byte count (`CTRL_MAX_WINDOWS * sizeof(struct ctrl_window)`), zeroed through the local `cw` before publishing the global pointer, publication word (`ctrl_windows`) stored LAST after `atomic_set(&ctrl_init_state, CTRL_INIT_READY)`'s predecessor state.
  - Failure policy is DEGRADED, not halt: `ctrl_init()` klogs `LOG_ERROR` and leaves `ctrl_init_state` at `CTRL_INIT_UNINIT` on OOM, matching the diagnostic-vs-load-bearing distinction §12 established. `ctrl_ready()` (new accessor) reports the outcome; `gallery_open()` now checks it up front instead of silently opening a non-functional dialog (Codex adversarial finding: the old code ignored every `ctrl_create_*()` -1 return and unconditionally logged "Control Gallery opened").
  - `get_ctrl_window()` and `get_or_create_ctrl_window()` both gate on `ctrl_ready()` first, so a degraded pool returns -1/NULL from every lookup path rather than dereferencing a NULL `ctrl_windows`.
- [x] Recorded the acceptance evidence, §10/§12 shape, from `build/kernel.map`, same build flavor (`-DKERNEL_TESTS` on).
  - BEFORE, at `56e11533e`: `.text` budget **95** bytes, `.rodata` 1826, `.data` 980, `.bss` 1323 (`__kernel_end` page-aligned to `0x7ff000`).
  - AFTER the `ctrl_windows` conversion: `__kernel_end` `0x7d4000`, per-section budgets `.text` 179023, `.rodata` 177458, `.data` 177108, `.bss` 177939 -- a reclaim of `0x7ff000 - 0x7d4000` = `0x2b000` = 176,128 bytes (43 pages), against a 180,736-byte array (44.13 pages); the extra ~0.13 page is accounted for by the pre-conversion page rounding already borrowing partway into the freed range, so the observed reclaim is the predicted whole-page count.
  - Guard output unchanged and unweakened: `scripts/build.sh` still prints the same `BSS check` line shape; the check itself was not touched.
- [x] Swept dead test registrations as the cheap secondary source. Found and removed 12, not the 2 the 2026-09-03 note anticipated.
  - All 12 were `test_suite_register_cat(...)`-registered functions with a genuinely empty body (mechanically verified: brace-balanced empty or comment-only), so each reported PASS while asserting nothing: `test_boot_perf_header_magic` (`test_boot_init.c`), `test_tls_constants` / `test_tls_expansion_post_codes` / `test_auxv_constants` (`test_peb_teb.c`), `test_mbr_gpt_constants` (`test_vfs.c`), `test_mdmp_signature` / `test_mdmp_version` / `test_mdmp_processor_arch` (`test_crashdump.c`), `test_filetime_ticks_per_second` (`test_nt_types.c`), `test_alpc_max_message_length` (`test_alpc.c`), `test_ixfs_magic_value` / `test_ixfs_version` (`test_ixfs.c`).
  - Removing the 12 functions plus their registration call sites moved `.text` from 179023 to 179503 bytes (480 bytes) and `.rodata` from 177458 to 177730 (272 bytes); small next to the pool conversion, but free and it makes the suite count honest -- `PASS: 32852 kernel + 17 user-mode tests passed` held exactly, unchanged, because the removed functions asserted zero conditions and contributed zero to that count either way.
  - Consistency Codex review (round 2) found 3 of the 12 were named, with concrete claims, by other TODOs' own checklist evidence: `TODO-31` §13 (item: "Unit test: `test_mbr_gpt_constants`") and `TODO-11` §12/§13's "6 TLS unit tests"/"9 auxv suites" counts. `test_mbr_gpt_constants` stays removed -- its claim was already covered by `_Static_assert`s two items up in the same `TODO-31` section, so a runtime dup would be tautological (Gate 10); `TODO-31` §13's item was rewritten to say so. `test_tls_constants` and `test_tls_expansion_post_codes` were REIMPLEMENTED in `test_peb_teb.c` with real derived-value/uniqueness assertions, restoring both `TODO-11` counts -- no `TODO-11` edit was needed.
  - `test_auxv_constants` was first reimplemented as 13 `TEST_ASSERT_EQ(AT_X, literal)` calls, then `scripts/lint.sh` Check 6 flagged every one as the same tautological-constant pattern already tracked as legacy debt in `test_exec.c` -- an external-ABI literal echoed against itself is still a compiler-already-enforces-it check, same as an internal one. Rewritten to assert pairwise distinctness across the 13 `AT_*` codes this codebase actually pushes into the auxv array, which is the real, previously-uncovered invariant (a collision would silently overwrite one entry's value with another's when `task_exec()` builds the vector).
- [x] Unparked what the new headroom actually admits, and said what it does NOT.
  - At 177,108 bytes of the tightest (`.data`) budget, every section this file's own reserve table and the parked-elsewhere XREFs name (each blocked by a fraction of ONE page, 95-347 bytes) now fits with over 400x the headroom that stalled them. This item names them; re-applying each preserved diff/patch and re-running its own build+test+review cycle is the actual unpark work and stays owned by §11's sweep -> XREF: this file §11 (item: "Unpark the ceiling-stalled kernel queue").
  - `02-kernel-core/TODO-04-system-logging.md` §14 (item: "Add the RUNTIME regression test driving the renderer at 7/8/9/10-digit `sec` values with guard bytes") -- now fits; not yet re-applied.
  - `02-kernel-core/TODO-09-x86-64-architecture.md` §20 (item: "`topology_init()` consume `per_cpu_data.core_type`") -- preserved diff re-measured at a full page against 95 bytes; now fits with room to spare; not yet re-applied.
  - `02-kernel-core/TODO-10-kernel-security-hardening.md` §32 (`HEAP_ZERO_ON_FREE` flip + 6 `TEST_CAT_FS` regression cases, patch at `.claude/state/deferred-TODO-10-kernel-security-hardening-s32-tests.patch`) -- now fits; not yet re-applied.
  - `02-kernel-core/TODO-11-peb-teb-user-abi.md` §28 (item: "Add an SMP regression test for TEB-published-without-kernel_gs_base", patch at `.claude/state/deferred-TODO-11-peb-teb-user-abi-s28.patch`) -- now fits; not yet re-applied.
  - `02-kernel-core/TODO-12-native-api-ssdt.md` §32 (tail-pack path, variable-length `unveil_entry`, ACCESS_MASK enforcement) -- now fits; not yet re-applied.
  - `02-kernel-core/TODO-26-power-management.md` §2 (S1 idle integration) -- now fits on headroom, but SPLIT-RECOMMENDED (7 items) still applies independent of the ceiling, so this one needs a split decision before re-attempt, not just a re-apply.
  - None of the above is re-implemented BY this item; that re-application, its own build/test verification, and its own review pass are §11's job, tracked per-item there.
- [x] Stated plainly: a FOURTH pass IS expected, and the reserve for it is named below (see Notes).
- [x] Commit: `"kernel/mm: third tactical reclamation pass -- buy a page of headroom"`

**Test checkpoint:** `bash scripts/build.sh` prints the `BSS check` line with `__kernel_end` at least one page below `0x7ff000` and the headroom recorded in the Notes. Full `scripts/test.sh` green, with the converted pool's owning suite green in its own right rather than only in the aggregate. `scripts/test-smoke.sh` boots to `C:\>`, because an allocation-failure regression in a boot-path pool surfaces as a hang rather than a failing assertion, and `scripts/test-smoke-matrix.sh` if the candidate is touched during boot. A control proves the guard still fires and was not relaxed by a byte. Scope: ONE conversion plus the headroom measurement; the permanent retirement stays §7, the address-space move stays §3, and the unpark sweep stays §11. Platforms: QEMU KVM + TCG; **bare metal**.

> **Test runner:** `scripts\debug\kernel\run-desktop-tests.bat` (SUITE=desktop) + `run-abi-tests.bat` (SUITE=abi) | 3 new ctrl_init suites + 3 reimplemented ABI suites, 0 failures -- 32863 kernel + 17 user tests overall

> **Notes:**
>
> - **What shipped** -- `ctrl_windows` (180,736 B) moved to a `pmm_alloc_pages_hhdm` pool, guarded by an atomic CAS (`ctrl_init_state`) + `ctrl_ready()`; plus a 12-function dead-test sweep (all empty-bodied, all previously PASS-on-nothing).
> - **How it integrates** -- `ctrl_init()` is now actually called (never was before), wired into `boot_desktop.c` Phase 3 before `gallery_open()`, which now checks `ctrl_ready()` first instead of opening a non-functional dialog on OOM.
> - **Downstream effects** -- `__kernel_end` `0x7ff000` -> `0x7d4000`, `.text` 95 -> 178735 bytes (post-fix-loop; still 176962+ headroom on the tightest section); unparks the ceiling-stalled queue named above; `PASS: 32863 kernel + 17 user-mode`, 0 failures throughout every fix round.
> - **Reserve for the next pass** -- **SUPERSEDED by §14.** This line names `tasks` 203776 B (`sched/task.c:69`) as next "needing its §12-recorded prerequisites", but §12 DISQUALIFIED that symbol (PIT IRQ handler writer, plus a Phase 1 / Phase 3 ordering gap) and only its reserve line softened it. Do not nominate `tasks`. The corrected menu is §14's.
> - **Canonical doc:** [docs/infrastructure/kernel-address-space.md](../../docs/infrastructure/kernel-address-space.md).
> - **Scope boundary** -- ONE conversion plus headroom measurement and the test sweep; re-applying unparked sections is §11's job, not this section's.

> **Verified:** 2026-09-03 | commit `67e7ca12c` | 8/8 items | build OK | `__kernel_end` 0x7ff000 -> 0x7d4000 (headroom 95 -> 178735 bytes .text) | 32863 kernel + 17 user tests | smoke matrix 4/4 (kvm 1+2 cpu, tcg 1+2 cpu) | lint 0 errors
> **Quality reviewed:** 2026-09-03 | Codex 7x (design, adversarial x2, re-adversarial, test-coverage, consistency, perf) + kernel-quality-auditor | 1H+6M+2L fixed, 0 open | scope: kernel-code-quality + desktop-code-quality

---

## 14. Fourth Tactical Reclamation Pass -- Reclaim the Test-Only Statics

> **Spawned-by:** root

§13 reclaimed 176,128 bytes on 2026-09-03 and set its own successor's trigger: *"trigger the fourth pass on headroom under one page, same signal §12 set"*. MEASURED 2026-09-05 at `ad84efa84`: `scripts/overnight/bss-headroom.py` reports per-section budgets of `.text` 1359, `.rodata` **2**, `.data` 964, `.bss` 1235 bytes, with `__kernel_end` page-aligned back to `0x7ff000`. The trigger has fired. Two days of kernel work spent the entire §13 reclaim, which is the measured refill rate this section should assume rather than treat as an anomaly.

**What a user hits if this is not done:** nothing kernel-side ships, again. At 2 bytes of `.rodata` no kernel section can add a single string literal, test name or const table, and the link fails rather than warning. Verified 2026-09-05 against `02-kernel-core/TODO-26`: every one of its 21 remaining sections is a kernel-code section and none is implementable at this ceiling, on top of the sections §13 already recorded as ceiling-deferred on 2026-09-03.

**The reserve handed forward is unsafe as written and must be re-derived, not inherited. The contradiction ORIGINATES in §12, not §13.** §12 assessed `tasks` (203,776 B, `sched/task.c:69`) and DISQUALIFIED it at line 532, then listed it in its own reserve at line 554 as merely "needs allocation before Phase 1 unmasks the timer, plus explicit zeroing", and repeated that softer framing in its Notes at line 571. §13 inherited §12's reserve wording rather than inventing it. So a reader following the reserve chain reaches a nomination that the same section's assessment had already refused, and converting `tasks` on that wording would put a NULL pointer under the PIT IRQ handler. The disqualification is the correct half and it still holds against this tree.

**Scope was SPLIT on the manifest's own verdict rather than waived.** `section-manifest.py` returns `SPLIT-RECOMMENDED` (7 work items, 6 subsystems, `waiver_required`). §13 waived the same verdict and shipped one section; this pass does not, because the natural seam here is real and cheap: the test-only statics carry NO production code path, while a pool conversion changes live kernel behaviour. Reclaiming the test statics is a strictly-safer, self-contained unit that buys enough headroom on its own. The `pipes` conversion is therefore §15, not an item here.

**Corrected candidate menu, measured from `build/kernel.map` at `ad84efa84` on 2026-09-05** (gap-derived sizes; `tasks` reproduces §12's recorded 203,776 exactly, which is the control that the method is reading real extents):

| Symbol            |   Size | Home                                | Verdict                                                   |
| ----------------- | -----: | ----------------------------------- | --------------------------------------------------------- |
| `klog_ring`       | 288000 | `klog.c:78`                         | ruled out by §10: written pre-PMM                         |
| `devices`         | 277504 | `xhci_dev.c:31`                     | ruled out by §10: hot-plug ISR writer                     |
| `tasks`           | 203776 | `sched/task.c:69`                   | DISQUALIFIED by §12, not merely gated; see the item below |
| `pipes`           |  73216 | `src/kernel/ipc/pipe.c:25`          | production pool: split out to §15                         |
| `cpu_data`        |  64000 | per-CPU data                        | unassessed; expect an ISR/boot-order disqualification     |
| `s_ureap_slot`    |  38208 | --                                  | unassessed                                                |
| `s_bls_fixture`   |  36992 | `test/test_boot_entry_parser.c:428` | TEST-ONLY: reclaimed here                                 |
| `ports`           |  37632 | --                                  | unassessed                                                |
| `glyph_cache`     |  36480 | --                                  | unassessed                                                |
| `s_iocp_pool`     |  33152 | --                                  | unassessed                                                |
| `s_parse_block`   |  32784 | `test/test_env.c:2470`              | TEST-ONLY: reclaimed here                                 |
| `g_cap_subs`      |  32768 | `test/test_knf.c:600`               | TEST-ONLY: reclaimed here                                 |
| `env_test_bigval` |  32016 | `test/test_env.c:61`                | TEST-ONLY: reclaimed here                                 |

**The four test statics are ALREADY a ceiling workaround, which is why they are the right target.** Each was made a shared file-scope buffer specifically to survive this gate, and two say so in their own comments: `test_boot_entry_parser.c:426-427` records *"two function-local statics cost 72 KB of test-build BSS and push the image over the 0x800000 user-base ceiling. Sharing reclaims 36 KB"*, and `test_env.c:59-60` records *"BSS-collision gate rejects the image, so the cases share this buffer"*. Sharing was the cheap half of the fix; moving them off `.bss` entirely is the rest of it, and it costs the production image nothing because none of these bytes serves production code.

- [x] Re-measured the whole reserve against the CURRENT `build/kernel.map` at `ad84efa84` before choosing, rather than acting on the recorded sizes.
  - The sizes above are gap-derived from adjacent symbol addresses, which over-attributes trailing anonymous data to the preceding symbol. That is accurate for large `.bss` arrays (the `tasks` control matches §12's independently recorded figure to the byte) and is NOT reliable for `.rodata`, where anonymous string literals dominate. Do not carry any `.rodata` figure into a decision without re-deriving it.
- [x] Re-assessed `tasks` against §12's disqualification: it STANDS, and §13's reserve wording is corrected above rather than carried forward.
  - §12 line 528 is explicit: `schedule()` writes it from the PIT IRQ handler (`task.c:1710`), `nm_handler` writes `tasks[current_task].fpu_used`/`.xsave_area` from #NM exception context (`task.c:369`), `sti` happens in Phase 1 (`boot_interrupts.c:473-495`) while `task_init()` runs in Phase 3 (`boot_desktop.c:99-103`) so a pointer would be NULL across every intervening tick, and it relies on BSS-zero state for an APC lock (`task.c:616-618`) while `pmm_alloc_pages_hhdm` does not zero.
  - Either confirm the disqualification stands and correct the reserve wording, or, if the boot-order prerequisite is genuinely landable, say so as its OWN section rather than smuggling a scheduler boot re-order into a tactical reclamation pass.
- [x] Moved the four test-only statics off `.bss` without weakening a single assertion: `s_bls_fixture` (36,992), `s_parse_block` (32,784), `g_cap_subs` (32,768), `env_test_bigval` (32,016).
  - Total is 134,560 bytes of `.bss` that existed solely to run tests. The vehicle is the repo's own `TEST_SCRATCH_KBUF` (`include/kernel/test/scratch.h`), not a hand-rolled allocation: it already routes by size (`kmalloc` at or below 4 KB, `pmm_alloc_contiguous` above), registers its free through `test_add_action` so cleanup fires on assertion-induced early return, and asserts plus returns on OOM. Using it made all four of the design review's recommendations fall out of an existing reviewed primitive.
  - The capacity constants are explicit (`ENV_TEST_BIGVAL_SZ`, `ENV_PARSE_BLOCK_SZ`, `KNF_CAP_SUBS_BYTES`) because `sizeof` on the converted pointer collapses to 8. The design review caught this at `test_env.c:66-68`, where the fill helper would have written seven bytes instead of 32,000 and every large-environ case would have passed over a tiny value.
  - Allocate lazily on first use and release at a defined point, so a suite that never runs pays nothing. `pmm_alloc_pages_hhdm` does not zero: reproduce each buffer's existing initial state explicitly rather than inheriting BSS-zero.
  - Preserve the sharing invariants the current comments record, notably `test_env.c`'s *"any case that mutates it restores it"* single-threaded contract. If a buffer becomes dynamically allocated, the restore contract still has to hold across cases.
  - An OOM path in a test must FAIL the test loudly, never silently skip it. A reclaim that converts a real assertion into a no-op on allocation failure is a coverage regression wearing a headroom win.
- [x] Confirmed the reclaim did not weaken the suites that own these buffers, by CONTROL rather than by inspection.
  - `SUITE=boot` 6138, `SUITE=abi` 2170, `SUITE=knf` 234, each green in its own right, plus the aggregate.
  - The count did not fall, and this was MEASURED rather than argued: the change was stashed, the tree rebuilt and the full suite re-run to get a true baseline of **33,889**, against **33,895** with the change. The delta is exactly +6, one `TEST_ASSERT_NOT_NULL` per scratch site, so no assertion was lost. A stamp elsewhere in the corpus reads 33,871; that is an older commit, and taking it as the baseline would have manufactured a +24 discrepancy out of nothing.
- [x] Recorded the acceptance evidence, §10/§12/§13 shape, from `build/kernel.map` in the same build flavor (`-DKERNEL_TESTS` on).
  - BEFORE at `ad84efa84`: `__kernel_end` page `0x7ff000`, budgets `.text` 1359, `.rodata` **2**, `.data` 964, `.bss` 1235; `tightest: .rodata`, `tight: true`.
  - AFTER: `__kernel_end` page `0x7df000`, budgets `.text` 131551, `.rodata` 134898, `.data` 132036, `.bss` 131667; `tightest: .text`, `tight: false`.
  - Reclaim `0x7ff000 - 0x7df000` = `0x20000` = **131,072 bytes, exactly 32 whole pages**.
  - Reconciled against DECLARED sizes, which are NOT the table's symbol gaps: `s_bls_fixture` 36,980 (`sizeof(boot_entries_parse_result_t)`), `s_parse_block` 32,783 (`ENV_VALUE_MAX` 32,767 + 16), `g_cap_subs` 32,768 (4,096 pointers), `env_test_bigval` 32,001, total **134,532 bytes** = 32.84 pages. The gap figures in the table above run 12-15 bytes higher per symbol because they absorb inter-symbol padding, which is exactly the over-attribution the first item warns about; using them here would be the same mistake in the same section.
  - 134,532 freed against 131,072 observed leaves 3,460 bytes, and that residue is expected rather than unexplained: `__kernel_end` only moves in whole pages, and the pre-conversion end `0x7feb2d` already sat 1,235 bytes into its final page. The freed bytes are not page-aligned, so the whole-page reclaim is `floor` of the total, not a clean division. **This bullet originally claimed a 0.13-page residue copied from §13's wording; 134,532 / 4096 leaves 0.84 of a page, and the consistency review caught the copied arithmetic.**
  - The guard was not touched: `scripts/build.sh` prints the same `BSS check` line shape, now reading `kernel BSS end 0x00000000007df000 < user base 0x800000`. The control that matters here is the stash-and-rebuild above, whose effect the probe demonstrably saw (a different `__kernel_end` and a different test total), rather than a mutation the compiler could fold away.
- [x] Named what the new headroom admits and what it does NOT, and handed the re-application to §11 rather than doing it here.
  - ADMITS: 131,551 bytes on the tightest section, against the 95-347-byte fractions that stalled the parked work. All 21 remaining `02-kernel-core/TODO-26` sections (§12-§25, §27, §29, §33-§36, §39) now fit on headroom, as do the §11 sweep candidates `TODO-04` §14, `TODO-09` §20, `TODO-10` §32, `TODO-11` §28 and `TODO-12` §32.
  - Does NOT admit: anything needing the address-space move itself (§3, §7, and the three `test_highhalf.c` assertions §11 keeps parked), and it does not clear a NON-ceiling blocker. `TODO-26` §2 still needs its own split decision, and the sections deferred on absent ACPI namespace evaluation or an absent hibernation write path are untouched by headroom.
  - Naming them is this section's job; re-applying each preserved diff and re-running its own build, test and review cycle stays §11's. -> XREF: this file §11 (item: "Run the status sweep for the FOURTH pass")
- [x] Stated the reserve for a FIFTH pass plainly, with each entry's VERDICT rather than a bare size, and the same under-one-page trigger.
  - §13's handoff failed because it recorded a size and a soft blocker where §12 had recorded a disqualification. The corrected menu is the table above, and every entry now carries a verdict.
  - FIFTH PASS, in order: `pipes` 73,216 (`src/kernel/ipc/pipe.c:25`) is ALREADY SCOPED as §15 and is the next move; then `cpu_data` 64,000, `s_ureap_slot` 38,208, `ports` 37,632, `glyph_cache` 36,480 and `s_iocp_pool` 33,152, all UNASSESSED and each needing the §10 bar walked before it is touched. `klog_ring`, `devices` and `tasks` stay DISQUALIFIED and must not be re-proposed without clearing the specific hazard recorded against each.
  - The four test-only statics are SPENT: there is no comparable test-only `.bss` left, so the fifth pass necessarily converts production state and carries a real failure mode. Trigger stays headroom under one page.
- [x] Commit: `"kernel/test: fourth tactical reclamation pass -- move the test-only statics off .bss"`

**Test checkpoint:** `bash scripts/build.sh` prints the `BSS check` line with `__kernel_end` at least one page below `0x7ff000` and the headroom recorded in the Notes. Full `scripts/test.sh` green with `SUITE=boot`, `SUITE=abi` and `SUITE=knf` each green in their own right and the aggregate assertion count not lower than before. `scripts/test-smoke.sh` boots to `C:\>`. A control proves the guard still fires. Scope: the four test-only statics plus the measurement and the reserve correction; the `pipes` pool conversion is §15, the permanent retirement stays §7, the address-space move stays §3, and the unpark sweep stays §11. Platforms: QEMU KVM + TCG.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) + `run-abi-tests.bat` (SUITE=abi) + `run-knf-tests.bat` (SUITE=knf) | 6138 + 2170 + 234 kernel tests, 0 failures -- 33895 kernel + 17 user tests overall

> **Notes:**
>
> - **What shipped** -- four test-only statics (134,560 B total) moved off `.bss` onto per-case `TEST_SCRATCH_KBUF` allocations, plus a partial-failure handle leak fixed in the KNF subscriber-cap case.
> - **How it integrates** -- no production code path changed; the vehicle is the existing `include/kernel/test/scratch.h` primitive, whose `test_add_action` registration frees each buffer on the suite drain, including after an assertion-induced early return.
> - **Downstream effects** -- `__kernel_end` `0x7ff000` -> `0x7df000`, `.rodata` budget 2 -> 134,898 bytes, tightest section now `.text` at 131,551; unparks all 21 remaining `TODO-26` sections and the §11 sweep candidates. `PASS: 33895 kernel + 17 user-mode` against a measured 33,889 baseline, the +6 being one scratch assertion per site.
> - **Reserve for the next pass** -- `pipes` 73,216 is scoped as §15; the test-only source is now SPENT, so the fifth pass necessarily converts production state. Trigger stays headroom under one page.
> - **Canonical doc:** [docs/infrastructure/kernel-address-space.md](../../docs/infrastructure/kernel-address-space.md).
> - **Scope boundary** -- the four test statics plus measurement and the reserve correction; the `pipes` conversion is §15, the permanent retirement stays §7, the address-space move stays §3, and the unpark sweep stays §11.

---

## 15. Fifth Tactical Reclamation Pass -- Convert the `pipes` Pool

> **Spawned-by:** §14 (split)

The production half of the fourth-pass candidate menu, split out of §14 on `section-manifest.py`'s `SPLIT-RECOMMENDED` verdict. §14 reclaims test-only `.bss` and touches no production code path; this section converts one live kernel pool and therefore carries a different failure mode, a different review surface, and a different rollback. Run it when the §14 headroom is spent, on the same under-one-page trigger §12 set, or earlier if a section needs more than §14 bought.

`pipes` (`src/kernel/ipc/pipe.c:25`, `static pipe_t pipes[PIPE_MAX]`, 73,216 bytes measured at `ad84efa84`) is the largest `.bss` consumer not already disqualified: `klog_ring` and `devices` were ruled out by §10 (pre-PMM writer, hot-plug ISR writer) and `tasks` by §12 (PIT IRQ handler writer, plus a Phase 1 / Phase 3 ordering gap). It is referenced from exactly one translation unit, which bounds the symbol-use audit to a single file.

- [ ] Assess `pipes` against the §10 bar before converting anything, and record the verdict either way.
  - Confirm no ISR or ISR-adjacent writer: pipe reads and writes should reach the pool only through syscall context, never an interrupt handler. If an ISR writer exists the symbol is disqualified like `tasks`, and that verdict is the section's deliverable.
  - Confirm the init ordering: `pipe_init()` must run before any consumer can reach the pool, with no window in which a published pointer is NULL.
  - Mechanical symbol-use audit: no `sizeof(pipes)`, no whole-array address-of, no static assert naming it, no compile-time consumer of its address, so the pointer conversion cannot collapse a `sizeof` silently.
- [ ] Convert `pipes` to frame-backed storage via `pmm_alloc_pages_hhdm`, following the `ctrl_windows` conversion §13 shipped and the `reg_value_pool` one at `registry.c:364-378`.
  - Explicit byte count, zeroed through a local before the global pointer is published, and the publication word stored LAST. `pipe_init()` already clears `in_use` per slot (`pipe.c:32`); record whether that is the whole initial invariant or only part of it.
  - Failure policy is DEGRADED, not halt, matching §12 and §13: klog `LOG_ERROR` on OOM and a readiness accessor that every lookup path gates on, so a degraded pool refuses cleanly instead of dereferencing NULL.
  - Guard against a second concurrent initialisation the way §13 did, with an atomic state machine rather than a plain flag, unless the call site provably admits only one caller.
- [ ] Record the acceptance evidence and the reserve for a SIXTH pass, same shape as §14.
- [ ] Commit: `"kernel/ipc: fifth tactical reclamation pass -- convert the pipes pool"`

**Test checkpoint:** `bash scripts/build.sh` prints the `BSS check` line with `__kernel_end` at least 17 pages below its pre-section value. Full `scripts/test.sh` green with `SUITE=ipc` green in its own right. `scripts/test-smoke.sh` boots to `C:\>`, because an allocation-failure regression in a pool the boot path touches surfaces as a hang rather than a failing assertion. A control proves the BSS guard still fires. Scope: ONE pool conversion plus its measurement. Platforms: QEMU KVM + TCG.

---

## OS Comparison

| ⭐  | Feature                           | 🪟 Win11                 | 🐧 Linux                         | 🚀 Impossible OS                                                   |
| --- | --------------------------------- | ------------------------ | -------------------------------- | ------------------------------------------------------------------ |
| 💎  | Kernel in upper canonical half    | ✅ `0xFFFF800000000000`+ | ✅ `0xffffffff80000000` (-2 GiB) | ⚠️ §1 pins `0xffffffff80000000`; §2-§3 move it                     |
| 💎  | Direct physmap of RAM (HHDM)      | ⚠️ PFN db + dynamic PTEs | ✅ `page_offset_base` physmap    | ✅ §2 HHDM (PML4 273-400, 64 TiB); §9 walkers route through it     |
| 💎  | 128 TB user / 128 TB kernel split | ✅ 48-bit split          | ✅ 48-bit split                  | ⚠️ §1 defines the split; §7 retires the ceiling                    |
| 💎  | Per-process address space         | ✅ per-process           | ✅ `mm_struct` per task          | ⚠️ PML4 per task (D01 T10 §8); high-share §6                       |
| 💎  | Kernel/user page-table isolation  | ✅ KVA Shadow            | ✅ KPTI                          | ⬜ Unblocked by §6 (D02 T10 §6)                                    |
| 💎  | KASLR                             | ✅ kernel ASLR           | ✅ KASLR                         | ⬜ Unblocked by §3 (D02 T10 §14)                                   |
| 💎  | SMEP / SMAP clean split           | ✅ enforced              | ✅ enforced                      | ⬜ Unblocked by §6 (D02 T10 §2)                                    |
| 💎  | PCID no-flush ring transitions    | ✅ with KVA Shadow       | ✅ with KPTI                     | ⬜ Unblocked by §6 (D02 T10 §7)                                    |
| 💎  | No hardcoded user ceiling         | ✅ no low ceiling        | ✅ no low ceiling                | ⬜ §7 retires `0x800000`                                           |
| ⭐  | 5-level paging (LA57, 128 PiB)    | ❌ not supported         | ✅ unconditional (6.10+)         | ⬜ Planned -- §8 (surpasses Win11)                                 |
| 💎  | Kernel pools dynamic, not static  | ✅ `ExAllocatePool*`     | ✅ slab / `kmem_cache`           | ✅ §10 registry + atom pools, §12 crash-recovery, §13 ctrl_windows |
| ⭐  | Layout as asserted single source  | ⚠️ undocumented publicly | ⚠️ macros + prose, no manifest   | ✅ §1 `memmap.h` + 20-assert gate (live)                           |
| ⭐  | Phys<->virt relations type-split  | ⚠️ single blended macro  | ⚠️ single `__pa`/`__va` pair     | ✅ §1 HHDM vs image, range-checked + rejecting                     |

> **After §1-§7:** Impossible OS matches the Windows 11 / Linux memory model -- higher-half kernel, private per-process lower half, and the security split that KASLR / SMEP / SMAP / KPTI build on.
> **After §8:** Impossible OS exceeds Windows 11, which has no 5-level paging support.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_highhalf()` -- register in `src/kernel/test/test_runner.c`. Use `TEST_CAT_MM` (memory management).

> [!WARNING]
> **The in-kernel suite is BLOCKED on the higher-half kernel, NOT on headroom (corrected 2026-07-17).** `test_highhalf.c` was authored and wired during §1 and tripped the guard on its first build: `BSS COLLISION: kernel BSS end (0x801000) >= user base (0x800000)`. That headroom blocker is GONE -- §10 moved the BSS end to `0x6c2000` (~1272 KiB free), and the §7 ALPC stash that once tripped the same guard now builds with room to spare. But the ceiling was never this suite's ONLY blocker: 3 of its 5 assertions below describe a kernel running in the upper half, which §3's low->high jump has not delivered (it is operator-deferred), so they would FAIL rather than pass. Re-add it as the first work item of §7, where it is filed -> XREF: `02-kernel-core/TODO-33 §11` (item: "Re-add `test_highhalf.c`").
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
