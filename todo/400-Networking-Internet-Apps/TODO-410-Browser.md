# P1102 — Web Browser

> **Goal:** From text-only HTML to a full browser with CSS and JavaScript.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Web Browser

### 1.1 Phase 1: Text-Only Browser (~500 lines)

**Prompt:** Start with the simplest possible browser: fetch a page via `http_get()` (Phase 07), strip HTML tags with a simple state machine (inside tag / not inside tag), display plain text in a scrollable window. Extract `<a href>` links and display as a numbered list. Click a link or type a URL in the address bar to navigate. Maintain a back/forward history stack of URLs. This is ~500 lines and proves the HTTP stack works end-to-end. After all items,sh clean`, commit `"apps: text-only web browser"`.


- [ ] Create `src/apps/browser/browser.c`
- [ ] HTTP GET: fetch page via `http_get(url, buffer, max)` (from Phase 07)
- [ ] Strip HTML tags: simple state machine (in tag / not in tag)
- [ ] Display plain text content in a scrollable window
- [ ] Extract `<a href="...">` links: display as numbered list
- [ ] Click link → navigate to URL
- [ ] Address bar: type URL, press Enter
- [ ] Back/Forward navigation history (stack of URLs)
- [ ] Commit: `"apps: text-only web browser"`

### 1.2 Phase 2: Basic HTML Renderer (~5,000 lines)

**Prompt:** Build an HTML parser (tokenizer → DOM tree with tag, attributes, children, text nodes). Support essential tags: h1-h6 (headings with larger font sizes via Phase 02 font system), p (paragraphs), br, b/strong/i/em (bold/italic), a href (clickable links), ul/ol/li (lists), img src (fetch via HTTP + decode via Phase 02 image loader), table/tr/td (basic grid), hr, pre/code (monospace). Layout engine: block flow (top-to-bottom) and inline flow (left-to-right with word wrapping at window width). Vertical scrolling for long pages. After all items,sh clean`, commit `"apps: HTML renderer"`.


- [ ] HTML parser: tokenizer → DOM tree (tag, attributes, children, text nodes)
- [ ] Support tags:
  - [ ] `<h1>`–`<h6>` — headings (larger font sizes)
  - [ ] `<p>` — paragraphs (vertical spacing)
  - [ ] `<br>` — line break
  - [ ] `<b>`, `<strong>` — bold
  - [ ] `<i>`, `<em>` — italic
  - [ ] `<a href>` — links (underlined, accent color, clickable)
  - [ ] `<ul>`, `<ol>`, `<li>` — lists (bullet/numbered)
  - [ ] `<img src>` — images via `image_load()` + HTTP fetch
  - [ ] `<table>`, `<tr>`, `<td>` — basic table layout
  - [ ] `<hr>` — horizontal rule
  - [ ] `<pre>`, `<code>` — preformatted text (monospace font)
- [ ] Layout engine: block flow (top-to-bottom), inline flow (left-to-right)
- [ ] Text wrapping at window width
- [ ] Vertical scrolling for long pages
- [ ] Commit: `"apps: HTML renderer (block/inline layout)"`

### 1.3 Phase 3: CSS Support (~15,000 lines)

**Prompt:** Stretch: CSS parser for selectors + properties. Box model (margin, padding, border, width, height). Properties: color, background-color, font-size, font-family, text-align. Selectors: element, class, ID. Cascading: inline > style block > default. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: CSS support"`.


- [ ] *(Stretch)* CSS parser: selectors + properties
- [ ] *(Stretch)* Box model: margin, padding, border, width, height
- [ ] *(Stretch)* Properties: color, background-color, font-size, font-family, text-align
- [ ] *(Stretch)* Selector types: element, class (`.foo`), ID (`#bar`)
- [ ] *(Stretch)* Cascading: inline style > `<style>` block > default
- [ ] Commit: `"apps: CSS support"`

### 1.4 Phase 4: JavaScript (Long-Term)

**Prompt:** Stretch: port QuickJS (MIT, ~35K lines, ES2020) or Duktape (MIT, ~60K lines, ES5.1). Add DOM bindings: document.getElementById, element.innerHTML. Event handling: onclick, addEventListener. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: JavaScript engine"`.


- [ ] *(Stretch)* Port **QuickJS** (MIT, ~35K lines, ES2020) or **Duktape** (MIT, ~60K lines, ES5.1)
- [ ] *(Stretch)* DOM bindings: `document.getElementById()`, `element.innerHTML`
- [ ] *(Stretch)* Event handling: `onclick`, `addEventListener`
- [ ] Commit: `"apps: JavaScript engine"`

### 1.5 Alternative: Port NetSurf / Dillo

**Prompt:** Evaluate NetSurf (GPL, ~200K lines, ported to many embedded and custom OSes) or Dillo (GPL, ~30K lines, minimalist). Port requires TCP, DNS, TLS, framebuffer, font rendering — all available from earlier phases. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: ported browser engine"`.


- [ ] Evaluate **NetSurf** (GPL, ~200K lines) — full browser, ported to many embedded/custom OSes
- [ ] Evaluate **Dillo** (GPL, ~30K lines) — minimalist browser
- [ ] Port selected engine to Impossible OS (requires: TCP, DNS, TLS, framebuffer, font rendering)

