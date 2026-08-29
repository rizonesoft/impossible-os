---
schema_version: 1
id: text-font-internationalization
domain: 08-graphics-ui
status: active
title: "TODO-02 -- Text, Font, and Internationalization Foundation"
---

# TODO-02 -- Text, Font, and Internationalization Foundation

> **Goal:** Upgrade the current TrueType font manager and text drawing code into a real desktop text stack: font catalog, fallback chains, emoji/color fonts, shaping, bidi, paragraph layout, caret and hit-testing services, and the Win32/Desktop integration points that make text-heavy apps feel real. The existing TTF renderer proved the desktop can draw glyphs; this TODO makes that text system complete, international, and reusable.

> [!IMPORTANT]
> **Current state:** `include/font_mgr.h` and `src/kernel/gfx/gfx_text.c` already load a small fixed font set from `C:\Impossible\Fonts\`, pre-bake ASCII 32-126 atlases for three sizes, and fall back to an LRU raster cache for uncached glyphs. `ttf_get()`, `ttf_draw_char()`, `ttf_draw_string()`, `ttf_measure_width()`, and `ttf_line_height()` already exist. `src/desktop/font.c` still carries the legacy ASCII bitmap font path for older UI callers. Desktop, controls, window manager, and shell code already call `ttf_get()` and `ttf_draw_string()` directly. What is missing is everything a full OS text stack needs beyond "draw this Latin string": font enumeration and installation policy, fallback chains, emoji/color-font handling, script shaping, bidi-aware layout, grapheme-safe cursor movement, paragraph measurement, composition hooks for IME, and shared font/dialog APIs for Win32 consumers.

> [!IMPORTANT]
> **Evaluate HarfBuzz before implementing complex-script shaping.** HarfBuzz is "Old MIT" licensed (verified 2026-08-17) and therefore GPL-3.0-compatible. Complex-script shaping (Arabic joining, Indic reordering, ligature and kerning resolution via OpenType GSUB/GPOS) is a domain where a fresh implementation is wrong for years in ways only native readers notice, which makes it a poor from-scratch candidate. Simple Latin layout over the existing `stb_truetype` rasteriser does not need it; anything past that does.
>
> Record the verdict here before implementing the shaping sections, per CLAUDE.md "Vendor-First Evaluation".
>
> **Verdict (2026-08-29): vendor.** HarfBuzz for shaping and FreeType for rasterization, as §7; §2 and §3 are written on top of them, and the existing `ttf_*` renderer is the bring-up fallback only.

## Inputs

- [`include/font_mgr.h`](../../include/font_mgr.h) -- current font slot and glyph-cache public API
- [`src/kernel/gfx/gfx_text.c`](../../src/kernel/gfx/gfx_text.c) -- current TTF loading, atlas baking, and draw path
- [`src/desktop/font.c`](../../src/desktop/font.c) -- legacy bitmap font path to retire or strictly scope
- [`src/desktop/controls.c`](../../src/desktop/controls.c) -- control text drawing and future caret/hit-test consumers
- [`src/desktop/terminal.c`](../../src/desktop/terminal.c) -- terminal text, selection, and composition consumer
- [`include/kernel/image.h`](../../include/kernel/image.h) -- color glyph and emoji atlas images when needed
- -> XREF: `TODO-14-win32-gdi-user32-stubs.md §4,§8` -- GDI text APIs and `ChooseFont` wrappers consume the shared text/font foundation
- -> XREF: `TODO-15-win32k-shadow-ssdt.md §4,§16,§21` -- Win32k font, text, and IME-facing syscalls build on this layer
- -> XREF: `D12 T05 §8` -- the Win32 subsystem's common-dialog and font-picker work needs one canonical font catalog and layout owner

## Outcome

- Font families, styles, defaults, and installed files are enumerated through one catalog API instead of hard-coded slot assumptions.
- Missing glyphs, emoji, and mixed-script text resolve through explicit fallback chains with predictable metrics.
- Paragraph layout, shaping, bidi, hit-testing, caret movement, and selection metrics become reusable services rather than app-specific math.
- Text input controls, terminal, Notepad, and Win32 `DrawText` consume one text foundation.
- Impossible OS text rendering becomes ready for international and accessibility-heavy workloads instead of remaining ASCII-first.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                            | Depends On                     | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------------ | ------------------------------ | :----: |
| 💎  |   1   | §1 Font catalog, enumeration, install/remove, and default stacks                                       | --                             |  [ ]   |
| 💎  |   2   | §2 Fallback chains, emoji, and color-font support                                                      | §1                             |  [ ]   |
| 💎  |   3   | §3 Shaping, bidi, line-break, and paragraph layout engine                                              | §1, §2                         |  [ ]   |
| 💎  |   4   | §4 Caret, hit-test, selection, and composition-aware text editing services                             | §3                             |  [ ]   |
| 💎  |   5   | §5 Desktop and Win32 wiring: `DrawText`, `ChooseFont`, `WM_FONTCHANGE`                                 | §1-§4                          |  [ ]   |
| ⭐  |   6   | §6 Persistent text-run cache and no-FPU steady-state draw path                                         | §2-§5                          |  [ ]   |
| 💎  |   7   | §7 Vendor FreeType + HarfBuzz -- freestanding rasterizer and shaper under §1-§3 (PROVENANCE + CREDITS) | §1 catalog; consumed by §2, §3 |  [ ]   |

> 💎 = parity work -- matches the text stacks used by Windows 11 and Linux desktops.
> ⭐ = exclusive work -- Impossible OS gets a cleaner and more predictable text engine.

---

## 1. Font Catalog, Enumeration, Install/Remove, and Default Stacks

Create the catalog layer that turns "slot 0/1/2" into a real system font inventory usable by shell, apps, and Win32 dialogs.

- [ ] Add `font_family_t`, `font_face_t`, and `font_catalog_t` in a new `include/font_catalog.h` plus `src/kernel/gfx/font_catalog.c`
- [ ] Implement `font_mgr_list()`, `font_mgr_find_family()`, `font_mgr_get_default_stack()`, and `font_mgr_reload()` on top of the current loader
- [ ] Add Registry-backed defaults for UI, mono, icon, and emoji stacks so shell and app code stop hard-coding slot numbers
- [ ] Add install/remove/refresh helpers for `.ttf`, `.otf`, and future collection files, including boot-time rescan of `C:\Impossible\Fonts\`
- [ ] Retire or strictly quarantine `src/desktop/font.c` bitmap rendering so legacy ASCII drawing cannot silently bypass the main font stack
- [ ] Emit `klog(LOG_INFO, "FONT", ...)` lines for catalog reload, added families, and failed font probes
- [ ] Commit: `"font: catalog foundation -- enumerate families, default stacks, install/remove, reload"`

**Test checkpoint:** `font_mgr_list()` returns the expected built-in families; changing the default UI font updates the reported default stack; serial shows `"FONT: catalog reload"` with family count and no stale slot-only assumptions. Test on: QEMU WHPX + TCG; bare metal.

## 2. Fallback Chains, Emoji, and Color-Font Support

Move from "one font or a single uncached glyph fallback" to real fallback stacks that can render mixed-script and emoji text correctly.

- [ ] Add explicit fallback lists per default stack: UI, mono, icon, emoji, and high-contrast-friendly alternates
- [ ] Add glyph-resolution logic that walks fallback chains predictably and caches the resolved face per codepoint range or text run
- [ ] Add emoji/color-font handling for codepoints that need bitmap or layered color glyphs instead of plain monochrome outlines
- [ ] Define missing-glyph and broken-font behavior explicitly so callers get diagnostic boxes or fallback glyphs rather than silent drops
- [ ] Add family/style matching rules for bold/italic/weight substitution so `CreateFontA` and font picker flows can request something real
- [ ] Log fallback and missing-glyph events with throttling so parity bugs can be triaged without flooding serial
- [ ] Commit: `"font: fallback foundation -- stack resolution, emoji/color glyphs, style substitution"`

**Test checkpoint:** A mixed Latin plus non-Latin string resolves through the expected fallback chain; emoji text uses the emoji path rather than blank boxes; serial shows `"FONT: fallback face="` for test strings. Test on: QEMU WHPX + TCG; bare metal.

## 3. Shaping, Bidi, Line-Break, and Paragraph Layout Engine

Add the layout layer between raw glyphs and real UI text, including mixed-script and right-to-left text handling.

- [ ] Add `text_run_t`, `text_layout_t`, and `text_paragraph_t` in a new `include/text_layout.h` plus `src/kernel/gfx/text_layout.c`
- [ ] Implement script-run segmentation, shaping contracts, bidi run ordering, and grapheme-safe line breaking with integer or fixed-point metrics
- [ ] Add width/height measurement, alignment, wrapping, ellipsis, and tab-stop handling for shell controls and Win32 `DrawText`
- [ ] Provide render-time data structures that decouple shaping/layout from actual drawing so apps and dialogs can measure before paint
- [ ] Define paragraph cache invalidation rules for text changes, font changes, DPI changes, and theme changes
- [ ] Emit `klog(LOG_INFO, "TEXT", "layout runs=%u lines=%u")` for bring-up and regression triage
- [ ] Commit: `"text: layout foundation -- shaping contract, bidi, line break, paragraph measure/render data"`

**Test checkpoint:** Measuring and drawing a wrapped paragraph yields stable line count and width; mixed LTR/RTL text respects run order; serial shows `"TEXT: layout runs="` with expected counts. Test on: QEMU WHPX + TCG; bare metal.

## 4. Caret, Hit-Test, Selection, and Composition-Aware Text Editing Services

Build the reusable editing primitives that controls, Notepad, terminal, and IME composition all need.

- [ ] Add caret metrics, codepoint-to-pixel hit-testing, and grapheme-safe left/right/home/end movement helpers
- [ ] Add selection-range to rectangle mapping so controls and editors can render exact highlight quads for wrapped text
- [ ] Add composition-underlines and pre-edit range hooks so the IME work in `TODO-16` can overlay inline composition cleanly
- [ ] Add shared helpers for clipboard range extraction, replace-selection, and selection expansion by word/line
- [ ] Update text controls and terminal integration points to consume the shared caret/hit-test API rather than bespoke pixel math
- [ ] Add `klog(LOG_INFO, "TEXT", "caret hit-test idx=%u")` bring-up logging for editor paths
- [ ] Commit: `"text: editing services -- caret, hit-test, selection rects, composition hooks"`

**Test checkpoint:** Clicking and moving through a wrapped string returns the expected caret index; selection rects match rendered lines; serial shows `"TEXT: caret hit-test"` for deterministic test cases. Test on: QEMU WHPX + TCG; bare metal.

## 5. Desktop and Win32 Wiring: `DrawText`, `ChooseFont`, `WM_FONTCHANGE`

Make the text stack the only legitimate owner for system font enumeration and text layout across shell and Win32 layers.

- [ ] Implement the `font_mgr_list()` / family/style APIs that the font picker dialog and `ChooseFontA` wrappers consume
- [ ] Update GDI and Win32 text wrappers to use `text_layout_t` measurement and render contracts instead of ad hoc string-width logic
- [ ] Add `WM_FONTCHANGE` or equivalent desktop broadcast semantics so controls and apps can refresh after install/remove/default-stack changes
- [ ] Update terminal, Notepad, dialog, and shell consumers to request families/stacks through the new catalog instead of slot IDs where practical
- [ ] Fix the current `ChooseFont` roadmap references so `D12 T05 §8` points at this TODO's catalog and integration sections, not a nonexistent font-manager owner
- [ ] Commit: `"text: integration wiring -- font picker, GDI/Win32 layout bridge, WM_FONTCHANGE"`

**Test checkpoint:** `ChooseFontA` sees the same family list reported by `font_mgr_list()`; changing the default UI font triggers a font-change broadcast and redraw; serial shows `"FONT: broadcast change"` on reload. Test on: QEMU WHPX + TCG; bare metal.

## 6. Persistent Text-Run Cache and No-FPU Steady-State Draw Path

Make text rendering more predictable than both old GDI and many Linux toolkit hot paths by caching shaped runs and minimizing runtime floating-point churn.

> [!TIP]
> Windows and Linux both provide strong text stacks, but their hot paths still depend heavily on framework-specific caches. Impossible OS can be cleaner by making shaped-run reuse and no-FPU steady-state drawing an explicit contract.

- [ ] Add persistent run-cache entries keyed by `(font stack, size, dpi, text hash, layout flags)` so repeated chrome and shell strings reuse shaping work
- [ ] Extend atlas management so steady-state shell drawing uses cached run metrics and glyph spans without new FPU work on every frame
- [ ] Add invalidation hooks for DPI, theme, font install, and fallback-chain changes so caches stay correct
- [ ] Expose opt-in APIs for shell and Win32k consumers to record reusable text scenes instead of re-shaping the same labels every frame
- [ ] Add `klog(LOG_INFO, "TEXT", "run cache hit=%u miss=%u")` counters for measurable performance review
- [ ] Commit: `"text: run cache -- persistent shaped runs, steady-state no-FPU draw path"`

**Test checkpoint:** Re-drawing the same labels twice yields more run-cache hits on the second pass; theme or DPI changes invalidate the affected entries; serial shows `"TEXT: run cache hit="` and `"miss="` counters. Test on: QEMU WHPX + TCG; bare metal.

---

## 7. Vendor FreeType and HarfBuzz as the Rasterizer and Shaper

> **Spawned-by:** root

Verdict on the vendor-first evaluation above: vendor. Windows 11 text quality comes from DirectWrite's hinting, subpixel positioning and OpenType shaping; a from-scratch shaper is wrong for years in ways only native readers notice. FreeType is the rasterizer, HarfBuzz the shaper, and the existing `ttf_*` renderer becomes the bring-up fallback that is retired once §1-§3 run on the vendored engine.

**Licenses (LICENSE files read 2026-08-29):** FreeType is dual FTL / GPL-2.0-or-later; take the GPL-2.0-or-later arm, because the FTL advertising clause is GPL-incompatible. HarfBuzz is "Old MIT" (verified 2026-08-17). Both go in `src/libs/PROVENANCE.md` and `CREDITS.md` in the vendoring commit.

**Files:** `src/libs/freetype/`, `src/libs/harfbuzz/`, `src/kernel/gfx/ft_osl.c` (allocator, FPU and file hooks), `Makefile` (first C++ translation units in the tree)

- [ ] Vendor FreeType under `src/libs/freetype/` with a freestanding `ftoption.h`
  - No stdlib: `kmalloc` / `pmm_alloc_contiguous` allocator hooks, VFS stream hooks, no PNG / BZip2 / HarfBuzz-in-FreeType modules
  - CR0.TS cleared and FPU state owned before any FreeType call (CLAUDE.md FPU/SIMD gotchas); the §6 no-FPU steady-state path caches rasterized runs so the FPU is touched only on a cache miss
  - PROVENANCE + CREDITS rows: upstream tag, license arm chosen (GPL-2.0-or-later), vendoring commit
- [ ] Vendor HarfBuzz under `src/libs/harfbuzz/` built freestanding (`HB_TINY` / `HB_NO_*` profile)
  - HarfBuzz is C++: compiled with clang-19 `-fno-exceptions -fno-rtti -nostdlib++ -ffreestanding` inside the kernel build, so the Makefile C++ rule and the freestanding `new` / `delete` / `__cxa_*` shims land here, gated so no other kernel code may use C++ without a recorded decision
  - PROVENANCE + CREDITS rows (Old MIT)
- [ ] Rewire §2 fallback and §3 shaping onto `hb_shape()` + `FT_Load_Glyph`; `text_run_t` carries HarfBuzz glyph infos and positions
  - Old `ttf_draw_string()` path stays behind a `TEXT_LEGACY_TTF` build flag until the vendored path passes the §3 checkpoint on bare metal, then is deleted
- [ ] Register the Fluent replacement faces from `08-graphics-ui/TODO-03 §9` (→ XREF) in the §1 catalog with `Segoe UI` / `Segoe UI Variable` / `Segoe Fluent Icons` alias rows
- [ ] Commit: `"text: vendor FreeType + HarfBuzz -- freestanding build, allocator/FPU hooks, PROVENANCE + CREDITS"`

**Test checkpoint:** Arabic and Devanagari sample strings shape with correct joining and reordering (glyph indices compared against a host `hb-shape` run of the same font file); `Segoe UI` alias resolves to Selawik; serial shows `"TEXT: shaper=harfbuzz rasterizer=freetype"`. Test on: QEMU WHPX + TCG; bare metal.

---

## OS Comparison

| ⭐  | Feature                           | 🪟 Win11                | 🐧 Linux                 | 🚀 Impossible OS |
| --- | --------------------------------- | ----------------------- | ------------------------ | ---------------- |
| 💎  | Font catalog + enumeration        | ✅ DirectWrite catalog  | ✅ Fontconfig + toolkit  | ⬜ Planned - §1  |
| 💎  | Fallback + emoji/color fonts      | ✅ DirectWrite fallback | ✅ HarfBuzz/Pango stacks | ⬜ Planned - §2  |
| 💎  | Shaping + bidi + layout           | ✅ DirectWrite layout   | ✅ Pango/Qt text layout  | ⬜ Planned - §3  |
| 💎  | Caret + hit-test editing services | ✅ RichEdit/TextSvc     | ✅ GTK/Qt text widgets   | ⬜ Planned - §4  |
| 💎  | Font picker + Win32 integration   | ✅ `ChooseFont` + GDI   | ✅ toolkit dialogs       | ⬜ Planned - §5  |
| ⭐  | Persistent shaped-run cache       | ⚠️ Framework-specific   | ⚠️ Toolkit-specific      | ⬜ Planned - §6  |
| 💎  | Vendored shaper + rasterizer      | ✅ DirectWrite (closed) | ✅ HarfBuzz + FreeType   | ⬜ Planned - §7  |

After §1-§5, Impossible OS reaches parity with the text and font capabilities expected from modern Windows and Linux desktop stacks. After §6, it adds a more explicit and deterministic text-cache contract that should keep shell and Win32 redraw paths cleaner and cheaper.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_text_font()` -- register in `src/kernel/test/test_runner.c`.
> This TODO likely needs a new `TEST_CAT_DESKTOP` or `TEST_CAT_GFX` category because the current categories do not cover reusable text and font infrastructure cleanly.

- [ ] Create `src/kernel/test/test_text_font.c` with:
  - `font_mgr_list()` returns the expected built-in families and default stacks
  - fallback resolution returns a usable face for a mixed-script sample string
  - `text_layout_measure()` returns stable width and line counts for wrapped and bidi samples
  - caret hit-testing and selection-rect mapping return expected indices and ranges
  - run-cache counters show a hit on the second identical draw
- [ ] Register in `test_runner_init()`: `test_register_text_font()`
- [ ] Commit: `"test: add text and font foundation test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] Serial shows `"FONT: catalog reload"` and `"FONT: fallback face="` during catalog and fallback tests
- [ ] Serial shows `"TEXT: layout runs="` and `"TEXT: run cache hit="` during layout and cache tests
- [ ] `ChooseFontA` or the font picker dialog sees the same family list reported by the catalog
- [ ] Text/font foundation tests pass via the new graphics/desktop test category
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal
