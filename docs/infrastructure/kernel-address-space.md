<!-- docs: covers=todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md -->
# Kernel Address Space -- Higher-Half Layout and Direct Map

> Canonical reference for the Impossible OS 64-bit virtual memory map. The machine-readable form of everything
> below is [`include/kernel/mm/memmap.h`](../../include/kernel/mm/memmap.h), where every window base and extent is
> pinned by `_Static_assert`. If this document and that header ever disagree, the header is authoritative and this
> document is the bug.
>
> Owner: [Higher-Half Kernel Relocation](../../todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md).
> The design section (this document) gates every later section of that TODO.

## Why this exists

The kernel currently links and loads **low**: `src/boot/linker.ld` sets `. = 1M`, and the image grows upward toward
the user-mode ELF range pinned at `0x800000` by `include/kernel/mm/user_range.h`. Kernel `.bss` and `USER_BASE` are
therefore on a collision course, and `scripts/build.sh` carries a hard `BSS COLLISION` guard to catch it.

As of 2026-08-29 that guard is **live, not theoretical**: `__kernel_end` is `0x7f9000`, leaving 28 KiB under the
`0x800000` firmware floor (`build/kernel.map`, and `scripts/build.sh` prints the headroom on every build). Read the
current figure from that build output rather than this line: it moves with every change to `.text`/`.bss`, and
`01-boot-platform/TODO-10` sections 31-32 moved it twice (down when the 16 KiB BSS stack array was removed, up again
as the boot-stack code landed). [Live-environment adoption](../../todo/02-kernel-core/TODO-22-environment-variables.md#23-live-environment-adoption-setcurrentenvironment-setenvironmentstrings-createenvironmentex)
tripped it on its first build by adding roughly 4 KiB of `.text`, and is now deferred on this work. Every kernel
section behind it is in the same position. Moving the kernel into the upper canonical half removes the ceiling
permanently and is the structural prerequisite for KASLR, a clean SMEP/SMAP split, KPTI, and PCID.

## The layout

Four-level paging, 48-bit canonical. Bases and extents are `MM_*` constants in `memmap.h`.

| Window | Base | Extent | Contents |
|---|---|---|---|
| User | `0x0000000000000000` | 128 TiB | Private per-process lower half |
| *(hole)* | -- | -- | Non-canonical gap (bits 63:47 must be uniform) |
| HHDM direct map | `0xffff888000000000` | 64 TiB | Fixed-offset alias of physical RAM; NX + RW |
| MMIO / fixmap | `0xffffc90000000000` | 32 TiB | Device memory (UC) and framebuffer (WC) |
| Per-CPU | `0xffffea0000000000` | 1 TiB | Per-CPU data reached via `GS_BASE` |
| Kernel image | `0xffffffff80000000` | 2 GiB | `.text` / `.rodata` / `.data` / `.bss` |

The kernel image is loaded at physical `MM_KERNEL_PHYS_BASE` (`0x200000`, 2 MiB) and linked at
`MM_KERNEL_IMAGE_BASE` = `MM_KERNEL_VIRT_BASE + MM_KERNEL_PHYS_BASE` = `0xffffffff80200000`.

## Decision 1 -- `KERNEL_VIRT_BASE` = `0xffffffff80000000`

**This is forced by the toolchain, not chosen by preference.**

The kernel is already compiled with `-mcmodel=kernel` (`Makefile:25`, alongside `-fno-pie` and `-mno-red-zone`).
That code model emits `R_X86_64_32S` relocations -- sign-extended 32-bit -- for every symbol reference, which only
resolve inside the top 2 GiB of the address space. Measured against our exact toolchain (clang-19 / ld.lld-19):

- Linking `-mcmodel=kernel` objects at `0xffffffff80000000` succeeds; `readelf -l` reports the `LOAD` at that base.
- Linking the same objects at a Windows-style `0xFFFF800000000000` **hard-fails**:
  `relocation R_X86_64_32S out of range: -140737488355280 is not in [-2147483648, 2147483647]`.
- The Windows-style base is only reachable via `-mcmodel=large`, which turns every symbol reference into a 64-bit
  absolute (`R_X86_64_64`) -- strictly worse codegen for no benefit here.

The kernel's file format and code model are strictly internal (see CLAUDE.md, "Kernel Binary Format"); nothing
Win32-ABI-visible depends on the kernel image's own base. Matching Linux's `-2 GiB` convention costs nothing and
buys direct toolchain support.

`MM_KERNEL_VIRT_BASE` is canonical under **both** 4-level and 5-level paging, so enabling LA57 never moves the
kernel image. The HHDM base does not share that property -- see Decision 6.

## Decision 2 -- bring-up site: the bootloader

There is **no `entry.asm`** in the tree; several comments claim one exists and they are stale. The real sequence is:

1. `load_kernel()` (`src/boot/uefi/bootx64.c:7811`) copies `PT_LOAD` segments and resolves `kernel_main` from the
   ELF symbol table, taking `st_value` **literally** -- no relocation is applied.
2. `ExitBootServices()` runs.
3. `setup_page_tables()` (`bootx64.c:10625`) builds the only early page tables: a 4 GiB identity map of 2 MiB pages,
   PML4 at fixed physical `0x70000`.
4. `jump_to_kernel()` (`bootx64.c:10660`) loads `CR3` and immediately calls `kernel_main` through a C function
   pointer.

Because `vmm_init()` (`src/kernel/mm/vmm.c:1407`) permanently *adopts* whatever PML4 is live in CR3 and never
rebuilds it, the table `setup_page_tables()` builds is the kernel's page table for the life of the system.

**The switch therefore runs in the bootloader.** `setup_page_tables()` additionally installs the high-half mapping,
and the existing call at `bootx64.c:10675` *is* the low-to-high transition:

- At the call, RIP is still low and identity-mapped, and the stack is the UEFI stack (also identity-mapped).
- CR3 already carries both the identity map and the high-half mapping.
- The call target is `kernel_main`'s high `st_value`, which that CR3 maps.
- CS is already a long-mode selector, so **no far jump, `lretq`, or CS reload is required.**

The alternative (a new `src/kernel/entry.asm` stub) would require changing the bootloader's symbol resolution to
target a low stub instead of `kernel_main`, and writing new assembly, for no benefit.

**Resequencing consequence:** with the bootloader site, `kernel_main` executes high from its first instruction, so
there is no "low kernel boots, reinterprets `boot_info` later" phase. The boot_info and framebuffer
pointer-translation work is **co-designed with the bring-up section** and ships with it, exactly as the linker split
and bring-up already must.

## Decision 3 -- ELF handoff

`load_kernel()` **already copies by `p_paddr`** (`bootx64.c:8506`), and already checks bounds against `p_paddr`.
Today that is indistinguishable from copying by `p_vaddr` because `linker.ld` has no `AT()` directive, so
`p_vaddr == p_paddr` for every segment. **No loader change is required** -- the original assumption that
`load_kernel()` must be rewritten does not hold.

What the linker section must do instead is make the split explicit:

- `.section : AT(ADDR(.section) - MM_KERNEL_VIRT_BASE)` so VMA is high and LMA stays low.
- Export `__kernel_phys_start` / `__kernel_phys_end` alongside the existing virtual `__kernel_start` /
  `__kernel_end`.

Verified against clang-19 / ld.lld-19 with a probe linker script:

```
LOAD  0x001000  0xffffffff80100000  0x0000000000100000  R E    <- VirtAddr high, PhysAddr low
__kernel_phys_start = 0x0000000000100000                       <- true physical
kernel_main         = 0xffffffff80100020                       <- st_value IS a high VA
```

That last line is why Decision 2 matters: once the linker split lands, `jump_to_kernel()` calls a high address, and
it only works if the high mapping is live in the CR3 loaded moments earlier.

## Decision 4 -- physical-to-virtual scheme: fixed-offset HHDM

A fixed-offset direct map (`virt = phys + MM_HHDM_BASE`) lets the kernel reach physical memory without
identity-mapping low memory after the switch.

**There are two distinct physical-to-virtual relations, and conflating them is the primary bug class here:**

| Relation | Applies to | Conversion |
|---|---|---|
| HHDM | Arbitrary RAM (page tables, PMM frames, buffers) | `virt = phys + MM_HHDM_BASE` |
| IMAGE | Kernel linker symbols (`__text_start`, `__kernel_end`, ...) | `virt = phys + MM_KERNEL_VIRT_BASE` |

A kernel symbol address is **not** an HHDM address. `memmap.h` therefore exposes them as separate, range-checked
APIs, and `mm_hhdm_to_phys()` **rejects** an image-window address rather than returning a plausible-looking wrong
physical address. A single blended `virt_to_phys(v) = v - HHDM_BASE` would silently corrupt every image symbol it
touched.

Page-table entries and CR3 always hold **physical** addresses. Any dereference of one goes through
`mm_phys_to_hhdm()`.

## Decision 5 -- direct-map alias policy

- **Sparse, RAM-only.** Alias only memory the UEFI memory map reports as usable RAM, after validation. The direct
  map is not a blanket alias of the physical address space.
- **Never MMIO.** Device memory stays in the MMIO window via `vmm_map_mmio_uc()`; framebuffer via
  `vmm_map_mmio_wc()`. Aliasing MMIO through a WB direct-map page fails on real hardware (CLAUDE.md, Bare Metal
  Gotchas).
- **NX + writable, never User.** The direct map carries data, never code, and is supervisor-only.
- **No writable alias of the kernel image.** A writable, executable low alias of kernel text silently defeats the
  W^X policy `src/kernel/security/wx.c` enforces, no matter how correct the image window's own permissions are.
  Either omit kernel-image pages from the direct map or mirror their final RO/NX permissions.

## Decision 6 -- transition plan and identity-map teardown

The bring-up identity map is **transient**. It is not retained.

This is worth stating plainly because the naive reading of the AP constraint (below) suggests keeping the 4 GiB
identity map forever, and that would be a security regression: `setup_page_tables()` marks every 2 MiB page
`Present | Writable | User | PageSize` (`bootx64.c:10648`) with no NX. Retaining it would preserve a writable,
executable, **user-accessible** alias of all kernel text and data at low addresses -- defeating Decision 5's alias
policy and `kernel_wx_protect()`, and handing ring 3 direct access to physical memory if those entries ever leaked
into a process PML4.

Teardown sequence:

1. **During bring-up:** identity map covers the bootloader's own RIP and stack so the CR3 load does not fault
   mid-stream, plus the high-half mapping for the call target.
2. **After the jump:** switch to a high-virtual stack; shrink the identity map to the AP low envelope only, and
   clear the User bit on what remains.
3. **After every AP acknowledges high entry:** retire the envelope with synchronized TLB invalidation, subject to a
   timeout policy.

### The AP low envelope is a hard constraint

`src/kernel/smp/ap_trampoline.asm` starts at `[ORG 0x8000]` with `AP_DATA` at `0x8E00`, and `smp.c:317` hands each
AP the BSP's **live** CR3. The AP's protected-mode stub loads it with a **32-bit** `mov eax, [AP_DATA]; mov cr3, eax`
(`ap_trampoline.asm:73`). Two consequences, both permanent:

- **The kernel PML4 physical frame must stay below 4 GiB** (`MM_PML4_PHYS_LIMIT`). A 32-bit load cannot express
  more.
- **The trampoline envelope must be identity-mapped in the same live CR3 that carries the high half**, until all APs
  ack. The AP path has no mechanism to load a second CR3 partway through bring-up, and `ap_entry`'s high address
  must already be reachable under that CR3.

### Canonical-address reconstruction is mandatory in every walker

`vmm_apply_nx_policy()` reconstructs virtual addresses from page-table indices as
`page_base = (pml4i << 39) | (pdpti << 30) | (pdi << 21)` (`vmm.c:1475`). For `pml4i = 511` that yields
`0x7F80000000000` -- a **positive, non-canonical** value that can never compare equal to a high `__text_start`.
Left as-is, the NX walk would silently skip the entire high-half kernel and still report NX as enabled: data stays
executable and nothing complains.

Every page-table walker must reconstruct addresses through `mm_canonical_from_indices()`, and the hardcoded
`kernel_base = 0x100000` at `vmm.c:1439` must become image-window bounds.

### Page-table walkers must stop dereferencing raw physical addresses

Today the VMM treats physical addresses as directly dereferenceable pointers, which is only true under the identity
map:

- `zero_page()` writes through a raw physical address (`vmm.c:178`).
- `get_or_create_table()` returns a raw PTE physical address as a pointer (`vmm.c:213`).
- `vmm_init()` builds `kernel_pml4` from a masked raw CR3 (`vmm.c:1407`).

Keeping the PML4 root below 4 GiB for the AP's sake does **not** make these valid once the broad identity map is
gone. The bring-up section converts each to `mm_phys_to_hhdm()`, keeping the stored PTE/CR3 values physical.

## Decision 7 -- image mapping granularity and the 2 MiB alignment rule

The image window is mapped with **4 KiB pages** so W^X can be enforced per section: with 2 MiB pages, `.text` and
`.rodata` share a page and one of them necessarily gets the wrong permissions.

Independently, `MM_KERNEL_PHYS_BASE` is pinned to **2 MiB** (`0x200000`) rather than the historical 1 MiB. A `PS=1`
(2 MiB) PDE requires a 2 MiB-aligned physical frame; `0x100000` sets PDE bit 20, which is **reserved** in a huge
PDE. Mapping the image window to a 1 MiB physical base with huge pages raises a reserved-bit page fault before
`kernel_main` can report anything. Pinning the LMA 2 MiB-aligned removes that hazard class permanently and keeps the
huge-page option legal for windows that want it, at a cost of 1 MiB of low physical memory.

The extra page-table pages the high-half mapping needs (PDPT, PD, and PTs for the image) must be reserved in the
bootloader's fixed low block **before** `ExitBootServices()` -- the existing six pages at `0x70000`-`0x75000` are
fully consumed by the identity map, and no allocator exists after EBS.

## The top-of-address-space wrap (and why a host gate exists)

The image window ends at `0xffffffffffffffff`. Any exclusive upper bound for it therefore **wraps to zero**:

```
MM_KERNEL_VIRT_BASE + MM_KERNEL_IMAGE_SIZE
  = 0xffffffff80000000 + 0x80000000
  = 0x0000000000000000      <- wraps
```

A range check written the ordinary way (`v >= BASE && v < BASE + SIZE`) thus reduces to `v < 0`, a constant false, and the API rejects **every** address it is asked about -- including every valid kernel symbol. `memmap.h` consequently uses the inclusive `MM_KERNEL_IMAGE_LAST` / `MM_KERNEL_PHYS_LAST`, and carries a `_Static_assert` pinning the wrap so that reintroducing the exclusive form fails the build with the reason attached.

Two properties make this bug class unusually dangerous here, and together they are why `tools/memmap-check/check.sh` exists:

1. **`_Static_assert` cannot see it.** A static-inline call is not an integer constant expression, so no assert can evaluate the helpers. An assert written as `BASE + (SIZE - 1) == UINT64_MAX` sidesteps the overflow and passes happily while the helper beside it is inert.
2. **clang does not diagnose it.** Measured on clang-19 with the kernel's exact flags (`-Wall -Wextra -Werror`): no diagnostic. Not with `-Wtype-limits`, not with `-Wtautological-type-limit-compare`, not even with `-Weverything`. Host gcc catches it immediately via `-Wtype-limits` ("comparison of unsigned expression in `< 0` is always false").

The kernel's own compiler is blind to this, so the host gate is the only automated net for it. It runs on gcc at zero kernel-image cost -- which is also what lets it run while the BSS ceiling blocks the in-kernel suite -- and it stays after that ceiling lifts.

## Sequencing consequences

- The linker split and the bring-up must ship together: after the split the kernel links high and nothing maps it
  there yet, so it does not boot until bring-up lands.
- The boot_info / framebuffer handoff is pulled forward and co-designed with bring-up (Decision 2), rather than
  following it.
- LA57 support re-bases the HHDM only; the kernel image base is paging-mode-invariant, and `CR4.LA57` cannot be set
  while `CR0.PG=1`, so enablement belongs in the same bootloader window that Decision 2 already uses.

## Related

- [`include/kernel/mm/memmap.h`](../../include/kernel/mm/memmap.h) -- authoritative constants and asserts
- [`docs/infrastructure/bare-metal-gotchas.md`](bare-metal-gotchas.md) -- MMIO/WB, GS_BASE, SMEP/SMAP rules
- CLAUDE.md "Safety Gates" -- User-bit-at-all-levels and guard-page invariants
