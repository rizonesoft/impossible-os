---
description: Memory allocation rules — when to use kmalloc vs pmm_alloc_contiguous
---

# Memory Allocation Skill

## The Cardinal Rule

> **PMM is the default allocator. `kmalloc` is the exception.**

The kernel heap is only **2 MiB**. Exhausting it causes **silent failures** — no crash, no error message, just broken features (hover detection, VFS, scheduling).

## Decision Tree

```
Is the allocation > 4 KB?
├── YES → pmm_alloc_contiguous()
└── NO
    ├── Is it font data, image data, file buffer, pixel buffer?
    │   ├── YES → pmm_alloc_contiguous()
    │   └── NO
    │       ├── Is it a small kernel struct (VFS node, task, string)?
    │       │   ├── YES → kmalloc() ✅
    │       │   └── NO → Think harder. Probably PMM.
    │       └── Is it temporary (allocated and freed in same function)?
    │           ├── YES, and ≤ 4 KB → kmalloc() ✅
    │           └── Otherwise → pmm_alloc_contiguous()
```

## `kmalloc()` — ONLY for these

| Use case | Typical size |
|----------|-------------|
| VFS nodes | ~100 bytes |
| Task/thread structs | ~200 bytes |
| Codex (registry) values | ~50 bytes |
| Short strings (paths, names) | ~128 bytes |
| Linked-list nodes | ~16-32 bytes |
| Small temp buffers (freed immediately) | ≤ 4 KB |

## `pmm_alloc_contiguous()` — for EVERYTHING ELSE

| Use case | Typical size |
|----------|-------------|
| Font file data (.ttf) | 50 KB – 3 MB |
| Glyph cache bitmaps | 200+ KB total |
| Framebuffer back buffer | 3.6 MB (1280×720×32bpp) |
| Image/icon buffers | 10 KB – 1 MB |
| Wallpaper data | 3.6 MB |
| File read buffers | Variable |
| Sound/PCM data | Variable |
| Any allocation > 4 KB | Always |

## Code Pattern

```c
/* PMM allocation (identity-mapped, no size limit) */
uint32_t pages = (size + 4095) / 4096;
uint64_t phys = pmm_alloc_contiguous(pages);
uint8_t *buf = (uint8_t *)(uintptr_t)phys;

/* kmalloc (2 MiB heap, small allocations only) */
void *ptr = kmalloc(small_size);  /* MUST be ≤ 4 KB */
```

## Known Bugs from Violating This Rule

| Commit | Bug | Root Cause |
|--------|-----|-----------|
| `9722a74` | Framebuffer flicker, no double buffering | Back buffer via kmalloc (3.6 MB > 2 MiB heap) |
| `f673e46` | JPEG decode crash | Image buffer via kmalloc |
| `5ea919b` | Hover detection broken, silent failures | Fluent font (2.6 MB) via kmalloc exhausted heap |

## Files That Need PMM Migration (Tech Debt)

- `src/kernel/gfx/stb_truetype_impl.c` — `STBTT_malloc` macro maps to `kmalloc` (stb internal allocs, typically small but unbounded)
- `src/kernel/gfx/gfx_core.c` — `gfx_create_surface()` uses `kmalloc(w * h * 4)` — **will crash for surfaces > 512×512**
- `src/kernel/gfx/gfx_blur.c` — scratch buffer via `kmalloc(max_dim * 4)` — safe for current 1280px max but fragile

> **Resolved:** `gfx_text.c` `load_ttf_file()` now correctly uses `pmm_alloc_contiguous()` for font data.

## How to Audit

```bash
# Find potential violations: kmalloc calls in non-core files
grep -rn 'kmalloc' src/kernel/gfx/ src/desktop/ --include='*.c'
```

Check each result: is the allocated size potentially > 4 KB? If yes, it must use PMM.
