# Runtime Image Decoding & JPG Wallpaper — Research

## Overview

Enable Impossible OS to decode JPEG, PNG, BMP, and other image formats **at runtime** (not just at build time). This allows users to set any `.jpg` as their desktop wallpaper, view photos, load custom icons, etc.

---

## Why Not Skia?

| Aspect | Skia | Our approach |
|--------|------|-------------|
| **Size** | ~2,000,000 lines C++ | ~7,500 lines C (stb_image) |
| **Language** | C++ (needs runtime, exceptions, STL) | C (works in freestanding kernel) |
| **Dependencies** | zlib, freetype, harfbuzz, ICU, libjpeg-turbo, libpng, libwebp | None (all self-contained) |
| **GPU** | Needs OpenGL/Vulkan/Metal backend | Pure software |
| **License** | BSD-3 ✅ | Public Domain ✅ |
| **Feasible in our kernel?** | ❌ No (years of porting work) | ✅ Yes (one `#include`) |

Skia is what Chrome and Android use — it's brilliant but massive. For Impossible OS, we get the same **image decoding** from `stb_image` and the same **2D rendering** from our `gfx_*` library. Combined, they cover everything Skia does for our needs.

---

## Current State

| Feature | Build time | Runtime |
|---------|-----------|---------|
| JPEG → Raw pixels | ✅ `jpg2raw` tool | ❌ Cannot decode JPGs |
| PNG → Raw pixels | ✅ `jpg2raw` tool | ❌ Cannot decode PNGs |
| BMP → Raw pixels | ❌ | ❌ |
| Raw BGRA → framebuffer | — | ✅ Works (wallpaper, icons) |

**Problem**: Wallpaper is converted to `.raw` at build time and baked into the initrd. Users can't change wallpaper to a custom `.jpg` file.

---

## Solution: Kernel-side stb_image

### What stb_image supports

| Format | Decode | Animated | Patent issues |
|--------|--------|----------|--------------|
| JPEG | ✅ | — | ❌ None |
| PNG | ✅ | — | ❌ None |
| BMP | ✅ | — | ❌ None |
| GIF | ✅ | ✅ | ❌ None |
| TGA | ✅ | — | ❌ None |
| PSD | ✅ (basic) | — | ❌ None |
| HDR | ✅ | — | ❌ None |

### Integration

```c
/* kernel_image.c — runtime image decoder */

/* Redirect stb_image memory to kernel heap */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO           /* no fopen/fread — we use VFS */
#define STBI_NO_LINEAR          /* skip linear float conversion */
#define STBI_NO_HDR             /* skip HDR support (saves code size) */
#define STBI_MALLOC(sz)         kmalloc(sz)
#define STBI_REALLOC(p,newsz)   krealloc(p, newsz)
#define STBI_FREE(p)            kfree(p)
#include "stb_image.h"
```

### Image loading API

```c
/* image.h */

typedef struct {
    uint32_t *pixels;       /* Decoded BGRA pixel data */
    uint32_t  width;
    uint32_t  height;
    uint32_t  channels;     /* 3 = RGB, 4 = RGBA */
} image_t;

/* Load image from a VFS path (auto-detects format from header bytes) */
image_t *image_load(const char *path);

/* Load image from memory buffer */
image_t *image_load_mem(const uint8_t *data, uint32_t size);

/* Free decoded image */
void image_free(image_t *img);

/* Scale image to fit a target size (for wallpaper fitting) */
image_t *image_scale(const image_t *src, uint32_t target_w, uint32_t target_h,
                     uint8_t mode);

#define IMAGE_FIT_FILL    0   /* Scale to fill, crop excess */
#define IMAGE_FIT_FIT     1   /* Scale to fit, letterbox */
#define IMAGE_FIT_STRETCH 2   /* Stretch to exact size */
#define IMAGE_FIT_CENTER  3   /* Center without scaling */
#define IMAGE_FIT_TILE    4   /* Tile/repeat pattern */
```

### Implementation

```c
/* image.c */

image_t *image_load(const char *path) {
    /* 1. Open file from VFS */
    struct vfs_node *file = vfs_open(path, VFS_O_READ);
    if (!file) return NULL;

    /* 2. Read entire file into buffer */
    uint8_t *buf = kmalloc(file->size);
    vfs_read(file, buf, file->size);
    vfs_close(file);

    /* 3. Decode with stb_image */
    image_t *img = kmalloc(sizeof(image_t));
    int w, h, channels;
    uint8_t *decoded = stbi_load_from_memory(buf, file->size,
                                              &w, &h, &channels, 4);
    kfree(buf);

    if (!decoded) {
        kfree(img);
        return NULL;
    }

    img->pixels = (uint32_t *)decoded;
    img->width = w;
    img->height = h;
    img->channels = 4;  /* always request RGBA */

    /* 4. Convert RGB → BGRA (our framebuffer format) */
    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t px = img->pixels[i];
        uint8_t r = (px >> 0) & 0xFF;
        uint8_t g = (px >> 8) & 0xFF;
        uint8_t b = (px >> 16) & 0xFF;
        uint8_t a = (px >> 24) & 0xFF;
        img->pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }

    return img;
}
```

---

## JPG Desktop Wallpaper

### How it works

```
1. User picks wallpaper via Settings Panel → Wallpaper
   or right-click image → "Set as Desktop Background"
2. Path saved to Codex: System\Theme\Wallpaper = "C:\Users\Default\Pictures\beach.jpg"
3. Desktop loads and decodes at runtime:
   image_t *bg = image_load("C:\\Users\\Default\\Pictures\\beach.jpg");
4. Scale to fit screen:
   image_t *scaled = image_scale(bg, fb_get_width(), fb_get_height(), IMAGE_FIT_FILL);
5. Blit to desktop surface
6. Free original, keep scaled version cached
```

### Wallpaper fit modes (like Windows)

| Mode | Behavior | Best for |
|------|----------|----------|
| **Fill** | Scale to cover screen, crop excess | Photos larger than screen |
| **Fit** | Scale to fit within screen, black bars | Keep entire image visible |
| **Stretch** | Distort to exact screen size | Not recommended |
| **Center** | No scaling, centered, solid color border | Pixel art, small images |
| **Tile** | Repeat pattern | Textures, patterns |

### Codex settings

```ini
[Theme]
Wallpaper = "C:\Impossible\Wallpapers\default.jpg"
WallpaperMode = "fill"       ; fill, fit, stretch, center, tile
WallpaperColor = "#1E1E2E"   ; background color for fit/center
```

---

## Image Scaling Algorithm

### Bilinear interpolation (for smooth scaling)

```c
/* Scale image using bilinear filtering — smooth result */
image_t *image_scale(const image_t *src, uint32_t tw, uint32_t th, uint8_t mode) {
    image_t *dst = image_create(tw, th);
    float x_ratio = (float)src->width / tw;
    float y_ratio = (float)src->height / th;

    for (uint32_t y = 0; y < th; y++) {
        for (uint32_t x = 0; x < tw; x++) {
            float sx = x * x_ratio;
            float sy = y * y_ratio;
            /* Sample 4 nearest source pixels, blend by sub-pixel position */
            dst->pixels[y * tw + x] = bilinear_sample(src, sx, sy);
        }
    }
    return dst;
}
```

For **downscaling** (4K photo → 1080p screen), use **box filter** averaging for better quality than bilinear.

---

## Additional Image Capabilities

### Image saving (for Paint, screenshots)

```c
/* Using stb_image_write (another single-header library) */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_MALLOC  kmalloc
#define STBIW_REALLOC krealloc
#define STBIW_FREE    kfree
#include "stb_image_write.h"

/* Save screenshot or Paint canvas */
int image_save_bmp(const image_t *img, const char *path);
int image_save_png(const image_t *img, const char *path);
```

### WebP support (future)

stb_image doesn't decode WebP. If needed, add Google's libwebp (BSD-3, ~30K lines).

### SVG support (future)

For scalable icons, add nanosvg (Zlib, single header, ~2500 lines).

---

## Memory Requirements

| Image | Size on disk | Decoded in RAM |
|-------|-------------|---------------|
| 1280×720 JPEG wallpaper | ~200 KB | 3.5 MB (BGRA) |
| 1920×1080 JPEG wallpaper | ~500 KB | 7.9 MB (BGRA) |
| 3840×2160 JPEG wallpaper | ~2 MB | 31.6 MB (BGRA) |
| 32×32 PNG icon | ~2 KB | 4 KB (BGRA) |

> [!WARNING]
> A 4K wallpaper uses ~32 MB of RAM when decoded. Our kernel heap needs to be large enough to handle this. At 1080p (our likely default for now), it's ~8 MB — manageable.

---

## Files

| Action | File | Lines (est.) | Purpose |
|--------|------|-------------|---------|
| **[NEW]** | `include/image.h` | ~40 | Image types, load/save/scale API |
| **[NEW]** | `src/kernel/image.c` | ~250 | stb_image integration, BGRA conversion, scaling |
| **[COPY]** | `include/stb_image.h` | — | Copy from `tools/` to kernel includes |
| **[NEW]** | `include/stb_image_write.h` | — | Download for image saving |
| **[MODIFY]** | `src/desktop/desktop.c` | ~30 | Load wallpaper from VFS path instead of initrd raw |
| **[MODIFY]** | `Makefile` | ~5 | Compile `image.c` into kernel |

---

## Implementation Order

### Phase 1: Kernel image decoder (2-3 days)
- [ ] Copy `stb_image.h` to kernel includes
- [ ] Create `image.c` with `kmalloc`/`kfree` redirects
- [ ] Implement `image_load()` from VFS path
- [ ] Implement `image_load_mem()` from buffer
- [ ] RGBA → BGRA conversion
- [ ] Test: decode a JPEG from initrd at boot

### Phase 2: JPG wallpaper (1-2 days)
- [ ] Modify `desktop.c` to load wallpaper via `image_load()`
- [ ] Support JPEG and PNG wallpapers directly (no build-time conversion)
- [ ] Implement `image_scale()` with bilinear interpolation
- [ ] Wallpaper fit modes (fill, fit, center)
- [ ] Read wallpaper path from Codex

### Phase 3: Image saving (1-2 days)
- [ ] Add `stb_image_write.h`
- [ ] `image_save_bmp()` and `image_save_png()`
- [ ] Used by Paint (Save As) and screenshots

### Phase 4: Image viewer app (future)
- [ ] Simple app to open and display images
- [ ] Zoom, pan, next/previous in folder
- [ ] Slideshow mode
