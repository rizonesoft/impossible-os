<!-- docs: covers=todo/02-kernel-core/TODO-10-kernel-security-hardening.md sources=src/kernel/cpu_security.c,include/kernel/msr.h,include/kernel/mm/vmm.h,src/kernel/mm/vmm.c,src/kernel/mm/heap.c,src/kernel/security/stack_canary.c,src/kernel/security/wx.c,src/kernel/kpti.c,include/kernel/kpti.h,Makefile,scripts/check-release-symbols.sh reviewed=2026-09-28 order=10 -->
# Kernel Security Hardening

## What is it?

Kernel security hardening is the set of CPU and kernel mitigations that make exploits harder: no-execute data, supervisor access controls, Spectre defenses, a hardened heap and stack, a read-only kernel image, and a release build that removes its own test surface. Some of it is fully active today. Several pieces exist as detected-but-disabled code waiting on one shared prerequisite, and one legacy system-call path has a known, unfixed flaw described below.

## How does it work?

`cpu_harden()` runs on the BSP and on every AP. It sets `EFER.NXE` through `cpu_enable_nx()` in [`cpu_security.c`](../../src/kernel/cpu_security.c), and `vmm_apply_nx_policy()` marks the non-code pages of the kernel image range no-execute. Firmware regions, MMIO, UEFI runtime memory and memory above the kernel image are skipped, so no-execute does not yet cover every mapping.

SMEP and SMAP have complete code paths, `cpu_enable_smep()` and `cpu_enable_smap()`, but neither turns its CR4 bit on: both first ask `hv_supports_cr4_smep_smap()`, which returns 0 because the shared boot page tables set the User bit on every kernel page, so enabling SMEP would fault on the kernel's own code. Serial reports `SMEP: skipped` and `SMAP: skipped` on every boot and every platform. The SMAP-bracketed `copy_from_user()` and `copy_to_user()` are ready for when it is enabled.

Kernel page-table isolation (KPTI) is in the same state: `kpti_init()` in [`kpti.c`](../../src/kernel/kpti.c) sets up a trampoline page and per-CPU kernel and user CR3 fields ([`kpti.h`](../../include/kernel/kpti.h)), but both CR3 values are the same and no entry path switches them. SMEP, SMAP and KPTI all wait on the same root cause: the kernel image shares its address range with user space, and moving it to the higher half is the prerequisite ([kernel address space](../infrastructure/kernel-address-space.md)).

Spectre v2 mitigation is active. `cpu_program_bsp_eibrs()` sets `IA32_SPEC_CTRL` once per CPU when enhanced IBRS exists, and the [`Makefile`](../../Makefile) builds with `-mretpoline -mretpoline-external-thunk` so CPUs without it get retpoline thunks instead of raw indirect branches. `cpu_issue_ibpb()` runs on cross-process context switches, and an `lfence` follows `swapgs` on entry from ring 3.

The kernel heap ([`heap.c`](../../src/kernel/mm/heap.c)) puts a cookie and redzones around every allocation, checks them on free under the heap spinlock, and stops with a heap-corruption bugcheck on a mismatch. Stack canaries use `-fstack-protector-strong`; `canary_init()` seeds the guard from RDRAND, with a TSC fallback, and `__stack_chk_fail()` in [`stack_canary.c`](../../src/kernel/security/stack_canary.c) is fatal.

The kernel image is write-xor-execute: `kernel_wx_protect()` and `kernel_rodata_protect()` in [`wx.c`](../../src/kernel/security/wx.c) make `.text` and `.rodata` read-only before SMP starts and halt if that fails.

Kernel task, thread, user-thread, AP, interrupt and boot stacks have an unmapped guard page below them, installed by `vmm_install_guard_page()` in [`vmm.c`](../../src/kernel/mm/vmm.c). The table holds up to 640 entries (`VMM_MAX_GUARD_PAGES` in [`vmm.h`](../../include/kernel/mm/vmm.h)), installation reports failure instead of dropping an entry, and the split and clear happen under one lock. A guard-page fault halts with a label naming the stack. Two ring-0 stacks are still unguarded: the ones `task_create_user()` and `task_fork()` allocate with `kmalloc`.

The `KERNEL_TESTS` build setting has two values. `on`, the default, links the full test surface, including a fault-injection system call. `off` removes `src/kernel/test/` and every test-only hook, and [`check-release-symbols.sh`](../../scripts/check-release-symbols.sh) compiles each file both ways and proves that no symbol gated on `KERNEL_TESTS` survives into the release build; a test-only helper defined unconditionally in a production file would not differ between the flavors, so its separate audit for those is advisory only.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `cpu_enable_nx()` | Enable no-execute on this CPU ([`cpu_security.c`](../../src/kernel/cpu_security.c)) |
| `cpu_enable_smep()`, `cpu_enable_smap()` | SMEP and SMAP activation, currently skipped |
| `copy_from_user()`, `copy_to_user()` | SMAP-bracketed user copies |
| `cpu_program_bsp_eibrs()`, `cpu_issue_ibpb()` | Enhanced IBRS setup and the context-switch barrier |
| `kpti_init()` | KPTI trampoline and CR3 fields, not yet switched ([`kpti.c`](../../src/kernel/kpti.c)) |
| `kmalloc()`, `kfree()` | Cookie- and redzone-checked heap ([`heap.c`](../../src/kernel/mm/heap.c)) |
| `canary_init()`, `__stack_chk_fail()` | Stack canary seed and failure handler ([`stack_canary.c`](../../src/kernel/security/stack_canary.c)) |
| `kernel_wx_protect()`, `kernel_rodata_protect()` | Read-only kernel code and data ([`wx.c`](../../src/kernel/security/wx.c)) |
| `vmm_install_guard_page()` | Stack guard pages ([`vmm.c`](../../src/kernel/mm/vmm.c)) |
| `KERNEL_TESTS=on` or `off` | Build flavor ([`Makefile`](../../Makefile)) |

## How do I use it?

Everything except the build flavor is always on; there is no `boot.conf` switch for no-execute, heap checks, canaries or W^X.

```bash
KERNEL_TESTS=off bash scripts/build.sh    # release flavor without the test surface
bash scripts/check-release-symbols.sh     # proves no flavor-gated test symbol reached the release image
bash scripts/test.sh SUITE=mm             # heap, W^X and guard-page tests
bash scripts/test.sh SUITE=security       # canary tests
bash scripts/test.sh SUITE=x86            # Spectre mitigation tests
```

Serial shows what actually turned on: the SMEP and SMAP skip lines, and `[wx]` lines reporting `.text` and `.rodata` read-only.

## What is not implemented yet?

- **SMEP and SMAP** are never enabled while kernel pages carry the User bit ([SMEP & SMAP: CR4 Activation + CLAC/STAC Wrappers](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#2-smep--smap-cr4-activation--clacstac-wrappers)).
- **KPTI** never switches CR3 on entry or exit ([KPTI SYSCALL CR3 Swap](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#4-kpti-syscall-cr3-swap)).
- **CET** shadow stacks and indirect branch tracking are not enabled ([CET Shadow Stack](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#9-cet-shadow-stack-kernel-ring-0)).
- **KASLR** waits on the same higher-half move ([KASLR](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#14-kaslr-rdrand-kernel-load-address)).
- **Secure Boot lockdown enforcement** is not wired ([Secure Boot Lockdown Enforcement](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#16-secure-boot-lockdown-enforcement)).
- **kCFI, FORTIFY_SOURCE, stack erasing on return to user, and KFENCE** are not in the tree ([kCFI](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#20-kcfi-software-control-flow-integrity-for-indirect-calls), [FORTIFY_SOURCE](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#21-fortify_source-bounds-checked-strmem-builtins)).
- **The remaining speculative-execution mitigations** (SSBD, STIBP, RSB stuffing, BHI and others) and a queryable mitigation report ([Spectre Predictor-Policy Mitigations](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#25-spectre-predictor-policy-mitigations-ssbd-stibp-rsb-bhi-its-retbleed)).
- **Legacy system-call pointer checks.** The legacy `sys_write`, `sys_read` and `sys_readfile` calls use a caller's pointer without checking that it points into user space. The fix is written but does not fit the current kernel image budget ([Legacy-Syscall User-Pointer Validation](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#31-legacy-syscall-user-pointer-validation)).
- **Guard pages for user-process kernel stacks**, and **a dedicated stack-overflow stop code**: a guard-page fault still uses the generic page-fault panic ([Kernel Stack Guard Pages](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md#13-kernel-stack-guard-pages)).

## How does it compare with Windows 11 and Linux?

Both ship no-execute, SMEP and SMAP, KPTI, Spectre v2 mitigations, CET, heap and stack hardening, guard pages, KASLR and a read-only kernel image; Linux also has stackleak and KFENCE. Impossible OS matches them on the Spectre v2 core, heap cookies and redzones, stack canaries, the read-only image and a checked release build without the flavor-gated test surface, and partly on no-execute (kernel image only) and guard pages (not yet on user-process kernel stacks). SMEP, SMAP, KPTI and KASLR wait on the higher-half kernel move, and CET, kCFI, FORTIFY_SOURCE, stackleak and KFENCE are not built. Windows HVCI and VBS need a hypervisor layer this project does not have.

## See also

- [Kernel Security Hardening roadmap](../../todo/02-kernel-core/TODO-10-kernel-security-hardening.md)
- [Kernel Address Space](../infrastructure/kernel-address-space.md)
- [Bare Metal Boot Hardening](../boot/bare-metal-hardening.md)
- [x86-64 Architecture Features](x86-64-architecture.md)
