# TODO-02 — Memory Security Hardening

> **Goal:** Harden the process and kernel address space against the full class of modern exploit techniques — ASLR and KASLR eliminate fixed-address attacks, SMEP/SMAP cut off ret2user and user-data pivots, NX/DEP enforces execute-never on all data pages, KPTI mitigates Meltdown-class side-channel leaks, CET shadow stacks protect return addresses, and a security layout report exposes the current state to diagnostics and the registry.

> [!IMPORTANT]
> **Boot ordering contract:** EFER.NXE, CR4.SMEP, and CR4.SMAP are activated during Phase 0 of CPU bring-up (→ XREF `01-boot-platform/TODO-04-cpu-boot-sequencing.md §2`). Sections §3–§5 of this TODO add the surrounding infrastructure (wrappers, audit, test coverage) that makes those bits effective — they do not move the activation point. Implement §3–§5 only after TODO-04-cpu §2 is done.

## Inputs

- [`include/kernel/mm/vmm.h`](../../include/kernel/mm/vmm.h)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c)
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c)
- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §1` — CPUID capability capture needed before §3, §4, §7
- → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §2` — EFER.NXE, CR4.SMEP, CR4.SMAP activation window; §3–§5 extend that work
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §2` — W^X enforcement; §5 NX/DEP and W^X are complementary layers
- → XREF: `02-kernel-core/TODO-04-peb-teb-user-abi.md §3` — `swapgs` on syscall entry/exit; §4 `stac`/`clac` framing must nest correctly inside syscall entry
- → XREF: `02-kernel-core/TODO-13-registry-completion.md` — `HKLM\SYSTEM\Security\*` keys written in §8

## Outcome

- Every user process has a unique stack, heap, and mmap base at each launch; ASLR cannot be disabled except via Registry debug key.
- The kernel loads at a different virtual base on each boot; a single kernel pointer leak cannot predict layout across reboots.
- CR4.SMEP prevents the kernel from executing any user-space page; any attempt faults immediately.
- CR4.SMAP prevents accidental kernel reads/writes of user memory; `copy_from_user` / `copy_to_user` are the only safe crossing points.
- All kernel data pages carry the NX (No-Execute) bit; no data page is executable under any mapping path.
- User-mode page tables carry only the syscall entry trampoline, severing the kernel address leak exploited by Meltdown.
- CET shadow stack is probed at boot; if hardware supports it the feature is enabled and `NtSetInformationThread(ThreadEnableShadowStack)` is stubbed in.
- `HKLM\SYSTEM\Security\LastASLRSeed` is written at each boot; `NtQueryVirtualMemory(MemoryImageInformation)` returns the process ASLR delta.

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On                          | Status |
| --- | :---: | ---------------------------------------------- | ----------------------------------- | :----: |
| 💎  |   1   | §5 NX/DEP — data mapping audit + enforce       | TODO-04-cpu §2, TODO-01 §2          |  [ ]   |
| 💎  |   2   | §3 SMEP — enable + test coverage               | TODO-04-cpu §1, §2                  |  [ ]   |
| 💎  |   3   | §4 SMAP + `copy_from_user` / `copy_to_user`    | §3 (CR4 baseline), TODO-04 §3       |  [ ]   |
| ⭐  |   4   | §1 User-space ASLR (mandatory for all procs)   | TODO-01 §3 demand paging            |  [ ]   |
| 💎  |   5   | §2 KASLR — bootloader offset + kernel reloc    | —                                   |  [ ]   |
| 💎  |   6   | §6 KPTI basic — per-mode page table split      | §2, VMM page table work             |  [ ]   |
| 💎  |   7   | §7 CET shadow stack stub                       | §3, §4 (CR4 baseline)               |  [ ]   |
| ⭐  |   8   | §8 Security layout report                     | §1, §2                              |  [ ]   |

> 💎 = parity — Windows and Linux both implement SMEP/SMAP, KASLR, NX/DEP, KPTI, and CET; Impossible OS must match.
> ⭐ = exclusive — ASLR mandatory for every process (no per-binary opt-in) and the structured security layout report are not surfaced the same way on Windows or Linux.

---

## 1. User-Space ASLR `[Opus]`

Randomize stack, heap, and mmap base addresses for every user process at launch — always on, no per-binary opt-in. Seed from `RDRAND ^ PIT ticks` so the entropy is hardware-backed and unpredictable across reboots.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/sched/task.c`

> [!IMPORTANT]
> Windows ASLR is opt-in per PE binary (`IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE`). Impossible OS enforces ASLR unconditionally for all user processes — no legacy escape hatch. The Registry key is a debug-only override, not a production disable.

- [ ] Add `g_aslr_enabled` flag (read from `HKLM\SYSTEM\Security\ASLR` at boot, default 1)
- [ ] Generate per-boot `g_aslr_seed`: `RDRAND ^ PIT ticks` at kernel init; fallback to TSC if RDRAND unavailable (CPUID leaf 1 ECX bit 30)
- [ ] On process creation: derive per-process delta from `g_aslr_seed ^ pid ^ spawn_tsc`
- [ ] Randomize user stack base: page-aligned offset of 1–255 pages from canonical stack top; pass to `vmm_alloc_stack()`
- [ ] Randomize heap (`brk`) start: offset 1–63 pages from end of BSS
- [ ] Randomize mmap region base: `mmap_base = DEFAULT_MMAP_BASE ^ (per_proc_delta & MMAP_OFFSET_MASK)`, page-aligned
- [ ] All offsets must be page-aligned; log chosen bases to serial at process spawn (debug builds only)
- [ ] Commit: `"mm: user-space ASLR — mandatory for all processes, RDRAND seed"`

## 2. KASLR — Kernel Address Space Randomization `[Opus]`

Randomize the kernel's virtual load base at each boot so a kernel pointer leak cannot predict layout across reboots. The bootloader picks a 2 MiB-aligned physical offset via RDRAND and passes it through `boot_info`; the kernel adjusts its page table base accordingly.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`, `src/kernel/mm/vmm.c`

> [!IMPORTANT]
> This requires the kernel to be position-independent at the physical level. The current build uses `-mcmodel=kernel` with a fixed virtual base. Audit whether relocation records in the kernel ELF are sufficient for a randomized physical offset, or whether the linker script needs adjustment. Design carefully before touching the linker script — a mistake here produces a kernel that triple-faults with no serial output.

- [ ] Bootloader: call `RDRAND` for a 64-bit value; mask to 2 MiB alignment; clamp to safe physical range (above 4 MiB, below top-of-RAM minus kernel size)
- [ ] Store `kernel_phys_offset` in `boot_info`; pass unchanged through `kernel_main()` entry
- [ ] Bootloader: load kernel ELF segments at `segment_paddr + kernel_phys_offset` instead of fixed `segment_paddr`
- [ ] Kernel: adjust page table mapping so kernel virtual range `[KERNEL_VIRT_BASE, ...]` maps to the randomized physical range
- [ ] Kernel: apply ELF relocation entries (`.rela.dyn` / `R_X86_64_RELATIVE`) at the randomized base to fix up absolute pointers
- [ ] Boot log: `[KASLR] kernel phys base = 0x%llx (offset +0x%llx)` (debug builds only)
- [ ] Write `HKLM\SYSTEM\Security\KASLR` = 1 after Registry is ready
- [ ] Commit: `"mm: KASLR — kernel address randomization via RDRAND bootloader offset"`

## 3. SMEP `[Opus]`

Supervisor Mode Execution Prevention: the CPU faults if the kernel attempts to execute a page with `U/S=1` (user-accessible), blocking ret2user exploit chains.

**Files:** `src/kernel/main/boot_hw.c`, `include/kernel/cpuid.h`

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §2` — CR4.SMEP is set in the Phase 0 activation window defined there. This section confirms the CPUID probe and adds test coverage; do not move the CR4 write out of that ordering.

- [ ] Probe CPUID leaf 7 subleaf 0 EBX bit 7 for SMEP support; store result in `cpu_features.smep`
- [ ] In `cpu_cr4_harden()` (Phase 0): set CR4 bit 20 (`CR4.SMEP`) if `cpu_features.smep`; skip silently on VMs without it
- [ ] Boot log: `[CPU] SMEP: enabled` or `[CPU] SMEP: not supported (hypervisor or old CPU)`
- [ ] Verify: attempt to call a user-space address from kernel context → confirm `#PF` with error-code bit 4 (`PFEC.I=1`) is raised
- [ ] Apply same SMEP enable in `ap_cpu_harden()` for every AP (→ XREF `01-boot-platform/TODO-04-cpu-boot-sequencing.md §4`)
- [ ] Commit: `"mm: SMEP — set CR4.SMEP, boot log, AP parity"`

## 4. SMAP + `copy_from_user` / `copy_to_user` `[Opus]`

Supervisor Mode Access Prevention: the CPU faults on any kernel access to a user-space page unless the kernel explicitly sets `RFLAGS.AC` (`stac`/`clac`). This forces all legitimate user-data reads and writes to go through audited wrappers.

**Files:** `include/kernel/mm/uaccess.h` (new), `src/kernel/mm/uaccess.c` (new), `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> `stac` / `clac` must be paired precisely: `stac` before the user-pointer access, `clac` immediately after. Any code path that returns without `clac` (error, early exit) leaves SMAP disabled for the rest of the kernel's execution on that CPU. Use a cleanup-block or `__attribute__((cleanup))` guard where early returns are possible.
> → XREF: `02-kernel-core/TODO-04-peb-teb-user-abi.md §3` — `swapgs` framing in syscall entry; ensure `stac`/`clac` are inside the post-`swapgs` window.

- [ ] Probe CPUID leaf 7 subleaf 0 EBX bit 20 for SMAP support; store in `cpu_features.smap`
- [ ] In `cpu_cr4_harden()` (Phase 0): set CR4 bit 21 (`CR4.SMAP`) if `cpu_features.smap`
- [ ] Boot log: `[CPU] SMAP: enabled` or `[CPU] SMAP: not supported`
- [ ] Create `include/kernel/mm/uaccess.h`: declare `copy_from_user(kern_dst, user_src, len)` and `copy_to_user(user_dst, kern_src, len)` with inline `stac` / `clac` guards; return bytes copied or -1 on fault
- [ ] Page fault handler: detect SMAP violation (`error_code & PFEC_SMAP`, bit 5 set, U=0, W=access type) → `PANIC("SMAP violation at 0x%lx from 0x%lx")`
- [ ] Audit all syscall handlers in `syscall.c`: replace raw user-pointer dereference with `copy_from_user()` / `copy_to_user()`
- [ ] Apply CR4.SMAP in `ap_cpu_harden()` for every AP
- [ ] Commit: `"mm: SMAP — CR4.SMAP + copy_from_user / copy_to_user wrappers, syscall audit"`

## 5. NX / DEP Mapping Audit `[Opus]`

Confirm `EFER.NXE` is set and audit every kernel and user data mapping to ensure the NX (No-Execute) bit is applied — no data page should ever be executable.

**Files:** `src/kernel/mm/vmm.c`, `src/kernel/main/boot_hw.c`

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §2` — EFER.NXE is set in Phase 0 there. This section confirms coverage and audits all mapping paths; do not duplicate the EFER write.
> → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §2` — W^X enforcement rejects write+exec at mprotect time; NX/DEP is the PTE-level enforcement of the X side.

- [ ] Confirm `EFER.NXE` read-back after Phase 0: `rdmsr(IA32_EFER) & EFER_NXE` → assert set; `PANIC` if clear (CPU does not support NX)
- [ ] Audit `vmm_map_pages()` and all direct PTE-write paths: data mappings (stack, heap, `MEM_COMMIT` anonymous, mmap data) must always set the NX bit; only `.text` / exec regions must have NX clear
- [ ] Kernel BSS, heap, and stack pages: confirm NX bit set in the identity-map and the kernel virtual range
- [ ] UEFI runtime data pages: confirm NX set after `call_set_virtual_address_map()` (→ XREF `01-boot-platform/TODO-01-uefi-hardening-secureboot.md §11`)
- [ ] Boot log: `[NX] EFER.NXE confirmed; all data mappings carry NX`
- [ ] Commit: `"mm: NX/DEP audit — confirm EFER.NXE, NX on all data pages"`

## 6. KPTI Basic `[Opus]`

Maintain a minimal user-visible page table that maps only the syscall entry trampoline, severing the kernel virtual address space from user-mode speculation — the kernel-side mitigation for Meltdown-class side-channel leaks.

**Files:** `src/kernel/mm/vmm.c`, `src/kernel/sched/entry.asm`, `include/kernel/mm/vmm.h`

> [!IMPORTANT]
> KPTI requires maintaining two PML4 roots per process: a full kernel PML4 and a shadow user PML4 with only the trampoline mapped. CR3 is swapped at each syscall entry and return. This interacts with KASLR (§2) — the trampoline virtual address must be the same in both PML4s to avoid a double-fault at the CR3 switch. Design the trampoline placement before starting implementation.

- [ ] Define a fixed trampoline virtual address in a canonical slot (e.g., top of user address space, one page): `KPTI_TRAMPOLINE_VIRT`
- [ ] On process creation: allocate a shadow `user_pml4` with only the trampoline page mapped (`R-X`, kernel-only)
- [ ] Syscall entry (before `swapgs`): swap `CR3` from `user_pml4` to `kernel_pml4`; trampoline page handles the transition
- [ ] Syscall return (`sysretq` path): swap `CR3` back to `user_pml4`
- [ ] `PCID` optimization: if `cpu_features.pcid` (→ XREF `01-boot-platform/TODO-04-cpu-boot-sequencing.md §5`), assign separate PCIDs to user and kernel PML4 to avoid TLB flush on each CR3 swap
- [ ] Boot log: `[KPTI] active — user PML4 contains trampoline only`
- [ ] Commit: `"mm: KPTI basic — per-mode PML4 + trampoline CR3 swap at syscall boundary"`

## 7. CET Shadow Stack Stub `[Opus]`

Probe for Intel CET (Control-flow Enforcement Technology) shadow stacks; if the hardware supports it, enable the feature and stub in the `NtSetInformationThread` hook so user-mode code can opt in.

**Files:** `src/kernel/main/boot_hw.c`, `include/kernel/cpuid.h`, `src/kernel/sched/syscall.c`

- [ ] Probe CPUID leaf 7 subleaf 0 ECX bit 7 (`CET_SS`) for shadow stack support; store in `cpu_features.cet_ss`
- [ ] If `cpu_features.cet_ss`: set `CR4.CET` (bit 23); write `MSR_IA32_U_CET` (0x6A0) to enable user-mode shadow stacks (`CET_U_ENDBR_EN | CET_U_SHSTK_EN`)
- [ ] Allocate a shadow stack page per thread in `thread_create()` if CET is active; store shadow stack pointer in thread struct
- [ ] Stub `NtSetInformationThread(handle, ThreadEnableShadowStack, &enabled, sizeof(enabled))` → set/clear CET shadow stack for the target thread (→ XREF `02-kernel-core/TODO-05-native-api-layer.md`)
- [ ] Boot log: `[CPU] CET shadow stack: enabled` or `[CPU] CET shadow stack: not supported`
- [ ] Commit: `"mm: CET shadow stack — probe, enable if present, NtSetInformationThread stub"`

## 8. Security Layout Report `[Sonnet]`

Surface the active security configuration to diagnostics, the registry, and user-mode queries so tools, the task manager, and future security policy code can inspect the state.

**Files:** `src/kernel/main/boot_hw.c`, `src/kernel/sched/syscall.c`, `include/kernel/mm/vmm.h`

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-13-registry-completion.md` — all `HKLM\SYSTEM\Security\*` writes require the registry to be initialized; call after `registry_init()` in the boot sequence.

- [ ] Write `HKLM\SYSTEM\Security\ASLREnabled` = 1/0 and `HKLM\SYSTEM\Security\LastASLRSeed` = `g_aslr_seed` after registry is up
- [ ] Write `HKLM\SYSTEM\Security\KASLREnabled` = 1 and `HKLM\SYSTEM\Security\KernelPhysBase` = `kernel_phys_offset` (debug builds only — omit in release)
- [ ] Write `HKLM\SYSTEM\Security\SMEPEnabled`, `SMAPEnabled`, `NXEnabled`, `KPTIEnabled`, `CETEnabled` = 1/0 from `cpu_features`
- [ ] Extend `NtQueryVirtualMemory` (`MemoryImageInformation` class) to return the per-process ASLR delta in `ImageBase` field
- [ ] `secinfo` shell command: print the full security feature table from the registry
- [ ] Serial log at boot: `[SEC] ASLR=1 KASLR=1 SMEP=1 SMAP=1 NX=1 KPTI=1 CET=0` (feature bitmask line)
- [ ] Commit: `"mm: security layout report — registry keys, NtQueryVirtualMemory, secinfo command"`

---

## OS Comparison


| ⭐ | Feature                                     | 🪟 Win11                                                      | 🐧 Linux                                          | 🚀 Impossible OS                                     |
|----|---------------------------------------------|------------------------------------------------------------|------------------------------------------------|---------------------------------------------------|
| ⭐ | User-space ASLR                             | ⚠️ Opt-in per PE (`IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE`) | ✅ Default on all processes; ELF               | ⬜ §1 — mandatory, no per-binary opt-out          |
| 💎 | KASLR — kernel base randomized at each boot | ✅ Default since Vista; random kernel                      | ✅ `CONFIG_RANDOMIZE_BASE`; `kaslr` boot param | ⬜ §2 — RDRAND offset in bootloader               |
| 💎 | SMEP — kernel cannot exec user pages        | ✅ Enabled on supported hardware since                     | ✅ Set in `setup_cr4()` if CPUID               | ⬜ §3 — `cpu_cr4_harden()` + AP parity            |
| 💎 | SMAP + `copy_from_user` wrappers            | ✅ `__readgsqword` + IRQL guards; no                       | ✅ `copy_from_user` with `stac`/`clac` in all  | ⬜ §4 — `copy_from_user` wrappers + syscall audit |
| 💎 | NX / DEP on all data mappings               | ✅ DEP default on; all data                                | ✅ NX on all anonymous +                       | ⬜ §5 — audit all VMM mapping paths               |
| 💎 | KPTI / KVA Shadow                           | ✅ KVA Shadow; auto-enabled on vulnerable                  | ✅ PTI (`CONFIG_PAGE_TABLE_ISOLATION`)         | ⬜ §6 — minimal trampoline PML4, CR3 swap         |
| 💎 | CET shadow stack                            | ✅ Hardware-enforced stack protection (Win10 2004+)        | ✅ Kernel + glibc support since                | ⬜ §7 — probe + enable + `NtSetInformationThread` |
| ⭐ | Structured security layout report at boot   | ❌ No single boot-time security summary                    | ⚠️ `dmesg` grep; no unified security           | ⬜ §8 — `secinfo` command + full registry         |

> **After parity items:** Impossible OS matches Windows and Linux on KASLR, SMEP, SMAP, NX/DEP, KPTI, and CET. User-space ASLR is stronger than Windows — mandatory for all processes rather than opt-in per PE binary. The structured `secinfo` command and boot-time security summary line give operator-visible confirmation that all mitigations are active without digging through `dmesg` or WinDbg.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Serial log shows `[SEC] ASLR=1 KASLR=1 SMEP=1 SMAP=1 NX=1 KPTI=1 CET=0` (QEMU has no CET)
- [ ] Launch two identical processes; `VirtualQuery(stack)` returns different base addresses across both runs — ASLR active
- [ ] KASLR: serial log shows `[KASLR] kernel phys base = 0x...` with different value on each reboot (debug build)
- [ ] SMEP test: kernel attempts to call user-space address → `#PF` with `PFEC.I=1`; kernel logs and panics cleanly
- [ ] SMAP test: kernel dereferences user pointer without `stac` → `#PF` with SMAP error-code bit; `copy_from_user()` succeeds
- [ ] NX test: attempt `JMP` to a data page → `#PF` with NX violation (`PFEC.I=1`, present page)
- [ ] `secinfo` shell command prints all security flags; all keys visible under `HKLM\SYSTEM\Security\`
- [ ] Commit: `"mm: memory security hardening — ASLR, KASLR, SMEP, SMAP, NX, KPTI, CET"`
