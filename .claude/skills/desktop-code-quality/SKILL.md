---
name: desktop-code-quality
description: Pre-flight quality checklist for desktop compositor and shell code (src/desktop/). Placeholder -- extend with compositor-specific gates when desktop development is active.
---

# Desktop Code Quality

> This skill auto-loads when writing desktop compositor code. Extend with domain-specific gates when desktop development is active.

## When This Applies

Every time you create or modify a `.c`, `.h`, or `.asm` file under `src/desktop/`.

## Pre-Write Checklist

### Gate 1: Freestanding Rules
- [ ] Same as `kernel-code-quality` Gate 1 -- no stdlib, `kernel/types.h`, ASCII only.

### Gate 2: Framebuffer Safety
- [ ] **WC mapping.** Framebuffer must be mapped via `vmm_map_mmio_wc()` (write-combining), never WB.
- [ ] **Pitch != width.** Always use `pitch` (bytes per scanline) not `width * bpp`. Pitch may be larger due to alignment.
- [ ] **Back-buffer pattern.** Compose to a back-buffer in WB memory, then blit to WC framebuffer. Never read from WC memory (hundreds of cycles per read).
- [ ] **Pixel format.** Check `boot_info.fb.pixel_format` (RGBX vs BGRX) -- don't assume one format.

### Gate 3: Compositor Event Loop
- [ ] **No blocking in the compositor loop.** The BSP event loop (`compositor_run()`) starves kernel threads. All work must be non-blocking or deferred.
- [ ] **Input polling.** VirtIO input and VBox mouse are polled from the compositor loop. If the loop blocks, input dies.

### Gate 4: SMP + Error Handling
- [ ] Same as `kernel-code-quality` Gates 2 + 9 -- SMP safety and complete error handling.

> **Extend this skill** with gates for damage tracking, z-order management, window clipping, GPU acceleration, and DPI scaling when those features are implemented.
