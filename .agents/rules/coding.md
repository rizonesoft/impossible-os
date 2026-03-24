---
globs: ["**/*.c", "**/*.h", "**/*.asm"]
---

# Impossible OS — Coding Rules

## Engineering Quality

> **Impossible OS is production-level. No workarounds. No hacks. No temporary fixes.**

- **Fix root causes, not symptoms.** If a bug requires restructuring, restructure. Quick patches create technical debt.
- **Optimize, don't settle.** Every subsystem must outperform Windows 11 and Linux. Benchmark-aware, cache-friendly, DMA-aligned.
- **Long-term thinking only.** Every line of code must be written as if it ships tomorrow. No "fix later" comments.

## Freestanding C

- **No standard library.** Never include `<stdio.h>`, `<stdlib.h>`, `<string.h>`, or any user-space headers. The build uses `-nostdinc` so even freestanding headers like `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` are **NOT available**. Use `#include "kernel/types.h"` for all integer types, `size_t`, and `NULL`.
- **No malloc().** Use `kmalloc()` (≤ 4 KB) or `pmm_alloc_contiguous()` (everything else).
- **No printf().** Use `printk()` for kernel output, `klog()` for logging.
- **PMM returns `uintptr_t`, not `void *`.** Always cast: `(void *)(uintptr_t)pmm_alloc_contiguous(n)`.

## Memory Allocation — Decision Tree

```
Is the allocation > 4 KB?
├── YES → pmm_alloc_contiguous()
└── NO
    ├── Is it font data, image data, file buffer, pixel buffer?
    │   └── YES → pmm_alloc_contiguous()
    ├── Is it a small kernel struct (VFS node, task, string)?
    │   └── YES → kmalloc() ✅
    └── Otherwise → pmm_alloc_contiguous()
```

**`kmalloc()` — ONLY for these:**

| Use case | Typical size |
|----------|-------------|
| VFS nodes | ~100 bytes |
| Task/thread structs | ~200 bytes |
| Codex (registry) values | ~50 bytes |
| Short strings (paths, names) | ~128 bytes |
| Linked-list nodes | ~16–32 bytes |

**`pmm_alloc_contiguous()` — for EVERYTHING ELSE:**

| Use case | Typical size |
|----------|-------------|
| Font file data (.ttf) | 50 KB – 3 MB |
| Framebuffer back buffer | 3.6 MB (1280×720×32bpp) |
| Image/icon buffers | 10 KB – 1 MB |
| File read buffers | Variable |
| Any allocation > 4 KB | Always |

**Code pattern:**

```c
/* PMM allocation (identity-mapped, no size limit) */
uint32_t pages = (size + 4095) / 4096;
uint64_t phys = pmm_alloc_contiguous(pages);
uint8_t *buf = (uint8_t *)(uintptr_t)phys;

/* kmalloc (2 MiB heap, small allocations only) */
void *ptr = kmalloc(small_size);  /* MUST be ≤ 4 KB */
```

**Known bugs from violating this rule:**

| Commit | Bug | Root Cause |
|--------|-----|-----------|
| `9722a74` | Framebuffer flicker, no double buffering | Back buffer via kmalloc (3.6 MB > 2 MiB heap) |
| `f673e46` | JPEG decode crash | Image buffer via kmalloc |
| `5ea919b` | Hover detection broken, silent failures | Fluent font (2.6 MB) via kmalloc exhausted heap |

## Known Gotchas

- **Never `#include <stdint.h>` or any angle-bracket header.** The build uses `-nostdinc` which strips the compiler's include search path entirely. Use `#include "kernel/types.h"` instead — it defines `uint8_t` through `uint64_t`, `int8_t` through `int64_t`, `size_t`, `ssize_t`, `uintptr_t`, and `NULL`. Only project-local headers via double-quotes are safe. *(Learned from VMBus build failure `daafc39`)*

- **SSE2 modules have special build rules.** Files using floating-point math (stb_truetype, stb_image, gfx_simd) are compiled with `-msse2` override. See the Makefile pattern rules.

- **CalVer versioning.** Version is auto-generated from build date: `YY.M.D` (e.g., `26.3.21`). Build number auto-increments from `.build_number`.

- **NTFS IndexLength includes `entries_offset`.** The `IndexLength` field at `NODE_HEADER + 0x04` measures from the **start of the node header** (includes `entries_offset`, typically `0x28 = 40` bytes). All entry-level operations (search, insert, split) work with offsets from `entries_base`. When **reading** IndexLength from disk, subtract `entries_offset` to get entries-only size. When **writing** back, add `entries_offset`. Mixing conventions causes `uint32_t` underflow → PAGE_FAULT. *(Learned from B-tree crash `671468c6`)*

- **Compiler-optimized backward byte loops cause 33-bit addresses.** Manual `for (i = len; i > 0; i--) dst[i-1] = src[i-1]` loops are miscompiled by `-O2` into unrolled sequences that produce addresses exceeding 32 bits. Use the `noinline` helper `ntfs_memmove()` in `ntfs_internal.h` for overlapping backward copies. *(Learned from B-tree PAGE_FAULT at `base + 0x100000000`)*

## Debugging PAGE_FAULTs

When the kernel crashes with `Stop code: PAGE_FAULT`:

1. **Get crash info** from serial log: `RIP` (instruction pointer), `CR2` (faulting address), register dump
2. **Map RIP to source:**
   ```bash
   llvm-addr2line-19 -e build/kernel.exe -f <RIP_hex>
   ```
3. **Disassemble crash site:**
   ```bash
   llvm-objdump-19 -d --start-address=0x<RIP-0x30> --stop-address=0x<RIP+0x40> build/kernel.exe
   ```
4. **Trace register corruption** backwards through the disassembly to find the source
5. **Recognize common patterns:**
   - `CR2 = valid_base + 0x100000000` → size/offset corruption, not base pointer — focus on arithmetic
   - Register value `0xFFFFFF__` → `uint32_t` underflow (e.g., `0xFFFFFF70 = 0 - 0x90`)
   - Crash in inlined function → RIP maps to header file; check generated assembly for `-O2` artifacts
6. **Verify symbols exist:** `llvm-nm-19 build/kernel.exe | head -5` (must show symbols, not "no symbols")

> [!IMPORTANT]
> Build must **NOT strip symbols** for debugging to work. Ensure `-g` is in CFLAGS and no `--strip-debug` or `llvm-strip` step runs on `build/kernel.exe`.

## Hardware Constraints

- **APIC-only interrupts.** Route all hardware interrupts via LAPIC/IOAPIC. Do NOT write new 8259 PIC routing code. The PIC is masked at boot. *(Legacy PIC masking code in `pic.c` is kept for boot-time disable only.)*
- **DMA-only storage.** Use AHCI (DMA + NCQ) or VirtIO for disk I/O. Do NOT use legacy IDE/ATA PIO polling (port 0x1F0–0x1F7).
- **UEFI GOP framebuffer.** The framebuffer is a linear 32bpp buffer from UEFI GOP. Do NOT write VGA text mode (0xB8000) code.
- **RCU for read-heavy structures.** Prefer Read-Copy-Update over spinlocks for VFS mount list, process tree, and Registry cache.

## BSS Growth — Address Space Collision

> [!CAUTION]
> Adding or expanding large `static` arrays (e.g., `ports[32]`, pool arrays) in kernel code grows the **BSS section**. Kernel BSS **must stay below** the user-mode base address (`0x800000`, set in `user/user.ld`). If BSS crosses this boundary, user-mode ELF loading will silently overwrite kernel data at runtime.

The build script (`scripts/build.sh`) automatically checks this after linking the kernel. If the check fails, either:
1. Increase the user base address in `user/user.ld`
2. Reduce kernel static allocations (move large arrays to PMM)

| Commit | Bug | Root Cause |
|--------|-----|------------|
| `f38670f` | GPF in `RegSetValueEx` after desktop loaded | NCQ struct expansion pushed BSS past `0x400000`; shell.exe overwrote `reg_value_pool` |
| `671468c6` | PAGE_FAULT in B-tree insert (CR2 = base + 4 GiB) | NTFS IndexLength mixed entries-only vs spec-format sizes → `uint32_t` underflow in memmove |
