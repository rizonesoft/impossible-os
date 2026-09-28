---
schema_version: 1
id: graphics-asset-foundation
domain: 08-graphics-ui
status: active
title: "TODO-01 -- Advanced 2D Graphics and Visual Asset Foundation"
---

# TODO-01 -- Advanced 2D Graphics and Visual Asset Foundation

> **Goal:** Turn the current `gfx_*`, image, icon, and cursor code into a complete reusable graphics substrate for the desktop, apps, and Win32k. The basic primitives, effects, runtime image decode, icon store, and cursor loader already exist; this TODO adds the missing render-target discipline, clip/transform/path state, scalable asset handling, theme-aware icon/cursor policy, thumbnailing, and the integration boundaries that keep shell and Win32 work from inventing parallel graphics stacks.

> [!IMPORTANT]
> **Current state:** `include/gfx.h` and `src/kernel/gfx/` already provide `gfx_surface_t`, rect/rounded/circle/line primitives, gradients, blur, Acrylic, Mica, drop shadow, reveal, alpha blits, and dirty-region helpers. `include/kernel/image.h` plus `src/kernel/image.c`, `image_scale.c`, and `image_save.c` already decode JPEG/PNG/BMP/GIF/TGA, scale images with fit modes, and save BMP/PNG. `src/kernel/ico.c` already loads ICO containers with PNG and DIB payloads. `include/icon_store.h` plus `src/kernel/icon_store.c` already provide a hybrid Fluent-font plus IRES icon store, and `include/cursor.h` plus `src/kernel/drivers/cursor.c` already load Xcur cursor themes with embedded fallbacks. What is still missing is the higher-level foundation that modern Windows and Linux stacks rely on: PMM-safe large render targets, explicit clip/transform state, vector/path rasterization for scalable assets, theme and DPI aware icon/cursor selection, thumbnail and preview caching, and one canonical graphics-asset contract for shell and Win32 consumers.

## Inputs

- [`include/gfx.h`](../../include/gfx.h) -- existing surface, primitive, effect, and dirty-rect API to extend
- [`src/kernel/gfx/gfx_core.c`](../../src/kernel/gfx/gfx_core.c) -- current surface allocation and primitive implementation
- [`src/kernel/gfx/gfx_blend.c`](../../src/kernel/gfx/gfx_blend.c) -- alpha compositing hot path
- [`src/kernel/gfx/gfx_gradient.c`](../../src/kernel/gfx/gfx_gradient.c) -- existing gradient helpers
- [`src/kernel/gfx/gfx_blur.c`](../../src/kernel/gfx/gfx_blur.c) -- current region blur implementation
- [`src/kernel/gfx/gfx_effects.c`](../../src/kernel/gfx/gfx_effects.c) -- Acrylic, Mica, shadow, and reveal effects
- [`include/kernel/image.h`](../../include/kernel/image.h) -- runtime image API and fit modes
- [`src/kernel/image.c`](../../src/kernel/image.c) -- runtime image decoding
- [`src/kernel/image_scale.c`](../../src/kernel/image_scale.c) -- image scaling logic
- [`src/kernel/ico.c`](../../src/kernel/ico.c) -- ICO container loader
- [`include/icon_store.h`](../../include/icon_store.h) -- icon lookup, tinting, and extension mapping API
- [`src/kernel/icon_store.c`](../../src/kernel/icon_store.c) -- current Fluent and IRES icon backends
- [`include/cursor.h`](../../include/cursor.h) -- cursor public API
- [`src/kernel/drivers/cursor.c`](../../src/kernel/drivers/cursor.c) -- Xcur cursor loader and fallback sprites
- -> XREF: `TODO-14-win32-gdi-user32-stubs.md §1,§3,§7` -- GDI and USER wrappers must consume one native graphics foundation, not fork their own raster model
- -> XREF: `TODO-15-win32k-shadow-ssdt.md §3,§5,§9,§24` -- Win32k drawing, bitmap, cursor, and display syscalls are downstream consumers

## Outcome

- Large off-screen surfaces, cached layers, and asset buffers use one explicit PMM-aware render-target API instead of scattered `kmalloc` assumptions.
- `gfx_*` gains clip-stack, transform, and path/vector state so scalable assets and future Win32k path APIs have a native implementation target.
- Images, icons, and cursors share one theme-aware, DPI-aware asset policy with scalable-vector fallbacks and deterministic size selection.
- Thumbnail and preview generation become reusable services instead of per-app ad hoc code.
- Shell, boot splash, apps, and Win32k all consume one graphics-asset foundation with clear ownership boundaries.

## Implementation Order

| ⭐  | Order | Deliverable                                                                 | Depends On | Status |
| --- | :---: | --------------------------------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | §1 Render-target allocator, views, and cached layers                        | --         |  [ ]   |
| 💎  |   2   | §2 Clip, transform, and state stack                                         | §1         |  [ ]   |
| 💎  |   3   | §3 Path, stroke, fill, and SVG-ready vector raster contract                 | §2         |  [ ]   |
| 💎  |   4   | §4 Theme-aware icon, cursor, and scalable asset pipeline                    | §1, §3     |  [ ]   |
| 💎  |   5   | §5 Thumbnail, preview, and multi-size asset cache                           | §4         |  [ ]   |
| ⭐  |   6   | §6 Recorded scene lists and deterministic re-render for shell and Win32k    | §2, §3, §5 |  [ ]   |
| 💎  |   7   | §7 SVG runtime -- vendored plutovg + plutosvg, clipPath, system SVG profile | §3, §4     |  [ ]   |
| 💎  |   8   | §8 Icon engine -- parts, deterministic incremental generation, validation   | §4, §7     |  [ ]   |
| ⭐  |   9   | §9 Missing icons -- detect, draft-generate, queue for polish                | §8         |  [ ]   |

> 💎 = parity work -- matches the reusable graphics and asset layers that Windows 11 and Linux already have.
> ⭐ = exclusive work -- Impossible OS goes beyond both with a simpler and more deterministic foundation.

---

## 1. Render-Target Allocator, Views, and Cached Layers

Replace the current "every surface is a `kmalloc` rectangle" assumption with an explicit render-target API that is safe for large frame-sized assets and reusable off-screen composition.

- [ ] Add `gfx_surface_create_ex()` and `gfx_surface_destroy_ex()` in `include/gfx.h` and `src/kernel/gfx/gfx_core.c` with allocator policy: `kmalloc` for small surfaces, `pmm_alloc_contiguous()` for large surfaces, and explicit ownership flags
- [ ] Add `gfx_surface_view()` for sub-rect and atlas views that wrap existing pixel storage without copying
- [ ] Add `gfx_layer_t` / `gfx_cached_layer_t` descriptors for compositor, menu, tooltip, and thumbnail render targets
- [ ] Add stride/alignment invariants and `klog(LOG_INFO, "GFX", ...)` diagnostics for large-surface alloc/free paths
- [ ] Update current in-tree callers that allocate large temporary graphics buffers (`gfx_drop_shadow`, thumbnail paths, icon/cursor scaling helpers) to use the new allocator helpers instead of open-coded size guesses
- [ ] Define one central "large surface" threshold and document it in the new helper comments so future shell and Win32 code do not re-invent memory rules
- [ ] Frosted surfaces are cached layers: taskbar, Start, flyouts and menus keep their blurred backdrop and recompute it only when the region behind them changes (`docs/design/shell.md#materials`)
  - `gfx_acrylic()` gains the grain term (`THEME_MAT_*_NOISE`, 4-6%) from a fixed tileable noise texture, so the frost reads identically at every frame
- [ ] Commit: `"gfx: render target foundation -- PMM-safe surfaces, views, cached layer descriptors"`

**Test checkpoint:** Creating and destroying a 3840x2160 off-screen surface succeeds without heap exhaustion; `gfx_surface_view()` addresses the expected sub-rectangle; serial shows `"GFX: surface_ex alloc"` and `"GFX: surface_ex free"` for the large-surface path. Test on: QEMU WHPX + TCG; bare metal.

## 2. Clip, Transform, and State Stack

Add the missing drawing state model so reusable UI and Win32 code can clip, nest, and reposition primitives without hard-coded coordinate math.

- [ ] Create `gfx_state_t` with clip rect, translation, scale, and save/restore depth in a new `include/gfx_state.h` plus `src/kernel/gfx/gfx_state.c`
- [ ] Implement `gfx_push_state()`, `gfx_pop_state()`, `gfx_set_clip_rect()`, `gfx_intersect_clip_rect()`, `gfx_translate()`, and `gfx_scale_q16()` using integer or fixed-point math only
- [ ] Wire primitive entry points (`gfx_fill_rect`, `gfx_draw_rect`, `gfx_fill_rounded_rect`, `gfx_draw_line`, `gfx_blit`, `gfx_blit_alpha`) through the active clip and transform state
- [ ] Add a "cheap save/restore" contract for nested widgets, popups, and Win32k DC state so consumers do not have to duplicate clipping logic
- [ ] Define failure semantics for stack overflow/underflow with explicit `klog(LOG_ERROR, "GFX", ...)` diagnostics instead of silent misrender
- [ ] Document how the state stack interoperates with the dirty-region tracker so clipped draws dirty only the intersected region
- [ ] Commit: `"gfx: state stack -- clip rects, transforms, save/restore, primitive integration"`

**Test checkpoint:** A translated and clipped draw affects only the expected pixels; nested push/pop returns the prior state exactly; serial shows `"GFX: state push depth=N"` and no underflow/overflow warnings. Test on: QEMU WHPX + TCG; bare metal.

## 3. Path, Stroke, Fill, and SVG-Ready Vector Raster Contract

Build the vector/path layer that current shell polish and future Win32k path/SVG work need, instead of treating scalable assets as special cases forever.

- [ ] Add `gfx_path_t` with `move_to`, `line_to`, `quad_to`, `cubic_to`, `arc_to`, and `close` builders in `include/gfx_path.h` and `src/kernel/gfx/gfx_path.c`
- [ ] Implement scanline fill and stroke helpers for convex and general UI paths with even-odd and non-zero winding modes
- [ ] Add dashed stroke, line join, and line cap support for menu chrome, selection outlines, vector icons, and future `NtGdi*Path*` consumers
- [ ] Add a compact "SVG-ready" command stream contract so asset loaders can translate path commands into the same native rasterizer without inventing a second geometry engine
- [ ] Extend unit-sized icon primitives (checkmarks, chevrons, separators, resize handles) to use the new path helpers where it removes duplicated hand-coded line math
- [ ] Add `klog(LOG_INFO, "GFX", "path raster segments=%u")` diagnostics for early bring-up and regression triage
- [ ] Vendor-first verdict (2026-09-28): the path rasterizer is the vendored plutovg canvas (§7), not a new engine; the items above become integration on top of it
  - plutovg ships FreeType's scanline rasterizer and stroker with fill rules, joins, caps, dashes, clipping and gradients; keep `gfx_path_t` as a thin wrapper so callers never depend on plutovg directly
- [ ] Commit: `"gfx: path raster foundation -- stroke/fill, joins/caps, SVG-ready command stream"`

**Test checkpoint:** Filling and stroking a known multi-segment path produces stable pixels across repeated runs; vector checkmark and chevron helpers render identically at 16/24/32 px; serial shows `"GFX: path raster segments="` for test cases. Test on: QEMU WHPX + TCG; bare metal.

## 4. Theme-Aware Icon, Cursor, and Scalable Asset Pipeline

**Design:** [`icons.md#system-icons`](../../docs/design/icons.md#system-icons), [`icons.md#the-set`](../../docs/design/icons.md#the-set)

Unify the current image, icon, and cursor code into one size-selection and theme-selection policy that matches modern desktop stacks.

- [ ] Add `gfx_asset_desc_t` / `gfx_asset_variant_t` metadata covering kind, nominal size, DPI scale, theme variant, and source format (PNG, ICO, CUR, ANI, SVG, Xcur, IRES)
- [ ] Extend `icon_store.c` to resolve theme-name plus size plus scale, with inheritance/fallback rules and an explicit vector-to-raster path for scalable icons
- [ ] Extend cursor loading to select best-fit variants by nominal size and scale, and add CUR/ANI container support alongside the current Xcur path
- [ ] Define one icon and cursor theme search order plus fallback rule set so shell, apps, and Win32 wrappers all select assets the same way
- [ ] Add size and color-policy hooks so monochrome icons, high-contrast assets, and large-cursor modes reuse the same selection engine instead of ad hoc overrides
- [ ] Keep `image_load*()` as the raster entry point, but add format probing and metadata return paths so callers can reason about animation, intrinsic size, and source type
- [ ] Wire the 16 new original icons into the shell: `system_icon_t` entries plus the `irespack` accepted list (`docs/design/icons.md`)
  - Today only the original 10 names are packed, so irespack logs `[SKIP] Unknown icon` for the rest at every size
  - `icons.ires` is 4.2 MB with 10 icons x 9 sizes; pack only the sizes the shell draws (16, 24, 32, 48, 96) before adding 16 more
- [ ] SVG-first (decided 2026-09-28): system icons, cursors and shell glyphs ship as SVG and are rasterised on demand at the exact size, scale and theme through §7
  - `icons.ires` shrinks to a small PNG fallback for contexts that run before the SVG renderer (boot splash, BSOD, early desktop); PNG, ICO, BMP and JPEG stay fully supported for Win32 apps and user images
- [ ] Runtime theming: an icon's colours come from `docs/design/tokens.json` token names recorded in its SVG, so light, dark, accent and high contrast re-tint the same source instead of shipping separate files
- [ ] Commit: `"gfx: visual asset pipeline -- theme-aware icon/cursor selection, scalable asset metadata, CUR/ANI support"`

**Test checkpoint:** Requesting the same icon at 16/32/64 px returns deterministic best-fit variants; high-contrast and large-cursor modes resolve the expected asset family; serial shows `"GFX: asset resolve theme="` with chosen size and format. Test on: QEMU WHPX + TCG; bare metal.

## 5. Thumbnail, Preview, and Multi-Size Asset Cache

Move preview generation and thumbnail reuse into one shared service so File Manager, Start Menu, dialogs, and shell surfaces do not each invent their own caches.

- [ ] Add `thumb_cache_get()`, `thumb_cache_put()`, and `thumb_cache_invalidate_path()` in a new `src/kernel/thumb_cache.c` plus public header
- [ ] Generate thumbnails for image, icon, shortcut, and executable surfaces through the same asset pipeline with deterministic size buckets (16, 32, 64, 128, 256)
- [ ] Store preview metadata (source mtime/size/hash, variant size, theme) so stale thumbnails can be invalidated correctly after file or theme changes
- [ ] Add a background worker contract for expensive preview generation so UI draw paths can request placeholders and fill in asynchronously
- [ ] Provide a plain preview API for dialogs, taskbar, Start Menu, and future shell surfaces: `preview_render(path, kind, size)`
- [ ] Define cache persistence and cleanup policy so thumbnails survive normal usage but do not become silent disk leaks
- [ ] Icon raster cache: an icon is rasterised once per (content hash, pixel size, scale, theme, accent) and reused; on disk like Windows' `iconcache.db`, so it survives reboots
  - First use renders and stores; later uses read the cache; a theme or accent change re-renders only the icons currently on screen, lazily; entries for sources that no longer exist are evicted
  - Invariant with a unit test: an unchanged icon is never rasterised twice for the same key
- [ ] Commit: `"gfx: thumbnail cache -- preview API, async generation contract, deterministic invalidation"`

**Test checkpoint:** Re-requesting the same thumbnail hits the cache; modifying the source file invalidates and rebuilds the cached preview; serial shows `"GFX: thumb cache hit"` and `"GFX: thumb cache rebuild"` for the expected path. Test on: QEMU WHPX + TCG; bare metal.

## 6. Recorded Scene Lists and Deterministic Re-render

Add a retained "record once, replay many" graphics layer that lets Impossible OS outperform the ad hoc immediate-mode redraw patterns common in legacy stacks.

> [!TIP]
> Windows GDI and most Linux UI toolkits expose rich immediate-mode drawing, but they still leave a lot of invalidation and replay policy to each framework. Impossible OS can do better by making deterministic scene recording a first-class primitive for shell and Win32k consumers.

- [ ] Add `gfx_cmd_buf_t` / `gfx_scene_t` with record/replay for rect, rounded rect, path, text-run, icon, image, and layer operations
- [ ] Store dirty-region ownership at the command or layer level so scene replay can skip unaffected subtrees instead of repainting entire overlays
- [ ] Add hash-based cache validation so unchanged menu, tooltip, toast, and shell flyout scenes reuse prior raster output across frames
- [ ] Expose a small scene-recording API that shell overlays and Win32k painting can adopt incrementally rather than forcing a flag day rewrite
- [ ] Add `klog(LOG_INFO, "GFX", "scene replay dirty=%u")` observability to compare scene replay against current full-redraw paths
- [ ] Commit: `"gfx: scene recording -- command buffer replay, dirty-subtree reuse, shell/win32k bridge"`

**Test checkpoint:** Replaying an unchanged recorded scene reuses cached output and redraws fewer pixels than the equivalent immediate-mode path; changing one child invalidates only that subtree; serial shows `"GFX: scene replay dirty="` and a lower dirty count on the second frame. Test on: QEMU WHPX + TCG; bare metal.

---

## 7. SVG Runtime Renderer (Vendored plutovg and plutosvg)

> **Spawned-by:** root

**Design:** [`icons.md#system-icons`](../../docs/design/icons.md#system-icons)

Impossible OS is SVG-first: icons, cursors and shell glyphs are drawn from SVG at whatever size and DPI they are needed. Vendor-first evaluation (2026-09-28) chose plutovg (the general 2D vector canvas) plus plutosvg (the SVG parser and renderer on top of it): pure C, no C++ runtime, active upstreams, licences MIT plus FreeType FTL and stb public-domain/MIT for bundled files, all GPL-3.0 compatible. Rejected: ThorVG (the widest SVG coverage, but about 28,000 lines of C++ to port freestanding), lunasvg (C++ with the STL), nanosvg (unmaintained by its own README, no `use` or `clipPath`).

- [ ] Vendor plutovg and plutosvg under `src/libs/`, pinned to a release; read each LICENSE file (and plutovg's `FTL.TXT`) before committing; add rows to `src/libs/PROVENANCE.md` and `CREDITS.md` in the same commit
- [ ] Freestanding port: `malloc`/`realloc`/`free` onto `kmalloc` (with `pmm_alloc_contiguous` for large surfaces), `math.h` onto the kernel's float math, atomics for reference counts; no stdio; build with the kernel's flags
- [ ] Implement `clipPath` in plutosvg (it is parsed but stubbed upstream) by porting the MIT clip logic from lunasvg, which draws on the same plutovg canvas; offer the patch upstream
- [ ] Define the system SVG profile the OS promises to render, in `docs/design/icons.md`
  - Inside: paths and basic shapes, fills and strokes with joins/caps/dashes, linear and radial gradients (both `gradientUnits`, `gradientTransform`), opacity, transforms, `defs`/`use`/`symbol`, `clipPath`
  - Outside the profile: masks, filters, text, CSS, scripts, animation, external references. The build rejects system icons that use them; third-party SVG outside the profile still renders as far as plutosvg goes, never crashes
- [ ] Parser hardening for untrusted SVG (app icons arrive inside executables): bounded nesting, element and path-command counts, `use` recursion depth, and a pixel-size cap; fuzz the parser on the host
- [ ] `svg_render(svg, len, px, scale, theme, out_surface)` into a `gfx_surface_t`, applying the theme's token colours (§4)
- [ ] Unit tests: every system icon renders without error at 16 and 256 px; a reference image comparison against the host renderer (`rsvg-convert`) within a small tolerance; malformed and hostile inputs are rejected cleanly
- [ ] Commit: `"gfx: SVG runtime -- vendored plutovg + plutosvg, clipPath, system SVG profile"`

**Test checkpoint:** the desktop draws every system icon from SVG at 100%, 150% and 200% scale, crisp at each; a malformed SVG from a test EIF is rejected without a crash. Test on: QEMU TCG, VirtualBox, bare metal.

## 8. Icon Engine (Build-Time Generator and Validator)

> **Spawned-by:** root

**Design:** [`icons.md#the-set`](../../docs/design/icons.md#the-set), [`icons.md#adding-or-changing-an-icon`](../../docs/design/icons.md#adding-or-changing-an-icon)

A host tool that builds the icon set from shared parts, so the set stays consistent (This PC and Network already share one monitor), validates every icon against the system SVG profile (§7), and never loses a hand-polished icon. It replaces the one-off scripts (`scripts/site/gen_recycle_bin_full.py`).

- [ ] Three modes, recorded per icon, so polish always persists: hand-authored, generated, and per-size overrides
  - **Hand-authored:** the SVG is the source; the engine only validates it and renders fallbacks, never rewrites it
  - **Generated:** a definition is the source. **Per-size override:** a hand-tuned file for one size always wins over generation
- [ ] Parts library: parametric SVG components (monitor, folder, document, drive, bin, badges and overlays) whose colours are token names from `docs/design/tokens.json`; icon definitions compose parts, overlays and parameters
- [ ] Deterministic output: the same definition always produces byte-identical SVG (fixed seeds for any procedural detail, such as the recycle bin's crumpled paper)
- [ ] Incremental: fingerprint each icon's inputs (definition, parts used, tokens used, engine version) and regenerate only icons whose fingerprint changed; an ordinary build touches nothing
- [ ] Direct-edit protection: record the fingerprint of every generated file; a hand edit to generated output fails the check with "move this change into the definition, or mark the icon hand-authored", and is never silently overwritten
- [ ] Validation for every icon (all modes): system SVG profile only; token colours only (no stray hex); gradient IDs prefixed with the icon name; legible at 16 px against the small-size rules
- [ ] Renders the PNG fallback set and updates `resources/icons/color/SOURCES.sha256`; wired into `scripts/site/build.py --check` so a stale render fails the commit
- [ ] Migrate: This PC and Network share the monitor part; folders share the folder part; the recycle bin becomes a definition (its paper generator becomes a part)
- [ ] Unit tests: determinism (two runs give identical bytes), incremental rebuild of exactly the changed icons, direct-edit detection, and that a hand-authored icon is never modified
- [ ] Commit: `"tools: icon engine -- parts, deterministic incremental generation, validation, fallback renders"`

**Test checkpoint:** changing the monitor part regenerates exactly This PC and Network and nothing else; a second run changes nothing; hand-editing a generated SVG fails the check with the move-it-to-the-definition message; a hand-authored icon survives an engine run byte-for-byte. Test on: WSL2 dev host.

## 9. Missing Icons -- Detect, Draft-Generate, and Queue for Polish

> **Spawned-by:** root

**Design:** [`icons.md#what-happens-when-an-icon-is-missing`](../../docs/design/icons.md#what-happens-when-an-icon-is-missing), [`icons.md#drafts-awaiting-polish`](../../docs/design/icons.md#drafts-awaiting-polish)

Decided 2026-09-28: when any section, the unattended run included, needs an icon that does not exist, a draft is generated in the same change instead of shipping the generic fallback; polish comes later and persists. The spec rule already binds today (drafts are hand-drawn SVGs until §8 exists); this section automates detection and generation so a missing icon cannot slip through. It does NOT re-cover §8's parts, modes, determinism or validation; it adds the fourth state (draft) and the trigger.

- [ ] Icon reference inventory: a host scanner lists every icon a build can ask for
  - `system_icon_t` names in `include/icon_store.h`, literal `icon_get_by_name("...")` arguments under `src/` and `user/`, EIF app-icon resources, and the rows of `docs/design/icons.md` The set
  - One JSON inventory under `build/`, deterministic, reused by the check and the engine
- [ ] Missing-icon check: every inventoried name must have a source (hand-authored SVG or engine definition) and a spec row; ships as a lint WARN, promoted to ERROR once the corpus is clean
  - Refusal control: a fixture that references `icon_get_by_name("no_such_icon")` fails; a name with both a source and a row passes
- [ ] Draft mode in the engine (a fourth state beside hand-authored, generated, per-size override): `status: draft` in the icon's definition, generated from the nearest parts and a named metaphor, passing every §8 validation
- [ ] `icon-engine draft <name> --metaphor <part>[,<overlay>] --needed-by <todo#section>` writes the definition, the spec row and the Drafts awaiting polish row in one step, so the run needs no hand drawing
- [ ] Promotion: polishing a draft (a definition edit, or a hand edit that makes it hand-authored) removes its drafts row; the check refuses a drafts row whose icon is no longer a draft, and a draft with no row
- [ ] The /design page and the docs coverage page show drafts with a "draft" badge, so what still needs polish is visible without reading the table
- [ ] Unit tests: inventory finds all four reference kinds; missing check pass and fail; draft generation is deterministic and validates; promotion clears the row; a hand edit of a draft is never overwritten
- [ ] Commit: `"tools: missing-icon check and draft icons -- inventory, draft mode, promotion"`

**Test checkpoint:** adding `icon_get_by_name("printer")` with no source fails the check naming `printer`; `icon-engine draft printer --metaphor document,badge` then produces a validated SVG, a spec row and a drafts row, and the check passes; a second run changes nothing; hand-editing the draft marks it hand-authored and removes the drafts row. Test on: WSL2 dev host.

## OS Comparison

| ⭐  | Feature                            | 🪟 Win11                    | 🐧 Linux                     | 🚀 Impossible OS |
| --- | ---------------------------------- | --------------------------- | ---------------------------- | ---------------- |
| 💎  | PMM-safe render targets            | ✅ DComp/D2D surfaces       | ✅ Cairo/Skia surfaces       | ⬜ Planned - §1  |
| 💎  | Clip + transform state             | ✅ GDI/D2D state            | ✅ Cairo/Qt painter          | ⬜ Planned - §2  |
| 💎  | Vector path + SVG-ready raster     | ✅ Direct2D/SVG paths       | ✅ Cairo + SVG loaders       | ⬜ Planned - §3  |
| 💎  | Theme-aware icon/cursor pipeline   | ✅ ICO/CUR + shell DPI      | ✅ Freedesktop themes        | ⬜ Planned - §4  |
| 💎  | Shared thumbnail service           | ✅ Shell thumbnails         | ✅ Tracker/GIO previews      | ⬜ Planned - §5  |
| ⭐  | Recorded deterministic scene lists | ⚠️ Framework-specific       | ⚠️ Toolkit-specific          | ⬜ Planned - §6  |
| ⭐  | SVG-first system icons and cursors | ⚠️ ICO bitmaps, fixed sizes | ⚠️ SVG themes, bitmap caches | ⬜ Planned - §7  |
| ⭐  | Icon engine with polish guarantees | ❌ Hand-made per size       | ❌ Hand-made per theme       | ⬜ Planned - §8  |

After §1-§5, Impossible OS reaches parity with the reusable graphics and asset layers that modern Windows and Linux desktop stacks already depend on. After §6, it gains a cleaner replay model that keeps shell and Win32k code fast without spreading invalidation policy across every consumer.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_gfx_assets()` -- register in `src/kernel/test/test_runner.c`.
> This TODO likely needs a new `TEST_CAT_DESKTOP` or `TEST_CAT_GFX` category plus `cat_names[]`, `cat_labels[]`, and `make test-desktop` wiring because the existing categories do not cleanly cover reusable graphics/asset infrastructure.

- [ ] Create `src/kernel/test/test_gfx_assets.c` with:
  - `gfx_surface_create_ex()` allocates and frees both small and large surfaces with the expected ownership flags
  - clip and transform state return the expected rects and pixel coordinates after push/pop
  - a known `gfx_path_t` stroke/fill raster produces the expected bounding box and sample pixels
  - asset resolution picks the expected icon/cursor variant for `(theme, size, scale)`
  - thumbnail cache hits after the first build and invalidates on source metadata change
- [ ] Register in `test_runner_init()`: `test_register_gfx_assets()`
- [ ] Commit: `"test: add graphics asset foundation test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] Serial shows `"GFX: surface_ex alloc"` and `"GFX: surface_ex free"` for a large render-target test
- [ ] Serial shows `"GFX: path raster segments="` for vector-path test cases
- [ ] Serial shows `"GFX: asset resolve theme="` for icon/cursor selection and `"GFX: thumb cache hit"` for thumbnail reuse
- [ ] Render-target, clip-state, path, and asset tests pass via the new graphics/desktop test category
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal
