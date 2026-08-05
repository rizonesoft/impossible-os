---
schema_version: 1
id: arch-abstraction-layer
domain: 16-architecture-ports
status: active
title: "TODO-01 -- Architecture Abstraction Layer"
---

# TODO-01 -- Architecture Abstraction Layer

> **Goal:** Extract all x86-64-specific code behind a clean HAL (Hardware Abstraction Layer) so the kernel compiles for multiple architectures from a single codebase. Create `arch/x86_64/` and `arch/aarch64/` source trees, move ~15 arch-specific files, define ~20 HAL function prototypes, and add `ARCH=` build system support. After this TODO, the x86-64 kernel builds and boots identically through the new arch/ structure -- zero functional change, pure refactor.

> [!IMPORTANT]
> This is a refactor-only TODO. The x86-64 kernel must boot on all 4 platforms (QEMU WHPX, TCG, VBox, bare metal) identically before and after. No new features, no ARM code yet -- just the structural split.

---

## Inputs

- `src/kernel/` -- current flat structure with arch-specific files mixed in
- `include/kernel/` -- headers with arch-specific types and inline asm
- `Makefile` -- single-arch build
- -> XREF: `02-kernel-core/TODO-31-kernel-bulletproofing.md` -- static asserts on struct offsets must survive the move
- -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md` -- allocator must remain arch-neutral

---

## Outcome

- `src/kernel/arch/x86_64/` contains all x86-64-specific source files (~15 files)
- `include/kernel/arch/x86_64/` contains arch-specific headers
- `include/kernel/arch/hal.h` defines the architecture-neutral interface (~20 functions)
- `Makefile` accepts `ARCH=x86_64` (default) and compiles only the correct arch/ tree
- All existing tests pass unchanged
- Build time unchanged (same number of compilation units)

---

## Implementation Order

| Star | Order | Deliverable                              | Depends On | Status |
| ---- | :---: | ---------------------------------------- | ---------- | :----: |
| 💎    |   1   | Define HAL interface (hal.h)             | --         |  [ ]   |
| 💎    |   2   | Create arch/x86_64/ directory, move files | §1         |  [ ]   |
| 💎    |   3   | Update Makefile for ARCH= variable       | §2         |  [ ]   |
| 💎    |   4   | Update all #include paths                | §2         |  [ ]   |
| 💎    |   5   | Verify 4-platform boot + all tests pass  | §3, §4     |  [ ]   |

---

## 1. Define HAL Interface

Create `include/kernel/arch/hal.h` with architecture-neutral prototypes.

**Files:** `include/kernel/arch/hal.h` (new)

- [ ] `void arch_interrupts_disable(void)` -- cli / msr daifset
- [ ] `void arch_interrupts_enable(void)` -- sti / msr daifclr
- [ ] `void arch_halt(void)` -- hlt / wfi
- [ ] `void arch_tlb_flush_page(uintptr_t addr)` -- invlpg / tlbi
- [ ] `void arch_tlb_flush_all(void)` -- reload CR3 / TLBI VMALLE1
- [ ] `uint64_t arch_read_timestamp(void)` -- rdtsc / cntvct_el0
- [ ] `uint32_t arch_cpu_id(void)` -- LAPIC ID / MPIDR_EL1
- [ ] `void arch_context_switch(struct task *prev, struct task *next)` -- save/restore regs
- [ ] `void arch_syscall_init(void)` -- STAR/LSTAR/FMASK / VBAR_EL1 setup
- [ ] `void arch_smp_init(void)` -- INIT/SIPI / PSCI
- [ ] `void arch_timer_init(void)` -- LAPIC timer / generic timer
- [ ] `void arch_mmu_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags)` -- PTE write
- [ ] `void arch_mmu_unmap_page(uintptr_t virt)` -- PTE clear + TLB flush
- [ ] `uintptr_t arch_mmu_create_address_space(void)` -- new PML4 / new PGD
- [ ] Commit: `"arch: define HAL interface -- 14 arch-neutral prototypes in hal.h"`

**Test checkpoint:** Header compiles. x86-64 implementations are thin wrappers around existing functions.

---

## 2. Create arch/x86_64/ and Move Files

Move x86-64-specific source files from `src/kernel/` to `src/kernel/arch/x86_64/`.

**Files to move:**

- [ ] `gdt.c` -> `arch/x86_64/gdt.c`
- [ ] `idt.c` -> `arch/x86_64/idt.c`
- [ ] `isr_stubs.asm` -> `arch/x86_64/isr_stubs.asm`
- [ ] `cpuid.c` -> `arch/x86_64/cpuid.c`
- [ ] `msr.c` -> `arch/x86_64/msr.c`
- [ ] `cpu_security.c` -> `arch/x86_64/cpu_security.c`
- [ ] `smp/smp.c` -> `arch/x86_64/smp.c`
- [ ] `smp/ap_trampoline.asm` -> `arch/x86_64/ap_trampoline.asm`
- [ ] `drivers/lapic.c` -> `arch/x86_64/lapic.c`
- [ ] `drivers/ioapic.c` -> `arch/x86_64/ioapic.c`
- [ ] `drivers/pit.c` -> `arch/x86_64/pit.c`
- [ ] `sched/syscall_entry.asm` -> `arch/x86_64/syscall_entry.asm`
- [ ] `sched/syscall_fast.c` -> `arch/x86_64/syscall_fast.c`
- [ ] Move corresponding headers to `include/kernel/arch/x86_64/`
- [ ] Commit: `"arch: move 13 x86-64-specific files to arch/x86_64/"`

**Test checkpoint:** Build succeeds. All tests pass. Boot on QEMU WHPX.

---

## 3. Update Makefile

Add `ARCH` variable to build system.

- [ ] `ARCH ?= x86_64` (default)
- [ ] `ARCH_SRCS = $(wildcard src/kernel/arch/$(ARCH)/*.c src/kernel/arch/$(ARCH)/*.asm)`
- [ ] Include `arch/$(ARCH)/` in CFLAGS `-I` path
- [ ] Verify `bash scripts/build.sh` still works without explicit ARCH=
- [ ] Commit: `"arch: add ARCH= build variable (default x86_64)"`

---

## 4. Update Include Paths

Fix all `#include` directives that reference moved files.

- [ ] Grep all source files for old include paths, update to new locations
- [ ] Verify no circular dependencies between arch/ and kernel/
- [ ] Commit: `"arch: update include paths for arch/x86_64/ tree"`

---

## 5. Verify 4-Platform Boot

Full regression test across all platforms.

- [ ] QEMU WHPX: boot to desktop, all tests pass
- [ ] QEMU TCG: boot to desktop, cmd.exe loads
- [ ] VirtualBox: boot to desktop
- [ ] Bare metal: boot to desktop
- [ ] Commit: `"arch: verified 4-platform boot after arch/ split"`

---

## OS Comparison

| ⭐   | Feature                             | 🪟 Win11            | 🐧 Linux                  | 🚀 Impossible OS                     |
| --- | ----------------------------------- | ------------------ | ------------------------ | ----------------------------------- |
| 💎   | Multi-arch source tree              | ✅ HAL + arch/ dirs | ✅ arch/ per architecture | ⬜ §1-§2 -- hal.h + arch/            |
| 💎   | Build-time arch select              | ✅ Build config     | ✅ `ARCH=` make variable  | ⬜ §3 -- `ARCH=` in Makefile         |
| ⭐   | Arch split preserves bulletproofing | ❌ No equivalent    | ❌ No equivalent          | ⬜ §5 -- static asserts survive move |

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===`
- [ ] `ARCH=x86_64 bash scripts/build.sh` -> same result
- [ ] All 200+ unit tests pass
- [ ] 4-platform boot verification
