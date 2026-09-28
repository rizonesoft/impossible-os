<!-- docs: covers=todo/02-kernel-core/TODO-31-kernel-bulletproofing.md sources=include/kernel/smp.h,include/kernel/boot_info.h,include/kernel/mm/user_range.h,include/kernel/idt.h,src/kernel/smp/ap_trampoline.asm,include/kernel/nt/service_numbers.h,include/kernel/vectors.h,src/kernel/idt.c,src/kernel/sched/task.c,src/kernel/mm/vmm.c,include/kernel/mm/vmm.h,include/kernel/fs/ixfs.h,include/kernel/security/sid.h,include/kernel/security/acl.h,include/kernel/security/token.h,src/kernel/drivers/framebuffer.c,user/user.ld,src/kernel/test/test_bulletproof.c,include/kernel/config.h,src/kernel/test/test_kernel_config.c reviewed=2026-09-28 order=31 -->
# Kernel Bulletproofing

## What is it?

Kernel bulletproofing is a five-layer defense applied to each fragile cross-file invariant in the kernel: a struct layout an assembly stub reads by fixed offset, a constant two independent components must agree on, a register-save order the interrupt path depends on. The layers are a compile-time `_Static_assert`, a runtime check at boot or first use, a unit test, a canary (guard page, checksum or stuck-state timer) where one fits, and a comment naming the invariant and who depends on it. The goal is that a change which silently breaks such a dependency fails the build or halts at boot with a named error, instead of corrupting state later.

## How does it work?

Each roadmap section picks one invariant and gives it all five layers; the GDT user-segment order that `SYSRET` depends on is the reference example. Two more show the shape. `struct per_cpu_data`, which `syscall_entry.asm` and the ISR stubs read through fixed `gs:` offsets, is pinned by eight `offsetof` asserts in [`smp.h`](../../include/kernel/smp.h) and checked by the `BP: assembly offsets` suite in [`test_bulletproof.c`](../../src/kernel/test/test_bulletproof.c). `struct interrupt_frame`, the register block the common ISR stub pushes and `iretq` restores, is pinned in [`idt.h`](../../include/kernel/idt.h) (`sizeof == 176` plus per-field offsets), and `isr_handler()` in [`idt.c`](../../src/kernel/idt.c) checks at runtime that the saved `CS` is a valid kernel or ring-3 selector and that the `gs:0` self-pointer reads back correctly, which catches a missed or doubled `swapgs`.

The same pattern covers the `boot_config` struct the bootloader and kernel both parse ([`boot_info.h`](../../include/kernel/boot_info.h), with `cmdline` pinned at offset 32) and the later `kernel_config_t` ([`config.h`](../../include/kernel/config.h), size and field offsets asserted, magic, version and size checked in `test_kernel_config.c`); the user-mode ELF range in [`user_range.h`](../../include/kernel/mm/user_range.h), mirrored by hand in [`user/user.ld`](../../user/user.ld); the AP trampoline data area at physical `0x8E00` ([`ap_trampoline.asm`](../../src/kernel/smp/ap_trampoline.asm)); IDT vector assignments in [`vectors.h`](../../include/kernel/vectors.h), whose asserts keep vectors such as `VECTOR_NT_SYSCALL` (`0x2E`) and `VECTOR_LINUX_SYSCALL` (`0x80`) unique and out of reserved ranges, backed by a warning in `idt_register_handler()` when a handler is overwritten; SSDT capacity in [`service_numbers.h`](../../include/kernel/nt/service_numbers.h), where the asserts bound `SSDT_MAIN_COUNT` and `SSDT_LAST_MAIN_INDEX` against the 1024-slot table and each other but do not count the `SSDT_Nt*` defines (an exact-count drift is left to `/audit-ssdt`); XSAVE and FXSAVE alignment in [`task.c`](../../src/kernel/sched/task.c); guard pages at stack and heap boundaries (`vmm_install_guard_page()` in [`vmm.c`](../../src/kernel/mm/vmm.c), tracked in a table of `VMM_MAX_GUARD_PAGES` = 640 entries); the IXFS on-disk layout ([`ixfs.h`](../../include/kernel/fs/ixfs.h)); the Windows security structures ([`sid.h`](../../include/kernel/security/sid.h), [`acl.h`](../../include/kernel/security/acl.h), [`token.h`](../../include/kernel/security/token.h)); the `exec_pending` scheduler flag, force-cleared after `EXEC_PENDING_STUCK_TICKS` (10) ticks; and the framebuffer bring-up checks in [`framebuffer.c`](../../src/kernel/drivers/framebuffer.c).

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `_Static_assert` in `smp.h`, `idt.h`, `boot_info.h`, `vectors.h`, `ixfs.h`, `sid.h`, `acl.h` | Compile-time layer: layout and constant checks |
| `test_register_bulletproof()` ([`test_bulletproof.c`](../../src/kernel/test/test_bulletproof.c)) | Registers the seven `BP:` suites under `TEST_CAT_ABI` |
| `vmm_install_guard_page()`, `vmm_split_huge_page()` | Canary layer: unmapped pages that turn an overflow into a labeled page fault ([`vmm.h`](../../include/kernel/mm/vmm.h)) |
| `isr_handler()` selector and `gs:0` checks | Runtime layer: per-interrupt frame and `swapgs` symmetry verification |
| `idt_register_handler()` overwrite warning | Runtime layer: vector collision detection |
| `SSDT_MAIN_COUNT`, `SSDT_LAST_MAIN_INDEX` | Compile-time bounds on the SSDT table ([`service_numbers.h`](../../include/kernel/nt/service_numbers.h)) |

## How do I use it?

Bulletproofing is a construction rule, not a runtime feature: when new kernel code creates a cross-file invariant, it gets the same five layers. The existing layers are verified by a clean build (every `_Static_assert` must hold) and by the ABI suite:

```bash
bash scripts/build.sh clean
bash scripts/test.sh SUITE=abi    # includes the seven BP: suites
```

On Windows, `scripts\debug\kernel\run-abi-tests.bat` runs the same suite under QEMU.

## What is not implemented yet?

Every implementation section of the roadmap has shipped. What remains open is the file's final verification pass: deliberately breaking each invariant one at a time to confirm it fails the build or halts the boot, and confirming the guard pages on VirtualBox and bare metal, not only QEMU ([Verification](../../todo/02-kernel-core/TODO-31-kernel-bulletproofing.md#verification)).

## How does it compare with Windows 11 and Linux?

Neither operating system documents a repo-wide invariant discipline of this shape. Linux uses `BUILD_BUG_ON` in places and generates `asm-offsets.h` for assembly-visible structures; Windows keeps its equivalents internal. Guard pages at stack boundaries exist in both (Windows kernel stacks, Linux `VMAP_STACK`); Impossible OS applies them to heap, user range, AP and IST stacks too. The `swapgs` self-pointer check and the stuck-scheduler-flag timer have no documented counterpart in either.

## See also

- [Kernel Bulletproofing roadmap](../../todo/02-kernel-core/TODO-31-kernel-bulletproofing.md)
- [Kernel Address Space](../infrastructure/kernel-address-space.md)
- [SSDT Master Table](ssdt-master-table.md)
- [Kernel Security Hardening](kernel-security-hardening.md)
