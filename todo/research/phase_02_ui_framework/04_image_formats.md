# Image Format Libraries — Research

## Overview

Impossible OS already uses [stb_image](https://github.com/nothings/stb) for JPEG/PNG decoding in the `jpg2raw` build tool. This document covers image format support **inside the OS** at runtime, for loading wallpapers, icons, photos, and future image viewing.

---

## Image Formats & Libraries

### JPEG

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **stb_image** | MIT / Public Domain | ✅ Yes | Single header, ~7500 lines | Already in our toolchain (`tools/stb_image.h`) |
| **libjpeg-turbo** | BSD-3 + IJG | ✅ Yes | ~50K lines, SIMD-accelerated | Industry standard, 2-6× faster than stb |
| **nanojpeg** | MIT | ✅ Yes | ~500 lines | Minimal decoder, no encoder |
| **TJpgDec** | BSD | ✅ Yes | ~800 lines | Designed for embedded/MCU |

> [!TIP]
> **stb_image is the best fit.** We already have it, it's public domain, single-header, and supports JPEG + PNG + BMP + GIF. For kernel-side decoding, just `#include "stb_image.h"` in a kernel module.

### PNG

| Library | License | Redistributable | Size | Notes |
|---------|---------|-----------------|------|-------|
| **stb_image** | MIT / Public Domain | ✅ Yes | Same as above | Already supports PNG |
| **lodepng** | Zlib | ✅ Yes | Single file, ~5000 lines | Encode + decode, no zlib dependency |
| **libpng** | Libpng license | ✅ Yes | ~70K lines, needs zlib | The reference implementation |
| **miniz** | MIT | ✅ Yes | Single file, deflate/inflate | Needed if using libpng or manual inflate |

### BMP

| Library | License | Redistributable | Notes |
|---------|---------|-----------------|-------|
| **stb_image** | Public Domain | ✅ Yes | Already supports BMP |
| **Manual parser** | N/A | N/A | BMP is trivial (~50 lines of custom code) |

### GIF

| Library | License | Redistributable | Notes |
|---------|---------|-----------------|-------|
| **stb_image** | Public Domain | ✅ Yes | Supports GIF (including animated) |
| **gifdec** | Public Domain | ✅ Yes | Tiny GIF decoder (~400 lines) |

### WebP

| Library | License | Redistributable | Notes |
|---------|---------|-----------------|-------|
| **libwebp** | BSD-3 | ✅ Yes | Google's WebP library, ~30K lines |
| No small alternatives | — | — | WebP is complex (VP8 based) |

### SVG (Vector)

| Library | License | Redistributable | Notes |
|---------|---------|-----------------|-------|
| **nanosvg** | Zlib | ✅ Yes | Single header, ~2500 lines, parse + rasterize |
| **plutosvg** | MIT | ✅ Yes | Tiny SVG renderer |

---

## What We Should Use

### Build-time (current — working)

| Format | Tool | Status |
|--------|------|--------|
| JPEG → Raw BGRA | `jpg2raw` (stb_image) | ✅ Working |
| PNG → Raw BGRA | `jpg2raw` (stb_image) | ✅ Working (just added) |

### Runtime (in-kernel or userland)

| Priority | Format | Library | Why |
|----------|--------|---------|-----|
| **P0** | Raw BGRA | Direct framebuffer | ✅ Already works (wallpaper, icons) |
| **P1** | BMP | Manual or stb_image | Simplest common format, Windows-native |
| **P2** | PNG | stb_image or lodepng | Icons, UI assets, lossless photos |
| **P3** | JPEG | stb_image | Photos, wallpapers |
| **P4** | SVG | nanosvg | Scalable icons for different DPI |
| **P5** | GIF | stb_image or gifdec | Animated content |

### Integration approach

```c
/* Compile stb_image into the kernel for runtime image decoding */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          /* We don't have stdio in the kernel */
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC  kmalloc   /* Use kernel heap */
#define STBI_REALLOC krealloc
#define STBI_FREE    kfree
#include "stb_image.h"
```

> [!WARNING]
> stb_image uses `malloc`/`realloc`/`free` internally. In the kernel, we'd need to redirect these to `kmalloc`/`krealloc`/`kfree`. Our current heap may need expansion for large image decodes.

---

## Licensing Summary

| Library | License | Commercial use | Redistribution | Modification | Attribution needed |
|---------|---------|---------------|----------------|--------------|-------------------|
| stb_image | Public Domain / MIT | ✅ | ✅ | ✅ | No (PD) / Yes (MIT) |
| lodepng | Zlib | ✅ | ✅ | ✅ | No |
| nanojpeg | MIT | ✅ | ✅ | ✅ | Yes |
| nanosvg | Zlib | ✅ | ✅ | ✅ | No |
| gifdec | Public Domain | ✅ | ✅ | ✅ | No |
| libjpeg-turbo | BSD-3 / IJG | ✅ | ✅ | ✅ | Yes |
| libpng | Libpng | ✅ | ✅ | ✅ | Yes |
| libwebp | BSD-3 | ✅ | ✅ | ✅ | Yes |
