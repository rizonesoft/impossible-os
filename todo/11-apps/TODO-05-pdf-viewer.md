---
schema_version: 1
id: pdf-viewer
domain: 11-apps
status: active
title: "TODO-05 -- PDF Viewer"
---

# TODO-05 -- PDF Viewer

> **Goal:** Build `pdfview.exe` -- a PDF 1.x viewer for Impossible OS that parses cross-reference
> tables, decompresses FlateDecode/ASCIIHex streams via miniz, renders text and graphics operators
> to a `gfx_surface_t`, displays embedded images via `image_load_mem`, and provides a full
> navigation UI (zoom, page nav, scroll, Ctrl+F search, continuous-scroll stretch).

> [!IMPORTANT]
> miniz (`mz_uncompress`) must be ported before §3 stream decompression --
> `→ XREF: 02-kernel-core/TODO-32 §3`.
> `image_load_mem()` from `include/kernel/image.h` is the image decode path (JPEG/PNG/BMP) -- no
> separate stbi call needed for most cases; use `stbi_load_from_memory()` directly only for
> color-space conversions not handled by the wrapper.
> TTF font rendering uses `ttf_get(slot, px)` + `ttf_draw_string()` from `include/font_mgr.h`.
> All buffers larger than 4 KB **must** use `pmm_alloc_contiguous()` -- PDF streams, page
> surfaces, and the object cache all qualify.

---

## Inputs

- `02-kernel-core/TODO-03-kernel-libraries.md §5` -- `mz_uncompress(dst, &dst_len, src, src_len)` (miniz FlateDecode)
- `include/kernel/image.h` -- `image_load_mem(img, data, size)` -- §5 image rendering
- `include/font_mgr.h` -- `ttf_get(slot, px)`, `ttf_draw_string()`, `ttf_draw_char()`, `ttf_measure_width()` -- §4 text rendering
- `include/gfx.h` -- `gfx_create_surface()`, `gfx_fill_rect()`, `gfx_blit()`, `gfx_drop_shadow()`, `gfx_scale_blit()` -- §4–§6
- `include/desktop/controls.h` -- `CTRL_SCROLLBAR` (`CTRL_SCROLLBAR_VERT`/`CTRL_SCROLLBAR_HORIZ`), `CTRL_TEXTBOX`, `CTRL_BUTTON` -- §6
- `include/desktop/wm.h` -- `wm_create_window()` -- §6
- `include/desktop/file_assoc.h` (→ XREF `09-desktop-shell/TODO-02 §1`) -- `file_assoc_set(ext, prog_id, app_path)` -- §9
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_stat` -- §1 file loading
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()`, `pmm_free_contiguous()` -- all large buffers

---

## Outcome

`pdfview.exe file.pdf` opens a PDF document in a windowed viewer. Pages are rendered to surfaces at configurable zoom (fit-width, fit-page, 50%–400%). Smooth keyboard and mouse wheel navigation. Ctrl+F searches text across pages with highlight overlay. Continuous-scroll stretch mode virtualizes multi-page rendering. `.pdf` files open in `pdfview.exe` from File Manager, browser downloads, and email attachments.

---

## Implementation Order

| Step | Section                          | 💎/⭐ | Dependency                             |
| ---- | -------------------------------- | ----- | -------------------------------------- |
| 1    | PDF Structure Parser             | 💎    | VFS file load                          |
| 2    | Page Tree Traversal              | 💎    | §1 object resolver                     |
| 3    | Stream Decompression             | 💎    | §2 stream refs, miniz `D12T01 §3`      |
| 4    | Content Stream Renderer          | 💎    | §3 decompressed streams, TTF font_mgr  |
| 5    | Image Rendering                  | 💎    | §3, `image_load_mem`                   |
| 6    | PDF Viewer UI                    | 💎    | §4 + §5 page surface, controls.h, wm.h |
| 7    | Text Search                      | ⭐    | §4 text operator cache                 |
| 8    | Continuous Scroll View (Stretch) | ⭐    | §6 stable UI                           |
| 9    | File Association                 | 💎    | §6 app exists, TODO-02 §1              |

---

## 1. PDF Structure Parser `[Sonnet]`

**Source file:** `src/apps/pdfview/pdf_parse.c`; header `include/apps/pdfview/pdf.h`

- [ ] Load entire PDF into `pmm_alloc_contiguous()` buffer via `vfs_open` + `vfs_read` (stream-safe for > 4 KB files)
- [ ] Validate header: scan first 1024 bytes for `%PDF-1.` → store version digit; reject non-PDF files
- [ ] Locate `startxref`: scan backwards from EOF for `startxref\n{offset}\n%%EOF`; parse decimal offset
- [ ] Parse `xref` table at that offset:
  - [ ] `xref\n{first_obj} {count}\n` header line
  - [ ] Each entry: 20-byte record `{byte_offset} {gen_num} {in_use_flag}\r\n`; in-use = `'n'`
  - [ ] Handle cross-reference streams (PDF 1.5+): stream object with `/Type /XRef` -- parse stream as compressed xref; decompress and parse entries
- [ ] Parse `trailer` dict (after xref table or in stream): extract `/Root {id} {gen} R`, `/Size`, `/Encrypt` (unsupported -- warn + exit if present and not `/V 0`)
- [ ] `pdf_get_object(id, gen, out_buf, &out_len)`: look up byte offset from xref; seek + read `{id} {gen} obj ... endobj`; return content bytes
- [ ] Object cache: `struct pdf_obj { id, gen, offset, parsed_dict, stream_offset, stream_len }[1024]`; built from xref pass; `kmalloc` for struct, `pmm_alloc_contiguous` for stream data

---

## 2. Page Tree Traversal `[Sonnet]`

**Source file:** `src/apps/pdfview/pdf_pages.c`

- [ ] Resolve `/Root` → `pdf_get_object(root_id, ...)` → parse catalog dict; extract `/Pages` reference
- [ ] Recursive page-tree walker `collect_pages(node_id, page_refs[], &count)`:
  - [ ] `pdf_get_object(node_id)` → parse dict; read `/Type` value
  - [ ] If `/Pages`: iterate `/Kids` array recursively; inherit `/MediaBox` from this node if child lacks one
  - [ ] If `/Page`: append `node_id` to `page_refs[]`
  - [ ] Max 4096 pages; `page_refs[]` allocated with `pmm_alloc_contiguous()`
- [ ] Per-page descriptor `struct pdf_page { obj_id, gen, media_box[4] (x0,y0,x1,y1 in points), contents_id, resources_id }` -- populated during flatten pass
- [ ] `pdf_page_count()` returns total number of pages
- [ ] `pdf_get_page(n)` returns pointer to `struct pdf_page` for 0-based page index

---

## 3. Stream Decompression `[Sonnet]`

**Source file:** `src/apps/pdfview/pdf_stream.c`

- [ ] `pdf_decompress_stream(obj_id, out_buf, &out_len, max_len)`:
  - [ ] Locate stream content: scan object bytes for `stream\r\n` (or `stream\n`); end at `endstream`
  - [ ] Read `/Filter` from stream dict (may be array of filters applied in order)
  - [ ] `/FlateDecode` (zlib deflate): `mz_uncompress(out_buf, &out_len, compressed, compressed_len)` -- `out_buf` from `pmm_alloc_contiguous()` if output > 4 KB
  - [ ] `/ASCIIHexDecode`: iterate hex pairs `[0-9A-Fa-f]{2}` → decode byte; `>` = end-of-data marker
  - [ ] `/ASCII85Decode`: decode base-85 groups of 5 chars → 4 bytes; `~>` = EOD
  - [ ] No filter / unknown filter: copy raw bytes; log warning for unknown filter name
  - [ ] Return decoded bytes and length; caller owns buffer and must `pmm_free_contiguous` when done
- [ ] `pdf_get_stream(obj_id, &buf, &len)`: wrapper that calls `pdf_decompress_stream` and caches result; second call returns cached pointer

---

## 4. Content Stream Renderer `[Opus]`

**Source file:** `src/apps/pdfview/pdf_render.c`

- [ ] Allocate page surface: `gfx_create_surface(page_width_px, page_height_px)` where `page_width_px = media_box_width_pts × (dpi / 72.0)` at current zoom; fill white `gfx_fill_rect(..., GFX_COLOR_WHITE)`
- [ ] Content stream tokenizer: scan decompressed bytes; tokenize into operands (numbers, strings `(...)`, names `/foo`, arrays `[...]`, hex strings `<...>`) and operators (alphabetic keywords)
- [ ] **Graphics state** (current transform `cm[6]`, current path, fill/stroke color, line width)
- [ ] **Text state** (font name, font size, text matrix Tm, leading, char/word spacing)
- [ ] **Operator dispatch** -- key operators:
  - [ ] `BT` / `ET`: begin/end text block; reset text matrix to identity
  - [ ] `Tf fontname size`: set current font; lookup in page Resources `/Font` dict → map to system font via `ttf_get()` (fallback: `FONT_BODY` for proportional, `FONT_MONO` for monospace)
  - [ ] `Td x y` / `TD x y`: move text position; update text matrix
  - [ ] `Tm a b c d e f`: set text matrix directly
  - [ ] `Tj (string)`: show string -- decode PDF string bytes → UTF-8; call `ttf_draw_string(surface, font, x_px, y_px, text, color)`
  - [ ] `TJ [(s1) k1 (s2) k2 ...]`: string array with kerning adjustments; draw each string segment, applying `-k/1000 × font_size` horizontal offset between segments
  - [ ] `"` / `'`: move to next line then show string
  - [ ] `m x y`: moveto -- start new sub-path
  - [ ] `l x y`: lineto -- append line segment
  - [ ] `c x1 y1 x2 y2 x3 y3` / `v` / `y`: Bézier curve segments (approximate with 8-segment polyline)
  - [ ] `re x y w h`: append rectangle sub-path
  - [ ] `f` / `F` / `f*`: fill path (even-odd or winding); `gfx_fill_rect` for rectangles; scan-line fill for polygons
  - [ ] `S` / `s`: stroke path; `gfx_draw_line(surface, x0, y0, x1, y1, color, line_width)`
  - [ ] `B` / `b`: fill then stroke
  - [ ] `n`: end path (no paint)
  - [ ] `cm a b c d e f`: concatenate matrix to CTM (scale/rotate/translate); update transform for subsequent drawing
  - [ ] `Do name`: paint XObject -- look up in Resources `/XObject` → if `/Subtype /Image` → §5; if `/Form` → recurse into form content stream
  - [ ] `q` / `Q`: push/pop graphics state stack (depth ≤ 28 per PDF spec)
  - [ ] `rg r g b` / `RG r g b`: set fill / stroke RGB color (0.0–1.0 → 0–255)
  - [ ] `g val` / `G val`: gray fill / stroke
  - [ ] Unknown operator: skip operands and continue (permissive)

---

## 5. Image Rendering `[Sonnet]`

**Source file:** `src/apps/pdfview/pdf_image.c`

- [ ] **XObject images** (`/Subtype /Image` in Resources):
  - [ ] Read `/Width`, `/Height`, `/BitsPerComponent`, `/ColorSpace`, `/Filter` from image dict
  - [ ] Decompress stream via `pdf_get_stream()` → raw pixel bytes
  - [ ] `/ColorSpace /DeviceRGB` (or `/RGB`): 3 bytes per pixel -- pass directly to `image_load_mem()` or assemble raw RGBA buffer
  - [ ] `/ColorSpace /DeviceGray` (or `/Gray`): 1 byte per pixel → expand to RGBA (R=G=B=gray, A=255)
  - [ ] `/ColorSpace /Indexed [/DeviceRGB N table_stream]`: pixel byte = palette index → look up RGB triple in palette
  - [ ] `/Filter /DCTDecode` (JPEG): pass raw stream bytes to `image_load_mem()` directly
  - [ ] `/Filter /FlateDecode` + raw pixels: decompress first, then assemble RGBA
  - [ ] Scale decoded RGBA to rendered `dst_w × dst_h` pixels using CTM; `gfx_scale_blit(surface, dst_x, dst_y, dst_w, dst_h, img_pixels, img_w, img_h)`
- [ ] **Inline images** (`BI` / `ID` / `EI` operators in content stream):
  - [ ] On `BI`: parse abbreviated key-value pairs (`/W`, `/H`, `/CS`, `/BPC`, `/F`, etc.)
  - [ ] On `ID`: read raw image data bytes from stream until `EI` token
  - [ ] Decode and blit exactly as XObject images above

---

## 6. PDF Viewer UI `[Sonnet]`

**Source file:** `src/apps/pdfview/pdfview_ui.c`

- [ ] **Window**: `wm_create_window("PDF Viewer -- {filename}", W, H)` (resizable)
- [ ] **Toolbar** (fixed 40 px height):
  - [ ] `[←]` prev page; `[→]` next page; page input `CTRL_TEXTBOX` (4-char wide, numeric); `of {total}` label
  - [ ] Zoom `CTRL_TEXTBOX` showing current % (`"100%"`); `[−]` / `[+]` 10% steps; dropdown: `Fit Width`, `Fit Page`, 50%, 75%, 100%, 125%, 150%, 200%
  - [ ] `[🔍]` search button (Ctrl+F); `[📄]` open-file button
- [ ] **Canvas area** (fills remaining window below toolbar):
  - [ ] Centered page surface with 4-px `gfx_drop_shadow()` halo on white background
  - [ ] `CTRL_SCROLLBAR_VERT` on right; range = page height − canvas height; updated on page render
  - [ ] Horizontal scrollbar (`CTRL_SCROLLBAR_HORIZ`) appears only when page_width_px > canvas_width_px
- [ ] **Zoom computation**:
  - [ ] `Fit Width`: `zoom = canvas_width / page_width_pts × 72`
  - [ ] `Fit Page`: `zoom = min(canvas_width / page_width_pts, canvas_height / page_height_pts) × 72`
  - [ ] Manual %: `zoom = pct / 100.0`
  - [ ] Clamp: 10% min, 1600% max
- [ ] **Input handling**:
  - [ ] Mouse wheel scroll → move vertical scrollbar value; `wm_mark_dirty()`
  - [ ] Ctrl+scroll → zoom ±10%
  - [ ] Page Up / Page Down → prev/next page; arrow keys → scroll 40 px per step
  - [ ] Enter in page input box → jump to that page (clamp to 1–total)
- [ ] **Status bar** (20 px): `{filename}` left; `Page {n} of {total}` center; `{zoom}%` right
- [ ] Page surface cached per render: re-render only on zoom change or page navigation

---

## 7. Text Search `[Sonnet]`

**Source file:** `src/apps/pdfview/pdf_search.c`

- [ ] **Search bar** (slides in below toolbar on Ctrl+F): `CTRL_TEXTBOX` + `[▲ Prev]` `[▼ Next]` `[✕ Close]`; focus on open
- [ ] **Text extraction**: `pdf_extract_text(page_n, text_spans[], &span_count)` -- re-run content stream tokenizer for page, collecting `{ text_str, x, y, w, h }` for each Tj/TJ result; store in per-page span cache
- [ ] **Search**: on query submit → iterate all pages → `strcasestr(span.text, query)` → record hits as `{ page_n, span_idx, match_offset }` list
- [ ] **Highlight overlay**: render match spans as semi-transparent yellow `gfx_fill_rect(surface, x, y, w, h, RGBA(255,220,0,100))` overlaid after normal page render; active match uses brighter orange
- [ ] **Navigation**: `[▼ Next]` cycles through matches (wraps to page 1 after last); `[▲ Prev]` reverses; jumps to the match's page and scrolls it into viewport; match count shown `"3 of 17"`
- [ ] Clear overlay when search bar closed or query cleared

---

## 8. Continuous Scroll View (Stretch) `[Sonnet]`

**Source file:** `src/apps/pdfview/pdf_scroll.c`

- [ ] Toggle via `View → Continuous Scroll` menu item or toolbar button
- [ ] **Virtual canvas height**: `sum(page_height_px for all pages) + (page_count − 1) × 16` (16 px gap between pages)
- [ ] **Viewport mapping**: given `scroll_offset` → compute which pages overlap viewport; only render those pages + 1 page above and below (`±1` page look-ahead/behind)
- [ ] Pages not in viewport: deallocate their `gfx_surface_t` with `pmm_free_contiguous()` (lazy re-render on scroll back)
- [ ] Blit rendered pages at their computed `y` positions in canvas; draw 16 px dark-gray separator between pages
- [ ] Scrollbar range = total virtual canvas height − viewport height
- [ ] Page indicator in status bar updates continuously as viewport center crosses page boundaries
- [ ] Text search (§7) works in continuous mode: navigate to match jumps scroll offset to center the match

---

## 9. File Association `[Sonnet]`

**Source file:** registered at `pdfview_ui_init()`

- [ ] `file_assoc_set(".pdf", "ImpossibleOS.PDFViewer", "C:\\Impossible\\System32\\pdfview.exe")`
- [ ] Command-line: `pdfview.exe C:\path\to\file.pdf` -- open and display immediately
- [ ] `pdfview.exe` without args → open-file dialog (`dialog_file_open()`) filtered to `*.pdf`
- [ ] Open from File Manager double-click → `file_assoc_open()` routes to `pdfview.exe` with path arg
- [ ] Open from browser download (→ XREF `11-apps/TODO-01 §8`) → same launch path
- [ ] Open from email attachment (→ XREF `11-apps/TODO-04 §6`) → viewer window per attachment click
- [ ] Recent files: `HKCU\Software\Impossible\PDFViewer\RecentFiles` (up to 10 paths, MRU order)

---

## OS Comparison


| ⭐  | Feature                                                              | 🪟 Win11               | 🐧 Linux                        | 🚀 Impossible OS                                 |
| --- | -------------------------------------------------------------------- | ---------------------- | ------------------------------- | ------------------------------------------------ |
| 💎  | PDF structure parse                                                  | ✅ Edge PDF / Acrobat  | ✅ Evince / Okular / Zathura    | ⬜ §1 -- xref table + object cache               |
| 💎  | Page tree traversal + MediaBox inheritance                           | ✅ Edge PDF            | ✅ Evince                       | ⬜ §2 -- recursive Kids flatten                  |
| 💎  | FlateDecode / ASCIIHex stream decompress                             | ✅ Edge PDF            | ✅ Evince                       | ⬜ §3 -- `mz_uncompress` + hex decode            |
| 💎  | Text operator rendering                                              | ✅ Edge PDF            | ✅ MuPDF                        | ⬜ §4 -- TTF font fallback + matrix              |
| 💎  | Image rendering                                                      | ✅ Edge PDF            | ✅ Evince                       | ⬜ §5 -- `image_load_mem` + CTM scale-blit       |
| 💎  | Navigation + zoom + fit-to-width                                     | ✅ Edge PDF            | ✅ Evince                       | ⬜ §6 -- CTRL_SCROLLBAR, zoom dropdown           |
| ⭐  | Ctrl+F text search with highlight overlay                            | ✅ Edge PDF (built-in) | ⚠️ Evince basic; Zathura manual | ⬜ §7 -- semi-transparent overlay; match count   |
| ⭐  | Continuous scroll with virtual canvas + page virtualization          | ✅ Edge PDF            | ✅ Evince continuous            | ⬜ §8 -- (Stretch) -- ; lazy dealloc,            |
| 💎  | `.pdf` file association + launch from File Manager / email / browser | ✅ Edge PDF default    | ✅ `xdg-open`                   | ⬜ §9 -- `file_assoc_set`, recent-files Registry |

Impossible OS renders PDFs using the same native TTF font stack used everywhere else in the OS --
no separate font engine, no embedded PDF font renderer -- giving consistent glyph metrics and zero
additional dependency.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Parser:** open a simple 1-page PDF (`hello.pdf` with `%PDF-1.4` header); serial log shows `xref found at offset N`, `trailer: Root {id}`, `object cache: N entries`; `pdf_page_count()` returns 1
- [ ] **Page tree:** multi-page PDF (5 pages); `pdf_page_count()` returns 5; each `pdf_get_page(n)` returns valid MediaBox
- [ ] **FlateDecode:** PDF with `/Filter /FlateDecode` content stream; `mz_uncompress` succeeds; decompressed bytes start with `BT` text operator
- [ ] **Text rendering:** PDF with text content; page rendered to surface; TTF `ttf_draw_string` called with correct string and position; page image shows visible text
- [ ] **Image rendering:** PDF with embedded JPEG image; `image_load_mem` called; image blitted to page surface at correct position and scale
- [ ] **UI:** `pdfview.exe test.pdf` opens window; toolbar shows `Page 1 of N`; `[→]` advances to page 2; `[←]` returns to page 1; zoom `[+]` increases %; Fit Width button fills canvas width
- [ ] **Scroll:** page taller than window; vertical scrollbar appears; mouse wheel scrolls; text stays sharp at scrolled positions
- [ ] **Text search:** Ctrl+F → type search term; first match highlighted in yellow; `[▼]` advances to next match; status shows `"2 of 5"`; close clears overlay
- [ ] **File association:** `.pdf` registered; File Manager double-click on test.pdf opens `pdfview.exe`
- [ ] Commit: `"apps: PDF viewer -- parser, renderer, text/image, search, file assoc"`
