---
description: Checklist for loading any asset (font, image, sound, cursor) from disk into kernel memory
---

# Add Asset Workflow

Follow this checklist **every time** you load data from disk (fonts, images, icons, sounds, cursors, config files, or any file buffer).

## Pre-Flight Checklist

Before writing any allocation code, answer these questions:

### 1. What is the maximum size of this data?

| Size | Allocator | Rationale |
|------|-----------|-----------|
| ≤ 4 KB | `kmalloc()` OK | Small enough for the 2 MiB kernel heap |
| > 4 KB | **`pmm_alloc_contiguous()`** | Heap exhaustion risk |
| Unknown | **`pmm_alloc_contiguous()`** | Assume worst case |

### 2. Is it one of these types? → **Always use PMM**

- Font file data (`.ttf`, `.otf`) — can be 50 KB to 3 MB
- Image/pixel buffers — framebuffer-sized or larger
- File read buffers — size depends on file
- Icon data — decoded images
- Sound data — PCM samples
- Wallpaper data — full-screen resolution
- Cursor sprite data — multiple sizes/frames
- Any buffer that persists for the kernel's lifetime

### 3. Is it temporary? (allocated → used → freed within one function)

- If ≤ 4 KB → `kmalloc()` OK
- If > 4 KB → Still use PMM, or use a scratch buffer

## PMM Allocation Pattern

```c
#include "kernel/mm/pmm.h"

/* Calculate pages needed (4096 bytes per page) */
uint32_t pages = (size + 4095) / 4096;

/* Allocate contiguous physical pages (identity-mapped) */
uint64_t phys = pmm_alloc_contiguous(pages);
if (!phys) {
    printk("[!!] Failed to allocate %d pages for asset\n", (uint64_t)pages);
    return -1;
}

/* Use directly as a pointer (identity-mapped in Impossible OS) */
uint8_t *buf = (uint8_t *)(uintptr_t)phys;

/* When done (if ever): */
pmm_free_contiguous(phys, pages);
```

## Common Mistakes

| Mistake | Consequence |
|---------|-------------|
| `kmalloc(font_file_size)` | Silent heap exhaustion, breaks hover/VFS/scheduling |
| `kmalloc(width * height * 4)` | Back buffer overflow, zero double buffering |
| Not checking `pmm_alloc_contiguous()` return | Null pointer write to physical address 0 |
| Forgetting to free PMM pages | Physical memory leak (acceptable for boot-time assets) |

## After Loading

- [ ] Verify the boot log shows `[OK]` for your asset
- [ ] Run `bash scripts/build.sh run` and confirm no regressions
- [ ] Check heap usage hasn't increased unexpectedly
