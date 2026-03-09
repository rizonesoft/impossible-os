# Image System

Runtime image decoding for Impossible OS using stb_image (v2.30, public domain).

## Supported Formats

JPEG, PNG, BMP, GIF, TGA — decoded via `stbi_load_from_memory()`.

## Key Files

| File | Purpose |
|------|---------|
| `include/stb_image.h` | Sean Barrett's single-header image decoder (7989 lines) |
| `include/kernel/image.h` | Image API: `image_t`, load, free |
| `src/kernel/image.c` | Implementation: tiered allocator, RGBA→BGRA, VFS |
| `include/freestanding/*.h` | Header shims for `<stdlib.h>`, `<string.h>`, etc. |

## API

| Function | Description |
|----------|-------------|
| `image_load(img, path)` | Load image from VFS path, decode, convert to BGRA |
| `image_load_mem(img, data, size)` | Decode from memory buffer |
| `image_free(img)` | Free decoded data (handles PMM and kmalloc) |

## Memory Strategy

> [!IMPORTANT]
> The kernel heap is 2 MiB. A 1280×720 RGBA image is 3.6 MiB.

**Tiered allocator:**

| Allocation size | Allocator | Use case |
|----------------|-----------|----------|
| ≤ 64 KB | `kmalloc` | stb_image internal work buffers |
| > 64 KB | `pmm_alloc_contiguous` | Decoded pixel data |

`image_free()` checks `image_t.from_pmm` to call the correct deallocator. A static
tracking table (16 entries) maps PMM pointers to sizes for `STBI_FREE` callbacks.

## Color Format

stb_image decodes to **RGBA** (R at byte offset 0). The framebuffer uses **BGRA**
(`0xAARRGGBB`). `rgba_to_bgra()` swaps R↔B channels post-decode.

## Build

`image.c` compiled with `SIMD_CFLAGS` (`-msse2`) + `-isystem include/freestanding`
for header shims. Freestanding defines: `STBI_NO_STDIO`, `STBI_NO_LINEAR`,
`STBI_NO_HDR`, `STBI_NO_THREAD_LOCALS`, `STBI_NO_SIMD`.
