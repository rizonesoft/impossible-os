# TODO-10 -- Kernel Security Hardening

> **Goal:** Activate every CPU security feature that `cpuid.c` detects where we still lack runtime enforcement: complete SMEP/SMAP (CR4 bits on real hardware), KPTI (separate user/kernel page tables + PCID), Spectre mitigations (IBRS/retpoline/IBPB + swapgs barriers), CET shadow stack, CET indirect branch tracking, and KASLR. Couple this with kernel-side hardening that does not require CPU support: heap magic cookies, redzone detection, stack canaries (`-fstack-protector-strong` + RDRAND-seeded `__stack_chk_guard`), and guard pages below kernel stacks. Without these, Impossible OS is exploitable via any 2018-era hardware vulnerability and trivially attackable by user-mode code; that is unacceptable for a production OS in 2026.

> [!IMPORTANT]
> **Current state (code-truth):** **NX is active:** `cpu_harden()` enables `EFER.NXE` on BSP and APs; `ap_trampoline.asm` sets NXE before paging; `vmm_apply_nx_policy()` in `boot_hw.c` walks page tables and applies PTE NX to non-text mappings. **SMEP/SMAP path exists but CR4 bits are not enabled today:** `hv_supports_cr4_smep_smap()` in `cpu_security.c` returns 0 unconditionally (boot PML4 still has User on kernel 2 MiB pages per `CLAUDE.md`), so `cpu_enable_smep()` / `cpu_enable_smap()` always return early; verify logs show SMEP/SMAP skipped. **`clac` is not emitted on the IDT common stub:** removed in `isr_stubs.asm` to avoid `#UD` on CPUs without SMAP in the test matrix; must return when SMAP is real. **Still absent:** KPTI, PCID use, `MSR_IA32_SPEC_CTRL` program, retpoline thunks, CET, heap cookies, `-fstack-protector-strong` in the kernel `Makefile` (still `-fno-stack-protector`), dedicated kernel stack guard policy beyond existing bulletproofing, KASLR slide. **Also track:** UMIP/PKU and advanced x86 features live under **T09** `TODO-09-x86-64-architecture.md` (see its Inputs XREF). **PKS (supervisor protection keys):** gap-analysis 2026-04-12 defers PKS enablement here; Linux documents it under memory protection keys (`docs.kernel.org/core-api/protection-keys.html`); own alongside SMEP/kernel direct-map policy once **T09** user PKU (§6) and PTE key plumbing exist.

---

## Inputs

- `src/kernel/cpuid.c`: `cpu_has(CPU_FEATURE_*)` detection already complete
- `src/kernel/mm/vmm.c`: `write_cr3`, `vmm_flush_tlb`, PTE format
- `src/kernel/smp/smp.c`: `wrmsr`/`rdmsr` helpers already present
- `src/kernel/smp/ap_trampoline.asm`: AP startup; must enable same CR4/MSR features on every CPU, not only the BSP
- `src/kernel/mm/heap.c`: `kmalloc`/`kfree` (heap hardening §8)
- `include/kernel/idt.h`: `struct interrupt_frame` (syscall/exception entry)
- `scripts/build.sh` / `Makefile`: compiler flag changes for §9 (canaries)
- -> XREF: `TODO-11-peb-teb-user-abi.md §5`: `swapgs` on INT 0x80 entry/exit path; must also emit IBRS save and STAC/CLAC inline ASM for SMAP compliance (KPTI §5 here)
- -> XREF: `TODO-12-native-api-ssdt.md §3`: SYSCALL/SYSRET fast path is where the CR3 swap for KPTI (§5) is inserted and IBRS enable (§6) happens on kernel entry; §4 (INT 0x2E path) also needs the same CR3 swap
- -> XREF: `TODO-15-security-reference-monitor.md §5`: MIC Low-IL processes are the primary beneficiaries of SMEP/SMAP (user code cannot exec kernel pages or read kernel memory)
- -> XREF: `TODO-27-crash-dump-generation.md §1`: `BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE` is the stop code emitted by §9 (`__stack_chk_fail`) and §8 (cookie mismatch)
- -> XREF: `TODO-27-crash-dump-generation.md §10`: crash `.dmp` and `X:\Crash\` directory ACL defaults (`dump_set_default_sd()`), optional `CrashDumpEncryption` registry path: review gates with this TODO before enforcement code ships
- -> XREF: `TODO-11-peb-teb-user-abi.md §13`: AT_RANDOM in the ELF auxv provides user-mode stack canary seed bytes, complementing §3's kernel-side `__stack_chk_guard` via shared RDRAND path
- -> XREF: `TODO-12-native-api-ssdt.md §5`: SSDT indices 0x01F0--0x01F4 and 0x02A2--0x02A4 reserved for Enclave and signing-level syscalls
- -> XREF: `TODO-12-native-api-ssdt.md §26`: SSDT hardware write-protection complements KASLR and SMEP/SMAP; #PF on SSDT write -> CRITICAL_STRUCTURE_CORRUPTION BugCheck
- -> XREF: `TODO-17-binary-system.md §3,§12`: ELF `PT_GNU_PROPERTY` (T17 §3) and PE `IMAGE_LOAD_CONFIG_DIRECTORY64` (T17 §12) carry per-binary CET IBT/SHSTK and CFG flags; this TODO's §7 (CET shadow stack) and §8 (CET IBT) consume those flags to decide enforcement
- -> XREF: `TODO-21-process-model-extensions.md §11`: per-process mitigation flags (`MIT_DEP_ENABLE`, `MIT_ASLR_FORCE`, etc.) consume §1 NX/DEP enforcement; mitigation API surface is authoritative in TODO-21
- -> XREF: `TODO-23-exception-dispatch-seh.md §3`: `#CP` (vector 21, CET shadow-stack violation) exception handler; §6 of this TODO enables CET SS, §3 of T23 routes the resulting `#CP` faults through `ki_dispatch_exception()`
- -> XREF: `01-boot-platform/TODO-02-uefi-hardening-secureboot.md §3`: `boot_info.secure_boot_enabled` / `HKLM\SYSTEM\SecureBoot\State` from UEFI `SecureBoot` variable; future `boot.conf` lockdown knob must be defined in this TODO when implemented (no §13 in TODO-01)
- -> XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §2, §5`: Phase 0 activation order (§2) calls `cpu_efer_harden()`/`cpu_cr4_harden()` from this TODO; AP hardening (§5) replicates the same features on each AP via `ap_cpu_harden()`
- -> XREF: `TODO-31-kernel-bulletproofing.md §10`: guard pages, split huge pages, and VM layout invariants; NX/SMEP/SMAP policy here must stay consistent with those checks
- -> XREF: `TODO-09-x86-64-architecture.md §6`: UMIP and PKU (`CR4.UMIP`, `PKRU`) are scoped there; keep Spectre swapgs/`gs:` sequencing aligned with this TODO and T11 §4

---

## Outcome

- NX bit is set on all non-code PTE entries; IA32_EFER.NXE is enabled on all CPUs; attempting to execute data pages faults immediately.
- SMEP and SMAP are enabled on all CPUs once kernel PTEs drop **User** from kernel pages; user-space execution and data access from kernel mode fault unless explicitly bracketed by `CLAC`/`STAC` (helpers exist; CR4 path still gated per IMPORTANT).
- KPTI gives every process a shadow user-page-table with no kernel text mapped; Meltdown becomes unexploitable.
- PCID allows KPTI CR3 switches without full global TLB flushes.
- IBRS is written on kernel entry; retpoline replaces all indirect branches; IBPB fires at context switches; Spectre v1/v2 mitigated.
- CET shadow stack enforces return-address integrity for the kernel; CET IBT enforces `ENDBR64` at every indirect call target.
- `kmalloc` allocations carry magic-cookie headers and redzones; corruption is caught immediately at `kfree`.
- Kernel stacks have an unmapped guard page; stack overflow raises a clean `#PF` instead of silently overwriting memory.
- KASLR randomizes the kernel load address at boot using RDRAND.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On                    | Status |
| --- | :---: | --------------------------------------------------- | ----------------------------- | :----: |
| 💎  |   1   | NX bit: EFER.NXE + PTE NX on all non-code mappings  | (none)                        |  [x]   |
| 💎  |   2   | SMEP & SMAP: CR4 activation + CLAC/STAC wrappers    | §1                            |  [/]   |
| 💎  |   3   | KPTI trampoline page + per-CPU CR3 fields           | §1, T11 §3                    |  [x]   |
| 💎  |   4   | KPTI SYSCALL CR3 swap                               | §3                            |  [ ]   |
| 💎  |   5   | KPTI IDT CR3 swap (all 256 vectors)                 | §3, §4                        |  [ ]   |
| 💎  |   6   | KPTI user_cr3 allocation + context switch           | §3, §4, §5                    |  [ ]   |
| 💎  |   7   | PCID: TLB tagging for KPTI (no-flush CR3 switch)    | §6                            |  [ ]   |
| 💎  |   8   | Spectre: IBRS/IBPB MSR + retpoline build flag       | T12 §2                        |  [ ]   |
| 💎  |   9   | CET shadow stack (kernel ring 0)                    | §1, §2                        |  [ ]   |
| 💎  |  10   | CET indirect branch tracking (IBT / ENDBR64)        | §9                            |  [ ]   |
| 💎  |  11   | Kernel heap hardening (cookies, redzone)            | T27 §1                        |  [ ]   |
| 💎  |  12   | Stack canaries (`-fstack-protector-strong`)         | T27 §1                        |  [ ]   |
| 💎  |  13   | Kernel stack guard pages                            | §1                            |  [ ]   |
| ⭐  |  14   | KASLR (RDRAND kernel load address)                  | §1, §6                        |  [ ]   |
| 💎  |  15   | Enclave and signing syscalls wired to SSDT          | §14, T12 §4                   |  [ ]   |

> 💎 = parity work: matches what Windows 11 and Linux already do.
> ⭐ = exclusive work: Impossible OS is superior or first.

---

## 1. NX Bit: EFER.NXE + PTE NX on All Non-Code Mappings

- [x] EFER bit definitions (`EFER_SCE`, `EFER_LME`, `EFER_LMA`, `EFER_NXE`) added to `kernel/msr.h`
- [x] `cpu_enable_nx()` in `cpu_security.c`: checks `cpu_has(CPU_FEATURE_NX)`, sets `EFER_NXE` via `msr_write()`
- [x] Called on BSP in Phase 0 after `cpuid_init()` via `cpu_harden()`; called on each AP in `ap_entry()`
- [x] `ap_trampoline.asm`: EFER set to LME + NXE + SCE before paging is enabled; no privilege gap between long mode entry and `cpu_harden()`
- [x] `VMM_FLAG_NX (1ULL << 63)` already defined in `vmm.h`
- [x] `vmm_map_page()` applies flags to leaf PTE only; intermediates get `PRESENT | WRITABLE` (correct per x86-64)
- [x] NX gated: `vmm_map_page()` strips `VMM_FLAG_NX` if `!cpu_has(CPU_FEATURE_NX)`; safe on older hardware
- [x] `vmm_apply_nx_policy()`: walks PML4->PDPT->PD->PT, sets NX on all non-text pages (2 MiB huge + 4 KiB entries); skips pages overlapping `__text_start`..`__text_end` (linker symbols added)
- [x] `vmm_flush_tlb_all()` called after NX application; logs count of NX'd pages + text range
- [x] Commit: `kernel/cpu,mm: EFER.NXE, AP trampoline NXE+SCE, PTE NX gate, vmm_apply_nx_policy` (landed as multiple commits)

**Test checkpoint:** `cpu_has(CPU_FEATURE_NX)` implies `EFER_NXE` set (MSR `0xC0000080`); `vmm_apply_nx_policy()` runs without panic; serial shows NX policy applied count; `POST16(0xD900)` series during `cpu_harden()`. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Verified:** 2026-04-12 -- all 10 items confirmed. `EFER_NXE` at `msr.h:41`. `cpu_enable_nx()` at `cpu_security.c:32-42`. `cpu_harden()` at `boot_hw.c:361`. AP sets NXE+SCE before long mode. `VMM_FLAG_NX` at `vmm.h:22`. `vmm_apply_nx_policy()` at `vmm.c:857-956` now splits 2 MiB huge pages overlapping .text into 4 KiB PTEs for per-page NX (fixed -- no more executable rodata/data gap). NX gated in `vmm_map_page()`. TLB flush after. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- kernel-code-quality 11 gates walked. EFER per-core MSR (no SMP race). Intel SDM Vol. 3A 4.1.4 compliant. Constants correct. O(n) one-time boot walk. Parity: matches Windows/Linux NXE per-page granularity. Accepted: none.

---

## 2. SMEP & SMAP: CR4 Activation + CLAC/STAC Wrappers

- [x] `read_cr4()`/`write_cr4()` + `CR4_SMEP`/`CR4_SMAP` in `cpu_security.c`
- [x] `cpu_enable_smep()` + `cpu_enable_smap()` call `hv_supports_cr4_smep_smap()` before touching CR4 (intended: skip WHPX/Hyper-V VM exits; enable KVM/VBox/bare metal when kernel PTE U/S is fixed)
- [/] **CR4.SMEP/SMAP actually set on any host:** blocked until shared boot PML4 clears **User** from kernel text/data huge pages (see `CLAUDE.md` SMEP/SMAP + `hv_supports_cr4_smep_smap()` in `src/kernel/cpu_security.c`); today the helper returns 0 always, so CR4 SMEP/SMAP never turn on
- [x] Called on BSP via `cpu_harden_post_pagetable()` after `vmm_apply_nx_policy()` and on each AP in `ap_entry()` after the same policy pass
- [x] `stac()`/`clac()` inlines + `KERNEL_ACCESS_USER_BEGIN()`/`KERNEL_ACCESS_USER_END()` macros in `cpu_security.h`; no-op if `!cpu_has(CPU_FEATURE_SMAP)`
- [x] `copy_from_user()` / `copy_to_user()` added in `cpu_security.c` with SMAP brackets
- [ ] Migrate existing syscall argument dereferences to use `copy_from_user`; deferred to **T12** `TODO-12-native-api-ssdt.md` (syscall / INT paths) when those sections implement user-buffer rules
- [ ] `ProbeForRead` / `ProbeForWrite`; deferred to T23 (SEH)
- [ ] Re-introduce `clac` at the top of `isr_common_stub` when SMAP is enabled for real (today removed: comment at `src/kernel/isr_stubs.asm` ~line 23; must be CPUID-gated or alternative-slots like Linux `FENCE_SWAPGS_*`, not an unconditional opcode)
- [ ] Commit: `kernel/security: CR4 SMEP/SMAP live, IDT clac/SMAP entry path` (partial: helpers landed; CR4 + IDT still open)

**Test checkpoint:** When `hv_supports_cr4_smep_smap()` returns non-zero after kernel PTE U/S is fixed: `CR4.SMEP` and `CR4.SMAP` read back set on KVM/VBox/bare metal; WHPX may still skip per hypervisor policy. Until then, expect klog "SMEP: skipped" / "SMAP: skipped". `copy_from_user`/`copy_to_user` succeed on valid user buffers. After `clac` returns to `isr_common_stub`, entry path leaves AC cleared for SMAP-on configs. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 3. KPTI Trampoline Page + Per-CPU CR3 Fields

Design and allocate the shared trampoline infrastructure that all KPTI ring transitions depend on. On x86-64, SYSCALL and interrupt entry fetch instructions from the current CR3 before any software runs -- so the entry code page, the CPU-selected stacks (RSP0/IST), the GDT/TSS, and the per-CPU data segment must all be mapped in user_cr3 with supervisor-only permissions.

> [!IMPORTANT]
> **Design constraint (Codex design review 2026-04-12):** LSTAR points to `syscall_entry` in kernel text. If kernel text is removed from user_cr3, SYSCALL faults before executing a single instruction. Similarly, IDT delivery pushes the exception frame onto TSS RSP0/IST stacks -- those must be mapped in user_cr3. The trampoline page is the foundation for all subsequent KPTI sections.

- [x] Allocate trampoline page via `pmm_alloc_frame()` in `kpti_init()` (`src/kernel/kpti.c`); `KPTI_TRAMPOLINE_VA = 0xFFFFFFFFFFFFF000` defined in `include/kernel/kpti.h`; identity-mapped and zeroed before stub copy
- [x] Assembly stubs in `src/kernel/kpti_trampoline.asm`: 4 stubs at offsets 0x000/0x080/0x100/0x180 (SYSCALL entry/return, ISR entry/return). JMP targets are placeholder addresses patched by S4/S5.
- [x] `kernel_cr3` (gs:104) and `user_cr3` (gs:112) added to `struct per_cpu_data` in `smp.h` with `_Static_assert` offset checks. BSP init in `smp_early_bsp_init()`, AP init in `ap_entry()`, both from current CR3. user_cr3 = kernel_cr3 until S6.
- [x] User_cr3 required pages documented in `kpti.h`: trampoline, RSP0 stacks, IST stacks (DF/NMI/MCE), GDT/TSS, per-CPU data -- all supervisor-only.
- [x] Commit: `"kernel/security: KPTI trampoline page + per-CPU CR3 fields"` (ad2d05e2)

**Test checkpoint:** Trampoline page allocated and mapped at fixed VA. Per-CPU `kernel_cr3`/`user_cr3` fields exist with correct offsets. Trampoline assembly compiles. Boot does NOT use user_cr3 yet -- this is infrastructure only.

> **Verified:** 2026-04-12 -- all 4 items confirmed. Trampoline at `kpti.c` allocated + mapped at `KPTI_TRAMPOLINE_VA` via `vmm_map_page`. 4 stubs in `kpti_trampoline.asm` preserve RAX via `gs:kpti_scratch` (Codex adversarial finding -- fixed). Per-CPU fields at gs:104-136 with 5 `_Static_assert` checks (Codex quality finding -- added). BSP+AP init from CR3. Required user_cr3 pages documented in `kpti.h`. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- kernel-code-quality 11 gates walked. Per-CPU fields SMP-safe (per-core). Intel SDM SYSCALL ABI: RAX preserved via scratch slot. JMP targets via per-CPU fields (not clobbered registers). All new GS offsets compile-time asserted. O(1) per-entry overhead. Accepted: none.

---

## 4. KPTI SYSCALL CR3 Swap

Wire the SYSCALL entry/exit path to swap CR3 via the trampoline page. LSTAR is redirected from `syscall_entry` to the trampoline's syscall stub. The stub loads `kernel_cr3` from per-CPU data, writes CR3, then jumps to the real `syscall_entry`. On SYSRET, the return path loads `user_cr3` and writes CR3 before executing SYSRETQ.

- [ ] Redirect LSTAR to `kpti_syscall_entry` in the trampoline page (-> XREF `src/kernel/sched/syscall_entry.asm`)
- [ ] Trampoline `kpti_syscall_entry`: SWAPGS, `mov rax, [gs:pcpu_kernel_cr3]`, `mov cr3, rax`, JMP to original `syscall_entry` (which skips its own SWAPGS since trampoline already did it)
- [ ] Trampoline `kpti_syscall_return`: `mov rax, [gs:pcpu_user_cr3]`, `mov cr3, rax`, SWAPGS, SYSRETQ
- [ ] Modify `syscall_entry.asm` to JMP to `kpti_syscall_return` instead of inline SWAPGS + SYSRET
- [ ] Gate: only activate when KPTI is enabled (boot flag `kpti=1` in boot.conf, default on for bare metal, off for hypervisors with EPT that already mitigate Meltdown)
- [ ] Commit: `"kernel/security: KPTI SYSCALL CR3 swap via trampoline"`

**Test checkpoint:** Syscall from ring 3 works with CR3 swap active. Serial shows `[KPTI] SYSCALL path active`. A syscall with user_cr3 = kernel_cr3 (no isolation yet) doesn't regress. Test on: QEMU WHPX, QEMU TCG; bare metal.

---

## 5. KPTI IDT CR3 Swap (All 256 Vectors)

Wire all interrupt/exception entry stubs to swap CR3 via the trampoline. Unlike SYSCALL (which doesn't change stack), IDT delivery pushes the exception frame onto TSS RSP0/IST stacks while user_cr3 is active -- so those stacks must be mapped in user_cr3 (supervisor-only).

- [ ] Map RSP0 and IST stack pages (DF/NMI/MCE) into every user_cr3 with Present + Writable + NX (supervisor-only, no User bit)
- [ ] Trampoline `kpti_isr_entry`: read saved CS from exception frame to check if ring 3 entry; if yes: `mov rax, [gs:pcpu_kernel_cr3]`, `mov cr3, rax`; then JMP to `isr_common_stub` (which skips its own SWAPGS since trampoline handled the ring check)
- [ ] Trampoline `kpti_isr_return`: before IRETQ from ring-3 return: `mov rax, [gs:pcpu_user_cr3]`, `mov cr3, rax`
- [ ] Redirect all 256 IDT entries from current stubs to `kpti_isr_entry` in the trampoline page
- [ ] Special handling: NMI and #DF use IST stacks and can nest -- CR3 swap must not clobber the IST stack or re-enter; use a dedicated IST trampoline stub that checks if CR3 is already kernel_cr3
- [ ] Gate: same `kpti=1` flag as S4
- [ ] Commit: `"kernel/security: KPTI IDT CR3 swap for all 256 vectors"`

**Test checkpoint:** Timer IRQ from ring 3 works with CR3 swap. Page fault from ring 3 works. NMI during syscall doesn't double-swap. #DF handler survives. Test on: QEMU WHPX, QEMU TCG; bare metal is critical (IST behavior differs under EPT vs native paging).

---

## 6. KPTI User CR3 Allocation + Context Switch

With trampoline and CR3 swap paths wired (S3-S5), allocate the actual sparse user_cr3 per process and activate Meltdown isolation.

- [ ] `vmm_create_user_cr3(task)`: allocate fresh PML4, copy only user-half PML4 entries (PML4[0..255]) from kernel PML4, plus the trampoline page, RSP0/IST stacks, GDT/TSS, per-CPU data page in the upper half (supervisor-only); all other kernel PML4 entries absent
- [ ] `vmm_sync_user_cr3(task)`: called on user page map/unmap; syncs the affected PML4 entry in user_cr3
- [ ] `vmm_destroy_user_cr3(task)`: frees the sparse PML4 and any intermediate page tables allocated for it
- [ ] Context switch (`task_switch`): update `smp_this_cpu()->user_cr3 = next_task->user_cr3`; `smp_this_cpu()->kernel_cr3 = next_task->kernel_cr3` (or the shared boot CR3 for kernel threads)
- [ ] Kernel threads (no user_cr3): set `user_cr3 = kernel_cr3` so the CR3 swap is a no-op
- [ ] Commit: `"kernel/security: KPTI dual page tables, user_cr3 allocation, Meltdown isolation active"`

**Test checkpoint:** `vmm_create_user_cr3()` returns valid PML4; user CR3 has no kernel text VA (`PML4[256..511]` entries absent except trampoline/stacks); Meltdown probe read of kernel VA from ring 3 faults. Context switch updates per-CPU CR3 pair. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 7. PCID: TLB Tagging for No-Flush CR3 Switch

- [ ] Enable `CR4.PCIDE (bit 17)` if `cpu_has(CPU_FEATURE_PCID)`: `cpu_set_cr4_bit(CR4_PCIDE)` during `cpu_harden_post_pagetable()` (or a dedicated `cpu_enable_pcid()` called from the same post-NX-policy window)
- [ ] Assign a 12-bit PCID to each process; stored in `task->pcid`:
  - PCID 0 = reserved for initial boot / no-PCID fallback
  - PCID 1..4094 = per-process; allocated from a monotone counter with wrap; on wrap, issue a global `INVPCID` (type 2 = global) to flush all stale TLB entries
  - `user_cr3` physical address gets `PCID` in bits [11:0]: `cr3_val = task->user_cr3_phys | task->pcid`
  - `kernel_cr3` uses `PCID + 0x800` convention (top bit set = kernel tag)

- [ ] When `CR4.PCIDE=1`, writing CR3 with bit 63 set (`NOFLUSH=1`) skips the TLB invalidation for that PCID; only entries with a different PCID are retained:
  ```asm
  ; Switch to kernel CR3 with NOFLUSH (bit 63 set in the value)
  mov rax, [gs:pcpu_kernel_cr3]
  bts rax, 63                     ; set NOFLUSH bit
  mov cr3, rax
  ```
- [ ] Use NOFLUSH on all hot-path CR3 writes (syscall entry/return, timer interrupt); use full-flush CR3 write only when `INVPCID` is needed (e.g., on `munmap` of a user page)
- [ ] `INVPCID` type 1 (`INVPCID_ADDR`) for single-VA invalidation: `invpcid [pcid, va]` in `vmm_flush_tlb(va)` when PCID is active

- [ ] Commit: `"kernel/security: PCID TLB tagging, NOFLUSH CR3 writes, INVPCID for targeted flush"`

**Test checkpoint:** `CR4.PCIDE` set when supported; `cr3` writes use bit 63 NOFLUSH on hot path; `invpcid` path exercised on unmap; no stray global flush on syscall ping-pong. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 8. Spectre Mitigations: IBRS/IBPB + Retpoline

- [ ] `MSR_IA32_SPEC_CTRL = 0x48`; `SPEC_CTRL_IBRS = 1`:
  ```c
  void cpu_spec_ctrl_enter_kernel(void) {
      if (cpu_has(CPU_FEATURE_IBRS))
          wrmsr(MSR_IA32_SPEC_CTRL, SPEC_CTRL_IBRS);
  }
  void cpu_spec_ctrl_exit_kernel(void) {
      if (cpu_has(CPU_FEATURE_IBRS))
          wrmsr(MSR_IA32_SPEC_CTRL, 0);
  }
  ```
- [ ] Insert `cpu_spec_ctrl_enter_kernel()` at kernel entry (syscall entry stub and every ISR common stub) and `cpu_spec_ctrl_exit_kernel()` at kernel exit (SYSRETQ / IRETQ); the MSR writes have ~20 cycle overhead; acceptable for syscall paths
- [ ] If `cpu_has(CPU_FEATURE_ENHANCED_IBRS)` (CPUID leaf 7 EDX bit 29): set IBRS once at boot and never clear it (Enhanced IBRS is always-on and has no exit overhead)

- [ ] `MSR_IA32_PRED_CMD = 0x49`; `PRED_CMD_IBPB = 1`
- [ ] `cpu_issue_ibpb()`: write 1 to `MSR_IA32_PRED_CMD` to flush the branch predictor on context switch; call in `sched_switch_task()` when switching between processes with different security domains (UIDs / token user SIDs differ); skip if same UID to reduce overhead

- [ ] Add `-mindirect-branch=thunk-extern` (GCC) or `-mretpoline` (Clang 19) to `CFLAGS` in `Makefile`; Clang 19 (`clang-19`) is already the compiler, so use `-mretpoline -mretpoline-external-thunk`
- [ ] Provide the retpoline thunk in `src/kernel/retpoline.asm` (one thunk per scratch register `rax`..`r15`; Clang emits `call __x86_indirect_thunk_rax` instead of `jmp rax`):
  ```asm
  __x86_indirect_thunk_rax:
      call .set_up_target
  .capture_spec:
      pause
      lfence
      jmp  .capture_spec
  .set_up_target:
      mov [rsp], rax
      ret
  ```
- [ ] Verify no `jmp *reg` or `call *reg` remains in kernel assembly after the build: `objdump -d build/kernel.elf | grep -E "jmp.*%r|call.*%r"`; must be empty
- [ ] Branch History Injection (BHI) and Spectre v1 **swapgs**: track Linux `spectre_bhi` / `spectre_v1` sysfs semantics; enable `BHI_DIS_S` or vendor clearing sequences where CPUID requires; add audited swapgs speculation barriers on ring-3 interrupt entry (`FENCE_SWAPGS_*` / `LFENCE` patterns per kernel Spectre guide; XREF T11 §3 INT/syscall paths)

- [ ] Commit: `"kernel/security: IBRS on kernel entry/exit, IBPB at context switch, retpoline build flag"`

**Test checkpoint:** `rdmsr(0x48)` shows IBRS during syscall body when enabled; `objdump` shows no bare `jmp *%r`; IBPB issued on cross-domain switch; BHI/swapgs items in the last §5 bullet satisfied on representative CPUs (klog or unit asserts). Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 9. CET Shadow Stack (Kernel Ring 0)

- [ ] MSR definitions:
  ```c
  #define MSR_IA32_S_CET           0x6A2   /* supervisor shadow stack control */
  #define MSR_IA32_PL0_SSP         0x6A4   /* ring-0 shadow stack pointer */
  #define MSR_IA32_INTERRUPT_SSP_TABLE 0x6A8 /* IST shadow stack table */
  #define S_CET_SH_STK_EN          (1ULL << 0)
  #define S_CET_WR_SHSTK_EN        (1ULL << 1)  /* WRSS instruction enable */
  #define S_CET_ENDBR_EN           (1ULL << 2)  /* IBT control (§7) */
  ```
- [ ] `cpu_enable_cet_ss()`:
  1. `cpu_set_cr4_bit(CR4_CET)`: enable CET in CR4
  2. `wrmsr(MSR_IA32_S_CET, S_CET_SH_STK_EN | S_CET_WR_SHSTK_EN)`
  3. Ensure `MSR_IA32_PL0_SSP` is set to the initial kernel shadow stack page's last 8 bytes (top of the shadow stack)

- [ ] Each kernel thread needs a shadow stack: one 4 KiB page per thread, marked `PTE_USER=0`, `PTE_NX=1`, and the special **supervisor shadow stack token** format (bit 1 of the 8-byte token set indicates this is the bottom of the shadow stack)
- [ ] `cet_alloc_shadow_stack(thread)`: `pmm_alloc_contiguous(1)`; write token at the end of the page; `vmm_map_page(shadow_stack_va, pa, PTE_SUPERVISOR_SHADOW_STACK)`; PTE bit 5 = 1 marks shadow-stack pages; processor enforces SHSTK semantics (only `RSTORSSP`/`SAVEPREVSSP` can write)
- [ ] On kernel thread creation in `src/kernel/sched/task.c` (`task_create()`): allocate shadow stack; set `task->shadow_stack_top`
- [ ] Context switch: save/restore `MSR_IA32_PL0_SSP` per thread

- [ ] Windows uses IST entries in the TSS for critical exceptions (#DF, #PF, NMI); each IST entry must also get a shadow stack in the `MSR_IA32_INTERRUPT_SSP_TABLE` (8-entry table, one VA per IST slot)
- [ ] `cet_init_interrupt_ssp_table()`: allocate 8 shadow stack pages; write their top VAs into the 64-byte `INTERRUPT_SSP_TABLE` structure; write the table physical address to `MSR_IA32_INTERRUPT_SSP_TABLE`

- [ ] Commit: `"kernel/security: CET shadow stack: CR4.CET, S_CET MSR, per-thread SSP allocation"`

**Test checkpoint:** `CR4.CET` set; `MSR_IA32_PL0_SSP` tracks per-thread shadow stack; forged return triggers `#CP` not hijacked RIP. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal (CET-capable CPU).

---

## 10. CET Indirect Branch Tracking (IBT / ENDBR64)

- [ ] Add `-fcf-protection=branch` to kernel `CFLAGS` (Clang 19 supports this); the compiler emits `ENDBR64` at the start of every function and every valid indirect call/jump target
- [ ] Verify: `objdump -d build/kernel.elf | grep endbr64 | wc -l`; must be > 0 (non-zero); count should match approximate function count
- [ ] Assembly files (`src/kernel/smp/ap_trampoline.asm`, ISR stubs, IDT stubs): manually add `endbr64` at each entry point that is reached via an indirect branch; NASM opcode: `db 0xF3, 0x0F, 0x1E, 0xFA`

- [ ] `cpu_enable_cet_ibt()`: add to `cpu_enable_cet_ss()` call sequence: `wrmsr(MSR_IA32_S_CET, rdmsr(MSR_IA32_S_CET) | S_CET_ENDBR_EN)`
- [ ] Legacy code mode: if a code region is loaded that does not have `ENDBR64` instructions (e.g., a legacy driver), temporarily disable IBT via `MSR_IA32_S_CET.NO_TRACK_EN` for that execution context; re-enable after (requires driver annotation `MODULE_FLAG_NO_IBT`)
- [ ] `ENDBR_EN` should be enabled after all kernel code is loaded and verified; setting it before loading a module without ENDBR64 would immediately fault

- [ ] Commit: `"kernel/security: CET IBT: ENDBR64 in kernel build, S_CET.ENDBR_EN activation"`

**Test checkpoint:** `objdump` shows `endbr64` prologues; indirect jump to target without `ENDBR64` faults when IBT on; `S_CET_ENDBR_EN` set only after stub audit. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 11. Kernel Heap Hardening: Cookies & Redzones

- [ ] Extend the `kmalloc` block header in `src/kernel/mm/heap.c`:
  ```c
  #define HEAP_COOKIE_MAGIC  0xDEADBEEFC0FFEE01ULL  /* XOR'd with alloc address */

  typedef struct {
      uint64_t  cookie;        /* HEAP_COOKIE_MAGIC ^ (uint64_t)block_ptr */
      uint32_t  size;          /* requested allocation size */
      uint32_t  redzone_front; /* 0xFEFEFEFE pattern detects underflow */
      /* user data follows */
      /* uint8_t redzone_back[8] after user data detects overflow */
  } kmalloc_header_t;
  ```
- [ ] `kmalloc(size)`:
  - Allocate `sizeof(kmalloc_header_t) + size + 8` bytes from the heap
  - Write `cookie = HEAP_COOKIE_MAGIC ^ (uint64_t)header_ptr`
  - Write `redzone_front = 0xFEFEFEFEFEFEFEFEULL`
  - Write redzone_back 8 bytes after user data = `0xBDBDBDBDBDBDBDBDULL`
  - Return `(header + 1)` (pointer to user data portion)
- [ ] `kfree(ptr)`:
  - Recover header = `(kmalloc_header_t *)ptr - 1`
  - Validate `cookie == HEAP_COOKIE_MAGIC ^ (uint64_t)header`: if mismatch -> `KeBugCheckEx(BUGCHECK_HEAP_CORRUPTION, ...)` (-> XREF `TODO-27-crash-dump-generation.md §1`)
  - Validate `redzone_front == 0xFEFEFEFEFEFEFEFEULL` (catches underflow)
  - Validate redzone_back == `0xBDBDBDBDBDBDBDBDULL` (catches overflow)
  - Zero the user data before returning to pool (`explicit_bzero`)
- [ ] `kmalloc_zeroed(size)`: like `kmalloc` but zeroes the user region immediately (for security-sensitive allocations like token structs, security descriptors)

- [ ] Extend `kmalloc_tagged(size, tag)` where `tag` is a 4-byte ASCII pool tag (e.g., `'TOKN'`, `'ALPC'`) stored in a 5th header field; `kfree_tagged(ptr, tag)` verifies the tag matches to catch mixed-pool use-after-free patterns; tag field is present only in debug builds (`KERNEL_DEBUG` defined)
- [ ] Existing callers that use plain `kmalloc` get implicit tag `'\0\0\0\0'`

- [ ] Commit: `"kernel/mm: kmalloc cookie + redzone hardening, kmalloc_zeroed, POOL_TAG debug"`

**Test checkpoint:** `kfree()` after cookie stomp raises `BUGCHECK_HEAP_CORRUPTION`; redzone violations caught; tagged debug build mismatches tag on `kfree_tagged`. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 12. Stack Canaries (`-fstack-protector-strong`)

- [ ] Remove `-fno-stack-protector` from `CFLAGS` in `Makefile`
- [ ] Add `-fstack-protector-strong`; protects functions that have:
  - local arrays or structs
  - address-taken local variables
  - calls to `alloca`; Clang 19 supports this exactly
- [ ] Verify no `__stack_chk_guard` linker error before the `__stack_chk_guard` init block in §9 runs

- [ ] `src/kernel/security/stack_canary.c`:
  ```c
  uintptr_t __stack_chk_guard = 0;  /* set by canary_init() */

  void canary_init(void) {
      uint64_t rand = 0;
      if (cpu_has(CPU_FEATURE_RDRAND)) {
          /* Three RDRAND retries per Intel spec */
          for (int i = 0; i < 3; i++) {
              uint8_t ok;
              __asm__ volatile(
                  "rdrand %0\n setc %1"
                  : "=r"(rand), "=r"(ok) : : "cc"
              );
              if (ok) break;
          }
      }
      if (!rand) {
          /* RDRAND unavailable: use RDTSC + ACPI timer XOR'd with kernel base */
          uint64_t tsc; __asm__ volatile("rdtsc" : "=A"(tsc));
          rand = tsc ^ (uint64_t)(uintptr_t)canary_init ^ 0xDEADC0DE00000000ULL;
      }
      /* Ensure canary never matches 0 or 0x00XXXXXXXX00 (null-terminator guard) */
      rand |= 0xFF00000000000000ULL;
      rand &= ~0x00000000000000FFULL;
      __stack_chk_guard = (uintptr_t)rand;
  }
  ```
- [ ] Call `canary_init()` very early in Phase 1 kernel init, before any stack-protected function is called (-> XREF `TODO-01-kernel-init-sequencing.md §3`)

- [ ] Add `__stack_chk_fail` handler to `src/kernel/security/stack_canary.c`:
  ```c
  __attribute__((noreturn)) void __stack_chk_fail(void) {
      KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE,
                   0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */, 0, 0, 0);
      __builtin_unreachable();
  }
  ```
- [ ] The `KeBugCheckEx` call triggers `panic_screen()` with the security stop code, generates a crash dump (-> XREF `TODO-27-crash-dump-generation.md §5`), and halts

- [ ] Commit: `"kernel/security: -fstack-protector-strong, RDRAND canary init, __stack_chk_fail"`

**Test checkpoint:** `__stack_chk_guard` non-zero after `canary_init()`; intentional overflow hits `BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE`; build uses `-fstack-protector-strong`. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 13. Kernel Stack Guard Pages

- [ ] Every kernel thread stack is allocated as `STACK_SIZE + PAGE_SIZE` pages; the first page (bottom of the stack, lowest address) is mapped with `PTE_PRESENT=0`; an unmapped guard page:
  ```c
  void stack_alloc_with_guard(task_t *t) {
      uintptr_t pa = pmm_alloc_contiguous(KERNEL_STACK_PAGES + 1);
      /* Map guard page as not-present */
      vmm_map_page(t->stack_guard_va, pa, 0 /* not present */);
      /* Map stack pages above guard */
      for (int i = 1; i <= KERNEL_STACK_PAGES; i++)
          vmm_map_page(t->stack_base_va + i * PAGE_SIZE,
                       pa + i * PAGE_SIZE, PTE_KERNEL_RW | PTE_NX);
      t->rsp0 = t->stack_base_va + (KERNEL_STACK_PAGES + 1) * PAGE_SIZE;
  }
  ```
- [ ] On `#PF` with fault address in the guard page VA range: trigger `KeBugCheckEx(BUGCHECK_KERNEL_STACK_INPAGE_ERROR, ...)` rather than a generic page fault BSOD; this distinguishes stack overflow from null pointer dereferences

- [ ] Each IST stack (`ist1`..`ist7` in the TSS) must also have a guard page below it; allocate with the same `stack_alloc_with_guard` helper; prevents nested-exception stack overflow from silently corrupting memory

- [ ] Commit: `"kernel/security: guard pages below kernel stacks and IST stacks"`

**Test checkpoint:** Guard VA `#PF` raises `BUGCHECK_KERNEL_STACK_INPAGE_ERROR`; IST stacks use same helper; no silent corruption below stack. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 14. KASLR: RDRAND Kernel Load Address

- [ ] The bootloader (`src/boot/uefi/bootx64.c`) currently loads the kernel ELF at its linked virtual base. For KASLR:
  1. Use UEFI `GetRNG` protocol (or `RDRAND` instruction) to generate a random 9-bit slide value `s` in `[0, 511]`
  2. Add `s * 2 MiB` to the kernel's linked virtual base: `kernel_slide = s * 0x200000`
  3. Load kernel ELF sections at `linked_va + kernel_slide` instead of `linked_va`
  4. Pass `kernel_slide` to the kernel in `boot_info.kaslr_slide`

- [ ] The kernel ELF must be built as a position-independent executable or carry a `.rela.text` / `.rela.data` relocation table
- [ ] Build flag: add `-pie -fPIE` to kernel CFLAGS, or use `-mcmodel=kernel` with explicit relocation entries; verify with `readelf -r build/kernel.elf` that `R_X86_64_64` entries are present
- [ ] Bootloader applies relocations: for each `R_X86_64_64` entry, add `kernel_slide` to the stored address at the given offset
- [ ] Linker map (`kernel.map`): add `kernel_slide` to all symbol addresses before writing `kernel.sym` (-> XREF `TODO-27-crash-dump-generation.md §3`) so `symtab_resolve` remains accurate after KASLR

- [ ] In `kernel_main()`: read `boot_info.kaslr_slide`; store in `g_kaslr_slide`; make it available to the module registry (T27 §3) and the crash dump writer (-> XREF `TODO-27-crash-dump-generation.md §4`) so dumps carry the slide value for post-mortem analysis
- [ ] `KASLR_BASE = __kernel_text_start + g_kaslr_slide`; all linker-symbol references throughout the kernel must add `g_kaslr_slide` when used as runtime addresses (or be recalculated from runtime `RIP`)

- [ ] Commit: `"kernel/security: KASLR: bootloader RDRAND slide, ELF relocation, kaslr_slide in boot_info"`

**Test checkpoint:** Two boots produce different `boot_info.kaslr_slide`; `kernel.sym` / module bases match slide; dump shows slide in vendor stream. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## 15. Enclave and Code Signing Syscalls Wired to SSDT

Register VBS/SGX enclave management and code signing verification syscalls in the SSDT. (-> XREF `TODO-12-native-api-ssdt.md §5`)

- [ ] `NtCreateEnclave(ProcessHandle, BaseAddress, ZeroBits, Size, InitialCommitment, EnclaveType, EnclaveInformation, InformationLength, EnclaveError)` -> SSDT 0x01F0
- [ ] `NtLoadEnclaveData(ProcessHandle, BaseAddress, Buffer, BufferSize, Protect, PageInformation, InformationLength, NumberOfBytesWritten, EnclaveError)` -> SSDT 0x01F1
- [ ] `NtInitializeEnclave(ProcessHandle, BaseAddress, EnclaveInformation, InformationLength, EnclaveError)` -> SSDT 0x01F2
- [ ] `NtTerminateEnclave(BaseAddress, WaitForThread)` -> SSDT 0x01F3
- [ ] `NtCallEnclave(EnclaveRoutine, WaitForThread, EnclaveRoutineReturn)` -> SSDT 0x01F4
- [ ] `NtSetCachedSigningLevel(Flags, InputSigningLevel, SourceFiles, SourceFileCount, TargetFile)` -> SSDT 0x02A2
- [ ] `NtGetCachedSigningLevel(File, Flags, SigningLevel, Thumbprint, ThumbprintSize, ThumbprintAlgorithm)` -> SSDT 0x02A3
- [ ] `NtCompareSigningLevels(FirstSigningLevel, SecondSigningLevel)` -> SSDT 0x02A4
- [ ] All functions return `NTSTATUS`
- [ ] Commit: `"kernel/security: wire Enclave and code signing syscalls to SSDT"`

**Test checkpoint:** `NtCreateEnclave` allocates enclave region. `NtSetCachedSigningLevel` stores signing level on file. `NtGetCachedSigningLevel` retrieves it. `NtCompareSigningLevels` returns correct ordering. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

---

## OS Comparison

| ⭐   | Feature              | 🪟 Win11     | 🐧 Linux       | 🚀 Impossible OS |
| --- | -------------------- | ----------- | ------------- | --------------- |
| 💎   | NX on data PTEs      | ✅ Long time | ✅ Long time   | ✅ Done §1       |
| 💎   | SMEP SMAP CR4        | ✅ Win8 / 10 | ✅ 3.x / 3.20  | ⏳ §2 partial    |
| 💎   | KPTI user PT         | ✅ Win10 PTI | ✅ 4.15 PTI    | ⬜ §3            |
| 💎   | PCID no flush CR3    | ✅ Yes       | ✅ Yes         | ⬜ §4            |
| 💎   | IBRS IBPB retpoline  | ✅ Yes       | ✅ spectre     | ⬜ §5            |
| 💎   | CET shadow stack     | ✅ 20H1+     | ✅ 6.6+        | ⬜ §6            |
| 💎   | CET IBT ENDBR64      | ✅ HVCI      | ✅ 6.6+        | ⬜ §7            |
| 💎   | Heap cookies redzone | ✅ Pool tags | ✅ SLUB        | ⬜ §8            |
| 💎   | Stack canaries /GS   | ✅ MSVC      | ✅ fssp strong | ⬜ §9            |
| 💎   | Stack guard pages    | ✅ Yes       | ✅ THREAD      | ⬜ §10           |
| 💎   | KASLR kernel base    | ✅ Yes       | ✅ RANDOMIZE   | ⬜ §11           |
| ⭐   | RDRAND canary KASLR  | ✅ Opaque    | ✅ pool        | ⬜ §9 §11        |
| ⭐   | KASLR slide in dump  | ❌ Opaque    | ❌ Opaque      | ⬜ §11 + T27     |

After §1 through §10, parity with Win11/Linux mitigations for NX through stack guards; §11 and §12 add KASLR and syscall surface. Linux skips mitigations without config; Windows ties IBT to HVCI. Impossible OS targets ring-0 enforcement without VBS; RDRAND-first canary and crash-dump-visible slide are quality extras.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_cpu_security()` (see `src/kernel/test/test_runner.c` and `include/kernel/test/test.h`; `test_cpu_security.c` not in tree yet).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_cpu_security.c` with:
  - NX verification: `EFER.NXE` bit set (read MSR 0xC0000080)
  - SMEP verification: CR4.SMEP set OR hypervisor EPT enforces (platform-dependent)
  - SMAP verification: CR4.SMAP set OR hypervisor EPT enforces
  - KPTI: user-mode PML4 entries don't map kernel text (when per-process PT active)
  - Stack canary: `__stack_chk_guard` is non-zero and randomized
  - KASLR: kernel base != default 0x100000 (when KASLR enabled)
  - W^X kernel pages: text pages are R-X (not writable), data pages are RW- (not executable)
  - Spectre v2: IBRS or retpoline active (check MSR or compiler flag)
  - CET: if CPU supports CET, IBT/SHSTK status verified
  - UMIP: if CPU supports UMIP, CR4.UMIP set
- [ ] Register in `test_runner_init()`: `test_register_cpu_security()`
- [ ] Commit: `"test: add CPU security hardening test suite"`

**Test checkpoint:** `test_cpu_security.c` exists; `test_register_cpu_security()` is declared in `test.h`, called from `test_runner_init()`, and registered under `TEST_CAT_BOOT`; `bash scripts/test.sh SUITE=boot` shows new assertions PASS (or `TEST_SKIP` only on unsupported hardware).

---

## Verification

- [ ] **NX**: map a heap page and attempt to `jmp` to it; must raise `#PF` with error code bit 4 (Instruction Fetch) set.
- [ ] **SMEP**: write a ring-3 code page VA into a kernel function pointer and call it; must raise `#PF` with bit 4 set before executing user code.
- [ ] **SMAP**: dereference a user-space pointer from kernel context without `STAC`; must raise `#PF` with bit 5 (Protection Key Violation) set.
- [ ] **KPTI**: in user mode, attempt to read a known kernel VA (`0xFFFF800000000000`) via a side channel; must receive `#PF` with no data leak.
- [ ] **IBRS**: verify `rdmsr(MSR_IA32_SPEC_CTRL) & 1` is `1` during kernel execution, `0` after SYSRETQ.
- [ ] **Retpoline**: `objdump -d build/kernel.elf | grep -E 'jmp\s+\*%|call\s+\*%'`; must produce zero lines (all indirect branches replaced).
- [ ] **CET SS**: corrupt a return address on the kernel stack; return must cause `#CP (Control Protection)` exception, not jump to the corrupted address.
- [ ] **Heap cookie**: call `kfree(ptr)` after zeroing the cookie field; must trigger `BUGCHECK_HEAP_CORRUPTION` BSOD.
- [ ] **Stack canary**: overflow a local array past the canary slot and return; must trigger `__stack_chk_fail` -> `BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE`.
- [ ] **Guard page**: write to `task->stack_guard_va`; must raise `BUGCHECK_KERNEL_STACK_INPAGE_ERROR`, not silent memory corruption.
- [ ] **KASLR**: two consecutive boots must load the kernel at different base addresses (verify via `dmpanalyze.exe` `ImpossibleOSInfoStream.KernelPath` showing different `kaslr_slide` values).
- [ ] **Platforms:** repeat the checks above on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal (SMEP/SMAP may be gated on some hypervisors per §2).
- [ ] **Remaining limits**: CET IBT requires every kernel assembly stub to carry `ENDBR64`; audit `src/kernel/smp/ap_trampoline.asm` and all IDT stubs before enabling `S_CET_ENDBR_EN`; missing ENDBR64 in a called indirect target causes an immediate `#CP` fault.
- [ ] Commit: `"kernel/security: CPU mitigations (NX/SMEP/SMAP/KPTI/Spectre/CET), heap/stack hardening, KASLR"`

**Test checkpoint:** Every unchecked bullet in this section is executed on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal before release; hypervisor SMEP/SMAP gating matches §2 expectations; no verification item remains open for a shipped mitigation milestone.

**Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot); add `TEST_CAT_BOOT` suites when `test_cpu_security.c` lands.

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | Removed ### N.M; stripped model tags; Goal/Current state join; ASCII/`s in [0,511]`; XREF §3 disambiguation (KPTI vs TODO-17); KASLR module -> TODO-27 §3; Impl `Depends On` TODO-*; compact OS table; dead TODO-32 -> test_runner pattern; §9 init wording; blank normalize; **Test checkpoint** restored for §1 through §2 and added §3 through §12 + platforms; Verification platform sweep; `run-boot-tests.bat`; History. |
| 2026-04-12 | validate | Impl Order `Depends On`: T11/T05/T16 shorthand, row 1 `(none)`; §1/§2 final `Commit:` lines; Inputs and body: colon/semicolon rewrites (no `--` sentence glue per CLAUDE); XREF bullets colon after path; OS row `T27`; after-table range wording; Unit Tests + Verification **Test checkpoint** blocks; `run-boot-tests.bat` line semicolon. **Flags:** §3+ blocked on T11 §3, T12 §2, T27 §1 until those sections land; `test_register_cpu_security` not wired yet (per Unit Tests). |
| 2026-04-12 | gap-analysis | Win11/Linux web inventory + kernel.org Spectre doc fetch; code-truth audit. Rewrote IMPORTANT callout + Goal; Impl row 2 `[/]`; §2 demotions (CR4 SMEP/SMAP, `clac`); OS table SMEP row `⏳ §2 partial`; §5 BHI/swapgs checklist; Inputs XREF T09 §4; mirrored note in `TODO-21` scope block. |
| 2026-04-12 | validate | Outcome SMEP/SMAP bullet aligned with gated CR4; §4 PCID hook text fixed (no fictitious `cpu_enable_smep_smap()`); §2 blank between list items removed; `copy_from_user` migration defer now cites **T12**; `alloca`/null pointer typos; `---` before Unit Tests; §5 test checkpoint mentions BHI/swapgs; IMPORTANT **T09** shorthand. **Flags:** parity rows still ⬜ for §3+; external deps T11 §3, T12 §2, T27 §1; `test_register_cpu_security` unwired. |
| 2026-04-12 | gap-analysis | Reciprocal from **T09** gap pass: IMPORTANT adds PKS defer (supervisor keys vs user PKU in T09 §4); T09 §3 lists `cpu_verify_hardening` raw `rdmsr` cleanup with XREF here. |
