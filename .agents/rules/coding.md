---
globs: ["**/*.c", "**/*.h", "**/*.asm"]
---

# Impossible OS — Coding Rules

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

## Hardware Constraints

- **APIC-only interrupts.** Route all hardware interrupts via LAPIC/IOAPIC. Do NOT write new 8259 PIC routing code. The PIC is masked at boot. *(Legacy PIC masking code in `pic.c` is kept for boot-time disable only.)*
- **DMA-only storage.** Use AHCI (DMA + NCQ) or VirtIO for disk I/O. Do NOT use legacy IDE/ATA PIO polling (port 0x1F0–0x1F7).
- **UEFI GOP framebuffer.** The framebuffer is a linear 32bpp buffer from UEFI GOP. Do NOT write VGA text mode (0xB8000) code.
- **RCU for read-heavy structures.** Prefer Read-Copy-Update over spinlocks for VFS mount list, process tree, and Registry cache.

