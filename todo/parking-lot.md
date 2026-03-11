# Parking Lot — Small Actionable Optimizations

> Items that improve performance, memory, or boot speed but aren't blocking
> any phase. Pick these up between major features or during polish passes.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.



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

---

## Window Drag Responsiveness

### ~~P12. Dirty-rectangle compositor during drag~~
- **Status:** ✅ Done
- Track old/new window rects, compute union as dirty region
- `wm_is_dragging()` + `wm_get_drag_dirty_rect()` API
- Full composite still runs (safe z-order), but only dirty region is swapped

### ~~P13. Use `fb_swap_rect()` during drag instead of `fb_swap()`~~
- **Status:** ✅ Done (bundled with P12)
- `fb_swap_rect()` on drag dirty region (~200 KB vs 3.6 MB)
- Cursor rect swapped separately (may be outside drag rect)
- Full `fb_swap()` preserved for non-drag composites

### ~~P14. Skip `terminal_render()` during drag~~
- **Status:** ✅ Done (bundled with P12)
- `terminal_render()` gated behind `!wm_is_dragging()`

### P15. Batch mouse events before compositing
- **Impact:** 5 × 1px drags → 1 × 5px drag = 1 composite instead of 5
- **Effort:** Small (~30 lines)
- **Files:** `main.c`, possibly `mouse.c`
- [ ] After reading one mouse event, drain the mouse FIFO for any additional pending events
- [ ] Accumulate deltas: `total_dx += dx`, `total_dy += dy`; keep last button state
- [ ] Apply accumulated delta as a single cursor move
- [ ] Then do one composite for the batched move
- [ ] Cap batch size (e.g., max 8 events) to avoid input lag

---

## GUI Performance & GPU Acceleration

### P16. Cached acrylic for taskbar & start menu
- **Impact:** Eliminates per-frame blur recomputation — currently `gfx_acrylic()` runs every composite frame for both taskbar (1280×40) and start menu (450×400+), each doing a full box blur + noise + tint
- **Effort:** Medium (~100 lines)
- **Files:** `desktop.c`, `wm.c`
- **Taskbar** (fixed position, always visible):
  - [ ] On wallpaper load/change: pre-blur the taskbar strip region → store as PMM-allocated cached texture
  - [ ] Each frame: fast-blit cached texture instead of recomputing `gfx_acrylic()`
  - [ ] Invalidate cache only when wallpaper changes or screen resolution changes
- **Start menu** (fixed position when open):
  - [ ] On menu open: snapshot + blur the menu region once → store as cached texture
  - [ ] Each frame while open: fast-blit cached texture
  - [ ] Invalidate on close (re-snapshot + re-blur on next open)
  - [ ] Right column darker overlay still applied per-frame (cheap alpha blend, no blur)
- **Invalidation triggers:**
  - [ ] Wallpaper change → invalidate both caches
  - [ ] Resolution change → invalidate + reallocate
  - [ ] Window move/resize behind taskbar → optionally invalidate taskbar cache (or accept stale blur as acceptable trade-off)

### ~~P17. AVX2 SIMD for blur and alpha blending~~
- **Status:** ✅ Done (commit `3e0cc53`)
- `simd_enable_avx()`: enables CR4.OSXSAVE + XCR0 (x87/SSE/AVX) at boot
- `simd_blend_pixels_avx2()`: 8 pixels/iter alpha blend via YMM registers
- `simd_blur_accum_avx2()`: 8 pixels/iter blur accumulate
- All functions use inline asm + `vzeroupper` for clean SSE/AVX transitions
- `fxsave_area_t` increased to 1024 bytes / 64-byte aligned for XSAVE
- Runtime dispatch via `simd_avx2_ok` flag; SSE2 fallback when unavailable
- **Note:** QEMU default (`qemu64`) lacks AVX2 — use `-cpu Haswell` to test

### P18. VirtIO-GPU 2D driver
- **Impact:** Hardware-accelerated rect fills, blits, page flips in QEMU
- **Effort:** Large (~2,000–3,000 lines)
- **Files:** new `src/kernel/drivers/virtio_gpu.c`, `virtio_gpu.h`
- **Prerequisites:** P16, P17 should be done first (better ROI)
- [ ] PCI enumeration: detect VirtIO GPU device (vendor 0x1AF4, device 0x1050)
- [ ] Map control/cursor virtqueues via VirtIO transport
- [ ] Implement `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D` — allocate GPU resources
- [ ] Implement `VIRTIO_GPU_CMD_SET_SCANOUT` — bind resource to display
- [ ] Implement `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` — upload pixel data
- [ ] Implement `VIRTIO_GPU_CMD_RESOURCE_FLUSH` — present to screen (page flip)
- [ ] Replace `fb_swap()` with VirtIO-GPU scanout flip
- [ ] Replace `fb_fill_rect()` with GPU fill command for large rects
- [ ] Cursor: use hardware cursor plane (eliminates cursor-in-compositor overhead)
- [ ] Fallback: keep VBE framebuffer path for non-VirtIO environments
