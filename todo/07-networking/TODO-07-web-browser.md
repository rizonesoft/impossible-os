---
schema_version: 1
id: web-browser-networking
domain: 07-networking
status: active
title: "TODO-07 -- Web Browser"
---

# TODO-07 -- Web Browser

> **Goal:** Build Impossible OS's flagship internet application in ten progressive sections: text-only browser proving the HTTP stack, a full HTML tokenizer + DOM tree, block/inline layout engine, tab management, image rendering, CSS cascade engine, JavaScript stub, bookmarks manager, download manager with HTTP Range resume, and a privacy + security layer (TLS padlock, cookie jar, mixed-content blocking, HTTP caching). A QUIC/HTTP 2 upgrade path is noted for future work (→ XREF `07-networking/TODO-08` when created).

> [!IMPORTANT]
> `http_get()`/`https_get()` from TODO-03 are the mandatory network primitives. `image_load_mem()` from `include/kernel/image.h` handles JPEG/PNG/BMP decoding -- the browser fetches image bytes via HTTP and passes them directly to this function. `wm_create_window()` from `include/desktop/wm.h` is the window creation API; `font_draw_string()` from `include/desktop/font.h` renders text. All source lives under `src/apps/browser/`; headers under `include/apps/browser/`. The DOM tree and layout engine are the two largest allocations in the browser -- use `pmm_alloc_contiguous()` for node arrays and render buffers > 4 KB; `kmalloc` only for small per-node structs (≤ 4 KB each). The CSS style engine (§6) depends on the DOM tree (§2) and the layout engine (§4) being stable before computed styles can be applied. Tab management (§3) must be added before HTML rendering to avoid a global-state refactor.

## Inputs

- `src/kernel/net/http.c` + `https_get()` -- `http_get(url, buf, max)` and `https_get(url, buf, max)` from TODO-03; browser calls these for all page and resource fetches
- `include/kernel/image.h` -- `image_load_mem(img, data, size)` for `<img src>` decoding (JPEG/PNG/BMP); `image_t { width, height, pixels }` for framebuffer blit
- `include/desktop/wm.h` -- `wm_create_window(title, x, y, w, h)` for browser window + tab bar; mouse/keyboard event callbacks
- `include/desktop/font.h` -- `font_draw_string(x, y, str, color, size)` for text rendering; `font_draw_char` for individual glyphs
- `include/desktop/controls.h` -- text input box, button, scrollbar controls for address bar and toolbar
- `src/kernel/fs/vfs.c` -- `vfs_open()`/`vfs_write()`/`vfs_read()` for saving downloads to `C:\Users\Default\Downloads\` and reading bookmarks JSON
- `include/registry.h` -- cookie persistence and browser settings in `HKCU\Software\ImpossibleBrowser`
- → XREF: `07-networking/TODO-03-http-tls.md` -- `http_get`, `https_get`, keep-alive pool, HTTP Range header, TLS cert verified flag (for padlock icon)
- → XREF: `07-networking/TODO-02-dns-sockets.md` -- `dns_resolve_dual()` for mixed-IPv4/IPv6 resource fetches
- → XREF: `07-networking/TODO-05-firewall.md` -- browser respects outbound firewall rules transparently; no browser-specific changes needed

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
| 💎  |   1   | §1 Text-only browser -- tag-strip state machine, scrollable window, link list, back/forward           | 11-apps/TODO-01 §9 | [ ] |
| 💎  |   2   | §2 HTML parser + DOM tree -- tokenizer, tag/attr/children, essential 20 tags                         | §1, 11-apps/TODO-01 §9 | [ ] |
| 💎  |   3   | §3 Tab management -- `browser_tab_t`, tab bar UI, Ctrl+T/W/Tab, 16-tab limit                        | §1, 11-apps/TODO-01 §9 | [ ] |
| 💎  |   4   | §4 Block/inline layout engine -- block flow, inline flow, word wrap, vertical scroll, clip           | §2, §3, 11-apps/TODO-01 §9 | [ ] |
| 💎  |   5   | §5 Image loading -- `<img src>` HTTP fetch + `image_load_mem`, inline blit, alt text                 | §2, 11-apps/TODO-01 §9 | [ ] |
| 💎  |   6   | §6 CSS parser + style engine -- tokenizer, cascade, box model, computed styles                       | §4, 11-apps/TODO-01 §9 | [ ] |
| 💎  |   7   | §7 JavaScript stub -- `<script>` detection, `[JavaScript disabled]` placeholder, `window`/`document` | §2, 11-apps/TODO-01 §9 | [ ] |
| 💎  |   8   | §8 Bookmarks manager -- `bookmarks.json`, Ctrl+D add/edit, dropdown, HTML import/export             | §3, §1, 11-apps/TODO-01 §9 | [ ] |
| 💎  |   9   | §9 Download manager -- progress window, pause/cancel, HTTP Range resume, `Downloads\` save          | §2, §4, 11-apps/TODO-01 §9 | [ ] |
| 💎  |  10   | §10 Privacy + security -- TLS padlock, mixed-content block, cookie jar, `ETag` caching              | §6, §3, 11-apps/TODO-01 §9 | [ ] |

---

## 1. Text-Only Browser

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings Text-Only Browser"`

## 2. HTML Parser + DOM Tree

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings HTML Parser + DOM Tree"`

## 3. Tab Management

**Design:** [`controls.md#tabs`](../../docs/design/controls.md#tabs)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings Tab Management"`

## 4. Block / Inline Layout Engine

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings Block / Inline Layout Engine"`

## 5. Image Loading in Browser

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings Image Loading in Browser"`

## 6. CSS Parser + Style Engine

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings CSS Parser + Style Engine"`

## 7. JavaScript Stub

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings JavaScript Stub"`

## 8. Bookmarks Manager

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings Bookmarks Manager"`

## 9. Download Manager

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#progress`](../../docs/design/controls.md#progress), [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] HTTP Range requests in `src/kernel/net/http.c`, for every downloader
- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings Download Manager"`

## 10. Privacy + Security

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `11-apps/TODO-01` §9 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] Request and response headers in `src/kernel/net/http.c`: cookies, `ETag`, `If-None-Match` and `Cache-Control`
- [ ] Shown to work here on the imported copy, as `11-apps/TODO-01` §9 brings it
- [ ] Commit: `"imports: browser brings Privacy + Security"`

---

## OS Comparison


| ⭐  | Feature                                          | 🪟 Win11                                   | 🐧 Linux                                             | 🚀 Impossible OS                                                      |
| --- | ------------------------------------------------ | ------------------------------------------ | ---------------------------------------------------- | --------------------------------------------------------------------- |
| 💎  | Text-only HTML browser                           | ✅ Edge + IE legacy; `lynx`                | ✅ `w3m`, `lynx`, `elinks` text browsers             | ⬜ §1 -- kernel-native `http_get` → tag-strip →                       |
| 💎  | HTML parser + DOM tree                           | ✅ Edge (Chromium Blink); IE (Trident);    | ✅ Firefox (Gecko), Chromium (Blink), WebKit;        | ⬜ §2 -- custom tokenizer; void-element auto-close; 20                |
| 💎  | Tab management -- 16 tabs, tab bar, Ctrl+T/W/Tab | ✅ Edge multi-tab; tab groups, vertical    | ✅ All major browsers support tabs                   | ⬜ §3 -- `browser_tab_t` per-tab DOM+history; tab bar                 |
| 💎  | Block/inline layout engine                       | ✅ Blink/Gecko full CSS layout including   | ✅ Same (Blink/Gecko/WebKit)                         | ⬜ §4 -- block + inline flow; word                                    |
| 💎  | Image loading                                    | ✅ Full image support (WebP, AVIF,         | ✅ Same format support                               | ⬜ §5 -- `image_load_mem`; 16-entry decoded cache; alt                |
| 💎  | CSS cascade                                      | ✅ Full CSS3 in Blink/Gecko; DevTools      | ✅ Same                                              | ⬜ §6 -- specificity sort, `!important`, em/%/px lengths,             |
| 💎  | JavaScript stub → QuickJS upgrade path           | ✅ V8 (Edge); SpiderMonkey (Firefox); full | ✅ Same                                              | ⬜ §7 -- stub with `[JavaScript disabled]` placeholder                |
| 💎  | Bookmarks                                        | ✅ Edge Favorites; IE bookmark import;     | ✅ All major browsers support bookmark               | ⬜ §8 -- `bookmarks.json`; NETSCAPE HTML import/export; 512           |
| 💎  | Download manager                                 | ✅ Edge download shelf; resume via         | ✅ Firefox/Chromium download manager; resume support | ⬜ §9 -- `http_get_range()`; 8 concurrent; pause/resume; `Downloads\` |
| 💎  | Privacy + security                               | ✅ Edge: HTTPS indicator, mixed content    | ✅ All major browsers implement these                | ⬜ §10 -- padlock/⚠ in address bar; cookie                            |

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
