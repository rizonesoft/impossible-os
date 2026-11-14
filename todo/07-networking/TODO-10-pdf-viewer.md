---
schema_version: 1
id: pdf-viewer-networking
domain: 07-networking
status: active
title: "TODO-10 -- PDF Viewer & Document Reader"
---

# TODO-10 -- PDF Viewer & Document Reader

> **Goal:** Superseded 2026-11-14 except §9: the PDF engine and app are Recto, bundled through `11-apps/TODO-05-pdf-viewer.md` §10 to §20 (operator decision). §9 (PDF from HTTP) stays and hands the downloaded file to Recto. The earlier goal, kept for the record: build a full PDF viewer: structure parser (xref, trailer, object streams), object model with stream decompression (FlateDecode/LZW/ASCII85), page tree walker, content stream interpreter (graphics + text operators), embedded font rendering via stb_truetype, image rendering (FlateDecode + DCTDecode via stb_image), page rasterizer compositing text+graphics+images, a scrollable viewer app with zoom/navigation/text search, HTTP-fetched PDF streaming, and an AcroForm stub. The PDF viewer is the standard document format reader required by every desktop OS.

> [!IMPORTANT]
> `stbi_zlib_decode_buffer()` from `include/stb_image.h` is the FlateDecode decompressor -- no separate miniz port is needed. `stbi_load_from_memory()` handles DCTDecode (JPEG streams in PDF). `stbtt_PackFontRange()` + `stbtt_GetPackedQuad()` from `include/stb_truetype.h` render embedded TrueType fonts. `fb_blit(dst_x, dst_y, ...)` from `include/kernel/drivers/framebuffer.h` composites pixel buffers to screen. All page render buffers > 4 KB use `pmm_alloc_contiguous()`. The sections build as a strict dependency chain: structure parser → object model → page tree → content stream → fonts + images → page renderer → viewer app. Do not start any section until the section it depends on passes its verification step.

## Inputs

- `include/stb_image.h` -- `stbi_zlib_decode_buffer(out, olen, in, ilen)` for FlateDecode; `stbi_load_from_memory(data, len, &w, &h, &ch, 4)` for DCTDecode (JPEG) and PNG image XObjects
- `include/stb_truetype.h` -- `stbtt_InitFont()`, `stbtt_PackFontRange()`, `stbtt_GetPackedQuad()`, `stbtt_ScaleForPixelHeight()` for embedded TrueType font rendering
- `include/kernel/drivers/framebuffer.h` -- `fb_blit(dst_x, dst_y, src, src_w, src_h, stride)` for compositing rasterized pages to screen
- `include/kernel/image.h` -- `image_t`, `image_load_mem()`, `image_scale()` for auxiliary image handling
- `include/desktop/wm.h` + `include/desktop/controls.h` -- `wm_create_window()`, `ctrl_create_button/scrollbar/textbox`, `ctrl_draw_all()` for viewer app UI
- `src/kernel/net/http.c` (TODO-03) -- `https_get(url, buf, max)` for §2 HTTP-fetched PDF streaming
- `src/kernel/fs/vfs.c` -- `vfs_open()`/`vfs_read()` for opening local `.pdf` files
- → XREF: `07-networking/TODO-03-http-tls.md` -- `https_get()` prerequisite for §2 PDF-from-HTTP
- → XREF: `07-networking/TODO-07-web-browser.md` -- §6 connects the browser's "Open PDF" flow to `pdfview_open_url()`; browser calls viewer directly rather than saving to disk

## Outcome

- `pdf_open(path)` parses xref, trailer, resolves all objects, decompresses streams.
- `pdf_get_page(doc, n)` returns the page dict; `pdf_render_page(doc, n, dpi)` returns a pixel buffer.
- Content stream interpreter handles all standard text and path operators; embedded TrueType fonts rendered via stb_truetype; fallback to system fonts.
- Images (FlateDecode + DCTDecode, DeviceRGB + CMYK) composited at correct page coordinates.
- Viewer app: zoom 50–400%, fit-to-width/page, prev/next navigation, thumbnail sidebar, Ctrl+F text search with highlights, `pdfview filename.pdf` shell command.
- HTTP-fetched PDFs stream directly into viewer without disk write.
- AcroForm stub detects form fields and renders them as interactive widgets.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                      | Depends On                                                           | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 PDF structure parser -- `%PDF` header, xref table, trailer, incremental updates, object streams | `vfs_read()` only; standalone parser                                |  [/]   |
| 💎  |   2   | §2 Object model -- indirect ref resolver, stream decompressor, dict/array/string type system     | §1 (raw bytes + xref offsets needed to locate objects)               |  [/]   |
| 💎  |   3   | §3 Page tree -- catalog → Pages, MediaBox, Contents stream, recursive Kids                       | §2 (dict/array deref needed to traverse page tree)                   |  [/]   |
| 💎  |   4   | §4 Content stream interpreter -- text operators (BT/ET/Tf/Td/Tj), path (m/l/S/f), state (q/Q/cm) | §3 (page dict provides Contents stream obj IDs)                     |  [/]   |
| 💎  |   5   | §5 Font handling -- embedded TrueType/Type1, ToUnicode CMap, stb_truetype render, system fallback | §4 (Tf operator is the font context for text rendering)              |  [/]   |
| 💎  |   6   | §6 Image rendering -- XObject images + inline, FlateDecode+DCTDecode, CMYK→RGB, fb_blit         | §4 (Do operator); §2 (streams); 08-graphics-ui/TODO-01 §5 decoder fix |  [/]   |
| 💎  |   7   | §7 Page renderer -- rasterize to bitmap, composite text+graphics+images, DPI scaling             | §5 fonts + §6 images (all content types must render before compositor) |  [/]   |
| 💎  |   8   | §8 PDF viewer app -- scroll, zoom, navigation, thumbnail sidebar, text search, print            | §7 (renderer must produce pixel buffers before viewer can display)   |  [/]   |
| 💎  |   9   | §9 PDF from HTTP -- `https_get` pipe to viewer, pdf:/https: URL, no disk write                  | §8 (viewer app must be ready to accept in-memory buffer); TODO-03    |  [ ]   |
| 💎  |  10   | §10 PDF forms stub -- AcroForm detection, field widgets, FDF/XFDF export                        | §8 viewer (form fields rendered as overlay on top of page bitmap)    |  [/]   |

---

## 1. PDF Structure Parser

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

## 2. Object Model

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

## 3. Page Tree

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

## 4. Content Stream Interpreter

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

## 5. Font Handling

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

## 6. Image Rendering

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

## 7. Page Renderer

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §13 (item: "Build recto-core and recto-pdf as static libraries for the user-mode t" at line 221)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §13; do not build this here

## 8. PDF Viewer App

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §15 (item: "Build the Recto app for Impossible OS from src/app with the engine" at line 260)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §15; do not build this here

## 9. PDF from HTTP

> **Changed 2026-11-14:** the viewer is Recto (`11-apps/TODO-05-pdf-viewer.md` §15, §18). Where an item below names `pdf_open_mem()` or `pdfview_open_url()`, the work is to hand the downloaded bytes to Recto through its open-from-memory entry and the app model activation.

`https_get(url, buf, max)` pipes PDF bytes directly into `pdf_open_mem()` without saving to disk. Open `pdf:` and `https:` URLs from browser "Open PDF" action. Memory-mapped stream.

**Files:** `src/apps/pdfview/pdfview_app.c` (extend)

> [!NOTE]
> `pdf_open_mem(data, len, &doc)`: accept a pre-loaded byte buffer instead of reading from VFS. Modify `pdf_open()` to call `pdf_open_mem()` internally (i.e., load into buffer first, then call shared parsing logic). Buffer sizing: cap at 64 MB (`pmm_alloc_contiguous(64 MiB)`); if response exceeds cap: return -EFBIG. `pdfview_open_url(url)`: `https_get(url, buf, max)` → on success: `pdf_open_mem(buf, len, &doc)` → `pdfview_open_window(doc)`. Browser integration: in `browser.c` §10 (from TODO-07), when a link ends with `.pdf` or `Content-Type: application/pdf`: call `pdfview_open_url(href)` instead of downloading. `pdf:` URI scheme: if `cmd_pdfview` receives a URL starting with `https://` or `http://`: call `pdfview_open_url()` rather than `vfs_open()`.

- [ ] `pdf_open_mem(data, len, &doc)` → 0 or -errno: skip VFS; use `data` directly as `doc->data`; call `pdf_parse_header/xref/trailer` on the buffer
- [ ] `pdfview_open_url(url)` → 0 or -errno: `pmm_alloc_contiguous(64 MiB)`; `https_get(url, buf, max)`; on success: `pdf_open_mem(buf, len, &doc)` + open window; on error: free buf + return -errno
- [ ] Update `cmd_pdfview()`: if argument starts with `http://` or `https://`: call `pdfview_open_url()` instead of `pdfview_open()`
- [ ] Browser hook in TODO-07: `browser_navigate()` checks `Content-Type: application/pdf` → route to `pdfview_open_url()`
- [ ] Log: `[PDF] HTTP stream %zu bytes from %s`
- [ ] Commit: `"apps/pdfview: HTTP streaming -- pdf_open_mem, pdfview_open_url, browser Content-Type hook"`

## 10. PDF Forms Stub

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box), [`controls.md#check-box-and-radio-button`](../../docs/design/controls.md#check-box-and-radio-button), [`controls.md#combo-box-and-drop-down`](../../docs/design/controls.md#combo-box-and-drop-down)

> **Deferred:** [L] superseded 2026-11-14: the PDF engine and viewer are Recto, bundled from source -> XREF: 11-apps/TODO-05 §15 (item: "Build the Recto app for Impossible OS from src/app with the engine" at line 260)

- [/] Superseded: replaced by Recto, see `11-apps/TODO-05-pdf-viewer.md` §15; do not build this here

---

## OS Comparison


| ⭐  | Feature                    | 🪟 Win11                                       | 🐧 Linux                                               | 🚀 Impossible OS                                            |
| --- | -------------------------- | ---------------------------------------------- | ------------------------------------------------------ | ----------------------------------------------------------- |
| 💎  | PDF structure + xref       | ✅ Edge PDF viewer; Adobe Acrobat;             | ✅ `poppler`/`mupdf`; full xref stream +               | ⬜ §1 -- xref table + stream; incremental                   |
| 💎  | Object model               | ✅ Full object model in `poppler`/Adobe        | ✅ `mupdf`/`poppler` full object model                 | ⬜ §2 -- stbi_zlib_decode for FlateDecode; hand-written LZW |
| 💎  | Page tree                  | ✅ Full page tree in all                       | ✅ Full page tree support                              | ⬜ §3 -- flat `page_map[]` for O(1) random                  |
| 💎  | Content stream interpreter | ✅ Full PDF operator set in                    | ✅ `mupdf` full operator set; `poppler`                | ⬜ §4 -- 50+ operators; CTM affine stack                    |
| 💎  | Embedded font rendering    | ✅ DirectWrite font rendering; full CMap       | ✅ FreeType2 in `poppler`/`mupdf`; full CMap           | ⬜ §5 -- stb_truetype render (already in OS)                |
| 💎  | Image rendering            | ✅ Full colorspace support in Acrobat/Edge     | ✅ JPEG via libjpeg; CMYK conversion                   | ⬜ §6 -- stbi_load_from_memory for JPEG (already in         |
| 💎  | Page renderer              | ✅ DirectX hardware-accelerated; 300 DPI print | ✅ Cairo/Skia software rasterizer in `poppler`/`mupdf` | ⬜ §7 -- software rasterizer; de Casteljau bezier           |
| 💎  | Viewer app                 | ✅ Edge PDF viewer + Adobe                     | ✅ Evince/Okular; thumbnail sidebar; text search       | ⬜ §8 -- 50–400% zoom + fit-to-width/page; 16-thumb         |
| ⭐  | PDF from HTTP              | ✅ Edge opens PDF URLs in-browser;             | ✅ Firefox opens PDFs via `pdf.js`                     | ⬜ §9 -- `⭐` true in-kernel streaming --                   |
| 💎  | PDF forms                  | ✅ Acrobat full form support; Edge             | ✅ Okular/Evince form fill; `poppler` FDF              | ⬜ §10 -- stub renders field widgets; FDF/XFDF              |

> **After §1–§10:** Impossible OS has a kernel-native PDF renderer using stb_truetype (already in the OS), stb_image's zlib decoder (already present), and a custom scanline rasterizer -- zero external PDF libraries required. The HTTP streaming path (`⭐`) opens PDFs from URLs without writing to disk, a capability native browsers handle via JavaScript (pdf.js) but that Impossible OS handles in the kernel directly.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Structure: `pdf_open("test.pdf", &doc)` on a valid PDF → `doc.page_count > 0`; serial log shows version + object count; on a broken xref: returns -errno
- [ ] Object model: `pdf_get_object(doc, 1, 0)` returns expected dict type; FlateDecode stream decompresses to correct size; `pdf_deref()` resolves a 3-hop indirect chain correctly
- [ ] Page tree: `pdf_get_page(doc, 0, &page)` returns non-zero MediaBox; `pdf_get_contents_stream()` returns non-empty buffer; inherited MediaBox from parent Pages node resolved
- [ ] Content stream: `pdf_interp_run()` on a simple page with `BT /F1 12 Tf 100 700 Td (Hello) Tj ET` produces a glyph record at (100,700) size 12; `q … Q` restores graphics state correctly
- [ ] Font: embedded TTF extracted + `stbtt_InitFont()` succeeds; `pdf_render_glyph()` produces non-zero alpha bitmap; ToUnicode CMap maps at least 26 ASCII letters correctly; fallback font used for Type1 font
- [ ] Images: FlateDecode image XObject decoded to correct width×height ARGB32 buffer; JPEG DCTDecode image decoded via `stbi_load_from_memory()`; CMYK→RGB conversion: K=0, C=1 → R=0
- [ ] Page render: `pdf_render_page(doc, 0, 96, &buf)` returns non-null pixel buffer; rendered bitmap displayed in QEMU via `fb_blit()` shows recognizable page text and layout at 96 DPI
- [ ] Viewer app: `pdfview test.pdf` opens window; zoom to 200% → page zooms; Next → page 2 renders; thumbnail sidebar shows small page previews; `Ctrl+F "hello"` highlights occurrences
- [ ] HTTP: `pdfview https://www.w3.org/WAI/WCAG21/wcag21.pdf` → page renders from streamed bytes (no file on disk); serial log shows `[PDF] HTTP stream N bytes`
- [ ] Forms: PDF with AcroForm renders text fields as white boxes with existing values; click on field → textbox overlay appears; `--export-fdf out.fdf` writes valid FDF file
- [ ] Commit: `"apps/pdfview: complete PDF viewer -- parser, renderer, stb_truetype fonts, HTTP stream, AcroForms"`
