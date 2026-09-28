<!-- docs: covers=todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md sources=include/kernel/mm/memmap.h,include/kernel/mm/user_range.h,src/boot/linker.ld,src/kernel/mm/vmm.c,include/kernel/mm/vmm.h,tools/memmap-check/check.sh reviewed=2026-09-28 order=33 -->
# Higher-Half Kernel Relocation

## What is it?

Higher-half relocation is the plan to move the kernel from the low address where it links today (1 MiB) into the upper canonical half of the 64-bit address space, as Windows 11 and Linux both do, and to give user space the whole private lower half instead of the fixed window at `0x800000`. It is the prerequisite for KASLR, a clean SMEP and SMAP split, page-table isolation and PCID. The layout design and the direct physical map have shipped; the relocation itself is parked on a sequencing decision. The decisions and their reasoning are in the [Kernel Address Space](../infrastructure/kernel-address-space.md) design document; this page is the status overview.

## How does it work?

[`memmap.h`](../../include/kernel/mm/memmap.h) is the single source of truth for the address-space layout: it defines `MM_KERNEL_VIRT_BASE` (`0xffffffff80000000`), `MM_KERNEL_PHYS_BASE` (`0x200000`), and the direct-map (HHDM), MMIO and per-CPU windows, each pinned by `_Static_assert` for canonical form and non-overlap. The bootloader installs the HHDM, a fixed-offset alias of physical memory, and the kernel's page-table walkers are being moved onto it: `pt_walk()` in [`vmm.c`](../../src/kernel/mm/vmm.c) resolves through the direct map and derives the physical CR3 root with `mm_hhdm_to_phys()`, and the remaining walker conversions are still open. The two translation helpers are deliberately separate: `mm_hhdm_to_phys()` and `mm_image_virt_to_phys()` each reject the other's inputs, so a linker address can never be mistaken for a direct-map address.

None of this has changed where the kernel runs. [`linker.ld`](../../src/boot/linker.ld) still sets `. = 1M`, so the kernel executes at a low address as before, and the identity map remains live. The flip itself (the linker VMA/LMA split and the jump in the bootloader) is parked: at the target physical load address a full-size kernel image collides with the low physical range that user-mode `exec` still writes into (`USER_ELF_BASE` = `0x800000` in [`user_range.h`](../../include/kernel/mm/user_range.h)), and choosing between moving user processes to private frames first or re-basing the kernel's load address is left to the operator. Everything after the flip is blocked behind the same decision.

Meanwhile the low layout still presses on the kernel. The kernel image must end below `USER_BASE` (`0x800000`), and `scripts/build.sh` refuses a build that crosses it. A series of tactical passes moved large static pools to dynamic allocation to buy room; at commit `f26c21502` the built image's `__kernel_end` symbol is `0x7da000` (`llvm-nm-19 build/kernel.exe`), which leaves 152 KiB before the ceiling.

## What are its interfaces?

| Interface | Purpose |
| --- | --- |
| `MM_KERNEL_VIRT_BASE`, `MM_KERNEL_PHYS_BASE`, `MM_HHDM_BASE` and the window asserts ([`memmap.h`](../../include/kernel/mm/memmap.h)) | Layout constants, the single source of truth |
| `mm_hhdm_to_phys()`, `mm_image_virt_to_phys()` | Type-split translation helpers, each rejecting the other's inputs |
| `pt_walk()`, `vmm_get_kernel_cr3()` ([`vmm.c`](../../src/kernel/mm/vmm.c)) | Page-table walking through the direct map; CR3 root derived on demand |
| `USER_ELF_BASE`, `USER_ELF_END`, `USER_PD_INDEX` ([`user_range.h`](../../include/kernel/mm/user_range.h)) | The low user window the relocation would retire |
| [`tools/memmap-check/check.sh`](../../tools/memmap-check/check.sh) | Host gcc gate for the layout constants and the top-of-address-space wrap |

## How do I use it?

There is no runtime change to use yet; the kernel boots low with the direct map present alongside the identity map. The layout invariants are checked on the host, independent of a kernel build, and the running kernel by the normal suites:

```bash
bash tools/memmap-check/check.sh
bash scripts/test.sh SUITE=mm
bash scripts/test-smoke.sh
```

## What is not implemented yet?

- **The remaining walker conversions.** Some page-table dereferences still go through the identity map ([VMM Walker Conversion -- Route Every Deref Through the HHDM Helper](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md#9-vmm-walker-conversion----route-every-deref-through-the-hhdm-helper)).
- **The linker split and the jump.** Parked on the operator decision about the physical collision with the `exec` window ([Linker VMA/LMA Split + Higher-Half Jump](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md#3-linker-vmalma-split--higher-half-jump)).
- **Descriptor tables, per-CPU state and AP bring-up at high addresses** ([Descriptor Tables + Per-CPU at High Addresses + AP Path](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md#4-descriptor-tables--per-cpu-at-high-addresses--ap-path)).
- **The high `boot_info` and framebuffer handoff and identity-map teardown** ([Bootloader / `boot_info` / Framebuffer High Handoff](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md#5-bootloader--boot_info--framebuffer-high-handoff)).
- **A shared high kernel half with a private low user half per process** ([Per-Process PML4: Kernel High Shared, User Low Private](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md#6-per-process-pml4-kernel-high-shared-user-low-private)).
- **Retiring the `0x800000` ceiling and its build guard** ([Retire the `0x800000` USER_BASE Ceiling + BSS Guard](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md#7-retire-the-0x800000-user_base-ceiling--bss-guard)).
- **Optional 5-level paging** ([5-Level Paging (LA57) Support -- Exceeds Win11](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md#8-5-level-paging-la57-support----exceeds-win11)).

## How does it compare with Windows 11 and Linux?

Windows 11 and Linux both run the kernel in the upper canonical half; Linux keeps a direct physical map, while Windows maps physical memory through its PFN database and dynamic PTEs. Both build page-table isolation (KVA Shadow, KPTI), KASLR, SMEP and SMAP, and per-process PCID tagging on that layout. Impossible OS has pinned the same target layout and built the direct map, and it already turns on `CR4.PCIDE` at boot, but the kernel still runs low, so isolation, KASLR, SMEP and SMAP, and per-process PCID tags with no-flush switches do not exist yet. The one planned point beyond Windows is 5-level paging: Linux supports LA57, Windows does not, and it is planned here but not started.

## See also

- [Higher-Half Kernel Relocation roadmap](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md)
- [Kernel Address Space](../infrastructure/kernel-address-space.md)
- [Kernel Security Hardening](kernel-security-hardening.md)
- [Kernel Bulletproofing](kernel-bulletproofing.md)
