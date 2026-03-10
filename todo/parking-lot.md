# Parking Lot — Small Actionable Optimizations

> Items that improve performance, memory, or boot speed but aren't blocking
> any phase. Pick these up between major features or during polish passes.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## Font System (`src/kernel/gfx/gfx_text.c`)

### ~~P1. Fix glyph cache page granularity trap~~
- **Status:** ✅ Done (bump allocator, commit `852b27e`+)
- Single 512 KB PMM block, sub-allocated with `glyph_pool_alloc()`

---

### P2. Texture atlas for glyph cache
- **Impact:** Better CPU cache locality, fewer pointer indirections
- **Effort:** Medium (~100 lines)
- **Files:** `gfx_text.c`, `font_mgr.h`
- [ ] Replace per-glyph bitmaps with one atlas image per (slot, size)
- [ ] Use `stbtt_BakeFontBitmap()` to pack all 95 ASCII chars into a single bitmap
- [ ] Store atlas as a single PMM allocation per (slot, size) — 5 slots × 5 sizes = 25 atlases
- [ ] Change `glyph_entry_t` to store atlas X/Y coordinates instead of `bitmap` pointer
- [ ] Update `ttf_draw_string()` to blit from atlas region instead of standalone bitmap
- [ ] Measure before/after draw time with FPS counter

---

### P3. Lazy (just-in-time) glyph rasterization
- **Impact:** Faster boot, less wasted memory for unused font sizes
- **Effort:** Small (~50 lines)
- **Files:** `gfx_text.c`
- [ ] Remove all `cache_rasterize_slot_size()` calls from `ttf_mgr_init()`
- [ ] In the cache lookup path (`ttf_draw_string` → cache miss), rasterize on demand
- [ ] Save the result to the cache/atlas for future frames
- [ ] Only rasterize font sizes actually requested by the UI
- [ ] Keep size 14 (title bar) and 12 (terminal) as "eager" to avoid first-frame jank
- [ ] Log lazy rasterization events: `[JIT] Rasterized slot %d size %d`

---

### P4. LRU cache for non-ASCII fallback glyphs
- **Impact:** Prevents frame drops when displaying `é`, `ñ`, Unicode, CJK
- **Effort:** Medium (~80 lines)
- **Files:** `gfx_text.c`, `font_mgr.h`
- **Depends on:** Phase 10 (Internationalization) for full benefit
- [ ] Define `struct lru_glyph { uint32_t codepoint; int slot; int px; uint8_t *bitmap; ... }`
- [ ] Allocate a fixed LRU ring buffer (128 entries) from PMM
- [ ] On cache miss for non-ASCII: check LRU before calling `stbtt_GetCodepointBitmap`
- [ ] On LRU hit: use cached bitmap directly (no FPU math)
- [ ] On LRU miss: rasterize, store in LRU (evict oldest), draw
- [ ] Save/restore FPU state only on actual rasterization (not LRU hit)

---

### P5. SIMD alpha blending for text rendering
- **Impact:** 4× faster text draw (blend 4 pixels per SSE2 instruction)
- **Effort:** Medium (~60 lines)
- **Files:** `gfx_text.c`, potentially `gfx_simd.h`
- **Prerequisites:** Already compiled with `-msse2`, `gfx_simd.h` included
- [ ] Identify the inner pixel loop in `ttf_draw_string()` / `ttf_draw_char()`
- [ ] Rewrite using SSE2 intrinsics: `_mm_load_si128`, `_mm_mullo_epi16`, `_mm_srli_epi16`
- [ ] Handle the scalar tail (when width % 4 != 0) with regular C
- [ ] Benchmark: measure ms per `ttf_draw_string()` call before/after
- [ ] Wrap in `#ifdef __SSE2__` for portability

---

### P6. Signed Distance Field (SDF) rendering
- **Impact:** One bitmap per glyph works at ALL sizes — 80% cache reduction
- **Effort:** Large (~200 lines)
- **Files:** `gfx_text.c`, `font_mgr.h`, new `gfx_sdf.c`
- **Prerequisites:** P2 (atlas) should be done first for clean integration
- [ ] Use `stbtt_GetGlyphSDF()` instead of `stbtt_GetCodepointBitmap()`
- [ ] Generate SDF at a single reference size (e.g., 32px) per glyph
- [ ] Store SDF atlas (one per slot, not per size)
- [ ] Implement SDF→pixel shader: `alpha = smoothstep(0.5 - spread, 0.5 + spread, sdf_value)`
- [ ] Smoothstep requires float — wrap in FPU save/restore
- [ ] Test at sizes 12, 14, 16, 20, 24 — verify no quality loss
- [ ] Measure memory savings vs bitmap approach

---

## Other Optimizations (Non-Font)

### P7. Audit remaining `kmalloc` usage in GFX/desktop code
- **Impact:** Prevent future heap exhaustion bugs
- **Effort:** Small (~15 min)
- [ ] Run: `grep -rn 'kmalloc' src/kernel/gfx/ src/desktop/ --include='*.c'`
- [ ] For each result: check if allocation could exceed 4 KB
- [ ] Migrate violations to `pmm_alloc_contiguous()`
- [ ] Add guard comments to any remaining legitimate `kmalloc` calls

### P8. Add heap usage monitoring
- **Impact:** Early warning before silent exhaustion
- **Effort:** Small (~30 lines)
- **Files:** `heap.c`, `heap.h`, `main.c`
- [ ] Add `heap_get_free()` / `heap_get_used()` functions
- [ ] Print heap stats at end of boot: `[OK] Heap: %u KB used / %u KB total`
- [ ] Optional: warn if heap > 75% used: `[!!] Heap pressure: %u%% used`
