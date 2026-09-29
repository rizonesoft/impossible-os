---
schema_version: 1
id: web-browser
domain: 11-apps
status: active
title: "TODO-01 -- Web Browser"
---

# TODO-01 -- Web Browser

**Domain:** `11-apps`
**Goal:** Deliver the flagship browser app for Impossible OS -- from a ~500-line text-only HTML fetcher to a full tab-based browser with HTML/CSS rendering, browser chrome, and settings -- demonstrating the complete networking stack end-to-end.

> [!IMPORTANT]
> **Depends on:** `07-networking/TODO-03-http-tls.md` -- `http_get(url, buf, max)`, `https_get()`, `http_post()`, TLS 1.2/1.3 (Mbed TLS) must be working. `ttf_draw_string()`/`ttf_measure_width()` from `include/font_mgr.h`. `image_load_mem()` from `include/kernel/image.h`. `CTRL_TABSTRIP` from `08-graphics-ui/TODO-05`.
> **Alternative path:** §6 tracks NetSurf / Dillo as a faster-path drop-in; evaluate before starting §2 (HTML parser). A port may save thousands of lines.

---

## Important Notes

- `http_get(url, buf, max)` and `https_get()` exist in `07-networking/TODO-03` -- these are the only network primitives the browser calls directly; no raw socket work needed here.
- `ttf_draw_string()`, `ttf_draw_char()`, and `ttf_measure_width()` exist in `include/font_mgr.h` -- the layout engine uses `ttf_measure_width` for per-word inline flow, no custom glyph shaping needed.
- `image_load_mem(img, data, size)` in `include/kernel/image.h` decodes JPEG/PNG from a memory buffer -- `<img src>` fetches via `http_get` into a buffer then calls this.
- `CTRL_TABSTRIP` (max 16 tabs) is defined in `08-graphics-ui/TODO-05`; each tab owns an independent `browser_tab_t` struct with its own URL, DOM, and scroll position.
- The CSS parser (§4) and JavaScript engine (§5) are stretch goals -- do not let them block §1–3 or §7–8.
- NetSurf/Dillo (§6) must be evaluated before investing in a custom HTML parser -- porting either saves ~10–15 K lines of bespoke work.
- Browser settings are stored under `HKCU\Software\Impossible\Browser\` -- the same registry used by other OS apps.
- No cookie jar in Phase 1; add it in §8 (settings) alongside history/cache/bookmark persistence.

---

## Inputs

| Path                                          | Purpose                                                                   |
| --------------------------------------------- | ------------------------------------------------------------------------- |
| `07-networking/TODO-03-http-tls.md`           | `http_get`, `https_get`, `http_post`, redirect + chunked TE               |
| `include/font_mgr.h`                          | `ttf_draw_string`, `ttf_measure_width`, `ttf_draw_char`                   |
| `include/kernel/image.h`                      | `image_load_mem()` -- decode JPEG/PNG from HTTP response                  |
| `include/desktop/wm.h`                        | `wm_create_window`, `wm_destroy_window`                                   |
| `include/desktop/controls.h`                  | `CTRL_BUTTON`, `CTRL_LABEL`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR`             |
| `08-graphics-ui/TODO-05`                      | `CTRL_TABSTRIP` (16 tabs, accent underline)                               |
| `include/registry.h`                          | `RegGetString()` / `RegSetString()` -- homepage, search engine, bookmarks |
| → XREF: `07-networking/TODO-03`               | HTTP/HTTPS client -- mandatory prerequisite                               |
| → XREF: `08-graphics-ui/TODO-05`              | `CTRL_TABSTRIP` -- browser tabs widget                                    |
| → XREF: `10-platform-services/TODO-08 §10–11` | IxUI `user32`/`gdi32` for window + rendering in user-mode                 |

---

## Outcome

- Phase 1: Text-only browser navigates HTTP/HTTPS URLs, strips HTML, displays plain text + numbered link list, back/forward history, address bar -- ~500 lines, proven in QEMU.
- Phase 2: Full HTML renderer -- tokenizer → DOM tree → block/inline layout engine with 14 tag types, images, tables, word-wrap.
- Phase 3: CSS box model, selectors, cascade.
- Phase 4 (long-term): JavaScript via QuickJS or Duktape.
- Browser chrome: multi-tab (`CTRL_TABSTRIP`), toolbar (←/→/↺/Home + address bar + 🔒), bookmarks bar, right-click context menu.
- Browser settings: homepage, search engine, download path, privacy controls.

---

## Implementation Order

| #   | Section                                                 | Tag        | Dep                                    | Mark |
| --- | ------------------------------------------------------- | ---------- | -------------------------------------- | ---- |
| 1   | Text-only browser (Phase 1, ~500 lines)                 | `[Sonnet]` | 07-networking/TODO-03                  | 💎   |
| 2   | HTML tokenizer + DOM tree                               | `[Opus]`   | §1                                     | 💎   |
| 3   | Layout engine (block + inline flow)                     | `[Opus]`   | §2                                     | 💎   |
| 4   | CSS parser + cascade (stretch)                          | `[Opus]`   | §3                                     | 💎   |
| 5   | JavaScript engine -- QuickJS/Duktape (long-term)        | `[Opus]`   | §3                                     | 💎   |
| 6   | Alternative: NetSurf / Dillo port evaluation            | `[Sonnet]` | §1                                     | 💎   |
| 7   | Browser chrome (tabs, toolbar, bookmarks, context menu) | `[Sonnet]` | §1 or §6, XREF: 08-graphics-ui/TODO-05 | ⭐   |
| 8   | Browser settings + history + cookie jar                 | `[Sonnet]` | §7                                     | 💎   |

---

## 1. Text-Only Browser (Phase 1) `[Sonnet]`

~500 lines. Proves the full network → render pipeline end-to-end.

- [ ] Settle engine vs app ownership with `07-networking/TODO-07-web-browser.md` (item: "Settle engine vs app ownership") before coding: §1-§5 here restate its engine
  - Names differ: header `include/apps/browser.h` vs `include/apps/browser/browser.h`, `dom_parse`/`dom_free` vs `html_parse`/`dom_free_tree`, a 16- vs 32-entry history.
  - Storage differs: `HKCU\Software\Impossible\Browser\` vs `HKCU\Software\ImpossibleBrowser`, and bookmarks in the Registry here vs `bookmarks.json` there.
  - Keep one engine spec, XREF the other, and keep §7 chrome here (TODO-07 already marks its tab bar drawing superseded by §7).
- [ ] Create `src/apps/browser/browser.c` + `include/apps/browser.h`
- [ ] `browser_fetch(url, buf, max)` → call `https_get(url, buf, max)` (falls back to `http_get` if port 80); return HTTP status code; log to serial on error
- [ ] HTML stripper: char-by-char state machine: `STATE_TEXT` / `STATE_IN_TAG` / `STATE_IN_COMMENT`; accumulate non-tag characters; collapse runs of whitespace (space, `\t`, `\n`, `\r`) to single space; strip `&lt;`/`&gt;`/`&amp;`/`&nbsp;` entities
- [ ] Link extractor: during tag state, detect `<a href="...">` attributes; push href + visible text into `browser_link_t link_list[64]`; display links as numbered list below body text with accent color
- [ ] Page renderer: `browser_render(surface, text, links, scroll_y)`:
  - `ttf_draw_string` line by line; line height = font size × 1.4
  - Wrap lines at window width using `ttf_measure_width` to detect overflow
  - Vertical clip: skip lines above `scroll_y`; stop at surface height
  - Links: render as `[N] link_text` in `theme_get()->accent` color
- [ ] Address bar: `CTRL_TEXTBOX` at top; Enter key → `browser_navigate(url)`
- [ ] Scroll: mouse wheel → `scroll_y += 40`; `CTRL_SCROLLBAR` on right edge
- [ ] Back/forward: `url_history[16]` ring; `←` / `→` buttons
- [ ] Status bar: `CTRL_LABEL` at bottom: `"Loading…"` / `"200 OK"` / `"Error: -errno"`
- [ ] HTTPS: 🔒 padlock icon (monochrome Fluent System Icons glyph `lock_closed_20`, vendored, tinted `text_primary`) in status bar when URL starts with `https://`
- [ ] Commit: `"apps: text-only web browser (~500 lines)"`

---

## 2. HTML Tokenizer + DOM Tree `[Opus]`

Tokenize raw HTML into a `dom_node` tree. This is the foundation for both §3 (layout) and §4 (CSS).

- [ ] Tokenizer -- char-by-char state machine (`src/apps/browser/html_tokenizer.c`):
  - States: `DATA`, `TAG_OPEN`, `TAG_NAME`, `ATTR_NAME`, `ATTR_VALUE_UNQUOTED`, `ATTR_VALUE_SINGLE`, `ATTR_VALUE_DOUBLE`, `SELF_CLOSE`, `COMMENT_START`, `COMMENT`
  - Emit tokens: `TOKEN_DOCTYPE`, `TOKEN_START_TAG(name, attrs[])`, `TOKEN_END_TAG(name)`, `TOKEN_TEXT(data)`, `TOKEN_COMMENT`
  - HTML entity decoder: `&lt;`→`<`, `&gt;`→`>`, `&amp;`→`&`, `&nbsp;`→` `, `&quot;`→`"`, numeric `&#N;` and `&#xN;`
- [ ] DOM tree builder (`src/apps/browser/dom.c`):
  - `struct dom_node { uint8_t type; /* ELEMENT / TEXT / COMMENT */ char tag[32]; dom_attr_t attrs[16]; struct dom_node *parent, *first_child, *next_sibling; char *text; }` -- all allocations via `kmalloc`/`kfree`
  - `dom_parse(html, len)` → run tokenizer; push/pop element stack; build tree; return root `<html>` node
  - `dom_find(node, tag)` → first descendant with matching tag name
  - `dom_get_attr(node, name)` → attribute value string or NULL
  - `dom_free(root)` → recursive `kfree` of entire tree
- [ ] Supported tag set: `html`, `head`, `title`, `body`, `h1`–`h6`, `p`, `div`, `span`, `br`, `b`, `strong`, `i`, `em`, `u`, `a`, `ul`, `ol`, `li`, `img`, `table`, `thead`, `tbody`, `tr`, `td`, `th`, `hr`, `pre`, `code`, `script` (content discarded), `style` (content passed to CSS parser in §4)
- [ ] `<title>` text → update window title bar via `wm_set_title()` / `SetWindowTextA()`
- [ ] Commit: `"apps: HTML tokenizer + DOM tree"`

---

## 3. Layout Engine (Block + Inline Flow) `[Opus]`

Converts the DOM tree into a rendered page on a `gfx_surface_t`. Core of a real browser.

- [ ] `struct layout_box { int32_t x, y, w, h; dom_node_t *node; struct layout_box *children; }` -- one box per block/inline element
- [ ] **Block formatting context** (top-to-bottom, each block on new line):
  - Block elements: `h1`–`h6`, `p`, `div`, `ul`, `ol`, `li`, `table`, `hr`, `pre`
  - Each advances `cursor_y` by its height + margin; margin: `p` = 12 px top+bottom; `h1` = 20 px; `h2`–`h6` scaled proportionally
- [ ] **Inline formatting context** (left-to-right, word-wrap):
  - Inline elements: `span`, `b`, `strong`, `i`, `em`, `u`, `a`, `code`, text nodes
  - Word-wrap: accumulate words with `ttf_measure_width`; if `cursor_x + word_w > window_w - margin`: new line
  - Line height: `font_size * 1.4` (use `ttf_font_t` size field)
- [ ] **Font sizes by tag** (relative to base 14 px): `h1`=2.0×, `h2`=1.75×, `h3`=1.5×, `h4`=1.25×, `h5`=1.1×, `h6`=1.0×; `b`/`strong`=bold variant; `i`/`em`=italic variant; `pre`/`code`=monospace font
- [ ] **Links**: `<a href>` → underline (1 px `gfx_draw_line` at baseline); accent color; `link_map[]` stores {x, y, w, h, href} for click hit-testing
- [ ] **Images** `<img src>`:
  - `dom_get_attr(node, "src")` → resolve relative URL against page base; `https_get(src_url, img_buf, MAX_IMG)` → `image_load_mem(&img, img_buf, len)` → `gfx_blit(surface, img.pixels, x, y, img.w, img.h)`
  - Respect `width`/`height` attributes if present; otherwise use decoded dimensions
- [ ] **Tables**: `<table>` → enumerate `<tr>` rows; for each row enumerate `<td>`/`<th>` cells; distribute column widths equally across `window_w`; render each cell as a nested block context
- [ ] **Lists**: `<ul>` → bullet `•` at list indent 24 px; `<ol>` → decimal counter; nested lists add 24 px per level
- [ ] **Horizontal rule**: `<hr>` → `gfx_draw_line` full width, 1 px, `theme_get()->border`
- [ ] **Vertical scroll**: total `page_height` accumulated during layout; clamp `scroll_y` to `[0, page_height - window_h]`; `CTRL_SCROLLBAR` thumb position = `scroll_y / page_height * scrollbar_h`
- [ ] `layout_render(surface, root, scroll_y, window_w)` → entry point; recompute layout if window resized
- [ ] Commit: `"apps: HTML layout engine (block + inline flow, word-wrap, images, tables)"`

---

## 4. CSS Parser + Cascade (Stretch) `[Opus]`

Adds styling to the layout engine. Stretch goal -- only start after §3 is complete and stable.

- [ ] CSS tokenizer: extract `selector { property: value; ... }` rules from `<style>` blocks and inline `style=""` attributes
- [ ] Selector matching: element name (`p`), class (`.foo` matches `class="foo"`), ID (`#bar` matches `id="bar"`); no compound selectors initially
- [ ] **Box model** properties applied to `layout_box`:
  - `margin` / `margin-top/right/bottom/left`, `padding` (same sides)
  - `width` / `height` (px or `auto`)
  - `border: Npx solid color` → draw border rect around box
- [ ] **Text** properties: `color` (hex, named: `red`, `green`, `blue`, `black`, `white`, `gray`), `background-color`, `font-size` (px), `font-family` (match against loaded TTF faces), `text-align: left/center/right`
- [ ] **Display** property: `display: block` (block context), `display: inline` (inline context), `display: none` (skip layout + render)
- [ ] **Cascade** order: inline `style=""` attr > `<style>` block > default stylesheet (per-tag defaults defined in a built-in table)
- [ ] Computed style struct `css_style_t` attached to each `dom_node` during layout
- [ ] Commit: `"apps: CSS parser + cascade (box model, color, font, display)"`

---

## 5. JavaScript Engine (Long-Term Stretch) `[Opus]`

> Long-term. Do not start until §3 is complete and §6 (NetSurf/Dillo) evaluation is done -- a port may include a JS engine.

- [ ] Evaluate **QuickJS** (MIT, ~35 K lines, ES2020) vs **Duktape** (MIT, ~60 K lines, ES5.1): QuickJS is preferred for ES2020 compliance; Duktape for smaller footprint
- [ ] Port selected engine: replace `malloc`/`free` → `kmalloc`/`kfree`; replace POSIX I/O; replace `printf` → `serial_write`; compile under `-ffreestanding -nostdinc`
- [ ] DOM bindings (minimum viable set):
  - `document.getElementById(id)` → find `dom_node` by `id` attribute
  - `document.querySelector(selector)` → basic selector match
  - `element.innerHTML = str` → re-parse subtree + re-layout
  - `element.textContent` / `element.style.color = "red"` → mutate node
  - `element.addEventListener("click", fn)` → add to `link_map` click callback
  - `window.location.href = url` → `browser_navigate(url)`
- [ ] Event loop: after page load, `setTimeout`/`setInterval` via PIT-driven timer callbacks
- [ ] `<script src="...">` → `https_get(src_url, ...)` → execute via JS engine
- [ ] Commit: `"apps: JavaScript engine (QuickJS/Duktape + DOM bindings)"`

---

## 6. Alternative: NetSurf / Dillo Port Evaluation `[Sonnet]`

Evaluate before investing in §2–§5. A port may deliver a full browser faster than building from scratch.

- [ ] Licence gate first: read the NetSurf and Dillo LICENSE files; a GPL-2.0-only upstream cannot be combined with this GPL-3.0-only project (CLAUDE.md vendor-first rule)
- [ ] **NetSurf** (GPL, ~200 K lines): has a custom layout engine (Hubbub HTML parser + LibCSS), Amiga/RISC OS/framebuffer backends already exist -- the framebuffer backend maps closely to Impossible OS `gfx_surface_t`
  - Prerequisites already met: TCP, DNS, TLS, framebuffer, TTF fonts
  - Evaluate: `#include` audit (POSIX deps: `stdio.h`, `string.h`, `stdlib.h` -- all need kernel substitutes)
  - Estimate port effort: ~2–4 weeks of include + libc replacement
- [ ] **Dillo** (GPL, ~30 K lines): minimal HTML/CSS, FLTK UI (must be replaced with IxUI); simpler but fewer features
- [ ] Decision gate: if NetSurf port is feasible → pursue §6 as primary path and reduce §2–§5 scope; if not → continue custom path
- [ ] Commit (if porting): `"apps: NetSurf/Dillo port to Impossible OS framebuffer"`

---

## 7. Browser Chrome `[Sonnet]`

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Owner of:** the work planned in `07-networking/TODO-07 §3 (tab bar drawing)`, which is superseded there so there is one implementation.

Multi-tab UI, toolbar, bookmarks bar, and context menu. Built on top of §1 (text browser) or §6 (port).

- [ ] **Tab bar**: `CTRL_TABSTRIP` (from `08-graphics-ui/TODO-05`, max 16 tabs); each tab has: `{ char title[64]; char url[512]; dom_node_t *dom_root; int32_t scroll_y; }` -- switching tabs restores DOM + scroll position without re-fetching
- [ ] New tab button `[+]`: opens `browser_new_tab()` → blank page; `browser_navigate(url)` fetches into active tab
- [ ] Close tab `×` on each tab header; last tab close → exit app or show new-tab page
- [ ] **Command bar** (below the tab bar, `THEME_SIZE_COMMAND_BAR_HEIGHT` (48) per `docs/design/shell.md#app-window-layout`, subtle buttons with Fluent glyphs):
  - `[←]` back (disabled if history empty) → pop history stack
  - `[→]` forward (disabled if no forward entries)
  - `[↺]` refresh → re-fetch current URL; re-parse + re-layout
  - `[Home]` (Fluent home glyph) → navigate to `HKCU\Software\Impossible\Browser\HomePage`
  - Address bar: `CTRL_TEXTBOX` fills remaining toolbar width; shows current URL; Enter → `browser_navigate()`; focus → select all text
  - 🔒 padlock (`lock_closed_20` Fluent glyph, tinted `text_primary`) if current URL is `https://`; 🔓 (`lock_open_20`, tinted `text_secondary`) for HTTP; no colour alone; a certificate error adds the `status_critical` glyph and text per `docs/design/controls.md#status-colours`
- [ ] **Bookmarks bar** (optional strip below toolbar, height 32 px, hidden if empty):
  - Load from `HKCU\Software\Impossible\Browser\Bookmarks` (REG_MULTI_SZ: `title|url` per entry)
  - Each entry: `CTRL_BUTTON`-style clickable label; right-click → "Remove bookmark"
  - `[★]` button in toolbar → add current page to bookmarks; write to registry
- [ ] **Right-click context menu** (on page area):
  - "Open link in new tab" (if right-click on a link)
  - "Save image as…" (if right-click on image) → `GetSaveFileNameA()` → write image bytes to file
  - "View source" → new tab showing raw HTML in `<pre>`-style monospace scroll view
  - "Copy link address" → clipboard via `OpenClipboard`/`SetClipboardData`
- [ ] **New-tab page**: shows bookmarks grid + recently visited URLs (last 8, from history)
- [ ] Commit: `"apps: browser chrome (tabs, toolbar, bookmarks, context menu)"`

---

## 8. Browser Settings + History + Cookie Jar `[Sonnet]`

Settings page (`browser://settings`) and privacy controls. All state in `HKCU\Software\Impossible\Browser\`.

- [ ] **Registry keys**:
  - `HomePage` (REG_SZ, default `"about:blank"`)
  - `SearchEngine` (REG_SZ, default `"https://google.com/search?q=%s"`) -- address bar non-URL input → substitute `%s` + navigate
  - `BlockPopups` (REG_DWORD, default 1) -- block `window.open()` calls in JS
  - `ClearHistoryOnExit` (REG_DWORD, default 0)
  - `DownloadPath` (REG_SZ, default `"C:\\Users\\Default\\Downloads\\"`)
  - `Bookmarks` (REG_MULTI_SZ, `"title|url"` per entry)
  - `History` (REG_MULTI_SZ, `"url|timestamp"` per entry, max 100)
- [ ] **History**: on each successful navigation → prepend `url|GetTickCount()` to `History` key; truncate at 100 entries
- [ ] **Cookie jar** (stretch): `Cookies` (REG_MULTI_SZ, `"domain|name|value|expires"` per cookie); `Set-Cookie` response header → parse + store; `Cookie` request header → send matching cookies
- [ ] **Download manager**: on `<a href>` to non-HTML content type → prompt save location (`GetSaveFileNameA()`); stream `https_get()` response to file with progress bar in status bar
- [ ] **Settings page** (`browser://settings` special URL):
  - IxUI window with sections: General (homepage, search engine, download path), Privacy (clear history, clear cookies, clear cache, `ClearHistoryOnExit` checkbox), Appearance (blank for now)
  - `[Clear Browsing Data]` → delete `History` + `Cookies` registry entries + page cache
- [ ] **Find in page**: Ctrl+F → toolbar input; highlight matching text on page (draw rect behind matching words)
- [ ] Commit: `"apps: browser settings, history, cookie jar, download manager"`

---

## OS Comparison


| ⭐  | Feature                                     | 🪟 Win11                            | 🐧 Linux                         | 🚀 Impossible OS                        |
| --- | ------------------------------------------- | ----------------------------------- | -------------------------------- | --------------------------------------- |
| 💎  | HTTP/HTTPS page fetch                       | ✅ Edge/WebView2                    | ✅ Firefox/Chrome                | ⬜ `https_get()`                        |
| 💎  | HTML renderer                               | ✅ Blink engine                     | ✅ Gecko/Blink                   | ⬜ custom or NetSurf port               |
| 💎  | CSS box model + selectors                   | ✅ Blink                            | ✅ Gecko/Blink                   | ⬜ §4 -- (stretch )                     |
| 💎  | JavaScript engine                           | ✅ V8                               | ✅ V8/SpiderMonkey               | ⬜ §5 -- (long-term , QuickJS/Duktape)  |
| 💎  | Multi-tab browser                           | ✅ Edge tabs                        | ✅ Firefox tabs                  | ⬜ `CTRL_TABSTRIP` (16 tabs)            |
| 💎  | HTTPS 🔒 padlock indicator                  | ✅ Edge                             | ✅ Firefox                       | ⬜ Fluent `lock_closed` icon            |
| 💎  | Bookmarks + history                         | ✅ Edge                             | ✅ Firefox                       | ⬜ Registry-backed                      |
| 💎  | Download manager                            | ✅ Edge                             | ✅ Firefox                       | ⬜ `https_get` stream to file           |
| 💎  | Find in page                                | ✅ Edge                             | ✅ Firefox                       | ⬜ highlight matching text              |
| ⭐  | Phase 1 text browser                        | ❌ No minimal mode                  | ❌ No minimal mode               | ⬜ runs without DOM/CSS                 |
| ⭐  | Entire browser built on OS's own HTTP stack | ❌ Chromium ships own network layer | ❌ Gecko ships own network layer | ⬜ reuses kernel `http_get`/`https_get` |
| ⭐  | Settings, bookmarks, history in OS Registry | ❌ Separate profile format          | ❌ SQLite profile                | ⬜ `HKCU\Software\Impossible\Browser\`  |

**Impossible OS advantage:** The browser is the first app that exercises every major OS subsystem simultaneously -- networking, TLS, TTF rendering, image decoding, IxUI windows, registry, and clipboard. Phase 1 delivers a working browser in ~500 lines by reusing the kernel's `https_get()` directly, with no Chromium or Firefox dependency. All state lives in the OS Registry, making profiles trivially inspectable and portable.

---

## Verification

**§1: Text browser**
- QEMU: launch `browser.exe`; type `http://example.com` → Enter; page text rendered in window; links shown as `[1] More information...`
- `https://example.com` → 🔒 padlock in status bar; `https_get` returns 200
- `[←]` after navigating two pages → returns to first page

**§2–3: HTML renderer**
- Navigate to a page with `<h1>`, `<p>`, `<b>`, `<ul>`, `<img>` → each tag renders correctly: heading larger, list with bullets, image blitted
- Long page → vertical scrollbar appears; scroll to bottom → content continues
- `<table>` with 3 columns → columns evenly distributed across window width

**§4: CSS**
- Page with `<style>p { color: red; font-size: 18px; }</style>` → paragraph text renders red at 18 px

**§7: Browser chrome**
- `[+]` tab → new tab opens; first tab still shows previous page on switch back
- Right-click on link → context menu with "Open in new tab"; click → new tab navigates to link URL
- `[★]` on a page → entry appears in bookmarks bar; click bookmark → navigates to saved URL

**§8: Settings**
- `browser://settings` URL → settings page renders; change homepage to `https://impossible-os.dev`; new `[🏠]` button navigates there
- `[Clear Browsing Data]` → `History` registry key cleared; new-tab page shows empty recently visited list
