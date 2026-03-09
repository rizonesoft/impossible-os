# Image System

Runtime image decoding and scaling for Impossible OS using stb_image (v2.30, public domain).

## Supported Formats

JPEG, PNG, BMP, GIF, TGA — decoded via `stbi_load_from_memory()`.

## Key Files

| File | Purpose |
|------|---------|
| `include/stb_image.h` | Sean Barrett's single-header image decoder (7989 lines) |
| `include/kernel/image.h` | Image API: `image_t`, load, scale, free |
| `src/kernel/image.c` | Decoding: tiered allocator, RGBA→BGRA, VFS |
| `src/kernel/image_scale.c` | Scaling: bilinear, box filter, fit modes |
| `include/freestanding/*.h` | Header shims for `<stdlib.h>`, `<string.h>`, etc. |

## API

| Function | Description |
|----------|-------------|
| `image_load(img, path)` | Load image from VFS path, decode, convert to BGRA |
| `image_load_mem(img, data, size)` | Decode from memory buffer |
| `image_scale(dst, src, w, h, mode)` | Scale image with fit mode |
| `image_free(img)` | Free decoded data (handles PMM and kmalloc) |

## Scaling

### Algorithms

| Condition | Algorithm | Quality |
|-----------|-----------|---------|
| Upscale or downscale ≤2x | **Bilinear interpolation** (16.16 fixed-point) | Smooth |
| Downscale >2x | **Box filter** (area average) | Sharper |

### Fit Modes (`image_fit_t`)

| Mode | Behavior |
|------|----------|
| `IMAGE_FIT_STRETCH` | Distort to exact target size |
| `IMAGE_FIT_FILL` | Scale to cover, center-crop excess |
| `IMAGE_FIT_FIT` | Scale to fit inside, letterbox with black |
| `IMAGE_FIT_CENTER` | No scaling, center on canvas |
| `IMAGE_FIT_TILE` | Repeat pattern to fill |

## Memory Strategy

> [!IMPORTANT]
> The kernel heap is 2 MiB. A 1280×720 RGBA image is 3.6 MiB.

**Tiered allocator:**

| Allocation size | Allocator | Use case |
|----------------|-----------|----------|
| ≤ 64 KB | `kmalloc` | stb_image work buffers, small images |
| > 64 KB | `pmm_alloc_contiguous` | Decoded pixel data, scaled output |

`image_free()` checks `image_t.from_pmm` to call the correct deallocator.

## Color Format

stb_image decodes to **RGBA** (R at byte offset 0). The framebuffer uses **BGRA**
(`0xAARRGGBB`). `rgba_to_bgra()` swaps R↔B channels post-decode.

## Wallpaper Loading

`desktop.c` reads the wallpaper path from Codex `System\Theme\Wallpaper`
(default: `C:\Impossible\Wallpapers\default.jpg`) and fit mode from `WallpaperMode`.

**Pipeline:** `image_load()` → (JPEG decode + RGBA→BGRA) → `image_scale(fit_mode)` → cached `image_t`

The JPEG stays compressed on disk (~200 KB). No build-time `jpg2raw` conversion needed.
The scaled result is cached in a static `image_t` — only re-decoded on change.

## Image Saving

`image_save_bmp(img, path)` and `image_save_png(img, path)` write images to VFS.
Uses `stb_image_write.h` with `STBI_WRITE_NO_STDIO` and a VFS write callback.

**Pipeline:** BGRA→RGBA swap → `stbi_write_*_to_func()` → accumulate in buffer → `vfs_write()`

PNG is lossless and compressed. BMP is uncompressed (larger) but simpler as fallback.
Used by: screenshot feature (§9.4), Paint app's Save As.

## Build

`image.c` compiled with `SIMD_CFLAGS` (`-msse2`) + `-isystem include/freestanding`
for header shims. `image_scale.c` uses standard CFLAGS (integer-only math).

