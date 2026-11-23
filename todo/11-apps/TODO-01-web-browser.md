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

| #   | Section                                                 | Tag        | Dep                                                                                    | Mark | Status |
| --- | ------------------------------------------------------- | ---------- | -------------------------------------------------------------------------------------- | ---- | :----: |
| 1   | Text-only browser (Phase 1, ~500 lines)                 | `[Sonnet]` | §9                                                                                     | 💎   |  [ ]   |
| 2   | HTML tokenizer + DOM tree                               | `[Opus]`   | §9, §1                                                                                 | 💎   |  [ ]   |
| 3   | Layout engine (block + inline flow)                     | `[Opus]`   | §9, §2                                                                                 | 💎   |  [ ]   |
| 4   | CSS parser + cascade (stretch)                          | `[Opus]`   | §9, §3                                                                                 | 💎   |  [ ]   |
| 5   | JavaScript engine -- QuickJS/Duktape (long-term)        | `[Opus]`   | §9, §3                                                                                 | 💎   |  [ ]   |
| 6   | Alternative: NetSurf / Dillo port evaluation            | `[Sonnet]` | §9, §1                                                                                 | 💎   |  [ ]   |
| 7   | Browser chrome (tabs, toolbar, bookmarks, context menu) | `[Sonnet]` | §9, §1, §6                                                                             | ⭐   |  [ ]   |
| 8   | Browser settings + history + cookie jar                 | `[Sonnet]` | §9, §7                                                                                 | 💎   |  [ ]   |
| 9   | Import Browser                                          | `[Sonnet]` | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §5 | 💎   |  [ ]   |

---

## 1. Text-Only Browser (Phase 1)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings Text-Only Browser (Phase 1)"`

---

## 2. HTML Tokenizer + DOM Tree

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings HTML Tokenizer + DOM Tree"`

---

## 3. Layout Engine (Block + Inline Flow)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings Layout Engine (Block + Inline Flow)"`

---

## 4. CSS Parser + Cascade (Stretch)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings CSS Parser + Cascade (Stretch)"`

---

## 5. JavaScript Engine (Long-Term Stretch)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings JavaScript Engine (Long-Term Stretch)"`

---

## 6. Alternative: NetSurf / Dillo Port Evaluation

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings Alternative: NetSurf / Dillo Port Evaluation"`

---

## 7. Browser Chrome

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings Browser Chrome"`

---

## 8. Browser Settings + History + Cookie Jar

**Moved:** browser

Built in Browser, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §9 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §9 brings it
- [ ] Commit: `"imports: browser brings Browser Settings + History + Cookie Jar"`

## 9. Import Browser

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** browser

Browser is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull browser` into `src/apps/browser/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open Browser on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose browser`
- [ ] Commit: `"apps: Browser imported"`

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
