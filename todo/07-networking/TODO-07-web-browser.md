---
schema_version: 1
id: web-browser-networking
domain: 07-networking
status: active
title: "TODO-07 -- Web Browser"
---

# TODO-07 -- Web Browser

> **Goal:** Build Impossible OS's flagship internet application in ten progressive sections: text-only browser proving the HTTP stack, a full HTML tokenizer + DOM tree, block/inline layout engine, tab management, image rendering, CSS cascade engine, JavaScript stub, bookmarks manager, download manager with HTTP Range resume, and a privacy + security layer (TLS padlock, cookie jar, mixed-content blocking, HTTP caching). A QUIC/HTTP 2 upgrade path is noted for future work (→ XREF `06-networking/TODO-08` when created).

> [!IMPORTANT]
> `http_get()`/`https_get()` from TODO-03 are the mandatory network primitives. `image_load_mem()` from `include/kernel/image.h` handles JPEG/PNG/BMP decoding -- the browser fetches image bytes via HTTP and passes them directly to this function. `wm_create_window()` from `include/desktop/wm.h` is the window creation API; `font_draw_string()` from `include/desktop/font.h` renders text. All source lives under `src/apps/browser/`; headers under `include/apps/browser/`. The DOM tree and layout engine are the two largest allocations in the browser -- use `pmm_alloc_contiguous()` for node arrays and render buffers > 4 KB; `kmalloc` only for small per-node structs (≤ 4 KB each). The CSS style engine (§4) depends on the DOM tree (§9) and the layout engine (§5) being stable before computed styles can be applied. Tab management (§5 in implementation order) must be added before HTML rendering to avoid a global-state refactor.

## Inputs

- `src/kernel/net/http.c` + `https_get()` -- `http_get(url, buf, max)` and `https_get(url, buf, max)` from TODO-03; browser calls these for all page and resource fetches
- `include/kernel/image.h` -- `image_load_mem(img, data, size)` for `<img src>` decoding (JPEG/PNG/BMP); `image_t { width, height, pixels }` for framebuffer blit
- `include/desktop/wm.h` -- `wm_create_window(title, x, y, w, h)` for browser window + tab bar; mouse/keyboard event callbacks
- `include/desktop/font.h` -- `font_draw_string(x, y, str, color, size)` for text rendering; `font_draw_char` for individual glyphs
- `include/desktop/controls.h` -- text input box, button, scrollbar controls for address bar and toolbar
- `src/kernel/fs/vfs.c` -- `vfs_open()`/`vfs_write()`/`vfs_read()` for saving downloads to `C:\Users\Default\Downloads\` and reading bookmarks JSON
- `include/registry.h` -- cookie persistence and browser settings in `HKCU\Software\ImpossibleBrowser`
- → XREF: `06-networking/TODO-03-http-tls.md` -- `http_get`, `https_get`, keep-alive pool, HTTP Range header, TLS cert verified flag (for padlock icon)
- → XREF: `06-networking/TODO-02-dns-sockets.md` -- `dns_resolve_dual()` for mixed-IPv4/IPv6 resource fetches
- → XREF: `06-networking/TODO-05-firewall.md` -- browser respects outbound firewall rules transparently; no browser-specific changes needed

## Outcome

- Text-only browser working end-to-end: `http_get` → tag strip → scrollable window → link navigation.
- HTML parser produces a complete DOM tree for any real-world HTML page.
- Block/inline layout engine renders readable multi-column web pages with scroll.
- Up to 16 browser tabs; Ctrl+T/W/Tab keyboard shortcuts.
- `<img src>` images decoded and blitted inline at correct dimensions.
- CSS cascade (element/class/ID selectors, specificity, box model) applied to DOM.
- JavaScript stub detects `<script>` blocks, shows placeholder, exposes no-op `window`/`document` surface.
- Bookmarks in `bookmarks.json`; Ctrl+D add; HTML import/export.
- Download manager with progress, pause/cancel, HTTP Range resume.
- TLS padlock in address bar; cookie jar (session-default/persistent opt-in); mixed-content block; `ETag`/`Cache-Control` HTTP caching.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                           | Depends On                                                                | Status |
| --- | :---: | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Text-only browser -- tag-strip state machine, scrollable window, link list, back/forward           | `http_get` (TODO-03); `wm_create_window`, `font_draw_string`             |  [ ]   |
| 💎  |   2   | §2 HTML parser + DOM tree -- tokenizer, tag/attr/children, essential 20 tags                         | §1 (window infrastructure proven; browser_t struct established)           |  [ ]   |
| 💎  |   3   | §6 Tab management -- `browser_tab_t`, tab bar UI, Ctrl+T/W/Tab, 16-tab limit                        | §1 (single-tab browser must exist before tabs are abstracted)             |  [ ]   |
| 💎  |   4   | §3 Block/inline layout engine -- block flow, inline flow, word wrap, vertical scroll, clip           | §2 (DOM tree is the layout engine's input); §3 tabs for per-tab viewport  |  [ ]   |
| 💎  |   5   | §5 Image loading -- `<img src>` HTTP fetch + `image_load_mem`, inline blit, alt text                 | §2 (DOM `<img>` node); §4 layout (image occupies a block box)             |  [ ]   |
| 💎  |   6   | §4 CSS parser + style engine -- tokenizer, cascade, box model, computed styles                       | §4 layout (must be stable before CSS modifies box dimensions)             |  [ ]   |
| 💎  |   7   | §9 JavaScript stub -- `<script>` detection, `[JavaScript disabled]` placeholder, `window`/`document` | §2 DOM (`<script>` tag captured in tokenizer)                             |  [ ]   |
| 💎  |   8   | §7 Bookmarks manager -- `bookmarks.json`, Ctrl+D add/edit, dropdown, HTML import/export             | §3 tabs (bookmarks are per-browser, not per-tab); §1 address bar          |  [ ]   |
| 💎  |   9   | §8 Download manager -- progress window, pause/cancel, HTTP Range resume, `Downloads\` save          | §2 DOM + §4 layout (right-click `<a href>` context menu); TODO-03 Range  |  [ ]   |
| 💎  |  10   | §10 Privacy + security -- TLS padlock, mixed-content block, cookie jar, `ETag` caching              | §6 CSS (padlock icon placement); §3 tabs (per-tab security state)         |  [ ]   |

---

## 1. Text-Only Browser `[Sonnet]`

`http_get(url)` → in-tag/not-in-tag state machine strips HTML tags → display plain text in a scrollable window. Extract `<a href>` links as a numbered list. Address bar (type URL + Enter). Back/forward URL history stack.

**Files:** `src/apps/browser/browser.c` (new), `include/apps/browser/browser.h` (new)

> [!NOTE]
> Tag stripping state machine: `state = OUTSIDE_TAG`. For each byte: if `c == '<'`: state = INSIDE_TAG; if `c == '>'`: state = OUTSIDE_TAG; else if state == OUTSIDE_TAG: emit byte to output. Link extraction: second pass over the raw HTML buffer; scan for `href="` or `href='`; extract the quoted URL; append to a `links[64]` array. Display: one scrollable `wm_create_window` (640×480); draw text starting at `(left_pad, top_pad)` with `font_draw_string`; newline tracking via `x` reset and `y += font_line_height`; scrollbar thumb position from `scroll_offset / total_content_height`. Back/forward: `url_history[32]` ring buffer; `history_pos` index; Back = `history_pos--`; Forward = `history_pos++`. Address bar: single-line text input control at the top of the window.

- [ ] `browser_t { char url[2048]; char *page_buf; size_t page_len; char links[64][2048]; int link_count; char url_history[32][2048]; int history_pos; int scroll_offset; }` in `browser.h`
- [ ] `browser_strip_tags(html, len, out_buf, out_max)` → out_len: OUTSIDE/INSIDE_TAG state machine; preserve `\n` on `</p>`, `<br>`, `</h1>`–`</h6>`; convert `&amp;`→`&`, `&lt;`→`<`, `&gt;`→`>`, `&nbsp;`→` `
- [ ] `browser_extract_links(html, len, browser)`: regex-free scan for `href=` → extract URL (handle both `"` and `'` delimiters); de-relativize against `browser->url` (prepend scheme+host for `/`-relative paths)
- [ ] `browser_navigate(browser, url)`: push current URL to history; `http_get` or `https_get` based on scheme; strip + extract; redraw
- [ ] `browser_draw(browser, win)`: clear window; draw stripped text with word wrap at window width; draw numbered link list below text; draw scrollbar
- [ ] `browser_handle_key(browser, key)`: Enter in address bar → `browser_navigate`; Up/Down arrows → `scroll_offset ±= font_line_height`; Backspace → `browser_navigate(history[pos-1])`
- [ ] `browser_open()`: `wm_create_window("Impossible Browser", 50, 50, 900, 600)`; navigate to default page (`about:blank` → show "Welcome" message)
- [ ] Commit: `"apps/browser: text-only browser -- tag strip, link list, address bar, back/forward"`

## 2. HTML Parser + DOM Tree `[Opus]`

Tokenizer → token stream (start tag, end tag, text, comment, DOCTYPE) → DOM tree (node: tag, attributes array, parent, children, text content). Support 20 essential tags.

**Files:** `src/apps/browser/html_parser.c` (new), `include/apps/browser/dom.h` (new)

> [!NOTE]
> This is `[Opus]` -- the HTML tokenizer + DOM builder are a novel engine with no prior implementation. The tokenizer must handle the messiness of real HTML: unquoted attribute values, self-closing tags (`<br>`, `<img>`, `<hr>`, `<input>`), omitted end tags, and nested comments. **Tokenizer states**: TEXT, TAG_OPEN, TAG_NAME, ATTR_NAME, ATTR_EQ, ATTR_VALUE_SQ, ATTR_VALUE_DQ, ATTR_VALUE_UNQUOTED, SELF_CLOSE, COMMENT, DOCTYPE. **DOM node**: `dom_node_t { uint8_t type; char tag[32]; dom_attr_t attrs[16]; uint8_t attr_count; struct dom_node *parent; struct dom_node *first_child; struct dom_node *next_sibling; char *text; }`. Node types: `DOM_ELEMENT=1`, `DOM_TEXT=2`, `DOM_COMMENT=3`. All `dom_node_t` allocations via `kmalloc` (each ≤ 4 KB); text buffers > 4 KB via `pmm_alloc_contiguous()`. **Essential tags**: h1–h6, p, br, b/strong, i/em, a, ul/ol/li, img, table/tr/td, hr, pre/code, form/input/button, script (captured, content stored verbatim for §9).

- [ ] `dom_attr_t { char name[64]; char value[256]; }` in `dom.h`
- [ ] `dom_node_t` as above; `dom_alloc_node(type)` → `kmalloc`; `dom_free_tree(root)` walks and frees
- [ ] `html_tokenizer_t { const char *src; size_t pos; size_t len; uint8_t state; char tag_buf[64]; char attr_name[64]; char attr_val[512]; }` in `html_parser.h`
- [ ] `html_tokenize_next(tok, &token)` → `token_t { type, tag, attrs[], text }`: advance one token; handle all tokenizer states; de-escape `&amp;`/`&lt;`/`&gt;`/`&nbsp;`/`&#NNN;`/`&#xNN;`
- [ ] `html_parse(src, len)` → `dom_node_t *root`: create `<html>` root; maintain open-element stack; push/pop on start/end tags; auto-close void elements (`br/img/hr/input`); text tokens → DOM_TEXT children of current node
- [ ] `dom_find_attr(node, name)` → value string or NULL
- [ ] `dom_walk(node, cb, userdata)`: depth-first pre-order traversal callback
- [ ] Replace text-strip renderer in §1 with a `dom_walk` callback that emits text nodes
- [ ] Commit: `"apps/browser: HTML tokenizer + DOM tree -- 20 tags, attrs, text nodes, void element auto-close"`

## 3. Tab Management `[Sonnet]`

`struct browser_tab_t` (URL, DOM root, scroll position, history stack, security state). Tab bar rendered across the top of the browser window. Ctrl+T new tab, Ctrl+W close, Ctrl+Tab cycle. Up to 16 tabs.

**Files:** `src/apps/browser/tabs.c` (new), `include/apps/browser/browser.h` (extend)

> [!NOTE]
> Tab bar height: 28 px. Each tab: title (first 20 chars of `<title>` tag or URL hostname), close × button, active tab highlighted. Tab switch: on click: `active_tab = i`; redraw full window. Each `browser_tab_t` has its own `page_buf` and DOM tree root -- switching tabs swaps which buffer/tree is rendered. Memory limit per tab: max 4 MB page buffer (4× `pmm_alloc_contiguous(1 MB)`); DOM node pool of 4096 nodes. New tab (Ctrl+T): `tabs[tab_count++] = new_tab("about:blank")`; clamp to 16. Close tab (Ctrl+W): free page_buf + `dom_free_tree()`; shift remaining tabs down. `browser_t` gains `browser_tab_t *tabs[16]; int tab_count; int active_tab;`.

- [ ] `browser_tab_t { char url[2048]; char title[64]; dom_node_t *dom_root; char *page_buf; size_t page_len; int scroll_offset; char history[32][2048]; int history_pos; uint8_t is_https; uint8_t cert_ok; }` in `browser.h`
- [ ] Migrate `browser_t` fields to `browser_tab_t`; `browser_t` gains tab array
- [ ] `tab_bar_draw(browser, win)`: iterate tabs; draw tab button (title + ×); highlight active
- [ ] `tab_bar_click(browser, x, y)` → handled: if click on × of tab i: `tab_close(browser, i)`; if click on tab body: `browser->active_tab = i`
- [ ] `tab_new(browser)`: allocate `browser_tab_t`; init; navigate to `about:blank`; redraw tab bar
- [ ] `tab_close(browser, i)`: free `page_buf` (pmm_free) + `dom_free_tree(dom_root)`; shift; if active tab closed: activate previous; clamp
- [ ] `browser_active_tab(browser)` → `browser_tab_t*`: `return &browser->tabs[browser->active_tab]`
- [ ] Keyboard: `Ctrl+T` → `tab_new`; `Ctrl+W` → `tab_close(active)`; `Ctrl+Tab` → `active_tab = (active_tab+1) % tab_count`
- [ ] Commit: `"apps/browser: tab management -- browser_tab_t, tab bar UI, Ctrl+T/W/Tab, 16-tab limit"`

## 4. Block / Inline Layout Engine `[Opus]`

Block flow (top-to-bottom): each block-level element (`div`, `p`, `h*`, `ul`, `table`) starts a new vertical flow. Inline flow (left-to-right + word wrap at viewport width). Intrinsic widths. Relative lengths (em, %). Vertical scroll for long pages. Clip overflow.

**Files:** `src/apps/browser/layout.c` (new), `include/apps/browser/layout.h` (new)

> [!NOTE]
> This is `[Opus]` -- the layout engine is a novel algorithm with no prior implementation. **Box model**: every DOM element produces a `layout_box_t { int x, y, w, h; int margin[4]; int padding[4]; int border[4]; uint8_t display; dom_node_t *node; struct layout_box *parent; struct layout_box *first_child; struct layout_box *next_sibling; }`. Block-level tags: `div, p, h1-h6, ul, ol, li, table, tr, pre, hr, br` -- each generates a box with `display=BLOCK`. Inline tags: `b, strong, i, em, a, span, code` -- `display=INLINE`. **Block layout**: for each child box of a block container: place at `(container_x + margin_left, current_y + margin_top)`; width = `container_w - margin_h - padding_h - border_h`; advance `current_y` by box height + margin_bottom. **Inline layout**: maintain a current-line buffer; append inline boxes until line width exceeds container width; then flush the line to a new row (word wrap). **Scroll**: `layout_height = total box tree height`; on scroll event: `scroll_offset += delta`; clip render to `[scroll_offset, scroll_offset + viewport_h]`. **Intrinsic sizes**: text nodes: measure via `font_string_width(text, font_size)`. Images: use `<img>` `width`/`height` attributes or `img->width`/`img->height` from `image_t`.

- [ ] `layout_box_t` as above; `display_t { BLOCK=0, INLINE=1, NONE=2, TABLE=3 }` in `layout.h`
- [ ] `layout_build(dom_root, viewport_w)` → `layout_box_t *root_box`: recursive; `dom_node_type_to_display(tag)` maps tag names to display types; allocate box per node; set widths top-down; calculate heights bottom-up
- [ ] `layout_block_children(parent_box, viewport_w)`: iterate `dom_node` children; for BLOCK children: call `layout_block()`; for INLINE children: accumulate into inline runs; flush runs into lines
- [ ] `layout_inline_run(run[], count, x, y, line_w, line_h)`: pack inline boxes left-to-right; on overflow: wrap to next line
- [ ] `layout_paint(box_root, win, scroll_offset, viewport_h)`: DFS paint all boxes in `[scroll_offset, scroll_offset+viewport_h]` clip region; background-color fill (from computed style, §6); text paint via `font_draw_string`; border paint via `gfx_rect_outline`
- [ ] `layout_free(root_box)`: free all box nodes (use `kmalloc`/`kfree` -- each box ≤ 4 KB)
- [ ] Scrollbar: compute `thumb_h = viewport_h² / layout_height`; `thumb_y = scroll_offset * viewport_h / layout_height`; draw in right 12 px column
- [ ] Replace §1 text renderer with `layout_build` + `layout_paint` for HTML pages
- [ ] Commit: `"apps/browser: block/inline layout engine -- block flow, inline word wrap, scroll, box model"`

## 5. Image Loading in Browser `[Sonnet]`

`<img src>` triggers `http_get()` for the URL, pipes bytes through `image_load_mem()`, renders the decoded image at the intrinsic or specified dimensions. Alt text fallback if load fails.

**Files:** `src/apps/browser/browser.c` (extend), `src/apps/browser/layout.c` (extend)

> [!NOTE]
> `<img>` node detected during DOM walk: `dom_find_attr(node, "src")` → URL. Relative URL resolution: same logic as link de-relativization in §1. `http_get(img_url, img_buf, max_img_size)` where `max_img_size = 8 * 1024 * 1024` (8 MB cap); use `pmm_alloc_contiguous()` for the fetch buffer. After fetch: `image_load_mem(&img, img_buf, img_len)`. If `width`/`height` attributes present: scale image using nearest-neighbour resize (simple `gfx_blit_scaled()`); else: use `img.width × img.height`. Render: in `layout_paint()` when box `display == BLOCK && dom_node->tag == "img"`: call `gfx_blit(win, img.pixels, box_x, box_y, img.width, img.height)`. Alt text fallback: if `http_get` returns -errno or `image_load_mem` returns -1: draw `[img: alt_text]` as a text box.

- [ ] `browser_load_image(img_url, base_url, &img_out)` → 0 or -errno: resolve relative URL; `pmm_alloc_contiguous(8 MiB)` for fetch buffer; `http_get` or `https_get`; `image_load_mem`; free fetch buffer
- [ ] Cache decoded images by URL: `img_cache[16] { char url[2048]; image_t img; }` -- check cache before re-fetching; evict LRU on overflow
- [ ] In `layout_build()`: for `<img>` nodes: call `browser_load_image()`; store `image_t *` in `layout_box_t` extra data; set box `w`/`h` from img dimensions (clamped to viewport width)
- [ ] In `layout_paint()`: for img boxes: `gfx_blit_scaled(win->fb, img, box_x, box_y, box_w, box_h)` (or `gfx_blit` if no scaling needed)
- [ ] Alt text fallback box: `display = INLINE`; text = `"[img: <alt>]"` in italic with gray color
- [ ] Lazy loading: kick off image fetches after initial text layout so the page is readable first; images trigger a re-layout on completion
- [ ] Commit: `"apps/browser: image loading -- http_get → image_load_mem, inline blit, alt fallback, 16-entry cache"`

## 6. CSS Parser + Style Engine `[Opus]`

CSS tokenizer → rule list (selector, property-value pairs). Cascade (specificity + `!important`). Computed style per DOM node: color, background-color, font-size/weight/style, margin/padding/border, display (block/inline/none), width/height.

**Files:** `src/apps/browser/css.c` (new), `include/apps/browser/css.h` (new)

> [!NOTE]
> This is `[Opus]` -- CSS specificity cascade is a complex algorithm. **CSS tokenizer states**: TEXT, COMMENT, SELECTOR, PROPERTY, VALUE, STRING_SQ, STRING_DQ. Build a flat rule array `css_rule_t[512]`. **Selectors**: element (`p`, `h1`), class (`.foo`), ID (`#bar`), descendant (space), attribute (`[href]`). **Specificity** (RFC 2009): `(ID_count * 100) + (class_count * 10) + (element_count * 1)`; `!important` → specificity 10000. **Cascade**: for each DOM node: collect all matching rules; sort by specificity; apply properties in specificity order; inherit inheritable properties (color, font-size, font-family) from parent if not set. **Computed style**: `css_computed_t { uint32_t color, bg_color; int font_size_px; uint8_t font_bold, font_italic; int margin[4]; int padding[4]; int border_w[4]; uint32_t border_color[4]; int width, height; uint8_t display; }`. **Length parsing**: `px` → integer; `em` → multiply by parent font-size; `%` → percentage of containing block width.

- [ ] `css_rule_t { char selector[256]; css_decl_t decls[32]; int decl_count; int specificity; }` and `css_decl_t { char property[64]; char value[256]; uint8_t important; }`
- [ ] `css_parse(src, len, rules, max_rules)` → rule count: tokenize CSS text; for each rule: parse selector + `{...}` block; parse declarations (`property: value [!important]`)
- [ ] `css_selector_matches(selector, node, dom_root)` → bool: handle element, `.class`, `#id`, descendant-space, `[attr]` selectors
- [ ] `css_compute_style(node, parent_style, rules, rule_count, &out_style)`: collect matching rules; sort by specificity; apply; inherit from parent_style
- [ ] `css_parse_color(str)` → uint32_t ARGB: handle `#RRGGBB`, `#RGB`, `rgb(R,G,B)`, and 16 named colors (black, white, red, green, blue, gray, etc.)
- [ ] `css_parse_length(str, parent_px, font_size_px)` → int px: handle `px`, `em`, `%`, `auto` (return -1 for auto)
- [ ] Integrate into `layout_build()`: call `css_compute_style()` per node; override default block/inline display with CSS `display` property; apply `width`/`height` overrides; store in `layout_box_t.style`
- [ ] Extract `<style>` block text from DOM during parse; call `css_parse()` on it; also parse `style=""` inline attributes
- [ ] Commit: `"apps/browser: CSS parser + cascade -- specificity, element/class/id selectors, box model props"`

## 7. JavaScript Stub `[Sonnet]`

Detect `<script>` blocks in the DOM. Display `[JavaScript disabled]` inline placeholder where scripts would have run. Expose `window.location`, `document.title`, `document.write` as no-ops to prevent hard crashes if JS stubs are exercised. Note QuickJS as the future full-engine upgrade path.

**Files:** `src/apps/browser/js_stub.c` (new), `include/apps/browser/js_stub.h` (new)

> [!NOTE]
> The `<script>` tag is captured verbatim in the DOM tree during tokenization (§2) -- its `text` child node holds the script content. The stub does not execute any JavaScript; it renders a placeholder box. `document.write()` no-op: if content was written by script before the stub was introduced, static rendering shows the page as-is. Future upgrade: QuickJS (MIT, ~35K lines, ES2020) is a freestanding C library that compiles under `-ffreestanding` with `malloc/free` redirected to `kmalloc/kfree`, similar to the Mbed TLS port (TODO-03 §3). A `js_eval(script_text, dom_root)` function will be the hook point; the stub installs a `js_eval` that always returns immediately, making the QuickJS port a drop-in replacement.

- [ ] `js_eval(script_text, dom_root)` → 0: stub; always return 0 immediately; log `[JS] script skipped (%zu bytes)` in debug mode
- [ ] During `layout_build()` for `<script>` node: call `js_eval()`; insert a `layout_box_t` with placeholder text `[JavaScript disabled]` in monospace gray
- [ ] `window_t { char location[2048]; }` static instance; `window_get_location()` → current tab URL; `window_set_title(str)` → update tab title
- [ ] `document_t` static instance: `document_get_title()` returns `<title>` text node content from DOM; `document_write(str)` → no-op (log warn)
- [ ] `js_stub_init()`: log `[Browser] JavaScript: stub mode (QuickJS upgrade path available)`
- [ ] Commit: `"apps/browser: JS stub -- script placeholder, window.location/document.title no-ops, QuickJS hook point"`

## 8. Bookmarks Manager `[Sonnet]`

`C:\Users\Default\AppData\Browser\bookmarks.json` (title + URL + timestamp). Ctrl+D add/edit. Bookmarks dropdown menu. HTML import (`NETSCAPE BOOKMARK FILE`) and export.

**Files:** `src/apps/browser/bookmarks.c` (new), `include/apps/browser/bookmarks.h` (new)

> [!NOTE]
> JSON format: `[{"title":"…","url":"…","ts":UNIX_TS}, …]` -- a flat JSON array. Use a simple hand-written JSON serializer/deserializer (no external library): for output: `sprintf`-style buffer building; for input: state-machine parser scanning for `"title":`/`"url":`/`"ts":` key patterns. Max 512 bookmarks. `Ctrl+D` shortcut: if current URL already bookmarked: open "Edit bookmark" dialog (title field + URL field + Save/Delete buttons); else: open "Add bookmark" dialog. Bookmarks dropdown: rendered as a popup menu below the "Bookmarks" toolbar button; click → navigate. HTML export (`NETSCAPE BOOKMARK FILE` format): `<!DOCTYPE NETSCAPE-Bookmark-file-1>` header; one `<DT><A HREF="url" ADD_DATE="ts">title</A>` per bookmark. HTML import: scan for `<A HREF=` tags and extract href/title pairs.

- [ ] `bookmark_t { char title[256]; char url[2048]; uint32_t timestamp; }` + `bookmarks[512]` + `bookmark_count`
- [ ] `bookmarks_load(path)` → count: `vfs_open` + read JSON; parse key-value pairs; populate array
- [ ] `bookmarks_save(path)`: `vfs_open` write; serialize all bookmarks to JSON array
- [ ] `bookmark_add(url, title)` / `bookmark_remove(index)` / `bookmark_find(url)` → index or -1
- [ ] `bookmarks_draw_menu(browser, win)`: popup rectangle below Bookmarks button; one row per bookmark; click → navigate + close menu; scrollable if > 20 entries
- [ ] `Ctrl+D` handler: `bookmark_find(current_url)` → if found: open edit dialog; else: open add dialog with pre-filled title from DOM `<title>` node
- [ ] `bookmarks_export_html(path)`: write NETSCAPE format to file
- [ ] `bookmarks_import_html(path)`: `vfs_open`; scan `<A HREF=` + `>title<`; call `bookmark_add()` for each
- [ ] Commit: `"apps/browser: bookmarks -- JSON save/load, Ctrl+D add/edit, dropdown menu, HTML import/export"`

## 9. Download Manager `[Sonnet]`

`<a href>` right-click → "Save As" queues a download. Progress list window (filename, size, %, KB/s, pause/cancel). Resume on reconnect via HTTP `Range:` header. Saves to `C:\Users\Default\Downloads\`.

**Files:** `src/apps/browser/downloads.c` (new), `include/apps/browser/downloads.h` (new)

> [!NOTE]
> HTTP Range resume: `http_get_range(url, start_byte, buf, max)` sends `Range: bytes=N-` header; if server responds 206 Partial Content: resume writing from `start_byte` offset in the file. Implement `http_get_range()` as an extension of `http_get()` in `src/kernel/net/http.c` (add a `range_start` parameter; 0 = full fetch). Download state machine: `DL_QUEUED`, `DL_ACTIVE`, `DL_PAUSED`, `DL_DONE`, `DL_FAILED`. Progress calculation: `pct = (bytes_received * 100) / content_length` (0 if Content-Length unknown). KB/s: `delta_bytes / (now_ms - last_measure_ms) × 1000 / 1024` updated every 500 ms. Max 8 concurrent downloads (additional queued).

- [ ] `download_t { char url[2048]; char filename[256]; uint64_t total_bytes; uint64_t received_bytes; uint8_t state; uint64_t speed_kbps; uint32_t start_time_ms; }` + `downloads[8]`
- [ ] `download_start(url, filename)`: allocate `download_t`; spawn background thread `download_thread(dl)`
- [ ] `download_thread(dl)`: `http_get_range(url, 0, buf, max)`; write 4 KiB chunks to VFS file in `C:\Users\Default\Downloads\<filename>`; update `received_bytes` + `speed_kbps`; on network error and `dl->state == DL_ACTIVE`: retry with `Range: bytes=received_bytes-`
- [ ] `downloads_window_draw(win)`: table of all downloads with columns: filename, size, progress bar, KB/s, Pause/Resume/Cancel buttons
- [ ] Pause: set `dl->state = DL_PAUSED`; thread sees state and suspends after current chunk; Resume: set `DL_ACTIVE`; thread calls `http_get_range(url, received_bytes, ...)`
- [ ] Right-click `<a href>` in layout → context menu → "Save As": prompt for filename; call `download_start(href_url, filename)`
- [ ] `download_manager_open()`: `wm_create_window("Downloads", ...)` showing `downloads[]` table; auto-refreshes every 500 ms
- [ ] Commit: `"apps/browser: download manager -- Range resume, progress window, pause/cancel, Downloads save"`

## 10. Privacy + Security `[Opus]`

Block mixed content (HTTP resource on HTTPS page). TLS padlock icon in address bar. Cookie jar (session-only default, persistent opt-in). `Cache-Control` + `ETag` HTTP caching.

**Files:** `src/apps/browser/security.c` (new), `src/apps/browser/browser.c` (extend), `src/kernel/net/http.c` (extend)

> [!NOTE]
> This is `[Opus]` -- mixed-content blocking is security-critical: loading HTTP resources on an HTTPS page leaks information and undermines TLS. **Mixed content**: when rendering an HTTPS page (tab's `is_https == 1`), any resource fetch with `http://` scheme is blocked; display `[blocked: mixed content]` placeholder; set `tab->mixed_content_blocked = 1` → show ⚠ in address bar instead of padlock. **Padlock icon**: draw 🔒 glyph (green) left of address bar text when `tab->is_https && tab->cert_ok`; draw ⚠ (yellow) if cert warning or mixed content; draw nothing for HTTP. **Cookie jar**: `cookie_t { char domain[256]; char name[64]; char value[512]; uint8_t secure; uint8_t http_only; uint64_t expires_ms; }` stored in `cookies[256]`; session cookies expire when browser closes; persistent cookies stored in `HKCU\Software\ImpossibleBrowser\Cookies` as Registry DWORD/SZ values. `Cookie:` header sent in `http_send_request()` if matching domain cookie exists. **HTTP caching**: `cache_entry_t { char url[2048]; char etag[256]; uint64_t max_age_ms; uint64_t cached_at_ms; char *body; size_t body_len; }` pool of 32 entries; on fetch: if cached and `max_age` not expired → return cached body; else add `If-None-Match: <etag>` header; on 304 Not Modified: return cached; on 200: update cache.

- [ ] `mixed_content_block(tab, resource_url)` → blocked: if `tab->is_https && strncmp(resource_url, "http://", 7) == 0`: log warn; return 1; else: return 0
- [ ] Wire `mixed_content_block()` into `browser_load_image()` and all resource fetch paths
- [ ] Address bar security indicator: after `tab->cert_ok` is set by `https_get()` (via `tls_conn_t` result): draw padlock; if `tab->mixed_content_blocked`: draw ⚠
- [ ] `cookie_t` + `cookies[256]`; `cookie_match(domain, cookies, matches_out)` → count; `cookie_set(name, value, domain, attrs)` parses `Set-Cookie:` header response
- [ ] `cookie_header_build(domain, buf, max)` → len: concatenate matching `name=value` pairs with `; ` separator
- [ ] Wire into `http_send_request()`: call `cookie_header_build(host, ...)` and append `Cookie:` header if non-empty; parse `Set-Cookie:` in `http_parse_headers()`
- [ ] `http_cache_t cache[32]`; `http_cache_lookup(url, &body, &len)` → hit/miss; `http_cache_insert(url, etag, max_age_ms, body, len)`
- [ ] Wire into `http_get()`: before fetch check cache; add `If-None-Match` if ETag known; on 304: return cached body; on 200 with `ETag`/`Cache-Control: max-age=N`: `http_cache_insert()`
- [ ] Commit: `"apps/browser: privacy+security -- mixed-content block, TLS padlock, cookie jar, ETag cache"`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | Text-only HTML browser                   | ✅ Edge + IE legacy; `lynx`               | ✅ `w3m`, `lynx`, `elinks` text browsers  | ⬜ §1 -- kernel-native `http_get` → tag-strip → |
| 💎   | HTML parser + DOM tree                   | ✅ Edge (Chromium Blink); IE (Trident);   | ✅ Firefox (Gecko), Chromium (Blink), WebKit; | ⬜ §2 -- custom tokenizer; void-element auto-close; 20 |
| 💎   | Tab management -- 16 tabs, tab bar, Ctrl+T/W/Tab | ✅ Edge multi-tab; tab groups, vertical   | ✅ All major browsers support tabs        | ⬜ §3 -- `browser_tab_t` per-tab DOM+history; tab bar |
| 💎   | Block/inline layout engine               | ✅ Blink/Gecko full CSS layout including  | ✅ Same (Blink/Gecko/WebKit)              | ⬜ §4 -- block + inline flow; word        |
| 💎   | Image loading                            | ✅ Full image support (WebP, AVIF,        | ✅ Same format support                    | ⬜ §5 -- `image_load_mem`; 16-entry decoded cache; alt |
| 💎   | CSS cascade                              | ✅ Full CSS3 in Blink/Gecko; DevTools     | ✅ Same                                   | ⬜ §6 -- specificity sort, `!important`, em/%/px lengths, |
| 💎   | JavaScript stub → QuickJS upgrade path   | ✅ V8 (Edge); SpiderMonkey (Firefox); full | ✅ Same                                   | ⬜ §7 -- stub with `[JavaScript disabled]` placeholder |
| 💎   | Bookmarks                                | ✅ Edge Favorites; IE bookmark import;    | ✅ All major browsers support bookmark    | ⬜ §8 -- `bookmarks.json`; NETSCAPE HTML import/export; 512 |
| 💎   | Download manager                         | ✅ Edge download shelf; resume via        | ✅ Firefox/Chromium download manager; resume support | ⬜ §9 -- `http_get_range()`; 8 concurrent; pause/resume; `Downloads\` |
| 💎   | Privacy + security                       | ✅ Edge: HTTPS indicator, mixed content   | ✅ All major browsers implement these     | ⬜ §10 -- padlock/⚠ in address bar; cookie |

> **After §1–§10:** Impossible OS has a kernel-native web browser with full HTML/CSS rendering, tabs, images, bookmarks, downloads, and TLS security -- all built on the in-kernel HTTP/HTTPS stack with zero external runtime libraries. The JavaScript stub with a clear `js_eval()` upgrade path means QuickJS can be dropped in as a freestanding port (analogous to Mbed TLS in TODO-03 §3) whenever needed, instantly upgrading the browser to ES2020 support.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Text-only: `browser_open()` → navigate to `http://example.com/` → readable text displayed; `<a href>` links numbered and clickable; back/forward works
- [ ] HTML: navigate to a real HTML page; `<h1>` rendered larger; `<b>` bolder; `<ul>/<li>` with bullet points; `<a href>` underlined and clickable
- [ ] Tabs: Ctrl+T opens second tab at `about:blank`; navigate independently; Ctrl+W closes; Ctrl+Tab cycles; 16th tab: 17th Ctrl+T is ignored
- [ ] Layout: `<p>` elements separated by vertical spacing; long lines wrap at window width; page taller than window is scrollable; overflow clipped
- [ ] Images: `<img src="http://...logo.png">` fetched and rendered inline at correct size; broken image shows `[img: alt text]`
- [ ] CSS: `<p style="color: red">` text appears red; `<style> h1 { font-size: 24px; } </style>` increases heading size; class selector `.highlight { background-color: yellow; }` applied
- [ ] JS stub: page with `<script>` renders `[JavaScript disabled]` in place of script output; no crash
- [ ] Bookmarks: Ctrl+D on `http://example.com/` → dialog appears with pre-filled title; Save → entry in Bookmarks dropdown; navigate to it from dropdown; export to HTML file; re-import
- [ ] Downloads: right-click on a `.zip` link → "Save As" → progress window shows KB/s + %; pause mid-download → resume from same byte offset (check with HTTP 206 response in QEMU)
- [ ] Security: `https://example.com/` → green padlock in address bar; HTTPS page with `<img src="http://...">` → `[blocked: mixed content]` + ⚠ icon; cookie set by server persists across navigations within session
- [ ] Commit: `"apps/browser: complete web browser -- HTML/CSS/tabs/images/bookmarks/downloads/security"`
