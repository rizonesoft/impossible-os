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

| ⭐  | Order | Deliverable                                              | Depends On             | Status |
| --- | :---: | -------------------------------------------------------- | ---------------------- | :----: |
| 🔥  |  10   | Tactical BSS headroom: large static pools -> dynamic     | --                     |  [x]   |
| 🔥  |  11   | Unpark the ceiling-stalled kernel queue (status sweep)   | §10                    |  [/]   |
| 🔥  |  12   | Second tactical BSS pass: reclaim a large static again   | --                     |  [x]   |
| 💎  |   1   | Memory-map design + canonical layout decision            | --                     |  [x]   |
| 💎  |   2   | Direct map construction (install HHDM; kernel still low) | §1                     |  [x]   |
| 💎  |   9   | VMM walker conversion -- derefs onto the HHDM helper     | §2                     |  [/]   |
| 💎  |   3   | Linker VMA/LMA split + higher-half jump (one unit)       | §1, §2, §9             |  [/]   |
| 💎  |   4   | Descriptor tables + per-CPU at high addresses + AP path  | §3                     |  [/]   |
| 💎  |   5   | `boot_info` / framebuffer handoff + identity teardown    | §1, §3, §9, D01 T01 §8 |  [/]   |
| 💎  |   6   | Per-process PML4: kernel high shared, user low private   | §3, D01 T10 §8         |  [/]   |
| 💎  |   7   | Retire `0x800000` USER_BASE ceiling + BSS guard          | §6                     |  [/]   |
| ⭐  |   8   | 5-level paging (LA57) support -- exceeds Win11           | §1, §3                 |  [/]   |

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
> **Deferred:** [M] the in-kernel `test_highhalf.c` suite cannot ship -- it tripped the BSS guard with zero headroom (reason: the ceiling this TODO exists to retire) -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation.md §7 (item: "Re-add `src/kernel/test/test_highhalf.c`" at line 249)
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
> **Deferred:** [M] worst-case arena tail (~4-5 MiB) reserved-but-unused (reason: efficiency, not correctness) -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation.md §2 (item: "Reclaim the unused worst-case arena tail" at line 123)
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
- [ ] Re-run `02-kernel-core/TODO-23` §2-§16 once the guard is gone -- the whole exception/SEH file is ceiling-parked (each section adds kernel `.text`); un-defer and implement in Implementation-Order once §1's types link
- [ ] Re-run `01-boot-platform/TODO-13` §28 once the guard is gone (item: "Commit: `\"tpm: crash-consistent record pairing and verified-read boot budget\"`"); design review is DONE and the diff is preserved, so the re-attempt is apply-then-re-verify
  - Reverted 2026-08-18 with the tree left green. The code built clean on its own; the section's mandatory unit tests pushed `.rodata` over a 4 KiB page, which cascaded `.data` and `.bss` each up one page and landed `__kernel_end` exactly on `0x800000`.
  - The diff is at `.claude/state/deferred-todo13-s28.patch` (gitignored, survives a session rollover but NOT a fresh clone). If it is lost, the §28 Deferred stamp records every design outcome so the redo is mechanical rather than a re-design.
  - DONE as far as THIS section is concerned, 2026-08-18: §12 cleared the guard tactically without waiting for §7's permanent retirement, and `01-boot-platform/TODO-13` §28 is unparked and back in its own file's queue -> XREF: this file §12 (item: "Convert exactly ONE assessed pool to frame-backed storage").
- [ ] The §10 headroom is FULLY CONSUMED again, so the "tactical" reclamation is a recurring need rather than a one-off, and this section should say which it is before §3 lands.
  - MEASURED 2026-08-18 at `d43b73d2b`: `.bss` runs `0x56d000..0x7feea5`, `__kernel_end` page-aligns to `0x7ff000`, leaving **347 bytes** before the guard fires. §11's Notes record `0x6c2000` and ~1272 KiB spare on 2026-07-17; roughly 1.27 MiB was reabsorbed in a month.
  - The failure is cascading rather than proportional, which makes it arrive without warning: shrinking `.rodata` by 859 bytes (or `.text` by 1183) moves every following page-aligned section down one page and clears the guard, so a section either fits with room to spare or fails by a whole page.
  - A second §10-shaped pass would need NEW targets: the three pools this file's conversion table nominated are already converted (`reg_value_pool`, `reg_key_pool` and `s_atoms` are now pointers, 24 bytes apart in `.bss`), so the table is spent and describes finished work.
  - The largest remaining `.bss` consumers, measured from `build/kernel.map`, are `klog_ring` 288000, `devices` 277504, `tasks` 203776, `ctrl_windows` 180736 and `s_recovered` 164000. The table already rules out the first two (pre-PMM, and hot-plug-ISR reachable); the last three are unassessed and are where a second pass would have to look.
  - ANSWERED 2026-08-18: it is recurring, and the tactical route now has its own unblocked owner rather than sitting as a note inside this deferred section -> XREF: this file §12 (item: "Assess the three unassessed candidates against the §10 bar").
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
> - **Reserve for the next pass** -- `ctrl_windows` 180736 then `tasks` 203776, both needing prerequisite work named in the items above; trigger on headroom under one page rather than on a failed build.
> - **Canonical doc:** [docs/infrastructure/kernel-address-space.md](../../docs/infrastructure/kernel-address-space.md).
> - **Scope boundary** -- ONE conversion plus its headroom measurement. The permanent ceiling retirement is §7, the address-space move is §3, and the boot-level degraded-path coverage is filed with the klog owner -> XREF: `02-kernel-core/TODO-04 §15` (item: "Cover the crash-recovery DEGRADED path at boot level").

> **Verified:** 2026-08-18 | commit `ef09a0579` | 6/6 items | build OK | `__kernel_end` 0x7ff000 -> 0x7d7000 (headroom 347 -> 167936 bytes) | 31341 kernel + 17 user tests | smoke matrix 4/4 (kvm 1+2 cpu, tcg 1+2 cpu) | lint 0 errors
> **Accepted:** [M] the boot-level DEGRADED path has no executable coverage -- allocation failure, the overlap route and the crash-region priority fallback all live inside `klog_crash_recover`, which `src/kernel/test/test_*.c` may not call (reason: infra; the `pmm_alloc_fail_next()` injector exists, the reachable CALL SITE does not) -> XREF: `02-kernel-core/TODO-04 §15` (item: "Cover the crash-recovery DEGRADED path at boot level")
> **Accepted:** [M] `pmm_free_contiguous` mutates the frame bitmap and `used_frames` unsynchronized, which is why the pool is not released on the Phase-2 success path (reason: pre-existing, repo-wide) -> XREF: `03-memory-concurrency/TODO-03 §1` (item: "**PMM bitmap SMP locking**" at line 103)
> **Quality reviewed:** 2026-08-18 | Codex 15x (design, test-coverage, adversarial x3, re-adversarial x8, consistency, perf) + kernel-quality-auditor | 5H+14M+3L fixed, 2M accepted-XREF, 1L rejected | scope: kernel-code-quality
---

## OS Comparison

| ⭐  | Feature                           | 🪟 Win11                 | 🐧 Linux                         | 🚀 Impossible OS                                                 |
| --- | --------------------------------- | ------------------------ | -------------------------------- | ---------------------------------------------------------------- |
| 💎  | Kernel in upper canonical half    | ✅ `0xFFFF800000000000`+ | ✅ `0xffffffff80000000` (-2 GiB) | ⚠️ §1 pins `0xffffffff80000000`; §2-§3 move it                   |
| 💎  | Direct physmap of RAM (HHDM)      | ⚠️ PFN db + dynamic PTEs | ✅ `page_offset_base` physmap    | ✅ §2 HHDM (PML4 273-400, 64 TiB); §9 walkers route through it   |
| 💎  | 128 TB user / 128 TB kernel split | ✅ 48-bit split          | ✅ 48-bit split                  | ⚠️ §1 defines the split; §7 retires the ceiling                  |
| 💎  | Per-process address space         | ✅ per-process           | ✅ `mm_struct` per task          | ⚠️ PML4 per task (D01 T10 §8); high-share §6                     |
| 💎  | Kernel/user page-table isolation  | ✅ KVA Shadow            | ✅ KPTI                          | ⬜ Unblocked by §6 (D02 T10 §6)                                  |
| 💎  | KASLR                             | ✅ kernel ASLR           | ✅ KASLR                         | ⬜ Unblocked by §3 (D02 T10 §14)                                 |
| 💎  | SMEP / SMAP clean split           | ✅ enforced              | ✅ enforced                      | ⬜ Unblocked by §6 (D02 T10 §2)                                  |
| 💎  | PCID no-flush ring transitions    | ✅ with KVA Shadow       | ✅ with KPTI                     | ⬜ Unblocked by §6 (D02 T10 §7)                                  |
| 💎  | No hardcoded user ceiling         | ✅ no low ceiling        | ✅ no low ceiling                | ⬜ §7 retires `0x800000`                                         |
| ⭐  | 5-level paging (LA57, 128 PiB)    | ❌ not supported         | ✅ unconditional (6.10+)         | ⬜ Planned -- §8 (surpasses Win11)                               |
| 💎  | Kernel pools dynamic, not static  | ✅ `ExAllocatePool*`     | ✅ slab / `kmem_cache`           | ✅ §10 registry + atom pools, §12 crash-recovery pool (1.41 MiB) |
| ⭐  | Layout as asserted single source  | ⚠️ undocumented publicly | ⚠️ macros + prose, no manifest   | ✅ §1 `memmap.h` + 20-assert gate (live)                         |
| ⭐  | Phys<->virt relations type-split  | ⚠️ single blended macro  | ⚠️ single `__pa`/`__va` pair     | ✅ §1 HHDM vs image, range-checked + rejecting                   |

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
