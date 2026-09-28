<!-- docs: covers=todo/03-memory-concurrency/TODO-02-memory-security.md sources=include/kernel/cpu_security.h,src/kernel/cpu_security.c,include/kernel/kpti.h,src/kernel/kpti.c,src/kernel/security/wx.c,include/kernel/nt/mitigation_policy.h,src/kernel/test/test_cpu_security.c,src/kernel/mm/vmm.c,src/kernel/main/boot_hw.c reviewed=2026-09-28 order=2 -->
# Memory Security Hardening

## What is it?

The set of CPU and page-table defences that stop a memory bug from becoming code execution: no-execute data pages, SMEP and SMAP, kernel page-table isolation (KPTI), address randomisation for the kernel and for processes, and CET shadow stacks. Only no-execute enforcement and the read-only kernel image are active today; SMEP, SMAP and KPTI have code that is deliberately not switched on, and user ASLR, CET and the boot security report are not built.

## How does it work?

This roadmap overlaps the [Kernel Security Hardening](../kernel/kernel-security-hardening.md) roadmap, which owns SMEP, SMAP, KASLR and KPTI and holds their code. Four of this file's eight sections are marked superseded by it, and this page describes the shared code from the memory side.

No-execute is on. `cpu_enable_nx()` in [`cpu_security.c`](../../src/kernel/cpu_security.c) sets `EFER.NXE`, and the VMM's `vmm_apply_nx_policy()` marks data pages no-execute during Phase 0; [`wx.c`](../../src/kernel/security/wx.c) then makes `.text` and `.rodata` read-only with `kernel_wx_protect()` and `kernel_rodata_protect()`.

SMEP and SMAP have complete enable paths, `cpu_enable_smep()` and `cpu_enable_smap()`, declared in [`cpu_security.h`](../../include/kernel/cpu_security.h). They skip turning the CR4 bits on because the shared boot page tables carry the User bit on kernel pages, so enabling SMEP would fault on the kernel's own code. The user-copy side is already in place: `copy_from_user()` and `copy_to_user()` bracket the copy with `stac`/`clac` through `KERNEL_ACCESS_USER_BEGIN` and `KERNEL_ACCESS_USER_END`, and recover from a fault on a bad user pointer through the `__uaccess_copy_from`/`__uaccess_copy_to` fixups, so enabling SMAP later changes no caller.

KPTI is prepared but inactive. [`kpti.h`](../../include/kernel/kpti.h) places the entry trampoline at `KPTI_TRAMPOLINE_VA` (`0xFFFFFFFFFFFFF000`) and gives each CPU kernel and user CR3 fields; [`kpti.c`](../../src/kernel/kpti.c) keeps `s_kpti_active` at 0 until the SYSCALL and IDT entry paths are redirected through the trampoline.

For processes, [`mitigation_policy.h`](../../include/kernel/nt/mitigation_policy.h) defines the Win32 mitigation policy classes, including `ProcessASLRPolicy`, but no code randomises process bases yet.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `cpu_enable_nx()`, `cpu_enable_smep()`, `cpu_enable_smap()`, `cpu_enable_umip()`, `cpu_harden()` | CPU feature activation ([`cpu_security.h`](../../include/kernel/cpu_security.h)) |
| `copy_from_user()`, `copy_to_user()` | Fault-recoverable, SMAP-bracketed user copies |
| `KERNEL_ACCESS_USER_BEGIN`, `KERNEL_ACCESS_USER_END` | Explicit user-access windows for code that cannot use the copy helpers |
| `kpti_init()`, `kpti_active()` | KPTI trampoline setup and state ([`kpti.h`](../../include/kernel/kpti.h)) |
| `kernel_wx_protect()`, `kernel_rodata_protect()` | Read-only kernel text and read-only data ([`wx.c`](../../src/kernel/security/wx.c)) |

## How do I use it?

Kernel code that reads or writes user memory calls `copy_from_user()` or `copy_to_user()` and checks for `-1`, never dereferencing a user pointer directly. Serial output at boot shows what actually turned on: the SMEP and SMAP skip lines and the `[wx]` read-only lines. The CPU security suite checks `EFER.NXE`, the CR4 state, the copy helpers and their fault injection in [`test_cpu_security.c`](../../src/kernel/test/test_cpu_security.c):

```bash
bash scripts/test.sh SUITE=x86
```

## What is not implemented yet?

- **User-space ASLR.** Every process loads at fixed bases ([User-Space ASLR](../../todo/03-memory-concurrency/TODO-02-memory-security.md#1-user-space-aslr-opus)).
- **KASLR.** The kernel loads at a fixed address; the owner is the hardening roadmap's KASLR section ([KASLR](../../todo/03-memory-concurrency/TODO-02-memory-security.md#2-kaslr----kernel-address-space-randomization-opus)).
- **SMEP and SMAP enforcement.** Blocked on per-process page tables that drop the User bit from kernel pages ([SMEP](../../todo/03-memory-concurrency/TODO-02-memory-security.md#3-smep-opus), [SMAP](../../todo/03-memory-concurrency/TODO-02-memory-security.md#4-smap--copy_from_user--copy_to_user-opus)).
- **An audit of every mapping for NX** and the boot log line that reports it ([NX / DEP Mapping Audit](../../todo/03-memory-concurrency/TODO-02-memory-security.md#5-nx--dep-mapping-audit-opus)).
- **KPTI switching.** No entry path changes CR3 yet ([KPTI Basic](../../todo/03-memory-concurrency/TODO-02-memory-security.md#6-kpti-basic-opus)).
- **CET shadow stacks** ([CET Shadow Stack Stub](../../todo/03-memory-concurrency/TODO-02-memory-security.md#7-cet-shadow-stack-stub-opus)).
- **The `secinfo` boot security report** ([Security Layout Report](../../todo/03-memory-concurrency/TODO-02-memory-security.md#8-security-layout-report-sonnet)).

## How does it compare with Windows 11 and Linux?

Windows 11 and Linux both ship KASLR, SMEP, SMAP, NX on all data, KPTI (KVA Shadow on Windows) and CET shadow stacks, and Linux randomises every process by default while Windows opts in per image. Impossible OS enforces NX and a read-only kernel image and has the SMAP-ready copy helpers, but every other row of the roadmap's comparison table is still open. The two planned differences are ASLR that is mandatory for every process rather than opt-in, and a single boot-time security summary that neither Windows nor Linux provides.

## See also

- [Memory Security Hardening roadmap](../../todo/03-memory-concurrency/TODO-02-memory-security.md)
- [Kernel Security Hardening](../kernel/kernel-security-hardening.md)
- [Virtual Memory Protection](vmm-memory-protection.md)
- [Higher-Half Kernel](../kernel/higher-half-kernel.md)
