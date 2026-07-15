---
schema_version: 1
id: higher-half-kernel-relocation
domain: 02-kernel-core
status: draft
title: "TODO-33 -- Higher-Half Kernel Relocation"
---

# TODO-33 -- Higher-Half Kernel Relocation

> **Goal:** Move the kernel image out of low memory into the upper canonical half of the 64-bit virtual address space (the Windows/Linux model), and give user space the entire private lower half via per-process page tables. This permanently eliminates the kernel-BSS-vs-`USER_BASE` collision class, retires the hardcoded `0x800000` user ceiling, and is the foundation that unblocks KASLR, a clean SMEP/SMAP split, and KPTI.

> [!IMPORTANT]
> **Current state:** The kernel links and loads **low** -- `readelf -l build/kernel.exe` shows the first `LOAD` at virtual `0x100000` (1 MiB), and the image grows upward. User-mode ELF is **pinned at `0x800000`** (8 MiB) by `include/kernel/mm/user_range.h` (single source of truth; included by `vmm.c`, `pmm.c`, `task.c`), with a guard page at `0x900000` and a manual `user/user.ld` mirror. Because kernel and user share one low layout, kernel static growth (`.bss`) presses up against `0x800000`; `scripts/build.sh` has a hard guard (`✗ BSS COLLISION` when BSS end >= `USER_BASE`). As of 2026-07-15 the ceiling is REACHED: the kernel `.bss` ends at `0x7fee58`, and `__kernel_end` page-aligns to `0x7ff000` -- the LAST page before `USER_BASE`, i.e. **zero pages of headroom**. Measured during TODO-22 s22: adding ~600 bytes of `.text` pushed `.rodata`/`.data`/`.bss` each up one page (they are page-aligned and chain), landing `__kernel_end` exactly on `0x800000` and tripping the guard; that section only shipped by consolidating redundant tests to claw back 609 bytes of `.text`. The next section that adds code WILL fail the guard. This is no longer a medium-term cleanup -- it blocks kernel growth now. **CONFIRMED 2026-07-15: the prediction landed.** TODO-22 §23 (live-environment adoption, ~4 KiB of new `.text` in `env.c`) tripped the guard on its first build -- BSS end exactly `0x800000` -- and is now deferred `[/]` on this TODO; a clean HEAD build immediately before it passed, so the section's own code was the entire delta. §23 is the first section this TODO has actually stalled, and every kernel section behind it is in the same position: this TODO is now the critical path for the kernel queue, not a parallel track. Per-process page tables already exist (`01-boot-platform/TODO-10 §8`: each task has its own PML4, CR3 switches on context switch), but the kernel itself is still mapped low and shared into every address space, so per-process isolation does not relieve the low-memory contention. SMEP/SMAP are blocked because the boot PML4 carries the User bit on all kernel 2 MiB pages.

> [!WARNING]
> **Foundational -- riskiest paging change in the system.** It touches the linker, `entry.asm`, boot page-table bring-up, descriptor tables, the bootloader handoff ABI, and the per-process memory model. §1 MUST pass `codex-design-review` before §2 starts. Sequence the sections strictly; each must be independently bootable on KVM/TCG before the next begins, or it is not done.

> [!IMPORTANT]
> **Runner autonomy + bare-metal sign-off policy (applies to EVERY section below).** The overnight runner DOES implement this TODO autonomously. For each section: implement it, then **verify on QEMU KVM + TCG** (`scripts/test-smoke.sh` boots to `C:\>` + the relevant unit tests pass). That KVM/TCG-green result IS the runner's "Verified" -- mark the section `[x]` with the Verified (KVM/TCG) + Quality-reviewed stamps and advance. **Bare-metal verification is a DEFERRED human sign-off, NOT an implementation prerequisite.** Wherever a section's Test checkpoint says "bare metal", that is the deferred sign-off: file it as a one-line item in [`overnight-todo.md`](../../overnight-todo.md) under "Waiting on a human answer" (e.g. "TODO-33 §3 higher-half bring-up: bare-metal SB-chain + early-paging sign-off") and continue -- do not block, defer, or stop the section on the absence of bare metal. **Risk accepted by the operator (2026-06-21):** a real-hardware-only paging bug may pass KVM/TCG and ship as green until the deferred bare-metal pass; the per-section sign-off items are the backstop. If a section cannot reach KVM/TCG-green (not merely bare-metal-unverified), THAT is a real blocker -- defer it `[/]` + Deferred + XREF as usual.

## Inputs

- [`include/kernel/mm/user_range.h`](../../include/kernel/mm/user_range.h) -- the `0x800000`/`0x900000` constants this TODO retires (§7)
- [`user/user.ld`](../../user/user.ld) -- user ELF base, manually mirrored from `user_range.h`
- [`scripts/build.sh`](../../scripts/build.sh) -- BSS-collision guard (`USER_BASE` check) + EIF base
- [`scripts/build-eif.py`](../../scripts/build-eif.py) -- `USER_ELF_BASE = 0x800000`
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- bootloader: loads kernel ELF, sets up identity map, hands off `boot_info`
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) -- handoff ABI (pointers translated across the high-half switch; `BOOT_INFO_VERSION`)
- the kernel linker script + `entry.asm` (kernel virtual base, early paging) -- confirm exact paths at implementation time
- -> XREF: `01-boot-platform/TODO-10-bare-metal-hardening.md §8` -- per-process page tables (the base §6 builds on; STRUCTURAL dependency)
- -> XREF: `01-boot-platform/TODO-01-boot-protocol-abi-handoff.md §8` -- boot protocol schema changelog (records the high-half handoff ABI change)

## Outcome

- Kernel text/data/bss live in the upper canonical half (Windows-style `0xFFFF800000000000` region / Linux-style `0xffffffff80000000`), mapped shared into every process.
- User space owns the entire private lower half; the `0x800000` ceiling, the `user_range.h` constants, the `0x900000` guard, and the `scripts/build.sh` BSS-collision guard are retired.
- A direct/physical map lets the kernel reach physical memory at a fixed high offset (HHDM-style) without identity-mapping low memory.
- KASLR, SMEP/SMAP, and KPTI are unblocked (owned by `03-memory-concurrency/TODO-02`); this TODO is their prerequisite.

## Implementation Order

| ⭐   | Order | Deliverable                                             | Depends On     | Status |
| --- | :---: | ------------------------------------------------------- | -------------- | :----: |
| 💎   |   1   | Memory-map design + canonical layout decision           | --             |  [ ]   |
| 💎   |   2   | Linker VMA/LMA split (kernel high virtual base)         | §1             |  [ ]   |
| 💎   |   3   | `entry.asm` higher-half bring-up + direct map           | §1, §2         |  [ ]   |
| 💎   |   4   | Descriptor tables + per-CPU at high addresses + AP path | §3             |  [ ]   |
| 💎   |   5   | Bootloader / `boot_info` / framebuffer high handoff     | §3, D01 T01 §8 |  [ ]   |
| 💎   |   6   | Per-process PML4: kernel high shared, user low private  | §3, D01 T10 §8 |  [ ]   |
| 💎   |   7   | Retire `0x800000` USER_BASE ceiling + BSS guard         | §6             |  [ ]   |
| ⭐   |   8   | 5-level paging (LA57) support -- exceeds Win11          | §1, §3         |  [ ]   |

> 💎 = parity work -- matches the Windows 11 and Linux memory model.
> ⭐ = exclusive work -- LA57 5-level paging is supported by Linux but **not** Windows; Impossible OS can surpass Win11 here.

---

## 1. Memory-Map Design + Canonical Layout Decision

Pin the target virtual layout BEFORE touching code. This section produces a design doc + constants, not runtime behavior; it is `codex-design-review` gated.

- [ ] Choose `KERNEL_VIRT_BASE`. **Default: Linux-style `0xffffffff80000000` + `-mcmodel=kernel`** (direct clang/ld.lld support, smallest correct change); Windows-style high-canonical only if `codex-design-review` finds a blocker. Record rationale.
- [ ] Define the full canonical map in one header (`include/kernel/mm/memmap.h`): kernel image window, direct/physical map base + extent, fixmap/MMIO window, per-CPU window, user lower-half extent.
- [ ] Decide the physical→virtual scheme (fixed direct-map offset / HHDM) so the kernel reaches physical memory without identity-mapping low memory after the switch.
- [ ] Document the transition plan: transient identity map during the jump, then its teardown; canonical-address (sign-extension) constraints for the chosen base.
- [ ] Write `docs/infrastructure/kernel-address-space.md` (the canonical layout reference) and run `codex-design-review` on it before any later section starts.
- [ ] Commit: `"docs: kernel address-space design -- higher-half layout + direct map"`

**Test checkpoint:** Design doc exists and passes `codex-design-review` with no unresolved High findings; `KERNEL_VIRT_BASE` and the direct-map offset are canonical (bits 48-63 sign-extended). No runtime change yet. Test on: N/A (design section).

## 2. Linker VMA/LMA Split

Relink the kernel at the high virtual base while still loading at the physical address the bootloader places it at.

- [ ] Set the kernel linker script VMA to `KERNEL_VIRT_BASE`; keep LMA at the physical load address (`AT(...)`), so symbols resolve high but the image loads low.
- [ ] Build with `-mcmodel=kernel` (or PIE if chosen in §1); resolve any absolute-address assumptions and relocation-type (`R_X86_64_*`) errors.
- [ ] Update any tool that reads the kernel ELF (bootloader ELF parser in `bootx64.c`, `scripts/build.sh` BSS check) to handle high VMA + low LMA.
- [ ] Confirm `readelf -l` shows kernel `LOAD` at the high virtual base with the physical address preserved.
- [ ] Commit: `"build: kernel linker VMA/LMA split -- high virtual base, low load address"`

**Test checkpoint:** `bash scripts/build.sh clean` -> `=== BUILD OK ===`; `readelf -l build/kernel.exe` shows VirtAddr `KERNEL_VIRT_BASE`, PhysAddr at the load address. (Kernel does not boot yet -- §3 establishes the mapping.) Test on: WSL TCG (build only).

> [!WARNING]
> **Regression risk:** After this section the kernel links high but nothing maps it there yet, so it will NOT boot until §3 lands. §2 and §3 must ship together or §2 stays on a branch. Rollback: revert the linker VMA change to the identity base.

## 3. `entry.asm` Higher-Half Bring-Up + Direct Map

Build the boot page tables that map the kernel high and the physical memory it needs, jump into the high half, then drop the transient identity map. Boot-path; POST16 codes assigned from a free block (verify against `boot_init.h`).

- [ ] In early boot, build page tables that map: (a) the kernel image physical pages -> `KERNEL_VIRT_BASE`, (b) the direct/physical map window, (c) a **transient** identity map of the current EIP region so the `mov cr3` does not fault mid-stream.
- [ ] Enable the new CR3, then far-jump/`lretq` to a label resolved at the high virtual address (the canonical higher-half handoff).
- [ ] After the jump, switch the stack to a high-virtual address and tear down the transient identity map (or hand a clean kernel PML4 to the first task).
- [ ] Replace any post-switch physical-memory access with direct-map (`phys_to_virt`) accessors; update `vmm` phys<->virt helpers to the fixed offset.
- [ ] Add `POST16` entry/exit codes around CR3-enable, the high-half jump, and identity-map teardown.
- [ ] Commit: `"boot: higher-half page-table bring-up + direct map + high-half jump"`

**Test checkpoint:** Serial shows the high-half POST16 sequence in order; kernel reaches its existing Phase-1 banner from a high virtual RIP (`llvm-addr2line` on a logged RIP resolves to a `KERNEL_VIRT_BASE` address). `=== BUILD OK ===` and the smoke test boots to `C:\>`. Test on: QEMU WHPX + TCG; **bare metal (mandatory -- early paging differs on real CPUs)**.

> [!WARNING]
> **Regression risk:** Highest-risk section. A wrong transient identity map or a non-canonical base triple-faults at the CR3 load with no serial output. Rollback: revert to the identity-mapped low kernel (§2+§3 as a unit). Keep `scripts/test-smoke.sh` green at every step.

## 4. Descriptor Tables + Per-CPU at High Addresses + AP Path

Move the GDT, IDT, TSS, and per-CPU/`GS_BASE` state to high virtual addresses, and route SMP AP startup through the high-half transition.

- [ ] Relocate GDT/IDT/TSS pointers (`lgdt`/`lidt`/`ltr`) to high-virtual addresses after the §3 switch; verify the GDT user-segment order (SYSRET) constraint still holds (`gdt.h` static asserts).
- [ ] Set `GS_BASE`/`KERNEL_GS_BASE` and per-CPU data to high-virtual addresses before any interrupt fires (bare-metal-gotcha: GS_BASE before first interrupt).
- [ ] AP trampoline: APs start in the low real/protected-mode trampoline, then load the shared kernel page tables and far-jump into the high half (mirror §3 for each AP).
- [ ] Re-point the IST stacks (DF/NMI/MCE) and their guard pages to high-virtual addresses.
- [ ] Add `POST16` codes around AP high-half entry.
- [ ] Commit: `"boot: descriptor tables + per-CPU + AP startup in the higher half"`

**Test checkpoint:** All CPUs reach the scheduler from high-virtual RIPs; an intentional #DF still shows the guard-page label. Serial shows per-AP high-half POST16. Test on: QEMU WHPX (2 CPUs) + TCG; **bare metal**.

## 5. Bootloader / `boot_info` / Framebuffer High Handoff

The bootloader runs identity-mapped and hands the kernel physical/low pointers; translate them across the switch and record the ABI change.

- [ ] Audit every `boot_info` pointer (memory map, ACPI tables, framebuffer base, command line, initrd/UKI sections) for physical vs virtual; access them via the direct map after §3.
- [ ] Decide framebuffer mapping: map the GOP framebuffer into the kernel's high MMIO window (WC) rather than touching its physical address directly.
- [ ] Bump `BOOT_INFO_VERSION` if the handoff contract changes (mirror header + kernel header together per the boot_info ABI rules); update the drift manifest.
- [ ] Record the high-half handoff in the boot protocol schema changelog (`D01 T01 §8`).
- [ ] Commit: `"boot: boot_info + framebuffer handoff translated for higher-half kernel"`

**Test checkpoint:** Desktop renders (framebuffer reachable via the high MMIO window); ACPI tables parse; `boot_info` validation passes. `boot-info-manifest` compare gate passes. Test on: QEMU WHPX + TCG; **bare metal** (real GOP framebuffer).

## 6. Per-Process PML4: Kernel High Shared, User Low Private

Layer the high-half kernel onto the existing per-process page tables: every process PML4 shares the kernel's upper-half entries and owns a private lower half.

- [ ] On PML4 creation (`TODO-10 §8` path), copy/alias the kernel's upper-half PML4 entries into every new process PML4 (shared, supervisor-only).
- [ ] User mappings go in the lower half only; remove the assumption that user pages live just above the kernel.
- [ ] Ensure kernel upper-half entries are marked supervisor (no User bit) -- this is the clean split that unblocks SMEP/SMAP and KPTI.
- [ ] Verify TLB/CR3 switch semantics: switching to a user process keeps the kernel mapped (high half) while swapping the lower half.
- [ ] Commit: `"mm: per-process PML4 -- kernel high-half shared, user low-half private"`

**Test checkpoint:** `cmd.exe` and a second user process run in separate PML4s, each with a private lower half and the shared kernel high half; a user pointer in process A is not valid in process B. Test on: QEMU WHPX + TCG; **bare metal**.

> [!NOTE]
> **Unblocks:** with the kernel high (supervisor-only) and user low (user-only), `03-memory-concurrency/TODO-02 §4/§5` (SMEP/SMAP) and `§6` (KPTI) become implementable. This section is their structural prerequisite.

## 7. Retire the `0x800000` USER_BASE Ceiling + BSS Guard

With user space owning the lower half, drop the hardcoded ceiling and all the bookkeeping that defended it.

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

## 8. 5-Level Paging (LA57) Support -- Exceeds Win11

Optional competitive edge: support 57-bit virtual addresses on capable hardware. Linux made LA57 unconditional in 6.10; Windows does not support 5-level paging at all.

> [!TIP]
> Supporting LA57 lets Impossible OS address 128 PiB of virtual space and surpass Windows 11, which is stuck at 48-bit (256 TiB). Gate on `CPUID` (LA57 leaf) + `CR4.LA57`; fall back to 4-level cleanly.

- [ ] Detect LA57 via `CPUID` and (optionally) honor a `boot.conf` opt-in; default to 4-level if absent.
- [ ] Parameterize the page-table walk and `KERNEL_VIRT_BASE`/direct-map for 5-level (sign extension at bit 56 vs 47).
- [ ] Boot identically on 4-level and 5-level hosts; log the active paging mode.
- [ ] Commit: `"mm: optional 5-level paging (LA57) -- 57-bit address space on capable CPUs"`

**Test checkpoint:** On a 4-level host the kernel boots in 4-level mode; on an LA57-capable host (or QEMU `-cpu ...,la57=on`) it boots in 5-level mode. Serial logs the active mode. Test on: QEMU TCG (la57 toggled); **bare metal (Arrow Lake / Zen 5)**.

---

## OS Comparison

| ⭐   | Feature                           | 🪟 Win11                 | 🐧 Linux                         | 🚀 Impossible OS                              |
| --- | --------------------------------- | ----------------------- | ------------------------------- | -------------------------------------------- |
| 💎   | Kernel in upper canonical half    | ✅ `0xFFFF800000000000`+ | ✅ `0xffffffff80000000` (-2 GiB) | ⬜ Planned -- §1-§3 (now low `0x100000`)      |
| 💎   | 128 TB user / 128 TB kernel split | ✅ 48-bit split          | ✅ 48-bit split                  | ⬜ Planned -- §1, §7                          |
| 💎   | Per-process address space         | ✅ per-process           | ✅ `mm_struct` per task          | ⚠️ PML4 per task (D01 T10 §8); high-share §6 |
| 💎   | Kernel/user page-table isolation  | ✅ KVA Shadow            | ✅ KPTI                          | ⬜ Unblocked by §6 (D03 T02 §6)               |
| 💎   | KASLR                             | ✅ kernel ASLR           | ✅ KASLR                         | ⬜ Unblocked by §3 (D03 T02 §2)               |
| 💎   | SMEP / SMAP clean split           | ✅ enforced              | ✅ enforced                      | ⬜ Unblocked by §6 (D03 T02 §4/§5)            |
| 💎   | No hardcoded user ceiling         | ✅ no low ceiling        | ✅ no low ceiling                | ⬜ §7 retires `0x800000`                      |
| ⭐   | 5-level paging (LA57, 128 PiB)    | ❌ not supported         | ✅ unconditional (6.10+)         | ⬜ Planned -- §8 (surpasses Win11)            |

> **After §1-§7:** Impossible OS matches the Windows 11 / Linux memory model -- higher-half kernel, private per-process lower half, and the security split that KASLR / SMEP / SMAP / KPTI build on.
> **After §8:** Impossible OS exceeds Windows 11, which has no 5-level paging support.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_highhalf()` -- register in `src/kernel/test/test_runner.c`. Use `TEST_CAT_MM` (memory management).

- [ ] Create `src/kernel/test/test_highhalf.c` with:
  - `phys_to_virt(p)` / `virt_to_phys(v)` round-trip equals the original for a sample physical page.
  - A kernel symbol address (e.g. `&kmain`) is `>= KERNEL_VIRT_BASE` (canonical higher half).
  - `KERNEL_VIRT_BASE` and the direct-map offset are canonical (bits 48-63 all 1, or bit-56 sign-extended under LA57).
  - A freshly created process PML4 shares the kernel upper-half entries and has an empty lower half (post-§6).
  - The retired `user_range.h` ceiling is gone or set to the new low base (post-§7).
- [ ] Register in `test_runner_init()`: `test_register_highhalf()`
- [ ] Author the matching runner bat: `scripts/debug/kernel/run-mm-tests.bat` (extend the existing MM bat; no new category needed).
- [ ] Commit: `"test: add higher-half kernel address-space test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===` (BSS-collision guard removed after §7).
- [ ] `readelf -l build/kernel.exe` shows kernel `LOAD` VirtAddr at `KERNEL_VIRT_BASE`, PhysAddr at the load address.
- [ ] Boot serial shows the higher-half POST16 sequence in order; a logged kernel RIP resolves (via `llvm-addr2line-19 -e build/kernel.exe`) to a high-virtual address.
- [ ] `scripts/test-smoke.sh` boots to `C:\>` with the kernel running high-half.
- [ ] Two user processes run in separate PML4s with private lower halves + shared kernel high half.
- [ ] `make test-mm` shows all PASS including the new higher-half suite.
- [ ] Runner bar: QEMU KVM + TCG green per section (smoke + unit tests). Bare-metal sign-off is DEFERRED -- file per-section items in `overnight-todo.md` (see the Runner-autonomy policy above).
- [ ] Commit: `"mm: higher-half kernel relocation -- complete"`
