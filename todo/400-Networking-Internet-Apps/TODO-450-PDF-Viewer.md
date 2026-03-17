# P1106 — PDF Viewer

> **Goal:** Parse and render PDF documents with navigation and zoom.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 6. PDF Viewer

### 6.1 Minimal PDF Parser

**Prompt:** Parse PDF structure: header `%PDF-1.x`, cross-reference table (xref) at end via `startxref` keyword, trailer with Root catalog. Parse page tree: catalog → Pages → individual pages. Decompress streams using deflate via miniz (Phase 03). After all items,sh clean`, commit `"apps: PDF parser core"`.


- [ ] Create `src/apps/pdfview/pdfview.c`
- [ ] Parse PDF file structure:
  - [ ] Header: `%PDF-1.x`
  - [ ] Cross-reference table (xref) at end of file
  - [ ] Trailer: root catalog reference
- [ ] Parse page tree: catalog → pages → individual page objects
- [ ] Decompress streams: deflate/zlib via `miniz` (from Phase 03)
- [ ] Commit: `"apps: PDF parser core"`

### 6.2 PDF Rendering

**Prompt:** Extract text operators from page content stream: Tf (set font), Td (move text position), Tj/TJ (show text). Map PDF fonts to system fonts as fallback. Render to `gfx_surface_t` (one surface per page). Render embedded images via `image_load()`. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: PDF rendering"`.


- [ ] Render text content: extract text operators (Tf, Td, Tj, TJ)
- [ ] Map fonts (use system fonts as fallback)
- [ ] Render to `gfx_surface_t` (one surface per page)
- [ ] Render embedded images (via `image_load()`)
- [ ] Commit: `"apps: PDF text and image rendering"`

### 6.3 PDF Viewer UI

**Prompt:** Display rendered page centered in window. Page navigation: Prev/Next buttons, page number input. Zoom: fit-to-width, fit-to-page, percentage, mouse wheel. Vertical scroll. Stretch: text search with highlight, continuous scroll. File association: .pdf → PDF Viewer. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: PDF viewer UI"`.


- [ ] Display rendered page centered in window
- [ ] Page navigation: Previous/Next buttons, page number input
- [ ] Zoom: fit-to-width, fit-to-page, percentage selection, mouse wheel
- [ ] Scrolling: vertical scroll through page
- [ ] *(Stretch)* Text search: find text on current page, highlight matches
- [ ] *(Stretch)* Multi-page continuous scroll view
- [ ] File association: `.pdf` → PDF Viewer
- [ ] Commit: `"apps: PDF viewer UI"`

