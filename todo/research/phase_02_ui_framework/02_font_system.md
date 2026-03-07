# Font System — Research

## Goal

Implement TrueType font rendering in Impossible OS and ship open-source fonts that match the **Segoe UI** look of Windows 11.

---

## Segoe UI — Why We Can't Use It

| Aspect | Details |
|--------|---------|
| **Developer** | Microsoft |
| **License** | ❌ **Proprietary** — bundled with Windows, cannot be redistributed |
| **Style** | Humanist sans-serif, optimized for screen readability |
| **Weights** | Light, Semilight, Regular, Semibold, Bold, Black |
| **Used in** | Windows 7-11 UI, Office, Edge |

---

## Open-Source Segoe UI Alternatives

These fonts are visually similar to Segoe UI and fully redistributable:

| Font | Similarity to Segoe UI | License | Redistributable | Notes |
|------|----------------------|---------|-----------------|-------|
| **Inter** | ⭐⭐⭐⭐⭐ 95% | OFL 1.1 | ✅ Yes | Best match. Designed for screens, same proportions, 9 weights |
| **Selawik** | ⭐⭐⭐⭐⭐ 98% | MIT | ✅ Yes | **Metrically identical to Segoe UI** — Microsoft-sanctioned open-source replacement |
| **Open Sans** | ⭐⭐⭐⭐ 85% | OFL 1.1 | ✅ Yes | Google, very popular, slightly wider |
| **Noto Sans** | ⭐⭐⭐⭐ 80% | OFL 1.1 | ✅ Yes | Google, covers all Unicode scripts |
| **Source Sans 3** | ⭐⭐⭐ 75% | OFL 1.1 | ✅ Yes | Adobe, clean but more geometric |
| **Roboto** | ⭐⭐⭐ 70% | Apache 2.0 | ✅ Yes | Android's default, slightly different feel |

> [!TIP]
> **Selawik is the perfect choice.** It was created by Microsoft specifically as an open-source, metrically-compatible replacement for Segoe UI. Same character widths, same line spacing — a drop-in substitute. MIT licensed.
>
> **Inter as a secondary.** For situations where you want something slightly more modern than Segoe UI, Inter is the gold standard for screen fonts.

### Selawik Details

```
Repository:  https://github.com/Microsoft/Selawik
License:     MIT (fully redistributable, no restrictions)
Created by:  Microsoft (!)
Weights:     Light, Semilight, Regular, Semibold, Bold
File size:   ~200 KB per weight (~1 MB total)
Metrics:     Identical to Segoe UI — same ascent, descent, line gap, character widths
Purpose:     "An open source replacement for Segoe UI"
```

### Monospace Fonts (for terminal/code)

| Font | License | Redistributable | Notes |
|------|---------|-----------------|-------|
| **Cascadia Code** | OFL 1.1 | ✅ Yes | Microsoft's terminal font, ligatures |
| **JetBrains Mono** | OFL 1.1 | ✅ Yes | Excellent readability, ligatures |
| **Fira Code** | OFL 1.1 | ✅ Yes | Mozilla, great ligatures |
| **Source Code Pro** | OFL 1.1 | ✅ Yes | Adobe, clean and simple |
| **Consolas** | ❌ Proprietary | ❌ No | Windows default mono — can't use |

> [!TIP]
> **Cascadia Code** is the ideal monospace font — it's Microsoft's own open-source terminal font (used in Windows Terminal). Pairs perfectly with Selawik for the UI.

---

## Font Rendering Library

### Current state

Impossible OS uses a **custom 8×16 bitmap font** (`font.c`) — fixed-size, no anti-aliasing, no Unicode, no TrueType.

### Options

| Library | License | Size | Features |
|---------|---------|------|----------|
| **stb_truetype** | Public Domain / MIT | ~5000 lines, single header | TrueType/OpenType rasterizer, anti-aliased, scalable |
| **libschrift** | ISC | ~1500 lines | Minimal TrueType, anti-aliased |
| **FreeType** | FreeType License (BSD-like) | ~150K lines | Industry standard, hinting, sub-pixel rendering |

> [!IMPORTANT]
> **stb_truetype is the clear winner.** Single header, public domain, already proven (we use stb_image). Renders any `.ttf` font to anti-aliased bitmaps at any size. No dependencies.

### Integration approach

```c
/* In the kernel — compile stb_truetype with kernel heap */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(sz,u)   kmalloc(sz)
#define STBTT_free(p,u)      kfree(p)
#include "stb_truetype.h"
```

---

## Architecture

### Font manager API

```c
/* font_mgr.h */

/* Font handle */
typedef struct {
    stbtt_fontinfo  info;
    const uint8_t  *data;      /* raw TTF data (from initrd or VFS) */
    float           scale;     /* current size scale factor */
} font_t;

/* System font slots */
typedef enum {
    FONT_UI = 0,        /* Default UI font (Selawik) */
    FONT_UI_BOLD,       /* UI bold weight */
    FONT_MONO,          /* Monospace (Cascadia Code) */
    FONT_MONO_BOLD,     /* Monospace bold */
    FONT_COUNT
} font_slot_t;

/* Initialize font system — load fonts from initrd */
int font_mgr_init(void);

/* Get a system font at a given pixel size */
font_t *font_get(font_slot_t slot, int pixel_size);

/* Draw a string to a surface with anti-aliasing */
void font_draw_string(gfx_surface_t *surface, font_t *font,
                      int x, int y, const char *text,
                      gfx_color_t color);

/* Measure text width/height without drawing */
int font_measure_width(font_t *font, const char *text);
int font_line_height(font_t *font);

/* Draw a single character, returns advance width */
int font_draw_char(gfx_surface_t *surface, font_t *font,
                   int x, int y, uint32_t codepoint,
                   gfx_color_t color);
```

### Font rendering pipeline

```
1. Load .ttf from initrd (zero-copy pointer)
2. stbtt_InitFont() — parse font tables
3. stbtt_ScaleForPixelHeight() — set desired size
4. For each character:
   a. stbtt_GetCodepointBitmap() — rasterize glyph to 8-bit alpha bitmap
   b. Alpha-blend glyph bitmap onto surface using text color
   c. Advance cursor by glyph width + kerning
```

### Glyph caching

Rasterizing every character every frame is expensive. Cache them:

```c
/* Pre-rasterize ASCII 32-126 at common sizes (12, 14, 16, 20, 24px) */
struct glyph_cache {
    uint8_t *bitmap;    /* alpha bitmap */
    int      width;
    int      height;
    int      x_offset;
    int      y_offset;
    int      advance;   /* horizontal advance in pixels */
};

/* Cache: 95 chars × 5 sizes = 475 entries, each ~200 bytes = ~95 KB */
static struct glyph_cache cache[FONT_COUNT][5][95];
```

---

## Recommended Font Bundle

| Slot | Font | Weight | File | Size |
|------|------|--------|------|------|
| `FONT_UI` | Selawik | Regular | `selawik-regular.ttf` | ~190 KB |
| `FONT_UI_BOLD` | Selawik | Semibold | `selawik-semibold.ttf` | ~190 KB |
| `FONT_MONO` | Cascadia Code | Regular | `cascadia-code-regular.ttf` | ~290 KB |
| `FONT_MONO_BOLD` | Cascadia Code | Bold | `cascadia-code-bold.ttf` | ~290 KB |
| **Total** | | | | **~960 KB** |

These get packed into the initrd and loaded at boot. Under 1 MB for a complete Windows 11-matching font set.

---

## Build Pipeline

```makefile
# Copy TTF fonts to initrd
@cp resources/fonts/selawik-regular.ttf $(BUILD_DIR)/initrd_files/
@cp resources/fonts/selawik-semibold.ttf $(BUILD_DIR)/initrd_files/
@cp resources/fonts/cascadia-code-regular.ttf $(BUILD_DIR)/initrd_files/
@cp resources/fonts/cascadia-code-bold.ttf $(BUILD_DIR)/initrd_files/
```

At boot, `font_mgr_init()` loads them from initrd (zero-copy) and copies to `C:\Impossible\Fonts\` on the IXFS.

---

## Rendering Quality Comparison

| Feature | Current (bitmap 8×16) | With stb_truetype |
|---------|----------------------|-------------------|
| Anti-aliasing | ❌ None (jagged edges) | ✅ 256-level alpha |
| Scalable sizes | ❌ Fixed 8×16 only | ✅ Any pixel size |
| Unicode | ❌ ASCII only (0-127) | ✅ Full Unicode |
| Kerning | ❌ Fixed-width | ✅ Proportional + kern pairs |
| Bold/italic | ❌ None | ✅ Via separate font files |
| Sub-pixel (ClearType) | ❌ No | 🟡 Possible with stb_truetype |
| Visual quality | Retro/pixelated | Modern/smooth |

---

## Implementation Order

### Phase 1: stb_truetype integration (2-3 days)
- [ ] Add `stb_truetype.h` to `tools/` or `include/`
- [ ] Download Selawik + Cascadia Code `.ttf` files
- [ ] Add fonts to initrd in Makefile
- [ ] Create `font_mgr.c` — init, load, basic rendering
- [ ] Replace `font_draw_char`/`draw_text` in desktop.c/wm.c

### Phase 2: Glyph caching (1-2 days)
- [ ] Pre-rasterize ASCII 32-126 at boot
- [ ] Cache lookup in `font_draw_char()`
- [ ] Benchmark: cached vs uncached rendering speed

### Phase 3: Copy to IXFS (1 day)
- [ ] Copy TTFs from initrd to `C:\Impossible\Fonts\` at boot
- [ ] Implement `font_load_from_path()` for future user fonts

### Phase 4: Advanced rendering (future)
- [ ] Sub-pixel rendering (ClearType-like)
- [ ] Font fallback chain (Selawik → Noto Sans for Unicode)
- [ ] Dynamic font loading from VFS

---

## Licensing

| Font / Library | License | Can redistribute | Must attribute | Can modify |
|---------------|---------|-----------------|----------------|-----------|
| **Selawik** | MIT | ✅ | ✅ (include license) | ✅ |
| **Cascadia Code** | OFL 1.1 | ✅ | ✅ (include license) | ✅ |
| **Inter** | OFL 1.1 | ✅ | ✅ (include license) | ✅ |
| **stb_truetype** | Public Domain | ✅ | ❌ | ✅ |
| **JetBrains Mono** | OFL 1.1 | ✅ | ✅ (include license) | ✅ |

All are safe to bundle with Impossible OS.
