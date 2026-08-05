---
schema_version: 1
id: pdf-viewer-networking
domain: 07-networking
status: active
title: "TODO-10 -- PDF Viewer & Document Reader"
---

# TODO-10 -- PDF Viewer & Document Reader

> **Goal:** Build a full PDF viewer: structure parser (xref, trailer, object streams), object model with stream decompression (FlateDecode/LZW/ASCII85), page tree walker, content stream interpreter (graphics + text operators), embedded font rendering via stb_truetype, image rendering (FlateDecode + DCTDecode via stb_image), page rasterizer compositing text+graphics+images, a scrollable viewer app with zoom/navigation/text search, HTTP-fetched PDF streaming, and an AcroForm stub. The PDF viewer is the standard document format reader required by every desktop OS.

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
- → XREF: `06-networking/TODO-03-http-tls.md` -- `https_get()` prerequisite for §2 PDF-from-HTTP
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
| 💎  |   1   | §1 PDF structure parser -- `%PDF` header, xref table, trailer, incremental updates, object streams | `vfs_read()` only; standalone parser                                |  [ ]   |
| 💎  |   2   | §2 Object model -- indirect ref resolver, stream decompressor, dict/array/string type system     | §1 (raw bytes + xref offsets needed to locate objects)               |  [ ]   |
| 💎  |   3   | §3 Page tree -- catalog → Pages, MediaBox, Contents stream, recursive Kids                       | §2 (dict/array deref needed to traverse page tree)                   |  [ ]   |
| 💎  |   4   | §4 Content stream interpreter -- text operators (BT/ET/Tf/Td/Tj), path (m/l/S/f), state (q/Q/cm) | §3 (page dict provides Contents stream obj IDs)                     |  [ ]   |
| 💎  |   5   | §5 Font handling -- embedded TrueType/Type1, ToUnicode CMap, stb_truetype render, system fallback | §4 (Tf operator is the font context for text rendering)              |  [ ]   |
| 💎  |   6   | §6 Image rendering -- XObject images + inline, FlateDecode+DCTDecode, CMYK→RGB, fb_blit         | §4 (Do operator invokes image XObject); §2 (stream decompression)    |  [ ]   |
| 💎  |   7   | §7 Page renderer -- rasterize to bitmap, composite text+graphics+images, DPI scaling             | §5 fonts + §6 images (all content types must render before compositor) |  [ ]   |
| 💎  |   8   | §8 PDF viewer app -- scroll, zoom, navigation, thumbnail sidebar, text search, print            | §7 (renderer must produce pixel buffers before viewer can display)   |  [ ]   |
| 💎  |   9   | §9 PDF from HTTP -- `https_get` pipe to viewer, pdf:/https: URL, no disk write                  | §8 (viewer app must be ready to accept in-memory buffer); TODO-03    |  [ ]   |
| 💎  |  10   | §10 PDF forms stub -- AcroForm detection, field widgets, FDF/XFDF export                        | §8 viewer (form fields rendered as overlay on top of page bitmap)    |  [ ]   |

---

## 1. PDF Structure Parser `[Opus]`

`%PDF-x.y` header. Cross-reference table: scan backward from `startxref`, parse `xref` section(s). Trailer dictionary with `Root`, `Encrypt`, `Info`. Incremental update support (multiple xref sections chained via `Prev`). Object streams (PDF 1.5 compressed xref via `ObjStm`/`XRef` stream types).

**Files:** `src/apps/pdfview/pdf_parser.c` (new), `include/apps/pdfview/pdf.h` (new)

> [!NOTE]
> This is `[Opus]` -- PDF xref parsing has multiple quirky edge cases: (1) cross-reference **tables** (traditional: `xref\n N F\n` sections with 20-byte entries each); (2) cross-reference **streams** (PDF 1.5: a compressed stream object at `startxref` with `W` array specifying field widths); (3) **incremental updates**: each revision appends a new xref + trailer; chain via `Prev` offsets in trailer dict. Algorithm: start at file end; scan backward for `startxref`; read offset; seek to offset; determine `xref` table or stream; if table: parse sections; if stream: decompress and parse per `W` field widths; merge into global `xref_table[MAX_OBJECTS]`. Object stream (`ObjStm`): a compressed stream containing multiple objects concatenated; parse the `N` and `First` fields for offset table; objects are stored compressed and resolved lazily. Max objects: 32768 initial (expand with `pmm_alloc_contiguous` if needed).

- [ ] `pdf_xref_entry_t { uint64_t offset; uint16_t gen; uint8_t type; }` -- type: `IN_USE=1`, `FREE=0`, `IN_STREAM=2` (for ObjStm objects); in `pdf.h`
- [ ] `pdf_doc_t { uint8_t *data; size_t len; pdf_xref_entry_t *xref; uint32_t xref_size; pdf_dict_t *trailer; uint8_t version_major, version_minor; }` in `pdf.h`
- [ ] `pdf_parse_header(doc)` → 0 or -EINVAL: find `%PDF-` in first 1 KiB; extract major.minor version
- [ ] `pdf_find_startxref(doc)` → file offset: scan last 1 KiB of file for `startxref\r\n` or `startxref\n`; parse offset integer
- [ ] `pdf_parse_xref_table(doc, offset)` → 0 or -errno: scan `xref` keyword; read section headers `first_obj count\r\n`; read 20-byte entries (`OOOOOOOOOO GGGGG N\r\n`); populate `doc->xref[]`
- [ ] `pdf_parse_xref_stream(doc, offset)` → 0 or -errno: read stream object; decompress; parse per `W` field widths (type/offset/gen); populate `doc->xref[]`
- [ ] `pdf_load_xref(doc)` → 0 or -errno: call appropriate parse function; follow `Prev` chain for incremental updates; newer entries override older for same object ID
- [ ] `pdf_parse_trailer(doc, trailer_offset)` → dict: parse `trailer\n<<...>>` block or stream dict
- [ ] `pdf_open(path, &doc)` → 0 or -errno: `vfs_open(path)`; `vfs_read()` full file into `pmm_alloc_contiguous()` buffer; `pdf_parse_header()`; `pdf_find_startxref()`; `pdf_load_xref()`; `pdf_parse_trailer()`
- [ ] Log: `[PDF] Version %u.%u, %u objects, xref type=%s`
- [ ] Commit: `"apps/pdf: structure parser -- xref table+stream, trailer, incremental updates, ObjStm index"`

## 2. Object Model `[Opus]`

Indirect object resolver (`N G obj … endobj`). Stream decompressor: FlateDecode (`stbi_zlib_decode_buffer`), LZWDecode (hand-written), ASCII85Decode, ASCIIHexDecode. Dictionary/array/string/name/integer/real/boolean/null type system. `pdf_get_object(doc, obj_id)` with recursive indirect ref deref.

**Files:** `src/apps/pdfview/pdf_objects.c` (new), `include/apps/pdfview/pdf.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the PDF object model requires a recursive parser for nested dicts, arrays, and indirect references with no fixed depth limit (legitimate PDFs nest arrays 10+ levels deep). **Token types**: `<<…>>` dict, `[…]` array, `(…)` string (with octal + `\n\r\t\b\f\\()` escapes), `<hex>` hex string, `/name` name, `N G R` indirect ref, integer, real, `true`/`false`/`null`. **Deref chain**: `pdf_get_object(doc, id)` → look up `xref[id].offset` → seek to offset → parse `id gen obj … endobj`; if the value is another `R` ref: recurse; cap depth at 16 to prevent loops. **Stream decompression**: after parsing stream dict: read `Length` (possibly itself an indirect ref); read raw stream bytes; check `/Filter` -- may be an array of filters to apply in sequence; decompress using the correct decoder; return decompressed bytes. FlateDecode: `stbi_zlib_decode_buffer(out, olen, src+2, len-2)` (skip 2-byte zlib header). LZWDecode: implement LZW decompressor (used in older PDFs and TIFF images); ~100 lines. ASCII85: `~>` terminus; 5-char groups → 4 bytes. ASCIIHex: `>` terminus; two-hex-digit pairs.

- [ ] `pdf_value_t` union + tag enum: `PDF_NULL, PDF_BOOL, PDF_INT, PDF_REAL, PDF_STRING, PDF_NAME, PDF_ARRAY, PDF_DICT, PDF_REF, PDF_STREAM` in `pdf.h`
- [ ] `pdf_dict_t { pdf_kv_t entries[64]; int count; }` and `pdf_array_t { pdf_value_t *items; int count; }` (items via `kmalloc` up to 64; larger via `pmm_alloc_contiguous`)
- [ ] `pdf_stream_t { pdf_dict_t *dict; uint8_t *data; size_t len; uint8_t *raw; size_t raw_len; }` -- `data` is decompressed; `raw` is original bytes
- [ ] `pdf_parse_token(src, pos, end, &val)` → new_pos: recursive descent tokenizer; handle all PDF token types; return `pdf_value_t`
- [ ] `pdf_parse_dict(src, pos, end, &dict)` → new_pos: `<<` → parse key/value pairs until `>>`
- [ ] `pdf_parse_array(src, pos, end, &arr)` → new_pos: `[` → parse values until `]`
- [ ] `pdf_get_object(doc, obj_id, gen)` → `pdf_value_t`: look up xref; seek; parse `N G obj`; parse value; parse `stream…endstream` if applicable; return
- [ ] `pdf_deref(doc, val)` → `pdf_value_t`: if `val.type == PDF_REF`: `pdf_get_object(doc, ref.id, ref.gen)`; recurse up to depth 16
- [ ] `pdf_decompress_stream(stream, filter_name)` → 0 or -errno: `"FlateDecode"` → `stbi_zlib_decode_buffer()`; `"LZWDecode"` → lzw_decode(); `"ASCII85Decode"` → ascii85_decode(); `"ASCIIHexDecode"` → hex_decode()
- [ ] `pdf_get_stream_data(doc, stream_dict, &data, &len)`: resolve Length; read raw; apply filter chain from `/Filter` (single name or array)
- [ ] `pdf_dict_get(dict, key)` → `pdf_value_t*`: case-sensitive key lookup; return NULL if absent
- [ ] Commit: `"apps/pdf: object model -- indirect resolver, FlateDecode/LZW/ASCII85 decompressor, type system"`

## 3. Page Tree `[Sonnet]`

Catalog → Pages node → recursive Kids traversal. Page count. Page dict: MediaBox (crop area), Resources (Font/XObject dicts), Contents stream (single or array). `pdf_get_page(doc, n)` → page dict.

**Files:** `src/apps/pdfview/pdf_pages.c` (new)

> [!NOTE]
> Page tree walk: `Catalog` dict (obj ID from `trailer[Root]`) → `Pages` dict (type=Pages) → `Count` (total pages) + `Kids` array (child page or Pages node IDs). `Kids` entries are either `Pages` nodes (recurse) or `Page` nodes (leaf). Efficient random access: build a flat `page_map[page_count]` array of page dict object IDs during open; `pdf_get_page(doc, n)` = `pdf_get_object(doc, page_map[n])`. Inherited attributes: MediaBox, Resources, Rotate can be on parent Pages node; if absent on Page: walk up via `Parent` ref until found. MediaBox: `[x0 y0 x1 y1]` -- PDF coordinate origin is bottom-left; viewer must flip Y. Content stream: `/Contents` may be a single stream ref or an array of stream refs; concatenate all into one logical stream before interpretation.

- [ ] `pdf_page_t { uint32_t dict_obj_id; float media_box[4]; int rotate; uint32_t resources_obj_id; uint32_t *content_obj_ids; int content_count; }` in `pdf.h`
- [ ] `pdf_build_page_map(doc)`: DFS page tree from `Catalog` → `Pages`; append each `Page` leaf's obj_id to `doc->page_map[]`; allocate via `pmm_alloc_contiguous()` if `page_count > 1024`
- [ ] `pdf_get_page(doc, n, &page)` → 0 or -ERANGE: validate `n < doc->page_count`; look up obj_id; resolve dict; extract MediaBox + Rotate + Resources + Contents; handle inherited attrs
- [ ] `pdf_get_contents_stream(doc, page, &buf, &len)` → 0 or -errno: if single ref: `pdf_get_stream_data()`; if array: concatenate all stream data into one buffer (`pmm_alloc_contiguous()`)
- [ ] `pdf_get_resources(doc, page)` → `pdf_dict_t*`: resolve Resources dict (possibly inherited)
- [ ] Log: `[PDF] Page count=%u, page 0: MediaBox=[%.0f %.0f %.0f %.0f]`
- [ ] Commit: `"apps/pdf: page tree -- catalog/Pages walk, page_map[], MediaBox, Resources, Contents concat"`

## 4. Content Stream Interpreter `[Opus]`

Parse and execute PDF graphics operators from the page content stream. Text state: `BT`/`ET`, `Tf` (font+size), `Td`/`TD`/`Tm`/`T*` (position), `Tj`/`TJ`/`'`/`"` (show text). Graphics state: `cm` (matrix), `q`/`Q` (push/pop), `w` (line width), `m`/`l`/`c`/`h` (path), `S`/`f`/`B` (stroke/fill), `Do` (XObject invoke), `cs`/`CS`/`sc`/`SC` (color).

**Files:** `src/apps/pdfview/pdf_interp.c` (new), `include/apps/pdfview/pdf_interp.h` (new)

> [!NOTE]
> This is `[Opus]` -- the PDF content stream interpreter is a novel virtual machine: it maintains a graphics state stack (CTM, color, line width, font, text position) and dispatches 50+ named operators. The coordinate system is PDF user space (origin bottom-left, Y increases up) -- all drawn primitives must be transformed to screen space (Y-flip: `screen_y = page_h - pdf_y`). **CTM (Current Transformation Matrix)**: 3×2 affine matrix `[a b c d e f]`; `cm` post-multiplies the CTM; `q` pushes a copy; `Q` pops. Text position: maintained as `(tx, ty)` in text space; `Td` translates by (dx, dy); `Tm` sets an absolute matrix; `T*` advances one line. **Operator dispatch**: scan the token stream; push non-operator tokens onto operand stack; on operator name: dispatch to handler function with stack contents. Operand stack: max 8 entries (`pdf_value_t stack[8]`). Unknown operators: log + skip.

- [ ] `pdf_gs_t { float ctm[6]; float color_fill[4]; float color_stroke[4]; float line_width; uint32_t font_obj_id; float font_size; float text_pos[2]; float text_matrix[6]; float char_spacing; float word_spacing; float text_leading; pdf_gs_t *prev; }` graphics state in `pdf_interp.h`
- [ ] `pdf_interp_ctx_t { pdf_doc_t *doc; pdf_page_t *page; pdf_gs_t *gs_stack; pdf_render_buf_t *render; }` interpreter context
- [ ] `pdf_interp_run(ctx, content_buf, len)` → 0 or -errno: tokenize content stream; operand stack loop; operator dispatch table
- [ ] Operator handlers: `op_BT/ET`, `op_Tf`, `op_Td/TD/Tm/Tstar`, `op_Tj/TJ/quote/dquote`, `op_cm`, `op_q/Q`, `op_w`, `op_m/l/c/v/y/h`, `op_S/s/f/F/B/b`, `op_Do`, `op_cs/CS/sc/SC/rg/RG/g/G/k/K`
- [ ] `op_Tj(ctx, str, len)`: for each character/glyph: look up glyph advance in current font; advance `text_pos[0]` by advance × font_size; record `{glyph, x, y, size, color}` in render buffer
- [ ] `op_TJ(ctx, array)`: iterate array; string elements → `op_Tj`; number elements → adjust `text_pos[0]` by `-n/1000 * font_size`
- [ ] `op_m/l/c/h`: append to path buffer; `op_S` → stroke path segments; `op_f` → fill path area
- [ ] `op_Do(ctx, name)`: look up XObject in Resources; if `Subtype=Image` → queue image render; if `Subtype=Form` → recurse interpreter with form's content stream + its own CTM
- [ ] `pdf_mat_multiply(a, b, out)`: 3×2 affine multiply; used by `cm` and `Tm`
- [ ] `pdf_transform_point(ctm, x, y, &sx, &sy)`: apply CTM + Y-flip for screen coords
- [ ] Commit: `"apps/pdf: content stream interpreter -- text+path+color+XObject operators, CTM, state stack"`

## 5. Font Handling `[Opus]`

Extract embedded TrueType font programs from `/FontFile2`. Parse `ToUnicode` CMap for glyph-ID → Unicode codepoint mapping. Render text via `stbtt_PackFontRange()` + `stbtt_GetPackedQuad()`. Fall back to system fonts (Inter/Selawik) for non-embedded fonts.

**Files:** `src/apps/pdfview/pdf_font.c` (new), `include/apps/pdfview/pdf_font.h` (new)

> [!NOTE]
> This is `[Opus]` -- font handling in PDF is security-adjacent: malformed font programs can cause buffer overruns in naive parsers; use stb_truetype's safe API (it does bounds-checked reads). **Embedded TrueType** (`/FontDescriptor → /FontFile2`): extract raw TTF bytes from the stream; call `stbtt_InitFont()` on the bytes; cache `stbtt_fontinfo` keyed by font obj_id. **ToUnicode CMap**: a stream containing a series of `beginbfchar`/`endbfchar` and `beginbfrange`/`endbfrange` blocks mapping CID → Unicode; parse into a `uint16_t cmap[256]` lookup table (for simple 1-byte encoding; for 2-byte CIDFonts, a 65536-entry table via `pmm_alloc_contiguous()`). **Glyph rendering**: `stbtt_ScaleForPixelHeight(info, font_size_px)` → scale; `stbtt_GetCodepointBitmap(info, sx, sy, codepoint, &bw, &bh, &bx, &by)` → alpha bitmap; composite alpha bitmap at text position into page render buffer using current fill color. **Type1 fallback**: Type1 fonts are not directly supported -- use the system fallback font. **Fallback fonts**: load `C:\Impossible\Fonts\Inter.ttf` (or Selawik) once at startup; use for any font without a valid FontFile2.

- [ ] `pdf_font_t { uint32_t obj_id; stbtt_fontinfo info; uint8_t *font_data; uint16_t cmap[256]; uint8_t has_embedded; uint8_t is_type1; char base_font[64]; }` + `pdf_font_cache[16]` + `font_cache_count`
- [ ] `pdf_font_load(doc, font_obj_id, &font)` → 0 or -errno: resolve font dict; get `/FontDescriptor → /FontFile2` stream; `stbtt_InitFont(&font->info, data, 0)`; parse ToUnicode CMap if present
- [ ] `pdf_cmap_parse(stream_buf, len, cmap_out)`: scan for `beginbfchar`/`endbfchar` + `beginbfrange`/`endbfrange`; populate `cmap[cid] = unicode_cp`
- [ ] `pdf_glyph_to_codepoint(font, glyph_id)` → unicode or glyph_id: look up `font->cmap[glyph_id]`; if 0 or no CMap: use glyph_id directly (works for Latin PDFs with standard encoding)
- [ ] `pdf_render_glyph(font, codepoint, x, y, size_px, color, render_buf)`: `stbtt_ScaleForPixelHeight()`; `stbtt_GetCodepointBitmapSubpixel()`; alpha-composite into `render_buf` at `(x, y)` with fill `color`
- [ ] `pdf_get_font(doc, resources, font_name)` → `pdf_font_t*`: look up `/Font/<name>` in resources dict; check cache; if miss: `pdf_font_load()`; add to cache
- [ ] Fallback font: at `pdf_init()`: load `C:\Impossible\Fonts\Inter.ttf` into global `fallback_font`; `pdf_render_glyph` falls back if `font->has_embedded == 0`
- [ ] Commit: `"apps/pdf: font handling -- FontFile2 TTF extract, ToUnicode CMap, stbtt render, fallback font"`

## 6. Image Rendering `[Sonnet]`

Inline images and XObject images. Decompress: FlateDecode (`stbi_zlib_decode_buffer`) + DCTDecode (JPEG via `stbi_load_from_memory`). Colorspace conversion: DeviceCMYK → DeviceRGB formula. Blit at correct page coordinates via `fb_blit`.

**Files:** `src/apps/pdfview/pdf_image.c` (new)

> [!NOTE]
> PDF image types: `Subtype=Image` XObject dict with `Width`, `Height`, `ColorSpace`, `BitsPerComponent`, `Filter`. FlateDecode + DeviceRGB: decompress raw bytes; each pixel is `R G B` (3 bytes); convert to ARGB32. DCTDecode (JPEG): raw stream bytes are a valid JPEG file; `stbi_load_from_memory(stream_data, len, &w, &h, &ch, 4)` decodes to RGBA32 directly -- width/height from the PDF dict must match. CMYK→RGB formula: `R = 255 × (1−C) × (1−K)`, `G = 255 × (1−M) × (1−K)`, `B = 255 × (1−Y) × (1−K)`. Coordinate transform: PDF image origin is bottom-left of the image box; apply CTM then Y-flip for screen placement. Inline images (`BI … ID … EI` operators): parse image dict inline in the content stream; decode and blit immediately. XObject images (`/Do` operator): queued by content stream interpreter (§4 `op_Do`); resolved and rendered here.

- [ ] `pdf_image_t { uint32_t width, height; uint8_t *pixels; }` (ARGB32, allocated via `pmm_alloc_contiguous`)
- [ ] `pdf_decode_image(doc, image_dict_obj_id, &img)` → 0 or -errno: get stream data; check Filter; check ColorSpace; decode accordingly; produce ARGB32 pixel buffer
- [ ] `pdf_decode_flatergb(data, len, w, h, bpc, &img)`: `stbi_zlib_decode_buffer()`; if `bpc==8`: treat raw bytes as RGB triplets; convert to ARGB32
- [ ] `pdf_decode_dct(data, len, &img)`: `stbi_load_from_memory(data, len, &w, &h, &ch, 4)` → RGBA32 in img.pixels
- [ ] `pdf_decode_cmyk(data, len, w, h, &img)`: apply CMYK→RGB formula per pixel; output ARGB32
- [ ] `pdf_render_image(ctx, img, x, y, w_pts, h_pts)`: transform `(x, y, w, h)` from PDF user space to screen pixels via CTM + Y-flip + DPI scale; `image_scale(&scaled, &src_img, screen_w, screen_h)` if dimensions differ; `fb_blit(screen_x, screen_y, scaled.pixels, screen_w, screen_h, screen_w*4)` into page render buffer
- [ ] Inline image parser: in `pdf_interp.c` handle `BI` → parse abbreviated key/value pairs (e.g., `/CS /RGB /BPC 8`) until `ID`; read pixel data until `EI`; call `pdf_decode_*`
- [ ] Commit: `"apps/pdf: image rendering -- FlateDecode+DCT decode, CMYK→RGB, CTM transform, fb_blit"`

## 7. Page Renderer `[Opus]`

Rasterize content stream to an in-memory ARGB32 bitmap at requested DPI (screen=96, print=300). Run the content stream interpreter over the page. Composite text glyphs + filled/stroked paths + images in draw order. Return `uint32_t *` pixel buffer with width × height.

**Files:** `src/apps/pdfview/pdf_render.c` (new), `include/apps/pdfview/pdf.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the page compositor must maintain correct Z-order (objects drawn in content stream order) and handle alpha-compositing of text glyphs (grayscale alpha bitmaps from stb_truetype) over colored backgrounds. **Render buffer**: `uint32_t *pixels = pmm_alloc_contiguous(w * h * 4)`; initialized to white (`0xFFFFFFFF`). **DPI scaling**: PDF user space is in points (1/72 inch); at 96 DPI: `scale = 96.0 / 72.0 = 1.333`; pixel dimensions = `ceil(media_box_w × scale) × ceil(media_box_h × scale)`. **Path rendering**: after collecting path segments from `m/l/c/h` operators, rasterize using a scanline fill algorithm for filled paths and a Bresenham segment rasterizer for stroked paths. **Alpha composite for text**: `dst = src_alpha × src_color + (1 - src_alpha) × dst_color` per channel; stb_truetype glyph bitmaps are 8-bit alpha. **Bezier curves** (`c` operator): subdivide to line segments at a flatness of 0.5 px (de Casteljau subdivision).

- [ ] `pdf_render_buf_t { uint32_t *pixels; int width, height; float dpi; float scale; }` in `pdf.h`
- [ ] `pdf_render_page(doc, page_n, dpi, &buf)` → 0 or -errno: `pdf_get_page()`; compute pixel dimensions; `pmm_alloc_contiguous(w*h*4)`; fill white; `pdf_get_contents_stream()`; `pdf_interp_run()`; return `buf`
- [ ] Text glyph composite: `pdf_composite_glyph(buf, alpha_bitmap, bw, bh, screen_x, screen_y, color)`: iterate pixel; alpha-blend with `color` over existing `buf.pixels[y*w+x]`
- [ ] Path scanline fill (`op_f`): build edge table from path segments; active edge table (AET) scanline loop; fill spans between even-odd or winding pairs; write fill color to pixels
- [ ] Path stroke (`op_S`): Bresenham or DDA for line segments; for each segment: draw `line_width` px thick; cap/join = square/miter (simple default)
- [ ] Bezier flatten: `bezier_flatten(p0, p1, p2, p3, segs_out[], max)` → seg_count: recursive de Casteljau with flatness test `< 0.5 px`
- [ ] `pdf_render_free(buf)`: `pmm_free(buf.pixels, buf.width * buf.height * 4)`
- [ ] Log: `[PDF] Rendered page %u: %ux%u at %.0f DPI in %u ms`
- [ ] Commit: `"apps/pdf: page renderer -- scanline fill, stroke, alpha glyph composite, bezier flatten"`

## 8. PDF Viewer App `[Sonnet]`

Scrollable single-page view. Zoom 50–400% + fit-to-width + fit-to-page. Page navigation (prev/next buttons, Go-to-page input). Thumbnail sidebar. Text selection + copy. Ctrl+F text search with highlight. Print via framebuffer. `pdfview filename.pdf` shell command.

**Files:** `src/apps/pdfview/pdfview_app.c` (new)

> [!NOTE]
> Viewer window: `wm_create_window("PDF Viewer", 50, 30, 1000, 720)`. Toolbar: Prev/Next buttons (`ctrl_create_button`), page number text box + "/ N" label, zoom dropdown (50%/75%/100%/125%/150%/200%/400%/Fit-Width/Fit-Page), Ctrl+F search box. Main canvas: scrollable frame; when page is rendered: blit `pdf_render_buf_t.pixels` to the canvas at the computed scroll offset. **Zoom**: compute `display_dpi = 96 × zoom_factor`; re-render on zoom change (cache last rendered page). **Scroll**: `scroll_offset_x`, `scroll_offset_y` in pixels; mouse wheel → `scroll_offset_y ±= 60 px`. **Thumbnail sidebar** (100 px wide): render each page at 24 DPI (fast); draw as small bitmaps; click → navigate. Cache up to 16 thumbnail bitmaps. **Text selection**: record glyph positions during `op_Tj`; on mouse drag: collect glyphs whose bounding boxes intersect the drag rectangle; highlight by drawing transparent blue rect over each glyph box; Ctrl+C → concatenate Unicode codepoints → clipboard. **Text search**: `pdf_search_page(doc, n, term, results[])` → scan glyph list for string matches; highlight matches with yellow rect.

- [ ] `pdfview_state_t { pdf_doc_t doc; int cur_page; float zoom; int scroll_x, scroll_y; pdf_render_buf_t cur_render; pdf_render_buf_t thumbs[16]; int thumb_valid[MAX_PAGES]; }` in `pdfview_app.c`
- [ ] `pdfview_open(path)`: `pdf_open()`; `pdf_build_page_map()`; render page 0 at 96 DPI; open window
- [ ] `pdfview_draw(state)`: blit `cur_render.pixels` at `(100 + scroll_x, 40 + scroll_y)` in window; draw scrollbars; draw toolbar; draw thumbnails
- [ ] `pdfview_goto_page(state, n)`: render page n at current zoom DPI; reset scroll; update page-number textbox
- [ ] `pdfview_zoom(state, factor)`: `state->zoom = factor`; `display_dpi = 96 × factor`; re-render current page
- [ ] `pdfview_fit_width(state)`: `zoom = window_canvas_w / page_w_px_at_96dpi`; re-render
- [ ] `pdfview_search(state, term)`: iterate pages; `pdf_search_page()`; show first match; draw highlight rects
- [ ] `pdfview_print(state)`: render page at 300 DPI; scale to framebuffer dimensions; `fb_blit()` full-screen; wait for keypress; restore desktop
- [ ] `cmd_pdfview(argc, argv)`: parse filename; `pdfview_open()`; register in shell command table; also register as `.pdf` file association
- [ ] Commit: `"apps/pdfview: viewer app -- zoom/scroll/navigation, thumbnail sidebar, text search, print"`

## 9. PDF from HTTP `[Sonnet]`

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

## 10. PDF Forms Stub `[Sonnet]`

Detect `AcroForm` dictionary in catalog. Render form fields (text, checkbox, radio, dropdown) as interactive widgets over the page bitmap. Export filled form data as `FDF`/`XFDF`.

**Files:** `src/apps/pdfview/pdf_forms.c` (new)

> [!NOTE]
> AcroForm detection: `pdf_dict_get(catalog, "AcroForm")` → non-NULL. Field array: `AcroForm[Fields]` → array of indirect refs to field dicts. Field dict: `/FT` (field type: `Tx` text, `Btn` button, `Ch` choice), `/T` (partial name), `/V` (current value), `/Rect` (bounding box in page space), `/P` (page ref). Rendering: after `pdfview_draw()` blits the page bitmap, iterate fields for the current page; for each field: transform `/Rect` from PDF user space to screen coordinates; draw a white rectangle with a 1 px black border; for `Tx` fields: render `/V` string with stb_truetype at 10 px; for `Btn/checkbox`: if `/V` == `/Yes` draw ✓ else draw empty box; for `Ch/dropdown`: render current value + dropdown arrow. Interactivity: mouse click inside a field rect → `ctrl_create_textbox` overlay for `Tx` fields; checkbox click toggles `/V`. FDF export: write `%FDF-1.2` header + `FDF` dict with `Fields` array of `{/T name /V value}` dicts.

- [ ] `pdf_field_t { char name[128]; char value[512]; uint8_t field_type; float rect[4]; uint32_t page_n; }` + `pdf_fields[128]` + `field_count`
- [ ] `pdf_load_form_fields(doc)` → count: `pdf_dict_get(catalog, "AcroForm")`; iterate `Fields` array; for each: resolve field dict; extract `/FT`, `/T`, `/V`, `/Rect`, `/P`; populate `pdf_fields[]`
- [ ] `pdf_forms_draw(state, page_n, win)`: iterate fields where `field.page_n == page_n`; transform rect to screen; draw white box + border; draw value text
- [ ] `pdf_forms_click(state, screen_x, screen_y)`: find field at screen position; `Tx` → `ctrl_create_textbox` overlay; `Btn` → toggle `/V`; `Ch` → show dropdown list
- [ ] `pdf_export_fdf(doc, path)`: write FDF file: `%FDF-1.2\n1 0 obj\n<</FDF <</Fields [<</T (name) /V (value)>> ...]>>>>\nendobj`
- [ ] `pdf_export_xfdf(doc, path)`: write XFDF XML: `<?xml ...><xfdf><fields><field name="..."><value>...</value></field>...`
- [ ] Register `--export-fdf` flag in `cmd_pdfview()`
- [ ] Commit: `"apps/pdfview: AcroForm stub -- field detect/render, Tx/Btn/Ch widgets, FDF/XFDF export"`

---

## OS Comparison


| ⭐   | Feature                    | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | -------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | PDF structure + xref       | ✅ Edge PDF viewer; Adobe Acrobat;        | ✅ `poppler`/`mupdf`; full xref stream +  | ⬜ §1 -- xref table + stream; incremental |
| 💎   | Object model               | ✅ Full object model in `poppler`/Adobe   | ✅ `mupdf`/`poppler` full object model    | ⬜ §2 -- stbi_zlib_decode for FlateDecode; hand-written LZW |
| 💎   | Page tree                  | ✅ Full page tree in all                  | ✅ Full page tree support                 | ⬜ §3 -- flat `page_map[]` for O(1) random |
| 💎   | Content stream interpreter | ✅ Full PDF operator set in               | ✅ `mupdf` full operator set; `poppler`   | ⬜ §4 -- 50+ operators; CTM affine stack  |
| 💎   | Embedded font rendering    | ✅ DirectWrite font rendering; full CMap  | ✅ FreeType2 in `poppler`/`mupdf`; full CMap | ⬜ §5 -- stb_truetype render (already in OS) |
| 💎   | Image rendering            | ✅ Full colorspace support in Acrobat/Edge | ✅ JPEG via libjpeg; CMYK conversion      | ⬜ §6 -- stbi_load_from_memory for JPEG (already in |
| 💎   | Page renderer              | ✅ DirectX hardware-accelerated; 300 DPI print | ✅ Cairo/Skia software rasterizer in `poppler`/`mupdf` | ⬜ §7 -- software rasterizer; de Casteljau bezier |
| 💎   | Viewer app                 | ✅ Edge PDF viewer + Adobe                | ✅ Evince/Okular; thumbnail sidebar; text search | ⬜ §8 -- 50–400% zoom + fit-to-width/page; 16-thumb |
| ⭐   | PDF from HTTP              | ✅ Edge opens PDF URLs in-browser;        | ✅ Firefox opens PDFs via `pdf.js`        | ⬜ §9 -- `⭐` true in-kernel streaming --  |
| 💎   | PDF forms                  | ✅ Acrobat full form support; Edge        | ✅ Okular/Evince form fill; `poppler` FDF | ⬜ §10 -- stub renders field widgets; FDF/XFDF |

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
