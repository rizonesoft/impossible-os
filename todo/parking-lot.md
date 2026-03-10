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

### ~~P3. Lazy (just-in-time) glyph rasterization~~
- **Status:** ✅ Done
- Only sizes 12 (terminal) and 14 (title bar) eagerly rasterized at boot
- Sizes 16, 20, 24 JIT-rasterized on first UI access via `cache_ensure_ready()`
- `[JIT] Rasterized slot %d size %d` logged on lazy rasterization

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

### ~~P5. Optimized text rendering (inline alpha blend)~~
- **Status:** ✅ Done (optimized scalar — SSE2 intrinsics blocked by freestanding cross-compiler)
- Pre-clipped column ranges per row — no per-pixel bounds checks
- Direct pixel buffer writes — no `gfx_put_pixel`/`gfx_blend_pixel` call overhead
- Inline alpha blend: `dst = (color*alpha + dst*inv) >> 8`
- Fast paths: skip transparent pixels, direct-write opaque pixels
- **Note:** True SIMD (inline asm like `gfx_simd.c`) can be added later for further 4× gain

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

### ~~P8. Add heap usage monitoring~~
- **Status:** ✅ Done (commit `3508ca7`)
- `heap_get_total/used/free` already existed in `heap.c`
- Added boot-time log: `[OK] Heap: X KB used / Y KB total (Z%)`
- Warns if >75%: `[!!] Heap pressure: Z% used — risk of silent exhaustion`

---

## Documentation & Guardrails

### P9. Update guardrails to reflect PMM migration
- **Impact:** Keep guardrail docs accurate after font system was fixed
- **Effort:** Small (~20 min)
- **Files:** Multiple
- [ ] Update `gfx_text.c` header comment: remove the `TODO: Migrate` line (it's done now)
- [ ] Update `rules.md` Known Gotchas: add the font PMM migration as a resolved example
- [ ] Update `.agent/skills/memory-allocation/SKILL.md`: remove `gfx_text.c` from "Tech Debt" section
- [ ] Update `.agents/workflows/add-asset.md`: add font system as a "good example" of correct PMM usage
- [ ] Review `gfx_text.c` `load_ttf_file()` error path: add comment that PMM pages are intentionally not freed (boot-time permanent)
- [ ] Verify `stb_truetype_impl.c` still redirects `malloc`/`free` to `kmalloc`/`kfree` (correct for small temp buffers)

### P10. Create/update memory management architecture docs
- **Impact:** Single source of truth for how memory is managed in Impossible OS
- **Effort:** Medium (~45 min)
- **Files:** `docs/architecture/memory-management.md` (create or update)
- [ ] Document the two-tier allocation model (kmalloc vs PMM) with diagram
- [ ] Document identity-mapped physical memory layout
- [ ] List all PMM consumers with sizes: framebuffer back buffer, font files, glyph pool, window framebuffers
- [ ] List all kmalloc consumers with typical sizes: VFS nodes, task structs, Codex values
- [ ] Document the glyph pool bump allocator: how it works, capacity, fragmentation characteristics
- [ ] Add a "capacity planning" section: current usage vs limits, warning thresholds
- [ ] Cross-reference `rules.md`, `/add-asset` workflow, and `memory-allocation` skill
- [ ] Add a Mermaid diagram showing memory regions at runtime

### P11. Add build-time `kmalloc` audit check
- **Impact:** Automated enforcement — catch violations before they ship
- **Effort:** Small (~15 lines in build script)
- **Files:** `scripts/build.sh` or new `scripts/lint-alloc.sh`
- [ ] `grep -rn 'kmalloc' src/kernel/gfx/ src/desktop/ --include='*.c'` at build time
- [ ] Whitelist known-safe calls (stb_truetype temps, small structs)
- [ ] Fail or warn on new `kmalloc` calls in GFX/desktop code without a `/* kmalloc OK: ... */` comment
- [ ] Add to CI/build pipeline
