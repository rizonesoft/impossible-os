---
schema_version: 1
id: kernel-security-hardening
domain: 02-kernel-core
status: active
title: "TODO-10 -- Kernel Security Hardening"
---

# TODO-10 -- Kernel Security Hardening

> **Validated:** 2026-06-28 | validate-todo-file clean (structure / IO table / XREF / test wiring); compacted §12 canary_init code block, added Notes blocks to §1/§2/§3
> **Gap-audited:** 2026-06-28 | gap-audit + codex-gap-audit (Win11 24H2 / Linux 6.x parity); 8 new sections filed (§17 image-W^X, §18 SSBD/STIBP/RSB/BHI/ITS/Retbleed, §19 MDS/VERW clears, §20 kCFI, §21 FORTIFY_SOURCE, §22 stackleak, §23 KFENCE, §24 mitigation-posture); §14 marked blocked on T33; §1 W^X-overclaim softened; §11 freelist + §14 module-ASLR retargeted to owners (D03 T03, T05/T07); HVCI/VBS noted as VTL1-only design exclusion

> **Goal:** Activate every CPU security feature that `cpuid.c` detects where we still lack runtime enforcement: complete SMEP/SMAP (CR4 bits on real hardware), KPTI (separate user/kernel page tables + PCID), Spectre mitigations (IBRS/retpoline/IBPB + swapgs barriers), CET shadow stack, CET indirect branch tracking, and KASLR. Couple this with kernel-side hardening that does not require CPU support: heap magic cookies, redzone detection, stack canaries (`-fstack-protector-strong` + RDRAND-seeded `__stack_chk_guard`), and guard pages below kernel stacks. Without these, Impossible OS is exploitable via any 2018-era hardware vulnerability and trivially attackable by user-mode code; that is unacceptable for a production OS in 2026.

> [!IMPORTANT]
> **Current state (code-truth):** **NX is active:** `cpu_harden()` enables `EFER.NXE` on BSP and APs; `ap_trampoline.asm` sets NXE before paging; `vmm_apply_nx_policy()` in `boot_hw.c` walks page tables and applies PTE NX to non-text mappings. **SMEP/SMAP path exists but CR4 bits are not enabled today:** `hv_supports_cr4_smep_smap()` in `cpu_security.c` returns 0 unconditionally (boot PML4 still has User on kernel 2 MiB pages per `CLAUDE.md`), so `cpu_enable_smep()` / `cpu_enable_smap()` always return early; verify logs show SMEP/SMAP skipped. **`clac` is not emitted on the IDT common stub:** removed in `isr_stubs.asm` to avoid `#UD` on CPUs without SMAP in the test matrix; must return when SMAP is real. **Now shipped since this callout:** `MSR_IA32_SPEC_CTRL` eIBRS program + retpoline thunks (§8; `-mretpoline` at `Makefile:26`, `cpu_program_bsp_eibrs()` in `cpu_security.c`), heap magic cookies (§11), `-fstack-protector-strong` in the kernel `Makefile` (§12; `Makefile:23` -- the remaining `-fno-stack-protector` occurrences are non-kernel build targets). **Still absent:** KPTI, PCID use, CET, dedicated kernel stack guard policy beyond existing bulletproofing, KASLR slide. **Also track:** UMIP/PKU and advanced x86 features live under **T09** `TODO-09-x86-64-architecture.md` (see its Inputs XREF). **PKS (supervisor protection keys):** gap-analysis 2026-04-12 defers PKS enablement here; Linux documents it under memory protection keys (`docs.kernel.org/core-api/protection-keys.html`); own alongside SMEP/kernel direct-map policy once **T09** user PKU (§5) and PTE key plumbing exist.

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
- -> XREF: `TODO-17-binary-system.md §3,§12`: ELF `PT_GNU_PROPERTY` (T17 §3) and PE `IMAGE_LOAD_CONFIG_DIRECTORY64` (T17 §12) carry per-binary CET IBT/SHSTK and CFG flags; this TODO's §9 (CET shadow stack) and §10 (CET IBT) consume those flags to decide enforcement
- -> XREF: `TODO-21-process-model-extensions.md §11`: per-process mitigation flags (`MIT_DEP_ENABLE`, `MIT_ASLR_FORCE`, etc.) consume §1 NX/DEP enforcement; mitigation API surface is authoritative in TODO-21
- -> XREF: `TODO-23-exception-dispatch-seh.md §3`: `#CP` (vector 21, CET shadow-stack RET mismatch OR IBT missing-`ENDBR64`) exception handler -- SHIPPED; §9 enables CET SS, §10 enables IBT, §3 of T23 handles the resulting `#CP` faults (user-mode via `ki_dispatch_exception()`, kernel-mode `KeBugCheckEx()` directly) (T23 §3 was a hard prerequisite for §9/§10 enable -- now satisfied, so a violation is caught rather than triple-faulting)
- -> XREF: `01-boot-platform/TODO-02-uefi-hardening-secureboot.md §3`: `boot_info.secure_boot_enabled` / `HKLM\SYSTEM\SecureBoot\State` from UEFI `SecureBoot` variable; future `boot.conf` lockdown knob must be defined in this TODO when implemented (no §13 in TODO-01)
- -> XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §2, §5`: Phase 0 activation order (§2) calls `cpu_efer_harden()`/`cpu_cr4_harden()` from this TODO; AP hardening (§5) replicates the same features on each AP via `ap_cpu_harden()`
- -> XREF: `TODO-31-kernel-bulletproofing.md §10`: guard pages, split huge pages, and VM layout invariants; NX/SMEP/SMAP policy here must stay consistent with those checks
- -> XREF: `TODO-09-x86-64-architecture.md §5`: UMIP and PKU (`CR4.UMIP`, `PKRU`) are scoped there; keep Spectre swapgs/`gs:` sequencing aligned with this TODO and T11 §4
- -> XREF: `03-memory-concurrency/TODO-02-memory-security.md §2, §3, §4, §6`: forked-ownership resolved 2026-07-16 -- this TODO is the SOLE owner of SMEP/SMAP (§2 here), KPTI (§3-§6 here), and KASLR (§14 here). Those TODO-02 sections are superseded (marked `[~]` with `> **Superseded by**` notes); TODO-02 retains user-space ASLR, NX/DEP, CET, and the security-layout report.

---

## Outcome

- NX bit is set on all non-code PTE entries; IA32_EFER.NXE is enabled on all CPUs; attempting to execute data pages faults immediately.
- SMEP and SMAP are enabled on all CPUs once kernel PTEs drop **User** from kernel pages; user-space execution and data access from kernel mode fault unless explicitly bracketed by `CLAC`/`STAC` (helpers exist; CR4 path still gated per IMPORTANT).
- KPTI gives every process a shadow user-page-table with no kernel text mapped; Meltdown becomes unexploitable.
- PCID allows KPTI CR3 switches without full global TLB flushes.
- eIBRS is set once per CPU (no hot-path toggle); retpoline replaces compiler-generated indirect branches and is the legacy-CPU mitigation; IBPB fires at cross-process context switches; Spectre v1/v2 mitigated.
- CET shadow stack enforces return-address integrity for the kernel; CET IBT enforces `ENDBR64` at every indirect call target.
- `kmalloc` allocations carry magic-cookie headers and redzones; corruption is caught immediately at `kfree`.
- Kernel stacks have an unmapped guard page; stack overflow raises a clean `#PF` instead of silently overwriting memory.
- KASLR randomizes the kernel load address at boot using RDRAND.

---

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                 | Status |
| --- | :---: | ---------------------------------------- | -------------------------- | :----: |
| 💎   |   1   | NX bit: EFER.NXE + PTE NX on all non-code mappings | (none)                     |  [x]   |
| 💎   |   2   | SMEP & SMAP: CR4 activation + CLAC/STAC wrappers | §1                         |  [/]   |
| 💎   |   3   | KPTI trampoline page + per-CPU CR3 fields | §1, T11 §3                 |  [x]   |
| 💎   |   4   | KPTI SYSCALL CR3 swap                    | §3, T33                    |  [/]   |
| 💎   |   5   | KPTI IDT CR3 swap (all 256 vectors)      | §3, §4, T33                |  [/]   |
| 💎   |   6   | KPTI user_cr3 allocation + context switch | §3, §4, §5, T33            |  [/]   |
| 💎   |   7   | PCID: TLB tagging for KPTI (no-flush CR3 switch) | §6                         |  [/]   |
| 💎   |   8   | Spectre: eIBRS/IBPB MSR + retpoline build flag | T12 §2                     |  [x]   |
| 💎   |   9   | CET shadow stack (kernel ring 0)         | §1, §2, T23 §3, D01T09 §10 |  [/]   |
| 💎   |  10   | CET indirect branch tracking (IBT / ENDBR64) | §9, T23 §3                 |  [/]   |
| 💎   |  11   | Kernel heap hardening (cookies, redzone) | T27 §1                     |  [x]   |
| 💎   |  12   | Stack canaries (`-fstack-protector-strong`) | T27 §1                     |  [x]   |
| 💎   |  13   | Kernel stack guard pages                 | §1                         |  [/]   |
| ⭐   |  14   | KASLR (RDRAND kernel load address)       | §1, §6, T33                |  [/]   |
| 💎   |  15   | Enclave and signing syscalls wired to SSDT | §14, T12 §4, T19           |  [/]   |
| 💎   |  16   | Secure Boot lockdown enforcement         | D01 T02 §5,§15             |  [/]   |
| 💎   |  17   | Kernel-image W^X (.text RO, .rodata RO-after-init) | §1                         |  [x]   |
| 💎   |  18   | CPU feature-flag 128-bit expansion (cpu_feature_mask_t) | §8                         |  [x]   |
| 💎   |  19   | Microarchitectural data-sampling clears (VERW/MDS) | §8                         |  [x]   |
| 💎   |  20   | kCFI software control-flow integrity     | §10                        |  [/]   |
| 💎   |  21   | FORTIFY_SOURCE bounds-checked str/mem builtins | (none)                     |  [/]   |
| 💎   |  22   | stackleak: erase kernel stack on return to user | §13                        |  [/]   |
| 💎   |  23   | KFENCE sampling UAF/OOB detector         | §11, §13                   |  [/]   |
| ⭐   |  24   | Mitigation visibility: queryable security posture | §17, §19, §25              |  [/]   |
| 💎   |  25   | Spectre predictor mitigations (SSBD/STIBP/RSB/BHI/ITS/Retbleed) | §18, §8                    |  [/]   |
| 💎   |  26   | Release-build test-surface exclusion (KERNEL_TESTS is uncond.) | (none)                     |  [x]   |
| 💎   |  27   | Release-flavor proof: seam-inventory gate + CI attestation | §26                        |  [x]   |
| 💎   |  28   | Test-only TUs outside `src/kernel/test/` (NTFS self-test etc.) | §26                        |  [x]   |
| 💎   |  29   | Guard unguarded test-only helpers in production TUs | §27, §28                   |  [x]   |
| 💎   |  30   | Signed CI attestation for the release-flavor proof gate | §27, §29                   |  [/]   |
| 💎   |  31   | Legacy-syscall user-pointer validation (`sys_write`, `sys_log`) | (none)                     |  [ ]   |
| 💎   |  32   | Post-ship follow-up backfill (2026-07-31 cohort) | --                         |  [ ]   |

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

> **Notes:**
> - Shipped: `EFER.NXE` (per-core MSR) + per-PTE NX gate (`VMM_FLAG_NX`) + `vmm_apply_nx_policy()` marking all non-code mappings no-execute.
> - Runs in `cpu_harden()` (BSP + AP, before long mode) for the MSR; `vmm_apply_nx_policy()` splits 2 MiB huge pages overlapping `.text` into 4 KiB PTEs for per-page NX, then `vmm_flush_tlb_all()`.
> - Effect: no executable rodata/data (per-PTE NX). NOTE: this is NX only -- making kernel `.text` read-only and `.rodata` RO-after-init (STRICT_KERNEL_RWX / full W^X on the kernel image) is a separate gap owned by §17.

> **Verified:** 2026-04-12 -- all 10 items confirmed. `EFER_NXE` at `msr.h:41`. `cpu_enable_nx()` at `cpu_security.c:32-42`. `cpu_harden()` at `boot_hw.c:361`. AP sets NXE+SCE before long mode. `VMM_FLAG_NX` at `vmm.h:22`. `vmm_apply_nx_policy()` at `vmm.c:857-956` splits 2 MiB huge pages overlapping .text into 4 KiB PTEs for per-page NX. NX gated in `vmm_map_page()`. TLB flush after. Accepted: none.
> **Quality reviewed:** 2026-04-12 -- kernel-code-quality 11 gates walked. EFER per-core MSR (no SMP race). Intel SDM Vol. 3A 4.1.4 compliant. Constants correct. O(n) one-time boot walk. Parity: matches Windows/Linux NXE per-page granularity. Accepted: none.

---

## 2. SMEP & SMAP: CR4 Activation + CLAC/STAC Wrappers

> [!NOTE]
> The boot-PML4 User-bit blocker (the `[/]` item below) is observed and bare-metal-verified by `01-boot-platform/TODO-10-bare-metal-hardening.md §8-§9`, which owns the bare-metal shared-page-table quirk (not this CR4 activation). KPTI `§3-§6` here build the clean kernel PML4 that actually unblocks CR4.SMEP/SMAP.

- [x] `read_cr4()`/`write_cr4()` + `CR4_SMEP`/`CR4_SMAP` in `cpu_security.c`
- [x] `cpu_enable_smep()` + `cpu_enable_smap()` call `hv_supports_cr4_smep_smap()` before touching CR4 (intended: skip WHPX/Hyper-V VM exits; enable KVM/VBox/bare metal when kernel PTE U/S is fixed)
- [/] **CR4.SMEP/SMAP actually set on any host:** blocked until shared boot PML4 clears **User** from kernel text/data huge pages (see `CLAUDE.md` SMEP/SMAP + `hv_supports_cr4_smep_smap()` in `src/kernel/cpu_security.c`); today the helper returns 0 always, so CR4 SMEP/SMAP never turn on
- [x] Called on BSP via `cpu_harden_post_pagetable()` after `vmm_apply_nx_policy()` and on each AP in `ap_entry()` after the same policy pass
- [x] `stac()`/`clac()` inlines + `KERNEL_ACCESS_USER_BEGIN()`/`KERNEL_ACCESS_USER_END()` macros in `cpu_security.h`; no-op if `!cpu_has(CPU_FEATURE_SMAP)`
- [x] `copy_from_user()` / `copy_to_user()` added in `cpu_security.c` with SMAP brackets
- [ ] Migrate existing syscall argument dereferences to use `copy_from_user`; deferred to **T12** `TODO-12-native-api-ssdt.md` (syscall / INT paths) when those sections implement user-buffer rules
- [ ] Harden the user-copy path to reject supervisor destinations (per-process User-PTE check), not just range-check below `MM_USER_PROBE_ADDRESS`; needs §2 User-bit drop + §3-§6 KPTI. -> XREF: `TODO-03-kernel-libraries.md` §5
- [ ] `ProbeForRead` / `ProbeForWrite`; deferred to T23 (SEH)
- [ ] Re-introduce `clac` at the top of `isr_common_stub` when SMAP is enabled for real (today removed: comment at `src/kernel/isr_stubs.asm` ~line 23; must be CPUID-gated or alternative-slots like Linux `FENCE_SWAPGS_*`, not an unconditional opcode)
- [ ] `KERNEL_ACCESS_USER_BEGIN/END` gate on BSP-global `cpu_has(CPU_FEATURE_SMAP)` -- STAC/CLAC #UD on a feature-skewed AP without SMAP. Gate on per-CPU live CR4.SMAP (header predicate) -- affects copy_*_user + `TODO-23 §13` try_copy_*/ProbeForWrite
- [ ] CR4.LASS (Linear Address Space Separation): when `CPUID.(7,1):EAX[6]` set (detected by `D02 T09 §15`), enable CR4.LASS to fault user/kernel address crossings in hardware; same clean-kernel-PML4 prereq as SMEP/SMAP (-> XREF: `D02 T09 §15`)
- [ ] Commit: `kernel/security: CR4 SMEP/SMAP live, IDT clac/SMAP entry path` (partial: helpers landed; CR4 + IDT still open)

**Test checkpoint:** When `hv_supports_cr4_smep_smap()` returns non-zero after kernel PTE U/S is fixed: `CR4.SMEP` and `CR4.SMAP` read back set on KVM/VBox/bare metal; WHPX may still skip per hypervisor policy. Until then, expect klog "SMEP: skipped" / "SMAP: skipped". `copy_from_user`/`copy_to_user` succeed on valid user buffers. After `clac` returns to `isr_common_stub`, entry path leaves AC cleared for SMAP-on configs. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Notes:**
> - Shipped so far: `read_cr4`/`write_cr4` + `CR4_SMEP`/`CR4_SMAP`, the `cpu_enable_smep`/`cpu_enable_smap` helpers (gated on `hv_supports_cr4_smep_smap()`), `stac`/`clac` inlines + `KERNEL_ACCESS_USER_*` brackets, and `copy_from_user`/`copy_to_user`.
> - Partial ([/]): CR4.SMEP/SMAP are NOT live -- the helper returns 0 until the shared boot PML4 drops User from kernel huge pages (owned by `01-boot-platform/TODO-10 §8-§9`); KPTI §3-§6 here build the clean kernel PML4 that unblocks it.
> - Open in-section: migrate syscall derefs to `copy_from_user` (T12), supervisor-dest rejection, `ProbeForRead/Write` (T23), `clac` back in `isr_common_stub`, CR4.LASS (D02 T09 §15).

> **Deferred:** [H] CR4.SMEP/SMAP activation not live -- the shared boot PML4 has User on all kernel 2 MiB pages so CR4.SMEP would #PF; needs the clean kernel PML4 (the KPTI §6 user_cr3 / per-process PT path), itself rooted in higher-half relocation. The helpers/CLAC-STAC/copy_*_user surface IS shipped ([x] items above). -> XREF: 01-boot-platform/TODO-10-bare-metal-hardening §9 (item: "All platforms ... SMEP/SMAP skipped -- boot PML4 ... has User on all kernel 2MiB pages" at line 411) + 02-kernel-core/TODO-33-higher-half-kernel-relocation

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

> **Notes:**
> - Shipped: KPTI trampoline page at `KPTI_TRAMPOLINE_VA` + per-CPU `kernel_cr3`/`user_cr3` fields (gs:104/112) with `_Static_assert` offset pins; 4 CR3-swap stubs in `kpti_trampoline.asm` (RAX preserved via `gs:kpti_scratch`).
> - Runs as infrastructure only: BSP/AP init both fields from current CR3 (`user_cr3 = kernel_cr3` until §6); no live CR3 swap yet.
> - Scope boundary: §4-§6 consume these fields for the actual SYSCALL/IDT/context-switch CR3 swaps; this section is the page + field substrate.

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

> **Deferred:** [H] blocked on the per-process clean kernel PML4 + higher-half relocation -- a user_cr3 that excludes kernel space (but keeps the trampoline + entry text) requires the higher-half memory model; §3 trampoline infra is the only ready piece. TODO-33 §1 pinned the layout (`MM_KERNEL_VIRT_BASE`), but the supervisor-only split this needs lands in §6. -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation §6 (item: "Commit: `\"mm: per-process PML4 -- kernel high-half shared, user low-half private\"`")

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

> **Deferred:** [H] blocked with §4 -- IDT delivery on all 256 vectors pushes the exception frame onto CR3-resident stacks before software runs; the dual-CR3 model it tags requires the higher-half clean PML4 (§4/§6 path). -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation §6 (item: "Commit: `\"mm: per-process PML4 -- kernel high-half shared, user low-half private\"`")

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

> **Deferred:** [H] blocked on higher-half relocation -- a user_cr3 with `PML4[256..511]` kernel entries absent is only meaningful once the kernel lives in the upper half (today it links low, so kernel + user share the lower half). This is THE Meltdown-isolation section; it also unblocks §2 SMEP/SMAP + §7 PCID. -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation §6 (item: "Ensure kernel upper-half entries are marked supervisor (no User bit)")

---

## 7. PCID: TLB Tagging for No-Flush CR3 Switch

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §5` -- the bare `CR4.PCIDE` bit is ALREADY set there (BSP `boot_phase1` + AP `ap_cpu_harden()`, via `cpu_pcid_enable()`). This section adds per-process PCID tagging + NOFLUSH CR3 on top of the already-active feature; it must NOT re-enable or duplicate the CR4 activation.

- [x] `CR4.PCIDE (bit 17)` activation -- DONE in `01-boot-platform/TODO-09 §5` via `cpu_pcid_enable()` (BSP + AP). This section consumes the already-set bit and adds tagging; do NOT re-enable it here
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

> **Deferred:** [M] depends on §6 (KPTI dual-CR3) -- PCID TLB tagging + NOFLUSH CR3 only matters once the KPTI CR3 ping-pong exists; `CR4.PCIDE` enable already shipped via the boot-domain (D02 T09 boot S5), so only the KPTI-coupled NOFLUSH/INVPCID work remains, gated on §6. -> XREF: 02-kernel-core/TODO-10 §6 (KPTI user_cr3 + context switch)

---

## 8. Spectre Mitigations: IBRS/IBPB + Retpoline

> [!NOTE]
> **Design (Codex design review 2026-06-28):** eIBRS-FIRST (set-once, no hot-path toggle); the legacy per-entry IBRS toggle is DROPPED (no alternatives/stub-patch infra; retpoline covers legacy CPUs). SPEC_CTRL writes preserve the per-CPU baseline. IBPB fails closed. eIBRS detection is `IA32_ARCH_CAPABILITIES[1]`, NOT CPUID.7:EDX[29].

- [x] Feature flags + constants: `CPU_FEATURE_ENHANCED_IBRS=62` (`ARCH_CAPABILITIES[1]`), `CPU_FEATURE_IBPB=63` (Intel 7.0:EDX[26] + AMD 0x80000008:EBX[12]), `COUNT->64`; `SPEC_CTRL_IBRS`/`PRED_CMD_IBPB`/`ARCH_CAP_IBRS_ALL` in `msr.h`
- [x] eIBRS-first: `cpu_program_bsp_eibrs()` sets `IA32_SPEC_CTRL = baseline | IBRS` once on the BSP post-IDT (boot_phase2, `msr_try_write` degrade); no hot-path toggle
- [x] AP SPEC_CTRL replay: `ap_apply_msr_profile()` per_cpu handler writes `own-baseline | IBRS` gated inline on the AP's own eIBRS; `CPU_FEATURE_SPEC_CTRL` added to `CPU_FEATURES_AP_PROBE_MASK` + `cpuid_probe_ap_features()` for the loop gate
- [x] Baseline preservation: SPEC_CTRL writes are `baseline | owned-bits`, never literal 0 (keeps firmware bits + §18's future SSBD/STIBP)
- [x] Legacy IBRS (no eIBRS): NO per-entry `wrmsr` toggle (design decision -- no alternatives infra); retpoline is the legacy-CPU Spectre-v2 mitigation
- [x] `cpu_issue_ibpb()` (gated `CPU_FEATURE_IBPB`) via `sched_cross_domain_ibpb()` from `schedule()`/`schedule_now()` on cross-process switches with a structural user-capable task (`cr3!=0`/`user_stack_base`); no IBPB on kthread switches
- [x] `-mretpoline -mretpoline-external-thunk` in `CFLAGS` (clang-19); `src/kernel/retpoline.asm` provides the 15 GP-register thunks (auto-discovered by the Makefile `find`)
- [x] Verified: `objdump` shows 0 `jmp/call *%reg` and 0 `*mem` in compiler-generated code; 600 `__x86_indirect_thunk_*` references present
- [x] NASM inventory (retpoline does not rewrite hand-written asm): `ap_trampoline.asm` `call rax` = boot-only exempt; `kpti_trampoline.asm` `jmp [gs:...]` = runtime, convert to safe targets -> XREF: §4
- [x] Spectre v1 **swapgs**: `lfence` after the conditional ring-3 entry swapgs in `isr_stubs.asm` (Linux `FENCE_SWAPGS_KERNEL_ENTRY`)
- [/] Scope boundary: §8 is the IBRS/IBPB/retpoline core. Additional predictor mitigations (SSBD, STIBP, RSB stuffing, concrete BHI_DIS_S, ITS, Retbleed) are owned by §25 (feature bits from §18); VERW microarchitectural-buffer clears are owned by §19

- [x] Commit: `"kernel/security: eIBRS set-once (SPEC_CTRL), IBPB at context switch, retpoline build flag"`

**Test checkpoint:** On an eIBRS CPU, `IA32_SPEC_CTRL` reads back `baseline | IBRS` permanently on the BSP AND every AP (set-once, no toggle); on non-eIBRS CPUs no SPEC_CTRL write (retpoline covers them). `objdump` shows no `jmp/call *%reg` or `*mem` in compiler-generated code. `cpu_issue_ibpb()` writes PRED_CMD on a cross-process switch and is a no-op when `!CPU_FEATURE_IBPB`. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 5 spectre/profile suites, 0 failures

> **Notes:**
> - Shipped: eIBRS set-once (`cpu_program_bsp_eibrs` + a SPEC_CTRL per_cpu AP-replay handler) + `cpu_issue_ibpb()` at cross-process scheduler switches + retpoline (`-mretpoline` + `retpoline.asm`) + a swapgs LFENCE in `isr_stubs.asm`.
> - Runs: SPEC_CTRL writes are `msr_try_write` per-CPU (BSP post-IDT, APs via profile replay preserving each AP's own baseline); IBPB is a single hot-path `wrmsr` gated on a structural cross-process user-capable check.
> - Effect: Spectre v2 mitigated -- eIBRS on modern CPUs, retpoline (0 compiler indirect branches, 600 thunks) on legacy; IBPB at domain crossings; swapgs v1 barrier on interrupt entry.
> - Scope boundary: SSBD/STIBP/RSB/BHI/ITS/Retbleed -> §25 (predictor policy; feature bits from §18); MDS/VERW -> §19; `kpti_trampoline` indirect jumps -> §4.
> **Verified:** 2026-06-28 | commit `3cb35221` | 10/11 items | build OK | smoke PASS (KVM 2.59s)
> **Quality reviewed:** 2026-06-28 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 1H+4M fixed | scope: kernel-code-quality

---

## 9. CET Shadow Stack (Kernel Ring 0)

> [!WARNING]
> **Deferred -- blocked on the remaining AP-TSS prerequisite owned elsewhere (Codex design review 2026-06-28; #CP routing shipped 2026-07-18).** Enabling supervisor CET (`CR4.CET` + `S_CET.SH_STK_EN`) is unsafe until it lands, so CET stays detected-but-disabled (the current, safe state -- `CPU_FEATURE_CET_SS` is probed from CPUID.(07H,0):ECX[7] in `cpuid.c`):
> 1. **#CP fault routing (RESOLVED)** -- SHIPPED in TODO-23 §3: `except_common_handler` registers a CET-gated `#CP` (vector 21) handler -- user-mode routes through `ki_dispatch_exception()` (`STATUS_STACK_BUFFER_OVERRUN`+subcode 0x39), kernel-mode calls `KeBugCheckEx(0x139, 0x39)` directly -- so a shadow-stack (or IBT) violation is caught rather than triple-faulting. Registration is gated on `cpu_has(CPU_FEATURE_CET_SS) || cpu_has(CPU_FEATURE_CET_IBT)` (both raise #CP), so it activates automatically once either capability is present. -> XREF: `02-kernel-core/TODO-23-exception-dispatch-seh.md §3`.
> 2. **AP IST shadow stacks** -- `MSR_IA32_INTERRUPT_SSP_TABLE` needs a per-CPU TSS so each AP's #DF/NMI/#PF IST entry gets its own shadow stack; the TSS/IST is BSP-only today (`gdt.c:123` "configures the BSP TSS only"). Enabling CET on APs without this turns any IST-backed exception into a recursive #CP. -> XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §10` (item: "Per-CPU TSS + IST" at line 399).
>
> Design review also corrected the draft below: the supervisor shadow-stack PTE marker is **Dirty (bit 6, `VMM_FLAG_DIRTY`) with Write=0**, NOT "bit 5" (bit 5 is `VMM_FLAG_ACCESSED`); and CET state must be saved as **per-thread `PL0_SSP` on every context switch, NOT via `IA32_XSS`/`XSAVES`** -- `xsave_area`/`fpu_used` are per-*task* and lazy, so XSS-backed CET state would corrupt SSP across same-process thread switches. XSS deferred until XSAVE ownership moves to `struct thread`.

- [x] Structured `#CP` (vector 21) handling -> XREF: `TODO-23 §3` (SHIPPED; CET-gated handler: user-mode routes through `ki_dispatch_exception()`, kernel-mode `KeBugCheckEx()` directly)
- [ ] **(prereq, blocks SMP enable)** Per-CPU AP TSS so `cet_init_interrupt_ssp_table()` can give each AP IST entry its own shadow stack -> XREF: `D01 T09 §10`. Until this + the enable trampoline land, `cpu_enable_cet_ss()` must NOT write `CR4.CET`.
- [x] FIXED (TODO-23 §3 review): `cpuid.c` probed `CPU_FEATURE_CET_SS` from EDX[7]; CET SHSTK is CPUID.(07H,0):ECX[7] per Intel SDM (EDX[7] reserved). Moved to the ECX block so `cpu_has(CET_SS)` is accurate on CET hardware (2 reviewers confirmed)
- [ ] **(enable safety)** CET enable must be a controlled no-return transition (CET-aware trampoline)
  - Seed the active call chain's SSP via the architectural save/restore-token sequence before any normal `RET`, else the first return after `CR4.CET` faults #CP on an empty shadow stack during bring-up
- [ ] Add `CPU_FEATURE_CET_SS` to `CPU_FEATURES_AP_PROBE_MASK` + `cpuid_probe_ap_features()` so per-AP enable gates on each AP's own `cpu_feature_local()` (CET may be P/E-core skewed), mirroring the §8 `SPEC_CTRL` AP-probe pattern
- [ ] MSR definitions:
  ```c
  #define MSR_IA32_S_CET           0x6A2   /* supervisor shadow stack control */
  #define MSR_IA32_PL0_SSP         0x6A4   /* ring-0 shadow stack pointer */
  #define MSR_IA32_INTERRUPT_SSP_TABLE 0x6A8 /* IST shadow stack table */
  #define S_CET_SH_STK_EN          (1ULL << 0)
  #define S_CET_WR_SHSTK_EN        (1ULL << 1)  /* WRSS instruction enable */
  #define S_CET_ENDBR_EN           (1ULL << 2)  /* IBT control (§10) */
  ```
- [ ] `cpu_enable_cet_ss()`:
  1. `cpu_set_cr4_bit(CR4_CET)`: enable CET in CR4
  2. `wrmsr(MSR_IA32_S_CET, S_CET_SH_STK_EN | S_CET_WR_SHSTK_EN)`
  3. Ensure `MSR_IA32_PL0_SSP` is set to the initial kernel shadow stack page's last 8 bytes (top of the shadow stack)

- [ ] **(deferred -- do NOT ship in MVP)** CET xstate reservation: set `IA32_XSS` bits 11/12 (CET_U/CET_S) for `XSAVES`/`XRSTORS` -- SUPERVISOR state via `IA32_XSS`, NOT `XCR0`. Owns the CET xstate window deferred from `01-boot-platform/TODO-09 §5`
  - Blocked: `xsave_area`/`fpu_used` are per-*task* and lazy (`struct task`); XSS-backed CET state corrupts SSP across same-process thread switches. Move XSAVE ownership to `struct thread` first, then convert the scheduler to `XSAVES`/`XRSTORS` with XSS-aware masks/sizing. MVP saves `PL0_SSP` per-thread directly instead (see Context switch item)
- [ ] Each kernel thread needs a shadow stack: one 4 KiB page per thread, marked `PTE_USER=0`, `PTE_NX=1`, and the special **supervisor shadow stack token** format (bit 1 of the 8-byte token set indicates this is the bottom of the shadow stack)
- [ ] `cet_alloc_shadow_stack(thread)`: `pmm_alloc_contiguous(1)`; write token at page end; map via a new `VMM_FLAG_SHADOW_STACK` encoding; add a VMM encoding unit test before any `CR4.CET` enable
  - Encoding = **`VMM_FLAG_DIRTY` (bit 6) set + Write (bit 1) clear + `VMM_FLAG_NX`** (Intel SDM supervisor-SHSTK marker). The draft's "bit 5" was wrong -- bit 5 is `VMM_FLAG_ACCESSED`. Processor then enforces SHSTK semantics (only `RSTORSSP`/`SAVEPREVSSP`/`WRSS` write)
- [ ] On kernel thread creation in `src/kernel/sched/task.c` (`task_create()`): allocate shadow stack; set `task->shadow_stack_top`
- [ ] Context switch: save/restore `MSR_IA32_PL0_SSP` per thread

- [ ] Windows uses IST entries in the TSS for critical exceptions (#DF, #PF, NMI); each IST entry must also get a shadow stack in the `MSR_IA32_INTERRUPT_SSP_TABLE` (8-entry table, one VA per IST slot)
- [ ] `cet_init_interrupt_ssp_table()`: allocate 8 shadow stack pages; write their top VAs into the 64-byte `INTERRUPT_SSP_TABLE` structure; write the table physical address to `MSR_IA32_INTERRUPT_SSP_TABLE`

- [ ] Commit: `"kernel/security: CET shadow stack: CR4.CET, S_CET MSR, per-thread SSP allocation"`

**Test checkpoint:** `CR4.CET` set; `MSR_IA32_PL0_SSP` tracks per-thread shadow stack; forged return triggers `#CP` not hijacked RIP. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal (CET-capable CPU).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | deferred -- VMM-SHSTK-encoding + `cpu_enable_cet_ss` gating-off tests land with the enable; CET is detected-but-disabled today so no enable surface to assert yet.
>
> **Notes:**
> - Deferred 2026-06-28 (updated 2026-07-18): `#CP` routing (`TODO-23 §3`) is now SHIPPED (CET-gated handler); the remaining CET-enable blocker is the per-CPU AP TSS for IST shadow stacks (`D01 T09 §10`). CET stays detected-but-disabled (the safe state) until that lands.
> - Design corrected the draft: supervisor-SHSTK PTE marker is Dirty/bit-6 + Write-clear (not "bit 5" = Accessed); CET state saves per-thread `PL0_SSP` on context switch, not `IA32_XSS`/`XSAVES` (XSAVE is per-task + lazy).
> - When unblocked: add `CET_SS` to the AP probe mask, `cpu_enable_cet_ss()` gated per-AP via `cpu_feature_local()` (mirrors §8 `SPEC_CTRL`), CET-aware no-return enable trampoline, then per-thread + IST shadow stacks.
>
> **Deferred:** [High] AP IST shadow stacks need per-CPU TSS -> XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §10` (item: "Per-CPU TSS + IST" at line 399). (The structured `#CP` routing blocker was resolved by `TODO-23 §3`.)

---

## 10. CET Indirect Branch Tracking (IBT / ENDBR64)

> [!WARNING]
> **Deferred -- inherits §9's CET-enable blockers (Codex design review 2026-06-28).** IBT enforcement (`S_CET.ENDBR_EN`) rides the same `CR4.CET` enable that §9 defers, and an indirect branch to a target lacking `ENDBR64` raises `#CP` -- so IBT cannot be enabled until §9's CET enable path lands (this section's `cpu_enable_cet_ibt()` ORs `ENDBR_EN` into the §9 `S_CET` write). The former `#CP` routing prereq is now SHIPPED in `02-kernel-core/TODO-23 §3` (CET-gated handler; user-mode via `ki_dispatch_exception()`, kernel-mode `KeBugCheckEx()` directly). The `-fcf-protection=branch` ENDBR64 emission + asm-stub `ENDBR64` audit (`ap_trampoline.asm`, ISR/IDT stubs) is harmless-when-off instrumentation but is held with the enable to avoid shipping dead, unenforced landing pads.

- [ ] Add `-fcf-protection=branch` to kernel `CFLAGS` (Clang 19 supports this); the compiler emits `ENDBR64` at the start of every function and every valid indirect call/jump target
- [ ] Verify: `objdump -d build/kernel.elf | grep endbr64 | wc -l`; must be > 0 (non-zero); count should match approximate function count
- [ ] Assembly files (`src/kernel/smp/ap_trampoline.asm`, ISR stubs, IDT stubs): manually add `endbr64` at each entry point that is reached via an indirect branch; NASM opcode: `db 0xF3, 0x0F, 0x1E, 0xFA`

- [ ] `cpu_enable_cet_ibt()`: add to `cpu_enable_cet_ss()` call sequence: `wrmsr(MSR_IA32_S_CET, rdmsr(MSR_IA32_S_CET) | S_CET_ENDBR_EN)`
- [ ] Legacy code mode: if a code region is loaded that does not have `ENDBR64` instructions (e.g., a legacy driver), temporarily disable IBT via `MSR_IA32_S_CET.NO_TRACK_EN` for that execution context; re-enable after (requires driver annotation `MODULE_FLAG_NO_IBT`)
- [ ] `ENDBR_EN` should be enabled after all kernel code is loaded and verified; setting it before loading a module without ENDBR64 would immediately fault
- [ ] Ring-3/per-process CET: consume per-binary CET flags (EIF `EIF_FLAG_CET_IBT`/`EIF_FLAG_CET_SHSTK`, PE CET_COMPAT, ELF `.note.gnu.property`) to program per-process `U_CET`/`PL3_SSP` once user CET lands -> XREF: `TODO-20 §10`

- [ ] Commit: `"kernel/security: CET IBT: ENDBR64 in kernel build, S_CET.ENDBR_EN activation"`

**Test checkpoint:** `objdump` shows `endbr64` prologues; indirect jump to target without `ENDBR64` faults when IBT on; `S_CET_ENDBR_EN` set only after stub audit. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | deferred -- ENDBR64-count + IBT-enable tests land with §9's CET enable path.
>
> **Notes:**
> - Deferred 2026-06-28 (updated 2026-07-18): IBT enforcement rides §9's deferred `CR4.CET`/`S_CET` enable and raises `#CP` on missing `ENDBR64`. With `#CP` routing now shipped (`TODO-23 §3`), it inherits only §9's remaining blocker: the CET enable path (+ AP TSS).
> - Held to avoid shipping dead, unenforced ENDBR64 landing pads; the `-fcf-protection=branch` flag + asm-stub audit land together with the enable.
>
> **Deferred:** [High] CET IBT enable inherits §9's remaining blocker -> XREF: this TODO §9 (CET enable path + AP TSS). (The `#CP` routing prereq was resolved by `02-kernel-core/TODO-23-exception-dispatch-seh.md §3`.)

---

## 11. Kernel Heap Hardening: Cookies & Redzones

- [x] Combined 48-byte `block_header` in `src/kernel/mm/heap.c` (size/next/cookie/req_size/tag/is_free/_pad/redzone_front)
  - `_Static_assert` pins size==48, %16==0, redzone_front@40 so the returned user pointer is 16-byte aligned (fixes the prior inconsistent 8/16 alignment for DMA / cast-to-struct callers)
- [x] `kmalloc(size)`: overflow guard + first-fit `kmalloc_locked` + cookie/redzone stamping; returns the 16-aligned pointer
  - Rejects `size > KMALLOC_MAX` (256 MiB) before any size+overhead arithmetic; stamps `cookie = HEAP_COOKIE_SECRET ^ header`, `redzone_front`, `redzone_back` at user+req_size
- [x] `kfree(ptr)`: `heap_classify` validation, then `KeBugCheckEx(BUGCHECK_IOS_HEAP_CORRUPTION)` on any mismatch -> XREF: `02-kernel-core/TODO-27-crash-dump-generation.md §1`
  - Checks `heap_owns` + 16-align + header-in-range BEFORE deref (wild/misaligned), then cookie + both redzones; double-free stays a silent no-op
- [x] **(design review)** irqsave `s_heap_lock` serializes kmalloc/kfree/krealloc/split/coalesce/heap_get_free (heap was lockless -- SMP gap)
  - On corruption `s_heap_poisoned` is set UNDER the lock so no CPU mutates the known-corrupt heap in the unlock->`KeBugCheckEx` window
- [x] `kmalloc_zeroed(size)`: always zeroes the user region (token / security-descriptor structs)
- [x] `kmalloc_tagged(size, tag)` + `kfree_tagged(ptr, tag)`: 4-byte pool tag in the header; tag mismatch on free -> BugCheck (mixed-pool UAF). Tag field always present; plain `kmalloc` tags 0
- [x] `init_on_alloc` default-on (`HEAP_INIT_ON_ALLOC`, Linux `INIT_ON_ALLOC` parity): zeroes every `kmalloc` user region; compile-time knob to disable
- [/] zero-on-free (`HEAP_ZERO_ON_FREE`): scrubs freed user data; coded but DEFAULT-OFF -- enabling it surfaces a pre-existing FS use-after-free (next item) that zeros a still-referenced `i_size`
- [x] Scope boundary: SLUB-style freelist hardening (pointer encoding, randomization, quarantine, per-CPU freelists) is owned elsewhere -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md`; §11 owns the kmalloc cookie/redzone/zeroing tier

- [x] Commit: `"kernel/mm: kmalloc cookie + redzone + irqsave lock hardening, kmalloc_zeroed/tagged, init_on_alloc"`

**Test checkpoint:** `kmalloc` returns 16-aligned pointers; init-on-alloc + `kmalloc_zeroed` regions read back zero; tagged alloc + matched-tag free round-trips; exact-size write does not trip redzones. Corruption paths (cookie/redzone/wild-free -> `BUGCHECK_IOS_HEAP_CORRUPTION`) are validated on bare metal -- they halt, so cannot be unit-tested. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | 139 kernel + 16 user-mode pass; smoke boots clean (knob off).
>
> **Notes:**
> - Shipped `src/kernel/mm/heap.c` hardening: 48-byte combined header (cookie + redzones), irqsave `s_heap_lock`, `heap_classify` validation -> `BUGCHECK_IOS_HEAP_CORRUPTION`, `kmalloc_zeroed`/`_tagged`, init-on-alloc, overflow + wild-pointer guards.
> - Design review (2 passes) adoptions: combined header, irqsave lock (heap was lockless), 16-align pinned by `_Static_assert`, `s_heap_poisoned` set-under-lock closing the unlock->BugCheck race; evidence in the commit message.
> - zero-on-free behind `HEAP_ZERO_ON_FREE` (default off): enabling it surfaced a pre-existing IXFS/vfs use-after-free (tracked as the open item above); the scrub is correct, the FS UAF must be fixed first.
> - Scope boundary: SLUB-style per-CPU freelist hardening owned by `03-memory-concurrency/TODO-03-advanced-allocator.md`.
>
> **Verified:** 2026-06-28 | commit `1a501a61` (+ review fixes) | 8/10 items | build OK | tests 139/139 + smoke PASS
> **Accepted:** [M] `coalesce_free_blocks` is O(n) per free (pre-existing flat-list cost; the O(n) membership walk this review introduced was replaced with an O(1) block-start bitmap) -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §4` (item: "`slab_free(cache, ptr)` -- push to per-CPU freelist" at line 171 -- O(1) free supersedes the flat-list coalesce)
> **Quality reviewed:** 2026-06-28 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 9H+4M+1L fixed, 1M accepted-XREF | scope: kernel-code-quality

---

## 12. Stack Canaries (`-fstack-protector-strong`)

- [x] `Makefile:23`: `-fno-stack-protector` -> `-fstack-protector-strong -mstack-protector-guard=global` (kernel CFLAGS only; boot/userland flag sets untouched)
  - `-mstack-protector-guard=global` makes the compiler read our global `__stack_chk_guard` SYMBOL, not a `%fs`/`%gs` TLS slot we have no per-CPU storage for. 1737 functions check the cookie post-build (objdump)
- [x] Define `uintptr_t __stack_chk_guard` + `__stack_chk_fail` in `src/kernel/security/stack_canary.c` (kernel is `-nostdlib`, no libssp, so both must be defined exactly once or the link fails)
- [x] Seed `__stack_chk_guard` from `rdrand_bytes()` (crypto-grade, usable right after Phase-0 CPUID); `RDTSC ^ __kernel_start ^ const` fallback when RDRAND absent (TCG)
  - csprng is NOT used (it seeds late in Phase 1 and a mid-boot re-seed would self-fault any live canary frame); single early RDRAND/TSC seed instead (design review)
- [x] `canary_init()` is `__attribute__((no_stack_protector))` + a pure `canary_massage()` (low byte 0 = terminator canary; bit 63 set = cookie never 0 / high byte non-zero)
- [x] Call `canary_init()` in `kernel_main` (`main.c`) after `boot_phase0` (CPUID up), before `boot_phase1` (design-review critical fix)
  - NOT inside a stack-protected phase fn (its epilogue would compare the new cookie against a prologue-saved zero and self-fault); `kernel_main` never returns, so no live frame spans the cookie write
- [x] `__stack_chk_fail()` (noreturn) -> `KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE, STATUS_STACK_BUFFER_OVERRUN(0xC0000409), ...)`
- [x] `KeBugCheckEx` triggers `panic_screen()` + crash dump via the existing bugcheck path -> XREF: `02-kernel-core/TODO-27-crash-dump-generation.md §5`

- [x] Commit: `"kernel/security: -fstack-protector-strong, RDRAND canary init, __stack_chk_fail"`

**Test checkpoint:** build uses `-fstack-protector-strong` (1737 cookie-checking functions); `__stack_chk_guard` seeded non-zero with low byte 0 + bit 63 set after boot; `canary_massage` invariants hold. Intentional overflow -> `BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE` is a bare-metal/boot check (it halts, not unit-testable). Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | canary massage + guard-seeded + seed-desc-bounds suites pass; 12959 kernel + 16 user-mode pass; smoke boots clean with build-wide canaries.
>
> **Notes:**
> - Shipped `src/kernel/security/stack_canary.c` + header: global `__stack_chk_guard`, `no_stack_protector` `canary_init()` (RDRAND seed, TSC fallback), pure `canary_massage`, `__stack_chk_fail` -> BugCheck. Makefile flag swap is build-wide.
> - Bootstrap order (design-review critical fix): `canary_init()` runs in non-returning `kernel_main` after Phase-0 CPUID, before `boot_phase1`; objdump confirms no self-check; smoke boots clean.
> - Design adoptions: `-mstack-protector-guard=global`, RDRAND-primary (csprng deferred), `no_stack_protector` on `canary_init`; per-finding evidence in the commit message.
> - Scope boundary: single global NT-style cookie; per-task/%gs canary + the deliberate-overflow BSOD test are not owned here.
>
> **Verified:** 2026-06-28 | commit `615abb2e` (+ review fixes) | 7/7 items | build OK | tests 12959/12959 + smoke PASS
> **Quality reviewed:** 2026-06-28 | Codex 6x (design, adversarial, consistency, perf, re-adversarial) | 5H+1M fixed | scope: kernel-code-quality

---

## 13. Kernel Stack Guard Pages

> [!NOTE]
> Guard pages are ALREADY installed on every kernel stack (the core of this section). The remaining work -- routing a guard `#PF` to the specific NT stop code, plus two latent guard-table bugs the design review surfaced -- needs frame-aware bugcheck infrastructure + an SMP-safe guard table and is tracked as the `[ ]` items below.

- [x] Every kernel thread / AP / uthread stack is allocated as N+1 pages and the bottom page is guarded via `vmm_install_guard_page(virt, label)` (`vmm.c:1233`: splits the 2 MiB huge page, clears the PTE, registers `(addr,label)` in `guard_pages[]`)
  - Call sites: kernel task stacks (`task.c:447,1684`), AP stacks (`smp.c:391`), uthread (`task.c:2778`), heap end (`heap.c:457`)
- [/] On `#PF` in a guard VA, `page_fault_handler` (`vmm.c`) checks `guard_page_lookup(cr2)` FIRST and renders a descriptive labeled BSOD via `panic_screen` (distinguishes stack overflow from a generic fault)
  - The SPECIFIC NT stop code `BUGCHECK_KERNEL_STACK_INPAGE_ERROR` (0x77) is NOT yet recorded -- `panic_screen` stores `bugcheck_code=0` for framed faults and keys the stop code off `frame->int_no`; needs the frame-aware bugcheck path below
- [x] Each IST stack (`#DF`/`NMI`/`MCE`) has a guard page below it via the same helper (`gdt.c:97`); prevents a nested-exception stack overflow from silently corrupting memory
- [ ] **(design review)** Frame-aware guard bugcheck: a guard `#PF` records the explicit NT stop code, not the PF err-code -> XREF: `02-kernel-core/TODO-27-crash-dump-generation.md §5`
  - Add `vmm_install_guard_page_ex(virt, label, BUGCHECK_CODE)` + a `bugcheck` field on the guard entry; route the fault to a frame-aware bugcheck entry that stores `g_last_bugcheck` + renders it (stacks/IST/AP -> `KERNEL_STACK_INPAGE_ERROR` 0x77, heap -> `IOS_HEAP_CORRUPTION`), keeping CR2 + PF err-code as evidence. No label-substring classification
- [ ] **(design review, latent bug)** Guard-table capacity + fail-fast registration
  - `MAX_GUARD_PAGES=32` (`vmm.c:530`) is undersized vs `TASK_MAX(32)` + APs + IST + heap, and `guard_page_register` is void + silently drops when full -> an untracked unmapped guard reports a generic fault + uninstall no-ops. Size to worst-case; register returns status; `vmm_install_guard_page` rolls back the unmap on failure; critical stack-guard install failure is fatal
- [ ] **(design review, latent bug)** SMP-safe guard table
  - The table is mutated at RUNTIME (task create/exec/uthread install + cleanup uninstall, `task.c`/`smp.c`) and read lock-free by the `#PF` handler on any CPU; compact-on-delete can race the lookup. Use a preallocated/append-only table + atomic active flags + release/acquire so a fault lookup never sees a partially-moved entry

- [ ] Commit: `"kernel/security: guard pages below kernel stacks and IST stacks"`

**Test checkpoint:** kernel/AP/IST stack guards installed (guard VA `#PF` -> labeled BSOD via `guard_page_lookup`, validated on bare metal -- it halts). The NT 0x77 stop-code + the capacity/SMP-safe table land with the `[ ]` items above. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | `test_vmm.c` guard-page install/lookup test passes; guard-fault BSOD is bare-metal-validated (halts).
>
> **Notes:**
> - Guard pages on all kernel/AP/IST stacks + heap are SHIPPED via `vmm_install_guard_page` (`vmm.c:1233`); `page_fault_handler` catches a guard hit first and renders a labeled BSOD.
> - Deferred 2026-06-28: the NT 0x77 stop-code routing needs a frame-aware bugcheck path; the design review also found two pre-existing latent guard-table bugs (32-entry capacity + runtime-mutation SMP race).
> - Scope boundary: crash-dump emission of the stop code is owned by `TODO-27 §5`; the bugcheck taxonomy lives in `bugcheck.h`.

> **Deferred:** [H] NT guard stop-code + frame-aware bugcheck path -> XREF: this section (item: "Frame-aware guard bugcheck" above). [H] guard-table capacity + fail-fast register (latent) -> XREF: this section (item: "Guard-table capacity + fail-fast registration" above). [M] SMP-safe guard-table publication (latent) -> XREF: this section (item: "SMP-safe guard table" above).

---

## 14. KASLR: RDRAND Kernel Load Address

> [!WARNING]
> BLOCKED on `02-kernel-core/TODO-33-higher-half-kernel-relocation` -- per its Goal, higher-half relocation "is the foundation that unblocks KASLR". The kernel currently links/loads in low memory; sliding that layout would mix relocation, address-space migration, and KASLR in the wrong owner and risks a non-bootable image. The base-slide *mechanics* (ELF relocation, `-fPIE`, bootloader `R_X86_64_64` apply) belong to TODO-33; this section owns KASLR *policy* (entropy, `kaslr_slide` in boot_info, slide-in-dump, tests) on that foundation.

- [ ] Module/driver load-address ASLR is a separate entropy domain owned by the module loaders -> XREF: `02-kernel-core/TODO-05-kernel-module-system.md` (kernel `.kmod`) + `10-platform-services/TODO-07-win32-pe-loader.md` (PE drivers)
- [ ] The bootloader (`src/boot/uefi/bootx64.c`) currently loads the kernel ELF at its linked virtual base. For KASLR (mechanics gated on TODO-33 higher-half relocation):
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

> **Deferred:** [H] blocked on higher-half relocation (see section WARNING) -- sliding the current low-linked layout mixes relocation + address-space migration + KASLR in the wrong owner and risks a non-bootable image. The base-slide mechanics belong to TODO-33; KASLR policy lands on that foundation. -> XREF: 02-kernel-core/TODO-33-higher-half-kernel-relocation §3 (item: "Commit: `\"boot: higher-half page-table bring-up + direct map + high-half jump\"`")

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

> **Deferred:** [M] blocked on prerequisite infrastructure -- the cached-signing-level syscalls need the code-integrity trust-policy engine, and `NtCreateEnclave` needs an SGX enclave subsystem (neither exists yet). Not a kernel-hardening primitive; this is syscall surface over those subsystems. -> XREF: 02-kernel-core/TODO-19-code-integrity-trust-policy.md (signing-level policy) + the SGX enclave subsystem owner (no TODO yet -- file when SGX support is scoped)

---

## 16. Secure Boot Lockdown Enforcement

Kernel-side enforcement policy gated on the canonical Secure Boot state. Owns the `boot.conf` lockdown knob reserved at the top of this TODO. Consumes the canonical `uefi_secureboot_*` state API from `01-boot-platform/TODO-02 §5` (detection) + `§15` (drift); does NOT re-read SB variables. -> XREF: `01-boot-platform/TODO-27-uefi-advanced.md §5` (deferred here -- §5's enforcement-policy core blocks on the kernel lockdown mechanism, which is this section).

**Files:** `src/kernel/uefi_runtime.c`, `src/kernel/uefi_vars.c`, `include/kernel/uefi_runtime.h`, `include/kernel/boot_info.h` (boot_config), `src/boot/uefi/bootx64.c` (boot.conf parse)

> [!WARNING]
> **Deferred 2026-06-28.** Half of this section's enforcement targets do not exist yet, and the implementable half needs careful security-critical changes to the existing lockdown-policy + firmware-variable paths (3 design-review requirements below). Split out for a focused effort rather than a rushed bolt-on.
> - **Blocked (no infrastructure to gate):** "disable unsigned `.kmod` load + raw user MSR/IO-port writes" has no module loader (-> XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md`) and no user MSR/IO write syscall to gate; "wire audit-log to PE-loader verification call sites" has no PE signature-verification call site (`pe.c` does only structural validation -> XREF: `10-platform-services/TODO-07-win32-pe-loader.md`).
> - **Design-review requirements for the implementable half:** (1) lockdown source-of-truth -- the `policy.lockdown` ratchet ALREADY seeds INTEGRITY on `secure_boot_enabled` (`policy_lock.c:440`); the `secure_boot_enforce` knob must gate THAT seed (or future gates read the level while the knob is off), and future gates consume `kernel_lockdown_level_get()`, NOT a parallel `g_system_state` bit. (2) drift fail-open -- the drift case is SB 1->0, so `kernel_lockdown_engage` from the drift edge must gate on the BOOT-SNAPSHOT SB + `secure_boot_enforce`, NOT live `uefi_secureboot_enabled()` (which is now 0). (3) var-guard scope -- refuse only DESTRUCTIVE PK/KEK/db/dbx clears (empty/delete) in `deployed_mode==1`, not all writes, so authenticated db/dbx revocations + key rotation still flow to firmware.

- [ ] `secure_boot_enforce` `boot.conf` key (snake_case) gating the lockdown seed (single source of truth)
  - Add to `struct boot_config` + mirror (`_reserved` byte, no `BOOT_INFO_VERSION` bump) + `bootx64.c` parse (clone the `firmware_rng` block); gate the `policy.lockdown=INTEGRITY` seed in `policy_lock.c:440` on it
- [ ] `kernel_lockdown_engage(reason)` DRIVES the existing `policy.lockdown` ratchet (not a parallel scalar)
  - `kernel_policy_set(KERNEL_LOCKDOWN_INTEGRITY, POLICY_CALLER_KERNEL)` (RATCHET, sticky-upward, idempotent); gated on (boot-snapshot SB authoritative) + `config.secure_boot_enforce`
- [ ] `uefi_secureboot_deployed_mode()` / `uefi_secureboot_audit_mode()` one-line accessors over `s_sb_deployed_mode` / `s_sb_audit_mode` (`uefi_runtime.c:1076`)
- [ ] DeployedMode write-protection in `uefi_var_set()` (`uefi_vars.c:33`), scoped to destructive clears
  - Refuse a DESTRUCTIVE clear (size==0 / delete) of PK/KEK/db/dbx when `deployed_mode==1` -> `STATUS_ACCESS_DENIED`, matching `s_sb_vars[]` name/GUID (`tpm_sb_reconcile.c:121`; `efi_guid_t` == `struct boot_uefi_guid`). Authenticated updates still pass to firmware
- [ ] Drift-triggered lockdown: from the revalidation worker drift 0->1 edge (`uefi_runtime.c:1882`), call `kernel_lockdown_engage("drift")` -- gated on the BOOT-SNAPSHOT SB, not live SB
- [ ] AuditLog infra: a `uefi_secureboot_audit_log(event)` API + `HKLM\SYSTEM\SecureBoot\AuditLog` registry writer (clone the `Vars` writer `uefi_runtime.c:1983`); ready for the PE-verification call sites when they exist
- [ ] Serial log: `[SecureBoot] policy: enforce=%u audit=%u deployed=%u`
- [ ] Commit: `"kernel/security: Secure Boot lockdown enforcement consuming canonical state"`

**Test checkpoint:** Setup Mode (`deployed_mode==0`): no write-protection. Enrolled PK + DeployedMode: `uefi_var_set(PK, empty)` -> `STATUS_ACCESS_DENIED`; an authenticated db update still passes. `secure_boot_enforce=1` + SB active: `kernel_lockdown_engage` raises `kernel_lockdown_level_get()` to INTEGRITY; serial shows the policy line. Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Notes:**
> - Deferred 2026-06-28: the SB lockdown ENFORCEMENT gates (.kmod/MSR-IO disable, PE-verification audit wiring) are blocked on non-existent infrastructure; the implementable half needs careful security-critical policy_lock.c + firmware-var changes.
> - Design review found 3 requirements baked into the items: single lockdown source-of-truth (gate the seed on the knob), drift fail-open fix (boot-snapshot gating), and a var-guard scoped to destructive clears only.
> - Scope boundary: the existing lockdown ratchet lives in `policy_lock.c`; SB state in `uefi_runtime.c`; the blocked gates are owned by `D04 T05` (modules) + `D10 T07` (PE loader).

> **Deferred:** [H] SB lockdown enforcement gates (.kmod/MSR-IO disable) -> XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md` (module loader is the gate owner). [H] audit-log wiring to PE signature verification -> XREF: `10-platform-services/TODO-07-win32-pe-loader.md` (no PE verify call site exists). [M] implementable subset (knob/engage/var-guard/drift/AuditLog) with the 3 design requirements -> XREF: this section (the `[ ]` items above).

---

## 17. Kernel-Image W^X: .text Read-Only + .rodata RO-After-Init

Complete W^X on the static kernel image (Linux `STRICT_KERNEL_RWX` / `mark_rodata_ro()`, Win11 HVCI). §1 marks non-code pages NX; this clears the WRITABLE bit on kernel `.text` and `.rodata` so a kernel write primitive cannot patch executable code or constant data. Distinct from `03-memory-concurrency/TODO-01 §2` (dynamic mprotect W^X policy). Shipped via `vmm_set_ro()` (the split-aware helper; the draft's `vmm_make_range_readonly`/`vmm.c:456` never existed).

- [x] Linker (`src/boot/linker.ld`): `__rodata_start`/`__rodata_end` (pinned to `.data` start) bracket `*(.rodata .rodata.*)` + the const `.bootproto`/`.reloc`/`.firmware_capsule_refused` into one drift-proof RO span
- [x] `kernel_wx_protect()` (`src/kernel/security/wx.c`): `vmm_set_ro(__text_start, size)` clears WRITABLE on `.text` (stays executable). The draft's `vmm_make_range_readonly` does not exist; `vmm_set_ro` is the real split-aware helper
- [x] `kernel_rodata_protect()`: `vmm_set_ro` over `[__rodata_start, __rodata_end)` (already NX via `vmm_apply_nx_policy`, so only the WRITABLE clear is new); span covers the const immutable sections through the `.data` boundary
- [x] Called in `boot_phase1` (`boot_interrupts.c:194`) after `cpu_pin_control_regs` (CR0.WP pinned), single-CPU before Phase 2 `smp_init` (`vmm_set_ro` is local-invlpg-only); fail CLOSED via `boot_halt` if either returns nonzero
- [x] `.text` patch-site audit: none exist (`idt.c:261` is a comment; KPTI/AP trampolines are in separately-allocated pages outside `__text_start..__text_end`); the CR0.WP-toggle bracket for a future live-patch path is documented in `wx.c`
- [x] Serial log: `[wx] kernel image: .text RO (%lu KiB)` + `.rodata RO (%lu KiB)`
- [/] `.rodata` RO-after-init `__ro_after_init` class: plain const `.rodata` is RO now; the init-mutable-then-RO class is deferred (next item)
- [ ] **(deferred)** `__ro_after_init` section convention: a `.init.data` output section + `__attribute__((section))` macro + `__init_end` so init-mutable globals go RO after boot. No such convention exists today -> XREF: this section
- [x] Commit: `"kernel/mm: STRICT_KERNEL_RWX -- .text RO, .rodata RO-after-init, W^X on the kernel image"`

**Test checkpoint:** `vmm_query_flags(__text_start)` shows WRITABLE clear + NX clear (executable); `vmm_query_flags(__rodata_start)`, a string-literal address, and `&firmware_capsule_refusal_sentinel` all show WRITABLE clear (orphan + immutable alloc sections covered); `[wx]` serial lines show non-zero RO counts; `.text`/`.rodata` write faults are bare-metal-validated (they #PF). Test on: QEMU WHPX, QEMU TCG, VirtualBox; bare metal.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | 4 W^X suites (.text RO, .rodata RO, orphan-section, immutable-alloc-section) pass; 148 kernel + 16 user-mode pass; smoke boots clean with W^X active.
>
> **Notes:**
> - Shipped `src/kernel/security/wx.c` + `wx.h`: `kernel_wx_protect`/`kernel_rodata_protect` clear WRITABLE on `.text` (1995 KiB) + `.rodata` (940 KiB) via `vmm_set_ro`, single-CPU in Phase 1 after the CR0.WP pin, fail-closed via `boot_halt`.
> - Adversarial fixes: fail-open W^X calls now `boot_halt` on failure; `__rodata_end` pinned to `.data` start so `.bootproto`/`.reloc`/`.firmware_capsule_refused` (const, never-written) are in the RO span (drift-proof bracket, not name enumeration).
> - Review fix: `cpu_pin_control_regs` now forces CR0.WP on (BSP + every AP) before pinning so the read-only PTE bits actually bind; `cpu_wp_enforced()` is a fail-closed guard at the W^X call site.
> - Scope boundary: dynamic `mprotect` W^X policy is `03-memory-concurrency/TODO-01 §2`; the `__ro_after_init` init-mutable class is the deferred item above (needs a section convention).

> **Verified:** 2026-06-28 | commit `4e5d48a1` (+ review fixes) | 6/8 items | build OK | smoke PASS (KVM 2.75s), security 1010/16 + mm W^X 4 suites pass
> **Deferred:** [M] `__ro_after_init` init-mutable-then-RO class -> XREF: this section (item: "`__ro_after_init` section convention" above -- needs a `.init.data` section + attribute macro that does not exist in the tree).
> **Quality reviewed:** 2026-06-28 | Codex 6x (design, adversarial, consistency, perf, re-adversarial) | 3H+1M+1L fixed, 1 deferred | scope: kernel-code-quality + boot-code-quality

---

## 18. CPU Feature-Flag 128-bit Expansion (`cpu_feature_mask_t`)

`g_cpu.flags` is a single `uint64_t` and `CPU_FEATURE_COUNT=64` is FULL (§8 took bits 62/63). The predictor-policy mitigations in §25 (SSBD, STIBP, BHI, ITS, Retbleed) need new feature bits, so the feature surface must widen to 128 bits FIRST. A single canonical bitset type with helpers -- not ad-hoc `uint64_t[2]` plus scalar `(1ULL << feat)` masks -- avoids the silent high-word truncation that would otherwise let AP validation publish an over-broad global mask or skip a mitigation on one CPU. (Split from the original combined §18 per design review; the mitigations are §25.) -> XREF: §25 (consumes the new bits), `D02 T09 §6` (AP feature validation).

- [x] `cpu_feature_mask_t { uint64_t w[2]; }` in `cpuid.h` + 8 inline helpers (`set`/`clear`/`test`/`and`/`andnot`/`subset`/`iszero`/`eq`; word=`feat>>6`, bit=`feat&63`); `_Static_assert(CPU_FEATURE_COUNT <= 128)`
- [x] `g_cpu.flags` + `per_cpu_data.features` (`smp.h` now includes `cpuid.h`) became `cpu_feature_mask_t`; `cpu_has` routes through `cpu_feature_test` -- all 264 call sites unchanged
- [x] `CPU_FEATURES_REQUIRED_MASK` / `CPU_FEATURES_AP_PROBE_MASK` are compound-literal `cpu_feature_mask_t` built by the X-macro `CPU_FEAT_W0_`/`W1_` word/bit split (shift-guarded); no bare `1ULL << CPU_FEATURE_*` masks remain
- [x] `set_flag_if`, `cpuid_probe_ap_features` (returns `cpu_feature_mask_t`), the direct `g_cpu.flags |=` in `cpuid.c`, and `cpu_feature_local` rewired through the helpers
- [x] AP intersection + `s_global_feature_mask` use the 2-word helpers; the struct mask publishes via an `s_global_mask_published` release/acquire flag; `cpu_feature_global_has(feat)` replaces `cpu_feature_global_mask()`
- [x] Tests (`test_cpu_security.c`, `TEST_CAT_X86`): word/bit split round-trips bits 63/64; a BSP-only synthetic high-word (bit 100) clears in `cpu_feature_and`; required-subset invariants via `cpu_feature_subset`
- [x] Commit: `"kernel/cpu: expand CPU feature flags to 128-bit cpu_feature_mask_t"`

**Test checkpoint:** `cpu_feature_test`/`set`/`andnot` round-trip across the 64-bit word boundary (bits 63 and 64); a BSP-only synthetic high-word feature is cleared by the intersection AND (no over-broad publish); the required mask is a subset of the BSP flags and of the probe mask; `[smp]` serial shows the intersection as `w1:w0`. Test on: QEMU WHPX, QEMU TCG (multi-CPU); bare metal.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 2 new feature-mask suites (word/bit split, high-word intersection) pass; 185 x86 + 16 user-mode pass; smoke boots clean (intersection `0x0:...`, w[1]=0 with no high-word features yet).
>
> **Notes:**
> - Shipped `cpu_feature_mask_t` (128-bit, `cpuid.h`) + 8 inline helpers; `g_cpu.flags`/`per_cpu_data.features`/the X-macro masks all migrated; `cpu_has` unchanged at its 264 call sites (routes through the one inline).
> - `s_global_feature_mask` is a struct (no 16-byte x86 atomic), so visibility rides a separate `s_global_mask_published` release/acquire flag; `cpu_feature_global_has(feat)` replaces `cpu_feature_global_mask()` and returns 0 until finalize publishes.
> - Scope boundary: infrastructure ONLY -- adds no feature bits (w[1] stays 0 in production); the predictor-policy mitigations that populate the high word are §25.

> **Verified:** 2026-06-28 | commit `031c83f0` (+ review fixes) | 6/6 items | build OK | smoke PASS (KVM 2.67s), x86 185/16 + 2 new feature-mask suites
> **Quality reviewed:** 2026-06-28 | Codex 6x (design, adversarial, consistency, perf, re-adversarial) | 1M fixed | scope: kernel-code-quality

---

## 19. Microarchitectural Data-Sampling Clears (MDS / TAA / SRBDS / MMIO)

VERW/MD_CLEAR buffer flush on every kernel-to-user return (Linux `mds`/`tsx_async_abort`/`srbds`/`mmio_stale_data`, Win11 microcode). Without it the kernel leaks stale fill/store/load-port data on each return. HIGH -- currently a complete blind spot.

- [x] `CPU_FEATURE_MD_CLEAR`=64 (`CPUID.(7,0):EDX[10]`, first word-1 feature of the §18 mask); `cpu_decide_mds()` reads `IA32_ARCH_CAPABILITIES` (new `ARCH_CAP_*` consts) once per CPU post-IDT (BSP `boot_storage.c` + AP tail)
- [x] VERW emit on the SYSRET (`syscall_entry.asm`) + IRET-to-user (`isr_stubs.asm`, ring-3 branch only) exits: `verw word [rel g_mds_verw_sel]` gated on `cmp byte [rel g_mds_verw_active],0`; const RO selector operand; ZF clobber harmless
- [x] TAA: `cpu_decide_mds()` writes `IA32_TSX_CTRL` (`RTM_DISABLE|CPUID_CLEAR`) when `ARCH_CAP_TSX_CTRL` advertised and not `TAA_NO` -- per-CPU `msr_try_write`, re-applied on every AP
- [/] SRBDS: report-only (`IA32_MCU_OPT_CTRL.RNGDS_MITG_DIS`); the posture surfacing is owned by §24 -> XREF: §24
- [x] MMIO stale data: covered by the same VERW path (full coverage also needs SRBDS/FB_CLEAR microcode, noted in the posture)
- [x] Commit: `"kernel/security: VERW/MD_CLEAR microarchitectural buffer clear on kernel->user return"`

**Test checkpoint:** `cpu_has(CPU_FEATURE_MD_CLEAR)` matches `CPUID.(7,0):EDX[10]` (a word-1 read of the 128-bit mask); when `g_mds_verw_active` is latched the exit paths `verw`, else it is a `cmp`/`jz` no-op; `g_mds_verw_active` is never set without `MD_CLEAR`; boot reaches userspace (smoke) with the modified exit paths. Test on: QEMU WHPX, TCG, bare metal.

> **Test runner:** `scripts\debug\kernel\run-x86-tests.bat` (SUITE=x86) | 2 MDS suites (MD_CLEAR word-1 read; VERW-gate-implies-capability, SKIP when off) | 186 x86 + 16 user-mode pass; smoke boots to userspace with the modified SYSRET/IRET exits.
>
> **Notes:**
> - Shipped `cpu_decide_mds()` + global `g_mds_verw_active`/`g_mds_verw_sel` (`cpu_security.c`) + `verw` gates in `syscall_entry.asm`/`isr_stubs.asm`; `CPU_FEATURE_MD_CLEAR`=64 is the first real word-1 consumer of the §18 128-bit feature surface.
> - Gate is a GLOBAL RIP-relative byte (the ISR exit reads it after swapgs, so a `gs:`-relative per-CPU read would be wrong); monotonic set-once-to-1, so on a skewed SMP set it latches on if ANY online CPU needs the clear.
> - Scope boundary: SRBDS `RNGDS_MITG_DIS` reporting is §24 (posture); the dead KPTI trampoline returns (`kpti_trampoline.asm`) must mirror the VERW when KPTI is activated -- owned by the KPTI activation work, not wired today.

> [!WARNING]
> VERW lands on the hottest path (every syscall/interrupt return). It is gated on the once-per-boot capability decision; on `MDS_NO` silicon the gate stays off and the exit is a single `cmp`/`jz`.

> **Verified:** 2026-06-28 | commit `e6733bb8` (+ review fixes) | 4/5 items | build OK | smoke PASS (KVM 2.71s, boots to userspace with the moved VERW exits), x86 186/16
> **Deferred:** [L] SRBDS `RNGDS_MITG_DIS` posture surfacing -> XREF: §24 (item: "enumerate every mitigation in this TODO with state {active, unsupported, disabled, n/a}" -- SRBDS is one such mitigation).
> **Quality reviewed:** 2026-06-28 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 4H+2M fixed | scope: kernel-code-quality

---

## 20. kCFI: Software Control-Flow Integrity for Indirect Calls

Clang kCFI (`-fsanitize=kcfi`, Linux `CONFIG_CFI_CLANG`) enforces that every indirect call targets a function of the matching type signature. Complements §10 hardware IBT for CPUs without CET-IBT and adds forward-edge type checking IBT alone does not.

> [!NOTE]
> Design validated + prototyped (2026-06-28), then reverted to keep `main` buildable -- the flag flip false-faults at the kernel->firmware ABI boundary and the exemption sweep is multi-iteration (see Deferred). The proven approach is baked into the items below.

- [ ] `__nocfi` macro: `#define __nocfi __attribute__((no_sanitize("kcfi")))` in a new `include/kernel/compiler.h` (function-scoped; keeps the type-id prologue, exempts only that function's indirect calls)
- [ ] kCFI trap detection via the `.kcfi_traps` section: linker.ld bounds + `kcfi_is_trap(rip)` in `idt.c`; the #UD path routes a trap-RIP to a `KERNEL_SECURITY_CHECK_FAILURE` bugcheck -> XREF `TODO-27-crash-dump-generation.md §1`
- [ ] `__nocfi` on `ssdt_dispatch` (the one genuine type-erased call: ~187 `NtXxx` cast to a uniform `SSDT_HANDLER`); IRQ/DPC/blkdev/test dispatch are already correctly typed
- [ ] FIRMWARE-ABI BOUNDARY SWEEP (blocker): exempt each TU calling a non-kCFI firmware pointer (`uefi_runtime.c`, `uefi_vars.c`, +ACPI/GOP/TPM) via a per-TU `filter-out -fsanitize=kcfi` Makefile rule; enumerate via smoke + addr2line
- [ ] Add `-fsanitize=kcfi` to kernel `CFLAGS` (Clang 19; NOT the UEFI bootloader sub-make); coexists with retpoline (§8) + stack-protector; `objdump` shows the prologues + a non-empty `.kcfi_traps`
- [ ] Commit: `"kernel/security: kCFI -- Clang type-id indirect-call enforcement + bugcheck on mismatch"`

**Test checkpoint:** `kcfi_is_trap()` identifies a real `.kcfi_traps` entry RIP and rejects a non-trap address (unit-testable without a bugcheck); boot reaches `C:\>` (smoke) with kCFI live, proving the firmware-boundary sweep is complete; `objdump` shows kCFI prologues. Test on: QEMU WHPX, TCG, bare metal.

> **Deferred:** [M] kCFI kernel-wide flip blocked on the firmware-ABI-boundary exemption sweep -- enabling `-fsanitize=kcfi` false-faults at EVERY kernel->firmware indirect call (UEFI runtime + UEFI vars confirmed; ACPI/GOP/TPM likely), each needing a per-TU kCFI exemption discovered iteratively via smoke. Detection design + `__nocfi` macro + `ssdt_dispatch` annotation are validated; remaining work is the bounded multi-iteration sweep. -> XREF: this section (the "FIRMWARE-ABI BOUNDARY SWEEP" item above).

---

## 21. FORTIFY_SOURCE: Bounds-Checked str/mem Builtins

Compile + runtime bounds checking on the `memcpy`/`strcpy`/`strcat`/`snprintf` family (Linux `CONFIG_FORTIFY_SOURCE`, MSVC `__builtin___*_chk`). Catches the buffer-overflow class at the call site where the destination object size is known.

> [!NOTE]
> Scope corrected by a compile probe (2026-06-28): `clang-19 --target=x86_64-elf -ffreestanding -nostdinc -D_FORTIFY_SOURCE=2 -O2` does NOT auto-emit `_chk` calls -- it emits plain `memcpy`/`strcpy`. FORTIFY_SOURCE is a libc-HEADER feature (glibc/Bionic/musl implement it as `pass_object_size` inline wrappers in `string.h`), NOT a pure compiler flag. So this needs header machinery, not a flag flip (see Deferred).

- [ ] Fortify header machinery in `include/libc/string.h`: `__pass_object_size__` + `__overloadable__` inline wrappers for the mem/str/snprintf family that `__builtin_object_size`-dispatch to the real fn or `__chk_fail`
- [ ] `_chk` slow paths in `src/libc/string.c` (glibc-ABI arg order): `__memcpy_chk`/`__memset_chk`/`__strcpy_chk`/`__strcat_chk`/`__snprintf_chk`/`__vsnprintf_chk` etc. -- compare op length to `dstlen`, tail-call real fn or `__chk_fail`
- [ ] `__chk_fail()` (noreturn) -> `KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE, STATUS_*_BUFFER_OVERRUN, ...)` mirroring `stack_canary.c:154`
- [ ] Latent-overflow handling (the risk): FS-mount name code (`fs/fat32`/`fs/ixfs`/`fs/ntfs` raw strcpy/strcat of disk names into fixed buffers) is the firing surface; stage `__chk_fail`->klog+continue to surface sites, audit/fix, then hard bugcheck
- [ ] Commit: `"kernel/libc: FORTIFY_SOURCE -- _chk string/memory builtins, bugcheck on overflow"`

**Test checkpoint:** the `_chk` slow paths pass correctly-sized ops through unchanged (positive path, unit-testable in `TEST_CAT_SECURITY`); a `memcpy` into an undersized fixed buffer triggers `__memcpy_chk` -> bugcheck (manual/bare-metal -- it halts, not unit-testable); boot reaches `C:\>` with FORTIFY live (no latent FS-mount overflow firing). Test on: QEMU WHPX, TCG, bare metal.

> **Deferred:** [M] FORTIFY_SOURCE needs the freestanding fortify-HEADER machinery (proven by compile probe: clang `-nostdinc -D_FORTIFY_SOURCE=2` emits plain `memcpy`, not `_chk` -- the rewrite lives in `string.h` `pass_object_size` wrappers, not the flag), plus handling the latent FS-mount-overflow exposure (disk names into fixed `fs/*` buffers) which can hard-fault boot until audited. Bounded but multi-part; the `_chk` set + signatures + the `__chk_fail` precedent are pinned. -> XREF: this section (the "Fortify header machinery" + "Latent-overflow handling" items above).

---

## 22. stackleak: Erase Kernel Stack on Return to User

Zero the used portion of the kernel stack on every syscall/interrupt return to user mode (Linux `CONFIG_KSTACK_ERASE` / former `STACKLEAK`). Prevents stale kernel data (pointers, secrets, canaries) left on the stack from leaking to a later syscall or via an info-leak primitive.

> [!NOTE]
> Toolchain check (2026-06-28): clang-19 has NO stackleak instrumentation (only `-fstack-protector*` / `-fstack-clash-protection`; Linux stackleak is a GCC plugin). So the "Clang stackleak instrumentation if available" alternative is OFF the table -- use the manual poison-scan-watermark path below. Still implementable; not blocked.

- [ ] Coherent erase model (design): on return-to-user POISON-fill (sentinel, NOT zero) the used span -- clears secrets AND keeps the scan invariant for the next return; deepest-use = scan up to the first non-poison byte (Linux KSTACK_ERASE)
- [ ] Hook placement (design): the erase runs while RSP still points at the KERNEL frame (BEFORE the exit restores user RSP) -- NOT the §19 VERW spot, which is post-pop-rsp; an asm wrapper passes explicit kernel stack top/bounds
- [ ] Stack inventory (design): poison + carry `stack_low`/`stack_high` for EVERY ring-3-return-capable kernel stack incl. the kmalloc'd `task_create_user()` stack (`task.c:563`); stackleak refuses unknown/unpoisoned stacks
- [ ] Cap + resume: bound scan+poison work per return (Linux STACKLEAK_SEARCH_DEPTH) with a defined partial-erase resume policy; build knob to disable for perf
- [ ] Commit: `"kernel/security: stackleak -- erase used kernel stack span on return to user"`

**Test checkpoint:** After a syscall that writes a sentinel deep on the kernel stack, a following syscall reads POISON (not the sentinel) at that offset (span re-poisoned); the erase is bounded by the watermark/cap (no over-write into the guard page); boot reaches `C:\>` with stackleak live. Test on: QEMU WHPX, TCG, bare metal.

> **Deferred:** [M] stackleak needs a re-planned + careful hot-path implementation per design review: poison-fill (not zero) for a coherent scan invariant; the erase hook must run BEFORE the exit restores user RSP (the §19 VERW spot is post-pop-rsp, unsafe for a C erase on the user stack with kernel GS); and the stack inventory must cover the kmalloc'd `task_create_user()` kernel stack (`task.c:563`), not just PMM+guard stacks. Bounded but multi-part (poison engine + asm wrapper + stack normalization + cap). -> XREF: this section (the corrected items above).

---

## 23. KFENCE: Sampling Use-After-Free / Out-of-Bounds Detector

A low-overhead page-granularity sampling allocator (Linux `CONFIG_KFENCE`) placing a fraction of allocations on guard-page-bracketed pages so OOB/UAF accesses fault deterministically in production. Complements §11's always-on cookie/redzone with page-fault-level detection.

> [!NOTE]
> Explored + design-reviewed (2026-06-28): self-contained `src/kernel/mm/kfence.c` + `kfence.h`; 3 hook sites (`kmalloc`/`kfree` heap.c, `page_fault_handler` vmm.c:565 BEFORE the 32-slot guard table); pool from `pmm_alloc_contiguous` + `vmm_unmap_page(virt,0)` guards (split the 2 MiB huge page first); init after `heap_init()` (boot_hw.c:529) gated on `kfence_ready`; own freelist spinlock, boot-init read-only range check (lock-free in #PF); tunables via `kernel_tunable_register`; terminal = report + `KeBugCheckEx` (new STOP code -- no SEH/RIP-advance exists, fatal is the only safe terminal). Two design findings folded into items 1-2 below.

- [ ] Sample 1/sample_rate via a SHARED helper covering `kmalloc`/`kmalloc_zeroed`/`kmalloc_tagged` (design: all 3 variants, not just kmalloc); `kfree` AND `kfree_tagged` dispatch `kfence_owns(ptr)` -> `kfence_free`
- [ ] Each object at a per-slot-randomized page side (design: left OR right, so under- AND over-flow cross a guard page) by an unmapped guard page; `kfence_handle_fault` classifies OOB-side vs UAF from `cr2` + slot state + side
- [ ] On free, unmap (or quarantine) the slot so a UAF access faults; recycle after a quarantine delay
- [ ] `kfence_report()` on a guarded fault: classify OOB vs UAF, log the object's alloc/free context; wire to the page-fault handler's guard-table path
- [ ] Sample rate + pool size are `boot.conf` tunables; disabled adds near-zero overhead
- [ ] Commit: `"kernel/mm: KFENCE -- sampling guard-page UAF/OOB detector"`

**Test checkpoint:** A test allocation forced onto the pool: an OOB write faults and `kfence_report` logs OOB; a UAF read after free faults and logs UAF; sampling off = normal kmalloc, no pool faults. Test on: QEMU WHPX, TCG, bare metal.

> **Deferred:** [M] KFENCE prototyped end to end (self-contained `kfence.c` ~280 lines: FIFO-ring quarantine, left/right placement, OOB/UAF classify; 5 heap-variant hooks + the `page_fault_handler` hook + boot init + tunables; builds), then reverted to keep `main` booting. Enabling (1/100) broke boot: `pmm_alloc_contiguous` placed the pool at 0x6ff000 (low memory next to the 0x70000 page tables), and `vmm_unmap_page` on the identity-map guard pages there corrupted kernel state -- a later #PF hit `handlers[14]==NULL` (idt.c default panic, never reaching the handler). FIX before re-enabling: place the pool in a dedicated VA away from the identity-map's critical low pages (or verify the PMM region is safe + that unmapping those frames cannot disturb the page tables), then re-verify the `kfence_handle_fault` wiring. -> XREF: this section (the items above).

---

## 24. Mitigation Visibility: Queryable Security Posture

Expose which CPU/kernel mitigations are active as structured queryable data. Linux scatters this across `/sys/devices/system/cpu/vulnerabilities/*`; Win11 hides it behind WMI/registry. Neither does it cleanly -- a single coherent posture report for operators, certification, and post-incident triage is an exclusive edge.

- [ ] `kernel_security_posture_t`: enumerate every mitigation in this TODO with state {active, unsupported, disabled, n/a}
- [ ] Each mitigation section registers its final state into the posture struct at init (single source of truth; no re-probing)
- [ ] `[secpost]` boot serial dump of the full posture after Phase 3 security init
- [ ] Query surface: an `NtQuerySystemInformation` class (or a `\Device`-style virtual file) returning the posture struct to user mode
- [ ] Commit: `"kernel/security: queryable mitigation posture -- [secpost] dump + NtQuerySystemInformation class"`

**Test checkpoint:** The `[secpost]` line lists every mitigation with a concrete state matching the host CPUID; the query class returns the same struct to a user-mode test. Test on: QEMU WHPX, TCG, bare metal.

> [!TIP]
> Linux exposes per-vuln files but no unified posture; Win11 hides it behind WMI/registry. A single coherent, queryable posture report is a genuine operator-experience edge over both.

> **Deferred:** [M] capstone section -- the posture's "each mitigation section registers its final state (single source of truth, no re-probing)" model needs the mitigations it reports to EXIST first; §25 (predictor policy) + §20-23 (kCFI/FORTIFY/stackleak/KFENCE) are deferred, so the posture would report mostly not-implemented today. Best built last, after the mitigations register live state. -> XREF: §25 (predictor mitigations -- the key input it reports) + §20/§21/§22/§23.

---

## 25. Spectre Predictor-Policy Mitigations: SSBD, STIBP, RSB, BHI, ITS, Retbleed

The predictor mitigations beyond §8's IBRS/IBPB/retpoline core that Win11 and Linux apply. NOT one SPEC_CTRL-replay problem (design review): each has a distinct control path, so each item names its real mechanism and is tested against THAT path, not only an `IA32_SPEC_CTRL` readback. Needs the 128-bit feature surface from §18. (Split from the original combined §18.) -> XREF: §18 (feature bits), §8 (IBRS/IBPB/retpoline core), `D02 T09 §11` (MSR-profile AP replay).

- [ ] New feature bits (need §18): `CPU_FEATURE_SSBD` (Intel SPEC_CTRL), `CPU_FEATURE_SSBD_AMD` (LS_CFG), `CPU_FEATURE_BHI_CTRL`; plus `ARCH_CAP_*` decode for RRSBA / BHI_NO / RFDS / ITS_NO in `msr.h`
- [ ] STIBP (cross-HT, `IA32_SPEC_CTRL` bit 1): set alongside IBRS when SMT is active + `CPU_FEATURE_STIBP`; joins the `s_bsp_msr_profile[]` SPEC_CTRL entry (AP replay)
- [ ] Intel SSBD (Spectre v4, `IA32_SPEC_CTRL` bit 2): gated on `CPU_FEATURE_SSBD`; joins the SPEC_CTRL profile; exposed as a tunable
- [ ] AMD SSBD (NON-SPEC_CTRL): `MSR_AMD64_LS_CFG` vendor path gated on `CPU_FEATURE_SSBD_AMD` -- its own profile entry + AP replay arm, separate from the SPEC_CTRL bit
- [ ] BHI_DIS_S (CVE-2024-2201) when advertised (`CPUID.(7,2):EDX[18]`): SPEC_CTRL.BHI_DIS_S, joins the profile; FALLBACK when absent: a short BHB-clearing loop on kernel entry (affected-model gated)
- [ ] RSB stuffing on cross-domain switch: standalone audited asm behind a probed per-CPU `s_rsb_fill_active` latch (mirrors `s_ibpb_active`), invoked from `sched_cross_domain_ibpb`; counter test proves it fires only on affected CPUs
- [ ] ITS (CVE-2024-28956): microcode + affected-model + alignment policy (NOT a SPEC_CTRL bit); detect via `IA32_ARCH_CAPABILITIES` + model table; log posture
- [ ] Retbleed (AMD Zen1/2, Intel): vendor/model-gated `unret` or IBPB-on-kernel-entry (NOT a SPEC_CTRL bit)
- [ ] Tests assert each mitigation's REAL control path (LS_CFG for AMD SSBD, RSB-fill counter, model-gate for ITS/Retbleed), not only an `IA32_SPEC_CTRL` readback; APs match BSP; a CPU lacking a feature takes no MSR write (no #GP)
- [ ] Commit: `"kernel/security: SSBD (Intel+AMD), STIBP, RSB stuffing, BHI_DIS_S, ITS, Retbleed predictor policy"`

**Test checkpoint:** Each mitigation verified via its REAL path: SPEC_CTRL bits for STIBP / Intel SSBD / BHI_DIS_S read back set; AMD SSBD reflected in `LS_CFG`; the RSB-fill counter increments on cross-domain switch; ITS/Retbleed posture matches the affected-model gate. APs match BSP. A CPU lacking a feature takes no MSR write (no #GP). Test on: QEMU WHPX (`-cpu` with spec flags), TCG, bare metal.

> **Deferred:** [M] 7-mitigation control-path unit (STIBP, Intel SSBD, AMD SSBD/LS_CFG, BHI_DIS_S + BHB-loop fallback, RSB stuffing asm, ITS, Retbleed), each on a distinct mechanism that needs its own gate + AP-replay + test. Specified + surface-mapped, not blocked: §18 feature surface is done; §8 already built the `s_bsp_msr_profile[]` SPEC_CTRL replay (STIBP/Intel-SSBD/BHI_DIS_S join it), the `s_ibpb_active` writability-latch pattern (RSB stuffing mirrors it), and `sched_cross_domain_ibpb` (the RSB-fill invocation site). Deferred from this pass as a focused fresh-context implementation: the §17-§19 work consumed this window, and a 7-mechanism unit + its design/adversarial/consistency/perf/re-adversarial convergence loop is too large to land correctly at the tail of a long session. Each `[ ]` item above is already control-path-correct (named real mechanism, tested against THAT path) from the §18/§25 restructure, so the next window implements directly. -> XREF: §18 (feature bits, done), §8 (IBRS/IBPB/retpoline + profile-replay scaffolding), `D02 T09 §11` (MSR-profile AP replay).

---

## 26. Release-Build Test-Surface Exclusion

Every `#ifdef KERNEL_TESTS` seam in the tree was live in the shipped kernel, because `Makefile` defined `-DKERNEL_TESTS` **unconditionally** and no build path omitted it. The guards therefore gated nothing: `build/kernel.map` carried `pmm_alloc_fail_next`, `pmm_alloc_fail_countdown_set`, and `nt_atom_reset_for_test`. These are not inert debug helpers -- `pmm_alloc_fail_next()` forces the next physical allocation to FAIL, and `nt_atom_reset_for_test()` wipes the global atom table.

> [!NOTE]
> **Found 2026-07-17 during `02-kernel-core/TODO-33 §10`** (Codex adversarial, [high], verified at `Makefile:29` + `build/kernel.map`). That section proposed a `KERNEL_TESTS`-gated atom-table mutator; the review showed the gate does not exist in practice, so the seam was dropped rather than shipped. The docstring in `include/kernel/mm/pmm.h` asserting "Released builds compile the entire surface out via `KERNEL_TESTS`" is FALSE against the Makefile and must be corrected or made true.

> [!NOTE]
> **Reachability corrected during implementation 2026-07-17.** This section originally claimed "None is SSDT-registered, so none is user-reachable today; the exposure is ROP/gadget surface". The SSDT half is true but the conclusion was NOT: the fault-injection seams are reachable from user mode through the **native** syscall table -- `SYS_FAULT_INJECT` (44, `syscall.h:51`) dispatches at `syscall.c` to `kmalloc/pmm/vmm/copy_user` fail-injection, gated only at RUNTIME by `boot.conf test=1`. So the pre-section exposure was a config-gated live syscall, not merely gadget surface. `KERNEL_TESTS=off` now drops the handler at compile time (the number stays reserved; it feeds `IMPOSSIBLE_OS_ABI_HASH`).

- [x] Added a validated `KERNEL_TESTS ?= on` knob to `Makefile` mirroring the `BUILD_ALT_BOOT` idiom (`$(error ...)` outside `{on,off}`); `on` emits `-DKERNEL_TESTS`, `off` emits `-UKERNEL_TESTS`
- [x] Flavor flag emitted LAST via `override CFLAGS +=` (after the base, `KERNEL_EXTRA_CFLAGS`, `BUILD_ALT_BOOT`); `override` also on `KERNEL_TESTS_FLAG` + the SIMD/AVX2/AVX512 derived vars, so no command-line assignment can contradict `off`
- [x] When `off`, `$(KERNEL_DIR)/test/` is `filter-out`-pruned from `C_SRCS`; `C_OBJS` derives from the pruned list, so no TU from THAT directory reaches the link (103 on `on`, 0 on `off`). Test TUs elsewhere are §28
- [x] Added `build/.kernel-tests.stamp` (content = the flavor) via the `.FORCE` + `cmp -s` idiom, so its mtime moves ONLY on a real flavor change and same-flavor rebuilds stay incremental
- [x] Made the stamp a REAL (not order-only) prereq of every C object rule: **15**, not the 8 this item first claimed (14 explicit + the pattern rule); placed after the `.c` so `$<` still resolves to the source
- [x] Guarded `test_usermode.h` with `#ifdef KERNEL_TESTS` + no-op `#else` inlines for all **9** functions: the 8 `boot_tests.c` calls plus `test_usermode_color_active()`, which `klog.c` reached via a header-bypassing `extern`
- [x] Seam audit: guarded 5 unguarded test-only globals still in the release map -- `nt_atom_reset_for_test`, `boot_rollback_reset_for_test`, `ci_policy_publish_for_test`, `ci_policy_reset_for_test`, `nt_audit_reset_for_test`
- [x] Flavor-gated the `SYS_FAULT_INJECT` handler + dispatch case: the release flavor drops the user-reachable fault-injection bridge; syscall 44 falls to the unknown default (-1). The NUMBER stays reserved (ABI-hash input)
- [x] Corrected the FALSE docstring at `include/kernel/mm/pmm.h` -- it now names the release flavor (`make KERNEL_TESTS=off`); the release PIPELINE turning that knob is §27
- [x] Wired 15 host-side contract tests in `scripts/test-tooling.sh` (flavor flags, 5 override-bypass vectors, default goal, `$(error)`, TU pruning, stamp coverage); documented the knob in `build.sh --help` + `development-tooling.md`
- [x] Commit: `"kernel/security: exclude the test surface from release builds"`

**Test checkpoint:** `KERNEL_TESTS=off bash scripts/build.sh` (or `make KERNEL_TESTS=off kernel`; the bare `make KERNEL_TESTS=off` default goal is `include/build_info.h`, NOT a kernel) links a kernel with no `src/kernel/test/` TU (test TUs OUTSIDE that directory still link -- §28 owns them); `make KERNEL_TESTS=bogus` fails fast on the `$(error ...)`; an `on -> off -> on` incremental sequence rebuilds the EXPLICIT-rule TU `build/kernel/icon_store.o` (it carries a seam at `icon_store.c:671`) on each flip, proving the stamp reaches the non-pattern rules; the default `on` build still passes the full `scripts/test.sh` suite, which depends on those seams. Test on: QEMU KVM + TCG.

> **Test runner:** `bash scripts/test-tooling.sh` (15 KERNEL_TESTS contract cases) | 524 tooling tests, 3 pre-existing unrelated hook failures (identical at clean HEAD)

> **Notes:**
> - Shipped the `KERNEL_TESTS={on,off}` build flavor: a validated `Makefile` knob, a `.kernel-tests.stamp` prereq on all 15 C object rules, `filter-out` pruning of `src/kernel/test/`, and release guards on 9 header functions + 5 test-only globals. Covers the seams + that directory ONLY.
> - Runs as `KERNEL_TESTS=off bash scripts/build.sh`; the flag is appended last with `override`, so no command-line assignment beats it. A flip rebuilds every TU; same-flavor rebuilds stay incremental.
> - Corrected the section premise: the seams were user-reachable via `SYS_FAULT_INJECT` (syscall 44), not just gadget surface; that handler is now flavor-gated. Codex design adoptions (2 [high]) in the commit message.
> - Canonical doc: [development-tooling.md "Build Flavors"](../../docs/infrastructure/development-tooling.md#build-flavors).
> - Scope boundary: §26 owns the MECHANISM; the proof a shipped image used it is §27, and test TUs outside `src/kernel/test/` (found by this section's review) are §28. -> XREF: §27 (release-flavor proof) + §28 (test-only TUs outside the test directory).

> **Verified:** 2026-07-17 | commit `2c34e615` | 10/10 items | build OK | smoke PASS (KVM 3.10s), 21504 kernel + 16 user tests, both flavors build, off map seam-free
> **Quality reviewed:** 2026-07-17 | Codex 7x (design + adversarial + consistency + perf + re-adversarial) | 2H+6M+1L fixed, 1M+1L deferred-XREF | scope: kernel-code-quality

---

## 27. Release-Flavor Proof: Seam-Inventory Gate and CI Attestation

§26 gives the build a release flavor; it does not prove a shipped image uses it. Both release paths package whatever flavor happens to sit in `build/`: `.github/workflows/release.yml:96-104` builds, tests, and packages ONE default-flavor artifact with no `KERNEL_TESTS` override, and `scripts/release/build-image.sh:2-6` is an INDEPENDENT path (release.yml never invokes it) that copies `kernel.exe` into the ESP of `build/release/disk.img`. A `KERNEL_TESTS=off` knob nobody turns ships exactly the kernel §26 exists to prevent.

> [!NOTE]
> **Gate design settled by Codex design review 2026-07-17 (three passes).** A `*_for_test` / `*_fail_*` NAME GLOB is REJECTED as sole proof: verified against `build/kernel.map` it misses `cpu_msr_profile_count`, `cpu_msr_profile_entry`, `cpu_bsp_pat_baseline` (`cpu_security.c:1905-1921`), `nls_test_set_active` (`nls.c:228`), `mouse_test_set_ps2_buttons` (`mouse.c:593`), and `kcrc32c_sw_test` (`kchecksum.c:158`) -- 4 of 6 slip the glob, so the gate would report green while state-mutating seams stay linked. A test-map-minus-release-map SUBTRACTION is REJECTED as tautological: that set is absent from the release map by construction, so the assertion can never fail. A REGEX/line scanner over `#ifdef KERNEL_TESTS` regions is REJECTED as insufficiently sound (3rd pass): it must correctly model nesting, multiline declarators, macro-generated definitions, guarded data, and exclude the tree's one inverse `#ifndef KERNEL_TESTS` guard (whose body is the RELEASE branch) -- a syntactically-valid but unrecognized seam reports false-green. The gate must derive an INDEPENDENT, compiler-backed inventory: per-TU compile at `KERNEL_TESTS` on and off with identical flags, `llvm-nm-19 --defined-only` each object, `on_only = defined(on.o) - defined(off.o)`; assert `build/kernel.map INTERSECT on_only = empty`. This is non-tautological because the per-TU objects derive independently of the release LINK, so a stale on-flavor object or an unpruned TU still trips it. **Sequencing (3rd pass, [high]):** §27's Test checkpoint requires the `off` map to PASS, but §28's items 1-3 name symbols (`ntfs_run_self_test`, `tpm_t_test_install`, `tpm_t_test_restore`) that are UNGUARDED today and therefore still link at `KERNEL_TESTS=off` (verified in `build/kernel.map` before §28 shipped) -- a known-gap allowlist was considered and REJECTED (it would knowingly attest a release containing the attacker-triggerable NTFS self-test, line 826). §28 MUST ship before §27's gate can pass with no allowlist; implement §28 first.

- [x] `check-release-symbols.sh` + `lib/release-seam-inventory.py`: PART A compiles all kernel TUs twice (-D/-U KERNEL_TESTS), `on_only=defined(on)-defined(off)`, asserts `kernel.map` disjoint; FAILS on an on-flavor map
- [x] §28's 3 extra test TUs cannot reach a release image: PART B's link trace proves none link (flavor-independent); an on-flavor compile DB also inventories them in PART A's `on_only` -> XREF: §28
- [x] PART B reads the `ld.lld --trace` inputs emitted as a link byproduct (`kernel.link-trace.txt`, sha-bound to `kernel.exe`, co-generated so no drift); rejects `build/kernel/test/*.o` + the 3 extra-TU objects
- [x] Wired `release.yml`: test-flavor build + `test.sh`, then clean `KERNEL_TESTS=off` rebuild + gate + extract-and-bind the kernel inside `system-disk.img` (sha + provenance) before packaging; installs `bear`
- [x] Provenance: link-time non-alloc `.ipos.provenance` marker (`provenance_release.c`, off-only); `--verify-provenance` uses read-only `readelf -p`; `release.yml` + `build-image.sh` both reject a kernel lacking it
- [x] Commit: `"kernel/security: prove release images carry no test surface"`

> [!NOTE]
> **Resolved by §29.** PART A's flavor-diff catches everything gated by `#ifdef KERNEL_TESTS`. A test-only helper defined UNCONDITIONALLY in a production TU (present in both flavors) was invisible to the diff and shipped in the release image (Codex adversarial flagged this false-GREEN). §29 guarded all 7 such helpers with `#ifdef KERNEL_TESTS` -- so they are now flavor-gated and PART A covers them -- and added the advisory PART A2 "referenced-only-by-test-objects" audit for a future one. -> XREF: §29.

**Test checkpoint:** `scripts/check-release-symbols.sh` FAILS on a `KERNEL_TESTS=on` `kernel.map` (proving it can fail) and PASSES on a `KERNEL_TESTS=off` one; a negative test adds a fresh seam symbol and confirms the gate catches it with no edit to the gate; `build-image.sh` refuses a test-flavor `kernel.exe`. Test on: QEMU KVM + TCG + a CI dry-run.

> **Note:** No kernel test surface -- `provenance_release.c` is a passive non-alloc section marker with no runtime logic; the testable surface is the host-side bash gate, validated by its own runs.

> **Test runner:** N/A (host-side bash gate, no kernel test surface) | validation: `check-release-symbols.sh` FAILS on-flavor (100 seams + test objects), PASSES off-flavor; fresh-seam negative test auto-covered; embedded==gated kernel sha bound; off-flavor smoke PASS (KVM)

> **Notes:**
> - Shipped: `check-release-symbols.sh` (3-part gate) + `lib/release-seam-inventory.py` (per-TU on/off nm diff) + `provenance_release.c` (non-alloc `.ipos.provenance` marker) + Makefile wiring + `release.yml`/`build-image.sh` enforcement.
> - Runs as CI release gate + `build-image.sh` guard: PART A (seam symbols) + PART B (link inputs, sha-bound trace) + PART C (provenance); packaging rebinds the kernel inside `system-disk.img` to the gated hash.
> - Codex adversarial (3 rounds): F2-F7 (drift/image-binding, read-only `readelf`, `bear`, fail-closed, dual-hash trace, full gate in `build-image`) + F8 (exact-2-record manifest) + F9 (compile-DB coverage) fixed; F1 + gate-robustness residuals (signed attestation, DB flag-fingerprint) scoped -> §29.
> - Canonical doc: [development-tooling.md "Build Flavors"](../../docs/infrastructure/development-tooling.md#build-flavors).
> - Scope boundary: §27 owns the PROOF + enforcement points; §26 owns the flavor mechanism; §28 owns test-only TUs; §29 owns unguarded test-only helper functions. -> XREF: §26 + §28 + §29.
> **Verified:** 2026-07-17 | commit `4eb1399d` | 6/6 items | build OK | smoke PASS (KVM 2.9s)
> **Accepted:** [M] `build-image.sh` repeats the full ~678-compile proof each image build -> XREF: 02-kernel-core/TODO-10 §30 (item: "Perf: `build-image.sh` validates the signed attestation ... validate-not-recompute")
> **Quality reviewed:** 2026-07-17 | Codex 7x (design, adversarial, consistency, perf) | 8H+3M fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## 28. Test-Only Translation Units Outside `src/kernel/test/`

§26 prunes `$(KERNEL_DIR)/test/` and guards the `#ifdef KERNEL_TESTS` seams, which is the build MECHANISM. It does not cover test code that lives outside that directory under an ordinary filename and carries NO `#ifdef` guard: a `KERNEL_TESTS=off` dry run still compiles `src/kernel/fs/ntfs/ntfs_test.c`, `src/kernel/fs/ixfs/ixfs_test.c`, `src/kernel/main/test_threads.c`, and `src/kernel/main/boot_tests.c`, plus the TPM transport test mutators. Zero objects under one directory is therefore NOT release test-surface exclusion.

> [!WARNING]
> **`ntfs_run_self_test()` is reachable from attacker-controlled data TODAY**, independent of any build flavor. `partition.c:904` calls it unconditionally for EVERY mounted NTFS volume; the only gate is `test_strcmp(vol->volume_name, "NTFS_TEST")` at `ntfs_test.c:3283`, and the volume label is data ON THE DISK. A USB stick labelled `NTFS_TEST` runs 69 create/delete/rename/journal/write tests against the mounted volume. This is a live security defect, not merely a release-flavor concern: the guard is necessary but a data-driven trigger for destructive code is the real bug.

- [x] Guarded `ntfs_run_self_test()` decl (`ntfs.h`) + the call site at `partition.c:904` with `#ifdef KERNEL_TESTS`; attacker-controlled label can no longer trigger it in a release image
- [x] Pruned `ntfs_test.c`/`ixfs_test.c`/`test_threads.c` via explicit Makefile `filter-out` at `KERNEL_TESTS=off`; first moved `shell_loader_func` (the PRODUCTION cmd.exe launcher, misfiled in `test_threads.c`) to `src/kernel/main/shell_loader.c`
- [x] Guarded all 7 `tpm_t_test_*` mutators (`install`/`restore`/`install_crb_buffers`/`restore_crb_buffers`/`budget_iters`/`budget_active`/`busy_ticks`) + declarations with `#ifdef KERNEL_TESTS`
- [x] `boot_tests.c` stays a config-gated launcher (test.h already no-ops `test_runner_*` at off); guarded `ixfs_test_performance()` call + the destructive CRUD demo block (Codex adversarial finding)
- [x] Content re-audit: no new stray test-only TUs found outside the four already handled; `mbedtls` `TEST_ASSERT`-style matches are vendored and excluded from `C_SRCS` (unbuilt)
- [x] Added `build-release-flavor` CI job to `build.yml`: clean `KERNEL_TESTS=off` build + verifies no pruned-TU object exists in the output
- [x] Evaluated `-Wmissing-prototypes`: 158 warnings across 124 files under a non-fatal dry run; deferred flipping it tree-wide given the `-Werror` fallout, per the item's own caution
- [x] Commit: `"kernel/security: exclude test TUs outside the test directory from release builds"`

**Test checkpoint:** a `KERNEL_TESTS=off` build compiles none of `ntfs_test.c` / `ixfs_test.c` / `test_threads.c`; `build/kernel.map` carries no `ntfs_run_self_test` / `ixfs_test_*` / `tpm_t_test_*` symbol; mounting a volume labelled `NTFS_TEST` on a release build performs no writes; the default `on` build still runs the NTFS self-test on that label and passes the full `scripts/test.sh` suite. Test on: QEMU KVM + TCG.

> **Test runner:** `bash scripts/test-tooling.sh` (4 new §28 contract cases) | 528 tooling tests, 3 pre-existing unrelated hook failures (identical at clean HEAD)

> **Notes:**
> - Shipped: guards on `ntfs_run_self_test`, 7 TPM test mutators, the `boot_tests.c` CRUD/IXFS-perf blocks, and whole-file guards on `ntfs_test.c`/`ixfs_test.c`; 3-TU Makefile prune; a `build-release-flavor` CI job; 4 host-side contract tests.
> - Runs via the existing `KERNEL_TESTS={on,off}` knob (§26); verified against real builds -- off-flavor `kernel.map` carries none of the 9 guarded symbols, on-flavor carries all 9.
> - Discovered mid-implementation: `shell_loader_func` (cmd.exe launcher) was misfiled in `test_threads.c`; moved to `shell_loader.c`, fixed a size TOCTOU + a success-path leak; PMM tiering was tried and reverted (unlocked bitmap, XREF TODO-03 §1).
> - Test coverage: `shell_loader_func`'s bounds/TOCTOU/leak fixes have no dedicated kernel test (needs live vfs+task infra); validated via smoke test (boot to `C:\\>` proves successful load+exec).
> - Canonical doc: [development-tooling.md "Build Flavors"](../../docs/infrastructure/development-tooling.md#build-flavors).
> - Scope boundary: §28 owns test-only TUs OUTSIDE `src/kernel/test/` and their call sites; §26 owns the flavor mechanism; §27 (next) owns the release-map proof gate, whose inventory now has a clean release map to assert against.

> **Verified:** 2026-07-17 | commit `3a1eb47b` | 8/8 items | build OK | smoke PASS (KVM 2.62s), 21504 kernel + 16 user tests, both flavors build, off map seam-free
> **Accepted:** [H] `task_create()`'s stack alloc via `pmm_alloc_contiguous()` predates this commit unchanged (reason: scope) -> XREF: 03-memory-concurrency/TODO-03-advanced-allocator §1 (item: "PMM bitmap SMP locking" at line 103)
> **Quality reviewed:** 2026-07-17 | Codex 3x (adversarial, consistency, perf) | 1M fixed, 1H accepted-XREF | scope: kernel-code-quality

---

## 29. Guard Unguarded Test-Only Helper Functions in Production TUs

§27's release proof (PART A flavor-diff) and §28's TU pruning both miss test-only helper FUNCTIONS defined UNCONDITIONALLY in production translation units: present in BOTH `KERNEL_TESTS` flavors, so the on/off symbol diff never sees them and they ship in the release image. Codex adversarial review of §27 flagged this as a concrete false-GREEN -- the off-flavor `kernel.map` carries `compositor_set_test_seed`/`compositor_get_test_seed` (`compositor.c`), `tpm_attest_test_reset` (`tpm_attest.c`; its own comment: "unit-test setup ONLY -- Not on any production path"), `boot_health_check_test_reset`/`_registered_count` (`boot_health_check.c`), `boot_load_status_test_save`/`_restore` (`boot_load_status.c`), and `tpm_evlog_fail_offset`/`s_evlog_fail_offset`.

- [x] Guarded all 8 test-only helpers with `#ifdef KERNEL_TESTS` (compositor seed pair, `tpm_attest_test_reset`, `boot_health_check_{test_reset,registered_count}`, `boot_load_status_test_{save,restore}`, `tpm_evlog_fail_offset`); off map clean
- [x] `check-release-symbols.sh` PART A2 + `lib/test-only-ref-audit.py`: compiler-derived "referenced-only-by-test-objects" inventory. ADVISORY only (design review: raw set over-reports, cannot be empty-asserted)
- [x] Re-ran on the off build: PART A now catches all 8 (flavor-gated); PART A2 advisory runs (584 legit un-wired candidates, the 8 absent). `release.yml` preserves the ON-flavor compile DB for A2
- [/] Gate robustness (§27 F8 + perf residual): signed CI attestation replacing the forgeable in-tree `kernel.link-trace.sha` + validate-not-recompute receipt; needs crypto signing -> XREF: §30
- [/] Gate robustness (§27 F9 residual): per-flag `compile_commands.json` fingerprint bound to the gated kernel; lives in F8's signed attestation -> XREF: §30
- [x] Commit: `"kernel/security: guard test-only helpers in production TUs"`

**Test checkpoint:** an off-flavor `kernel.map` carries none of the 8 enumerated helper symbols (each now `#ifdef KERNEL_TESTS`-gated, so `check-release-symbols.sh` PART A catches any regression); the on-flavor build still defines them and passes `scripts/test.sh`. PART A2's advisory inventory writes `build/test-only-ref-audit.txt` and never blocks a release (it over-reports; see §29 body). Test on: QEMU KVM + TCG.

> **Scope boundary:** §29 owns test-only HELPER FUNCTIONS defined in production TUs (present in both flavors): the `#ifdef KERNEL_TESTS` guards + the advisory PART A2 audit. §27 owns the proof gate; §28 owns whole test TUs; §30 owns the F8/F9 gate-robustness residuals (signed attestation + DB flag-fingerprint). -> XREF: §27 (seam-inventory gate) + §28 (test-only TUs outside the test dir) + §30 (signed attestation).

> **Note:** No kernel test surface -- the guards remove symbols, they add no behavior; validation is the host-side release-symbol gate (off-flavor `kernel.map` seam-free) + both-flavor `-Werror` builds.

> **Test runner:** N/A (host-side release gate, no new kernel test surface) | validation: both KERNEL_TESTS={on,off} builds `-Werror` clean; off `kernel.map` carries none of the 8 guarded symbols; PART A + advisory A2 run.

> **Notes:**
> - Shipped: `#ifdef KERNEL_TESTS` guards on 8 test-only helpers across 5 production TUs + headers; new `scripts/lib/test-only-ref-audit.py` advisory auditor + `check-release-symbols.sh` PART A2.
> - How it runs: the guards make all 8 flavor-gated so PART A's diff catches any regression; PART A2 is an ADVISORY audit (needs the ON-flavor compile DB `release.yml` preserves), writes `build/test-only-ref-audit.txt`, never blocks.
> - Downstream: closes §27's accepted [H] unguarded-helper residual (stamp swept); F8/F9 gate-robustness residuals rehomed to the new §30. Codex design + adversarial adoptions in the commit message.
> - Canonical doc: [development-tooling.md "Build Flavors"](../../docs/infrastructure/development-tooling.md#build-flavors).
> - Scope boundary: §29 owns the helper guards + advisory audit; §27 owns the proof gate; §28 owns whole test TUs; §30 owns the signed attestation + DB fingerprint.
> **Verified:** 2026-07-17 | commit `04d4cbd3` | 3/5 items ([x]; 2 [/] deferred to §30) | build OK (both flavors) | smoke PASS (KVM 2.97s), off gate PASS, 21504+16 tests PASS
> **Deferred:** [M] F8/F9 gate-robustness residuals (signed CI attestation + per-flag compile-DB fingerprint + validate-not-recompute receipt) need cryptographic signing infrastructure (a new tool dependency + a keyless-OIDC-vs-keyed operator decision), so they are operator-reserved. Rehomed as concrete items -> XREF: §30 (F8/F9/Perf items).
> **Quality reviewed:** 2026-07-17 | Codex 5x (design, adversarial, consistency, perf) | 1H+6M fixed | scope: kernel-code-quality

---

## 30. Signed CI Attestation for the Release-Flavor Proof Gate

§27's PART B binds the link trace to `kernel.exe` with an in-tree `kernel.link-trace.sha` manifest, and §29's gate re-derives the full ~678-compile proof on every `build-image.sh` run. Both gaps need a tamper-resistant, reusable attestation an in-tree file cannot provide: a build-tree writer can rewrite all three sha records, and an unsigned in-tree receipt is only a forgeable cache hint (Codex design review 2026-07-17). Closing this needs cryptographic signing and is operator-reserved -- it adds a signing-mechanism decision (Sigstore/cosign keyless via GitHub OIDC vs a keyed release identity) and a new tool dependency.

- [/] F8: signed CI attestation over {`kernel.exe` sha, `kernel.link-trace.txt` sha, `compile_commands.json` flag-fingerprint}, replacing the forgeable in-tree `kernel.link-trace.sha` so PART B resists a build-tree writer -> XREF: §27 F8
- [/] F9: include the per-flag `compile_commands.json` fingerprint in the attestation so a covered TU compiled with drifted flags (not just a missing TU) trips the gate -> XREF: §27 F9 + §29
- [/] Perf: `build-image.sh` validates the signed attestation (kernel sha + fingerprint match) to skip the full ~678-compile recompute when valid, else full recompute (validate-not-recompute) -> XREF: §27 accepted [M] perf residual
- [/] Operator decision: choose the signing mechanism (cosign keyless OIDC vs keyed identity) and accept the new tool dependency
- [ ] Commit: `"kernel/security: signed CI attestation for the release-flavor gate"`

**Test checkpoint:** the release gate rejects a tampered `kernel.link-trace.txt` even when its in-tree sha record is rewritten to match; a `build-image.sh` re-run over an unchanged, attested kernel validates without recompiling; a covered TU rebuilt with a changed flag fails the fingerprint check. Test on: a CI dry-run + local `build-image.sh`.

> **Scope boundary:** §30 owns the tamper-resistant signed attestation + the reusable validate-not-recompute receipt + the DB flag-fingerprint (§27 F8/F9 + perf residuals). §27 owns the proof gate; §29 owns the helper guards + advisory audit. -> XREF: §27 + §29.

> **Deferred:** [H] signed CI attestation + DB flag-fingerprint (F8/F9) need cryptographic signing infrastructure (a new tool dependency + a keyless-OIDC-vs-keyed operator decision), which is operator-reserved -> awaiting operator decision on the signing mechanism (XREF from §29 items "Gate robustness (§27 F8 ...)" + "(§27 F9 ...)").

---

## 31. Legacy-Syscall User-Pointer Validation

`sys_write` (`src/kernel/sched/syscall.c`) casts the caller-supplied `buf` straight to `const char *` and dereferences it in kernel context: no user-range check, no overflow-safe bounds, no `copy_from_user`. A ring-3 process can therefore pass ANY kernel address and have the kernel read that memory and emit it byte by byte to serial (and to the terminal window) until it hits a NUL or `len`. The kernel links at a fixed address, so a caller can compute the address of any static it wants from the matching image. That is an arbitrary-kernel-memory disclosure primitive, not merely a bad-pointer crash: it reads out stack canaries, key material, and any secret the kernel holds in a static buffer.

> [!WARNING]
> Found 2026-07-29 by the adversarial review of `00-infrastructure/TODO-04` §25, which needed the opposite property. That section authenticates launcher records with a per-boot nonce held in a kernel static; a binary that can replay kernel memory to serial can echo the tag without ever learning its bytes, so §25's non-forgeability holds only against callers that cannot exercise this primitive. The nonce is a symptom -- the disclosure is the defect.

- [ ] Validate the whole `[buf, buf + len)` range against the user address window with overflow-safe arithmetic (a
      `buf + len` that wraps must be refused, not truncated) before any dereference, and reject a range that is not entirely user-accessible with the same status a bad handle gets. `include/kernel/mm/user_range.h` owns the window bounds; do not restate them
- [ ] Copy through a kernel bounce buffer on the recoverable user-copy path rather than dereferencing the user pointer in
      place, so a page that disappears between validation and use faults recoverably instead of taking the kernel down. `copy_from_user` already exists for the NT syscall surface (`nt_syscall.c`) -- reuse it rather than adding a second mechanism
- [ ] Audit every remaining legacy (non-NT) syscall that consumes a caller pointer for the same shape, `sys_log` included,
      and fix each rather than only the one the review named. A per-syscall inventory belongs in the commit message
- [ ] Add a ring-3 regression test that passes a kernel address to the offending syscalls and asserts a refusal rather
      than a dump, plus a bounds test for the wrapping `buf + len` case
- [ ] Commit: `"security: validate user pointers in the legacy syscall surface"`

**Test checkpoint:** A ring-3 binary calling `sys_write(1, <kernel address>, N)` receives an error status and produces NO serial output from kernel memory; a call with `buf + len` wrapping past the top of the address space is refused; ordinary user-buffer writes are unaffected and the user-mode suite stays green. Test on: QEMU TCG, QEMU KVM.

---

## 32. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 11:
- [ ] **(blocks zero-on-free)** Fix the IXFS/vfs use-after-free that `HEAP_ZERO_ON_FREE=1` surfaces, then flip the knob on
  - A vnode/inode `i_size` is read after free; the scrub zeros it -> `file->size`=0 -> `cmd.exe` load + mmap content break. Trace the node lifecycle in `src/kernel/fs/ixfs/ixfs_ops.c` + `src/kernel/fs/vfs.c`

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐   | Feature                | 🪟 Win11         | 🐧 Linux             | 🚀 Impossible OS             |
| --- | ---------------------- | --------------- | ------------------- | --------------------------- |
| 💎   | NX on data PTEs        | ✅ Long time     | ✅ Long time         | ✅ Done §1                   |
| 💎   | SMEP SMAP CR4          | ✅ Win8 / 10     | ✅ 3.x / 3.20        | ⏳ §2 partial                |
| 💎   | KPTI user PT           | ✅ Win10 PTI     | ✅ 4.15 PTI          | ⬜ §3                        |
| 💎   | PCID no flush CR3      | ✅ Yes           | ✅ Yes               | ⬜ §7                        |
| 💎   | IBRS IBPB retpoline    | ✅ Yes           | ✅ spectre           | ✅ eIBRS+IBPB §8             |
| 💎   | CET shadow stack       | ✅ 20H1+         | ✅ 6.6+              | ⏸ §9 (deferred)             |
| 💎   | CET IBT ENDBR64        | ✅ HVCI          | ✅ 6.6+              | ⏸ §10 (deferred)            |
| 💎   | Heap cookies redzone   | ✅ Pool tags     | ✅ SLUB              | ✅ §11 cookie+redzone+lock   |
| 💎   | Stack canaries /GS     | ✅ MSVC          | ✅ fssp strong       | ✅ §12                       |
| 💎   | Stack guard pages      | ✅ Yes           | ✅ THREAD            | ⏸ §13 (partial)             |
| 💎   | KASLR kernel base      | ✅ Yes           | ✅ RANDOMIZE         | ⏸ §14 (partial)             |
| ⭐   | RDRAND stack canary    | ✅ /GS           | ✅ -fstack-protector | ✅ §12 RDRAND + TSC fallback |
| ⭐   | KASLR slide in dump    | ❌ Opaque        | ❌ Opaque            | ⬜ §14 + T27                 |
| 💎   | Secure Boot lockdown   | ✅ HVCI lockdown | ✅ lockdown LSM      | ⬜ §16                       |
| 💎   | Kernel image W^X       | ✅ HVCI          | ✅ STRICT_RWX        | ✅ .text+.rodata RO          |
| 💎   | SSBD STIBP RSB BHI     | ✅ Yes           | ✅ spectre           | ⏸ §25 (deferred)            |
| 💎   | MDS VERW buf clear     | ✅ ucode         | ✅ VERW              | ✅ VERW on exit              |
| 💎   | kCFI type-safe icall   | ✅ xFG/CFG       | ✅ CFI_CLANG         | ⬜ §20                       |
| 💎   | FORTIFY_SOURCE         | ✅ MSVC chk      | ✅ _chk              | ⬜ §21                       |
| 💎   | stackleak erase stk    | ❌ No            | ✅ KSTACK_ERASE      | ⬜ §22                       |
| 💎   | KFENCE UAF/OOB sample  | ❌ No            | ✅ KFENCE            | ⬜ §23                       |
| ⭐   | Mitigation posture API | ⚠️ WMI          | ⚠️ sysfs            | ⬜ §24                       |
| 💎   | Test code out of build | ✅ free/checked  | ✅ Kconfig KUNIT off | ✅ §26/§28 TUs + §29 helpers |
| 💎   | Release flavor proven  | ✅ WHQL signing  | ✅ distro CI         | ✅ §27 seam+trace+provenance |

After §1 through §13, parity with Win11/Linux mitigations for NX through stack canaries and guard pages; §14 through §16 add KASLR, enclave/signing syscalls, and lockdown. §17 through §25 close the image-W^X (§17), 128-bit feature surface (§18), MDS/VERW data-sampling (§19), software-CFI/kCFI (§20), FORTIFY_SOURCE (§21), stackleak (§22), and KFENCE (§23) gaps, plus a queryable mitigation posture (§24, exclusive) and the transient-execution predictor policy (SSBD/STIBP/RSB/BHI/ITS/Retbleed, §25). HVCI/VBS/HVPT/HyperGuard/KDP are Win11-only (require a VTL1 hypervisor tier Impossible OS does not have); §16 lockdown is the closest analogue. KASLR (§14) and the SMEP/SMAP+KPTI split sequence behind TODO-33 higher-half relocation.

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
- [ ] **eIBRS**: on an eIBRS CPU, `rdmsr(MSR_IA32_SPEC_CTRL) & 1` is `1` PERMANENTLY (set-once, including across SYSRETQ -- no per-entry toggle); on non-eIBRS CPUs there is no SPEC_CTRL write (retpoline covers them).
- [ ] **Retpoline**: `objdump -d build/kernel.elf` shows zero compiler-generated indirect branches via register OR memory operand (`jmp/call *%reg` and `jmp/call *mem`); hand-written NASM indirect jumps are inventoried separately (§8 NASM inventory).
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
