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
- -> XREF: `TODO-18-ui-framework-engines.md §12` -- the composition visual tree draws through the §1 render targets and the §2 clip and transform stack

## Outcome

- Large off-screen surfaces, cached layers, and asset buffers use one explicit PMM-aware render-target API instead of scattered `kmalloc` assumptions.
- `gfx_*` gains clip-stack, transform, and path/vector state so scalable assets and future Win32k path APIs have a native implementation target.
- Images, icons, and cursors share one theme-aware, DPI-aware asset policy with scalable-vector fallbacks and deterministic size selection.
- Thumbnail and preview generation become reusable services instead of per-app ad hoc code.
- Shell, boot splash, apps, and Win32k all consume one graphics-asset foundation with clear ownership boundaries.

## Implementation Order

| ⭐  | Order | Deliverable                                                                              | Depends On   | Status |
| --- | :---: | ---------------------------------------------------------------------------------------- | ------------ | :----: |
| 💎  |   1   | §1 Render-target allocator, views, and cached layers                                     | --           |  [ ]   |
| 💎  |   2   | §2 Clip, transform, and state stack                                                      | §1           |  [ ]   |
| 💎  |   3   | §3 Path, stroke, fill, and SVG-ready vector raster contract                              | §2           |  [ ]   |
| 💎  |   4   | §4 Theme-aware icon, cursor, and scalable asset pipeline                                 | §1, §3       |  [ ]   |
| 💎  |   5   | §5 Thumbnail, preview, and multi-size asset cache                                        | §4           |  [ ]   |
| ⭐  |   6   | §6 Recorded scene lists and deterministic re-render for shell and Win32k                 | §2, §3, §5   |  [ ]   |
| 💎  |   7   | §7 SVG runtime -- vendored plutovg + plutosvg, clipPath, system SVG profile              | §3, §4       |  [ ]   |
| 💎  |   8   | §8 Icon engine -- parts, deterministic incremental generation, validation                | §4, §7, §12  |  [ ]   |
| ⭐  |   9   | §9 Missing icons -- detect, draft-generate, queue for polish                             | §8, §11, §12 |  [ ]   |
| ⭐  |  10   | §10 Automatic icon polish and promotion -- rubric, scoring, review sheet, no manual step | §9, §12      |  [ ]   |
| ⭐  |  11   | §11 Icon and cursor contract for Impossible Brand -- generated, drift-checked            | --           |  [x]   |
| ⭐  |  12   | §12 Brand import kind -- explicit resource mappings, upgrade keeps other resources       | --           |  [ ]   |
| ⭐  |  13   | §13 Switch-over to the brand pack -- reconcile, import, split the icon spec              | §11, §12     |  [ ]   |
| ⭐  |  14   | §14 Adopt redesigned defaults from the brand pack -- wallpaper and cursor theme          | §12, §15     |  [ ]   |
| 💎  |  15   | §15 Asset loader and packer integrity -- validated, fuzzed, fail-loud build              | --           |  [ ]   |
| ⭐  |  16   | §16 Verified icon render and install pipeline on the contract                            | §11          |  [ ]   |
| ⭐  |  17   | §17 Compiler-evaluated brand contract inputs                                             | §11          |  [ ]   |

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
- [ ] Runtime theming: icon colours are `var(--<token>)` names from `docs/design/tokens.json` (§11 profile), so light, dark, accent and high contrast re-tint one source instead of shipping separate files
- [ ] Fix the Xcursor loader's temp-buffer release (`cursor.c:485,493`): it passes a frame number to `pmm_free_frame()`, which takes an address, so it frees low frames and leaks the buffer
  - Release the buffer with `pmm_free_contiguous()` on both the short-read and the normal path
  - Found while writing the docs pages (`00-infrastructure/TODO-10-documentation-site.md` §16); verified at source, not reproduced at runtime
- [ ] Bound each decoded Xcursor image to its 24x24 pool slot (`cursor.c:418-438`, `:519-525`): the size nearest 24 is copied whatever its dimensions, up to 256x256
  - A cursor file without a 24 px image overflows into the next shape's slot and later past the 48x48 `saved_under` buffer in `cursor_draw()`
  - Reject or downscale oversize images; a unit test feeds a 32 px-only file
  - Found while writing the docs pages (`00-infrastructure/TODO-10-documentation-site.md` §16); verified at source, not reproduced at runtime
- [ ] Bounds-check the ICO and `icons.ires` parsers against the file size: `ico.c:242` computes `offset + dsize` in 32 bits (wraps), and `icon_store.c:186-195` trusts `icon_count` and `name_offset`
  - `decode_bmp_dib()` also never checks the pixel rows and AND mask against the entry size
  - Both parse files read from disk; `ico.c` has no callers yet, `icons.ires` loads at every desktop start
  - Found while writing the docs pages (`00-infrastructure/TODO-10-documentation-site.md` §16); verified at source, not reproduced at runtime
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
- [ ] Fix the stb_image realloc shim before thumbnails decode user files: define `STBI_REALLOC_SIZED` and copy `min(old, new)` bytes (`src/kernel/image.c:136-163`)
  - Found 2026-09-29 by the Codex review of the networking docs pass (`00-infrastructure/TODO-10` section 15), verified at source: `pmm_track_size()` returns 0 for a `kmalloc` block, so `stbi_realloc_wrapper()` copies `new_size` bytes out of the smaller old buffer.
  - stb_image sizes its PNG IDAT buffer at max(first chunk, 4096) and doubles it through that path (`include/stb_image.h:5184-5191`), so a PNG whose first IDAT chunk fits in 4 KiB and whose later chunks outgrow it makes the kernel read past a heap block. Test: such a multi-IDAT PNG decodes correctly. Blocked consumers that point here: `07-networking/TODO-07-web-browser.md` §5 and `07-networking/TODO-10-pdf-viewer.md` §6.
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
- [x] Define the system SVG profile the OS promises to render: `docs/design/svg-profile.json`, cited by `docs/design/icons.md` (shipped in §11)
  - Inside: paths and basic shapes, fills and strokes with joins/caps/dashes, linear and radial gradients (both `gradientUnits`, `gradientTransform`), opacity, transforms, `defs`/`use`/`symbol`, `clipPath`
  - Outside the profile: masks, filters, text, CSS, scripts, animation, external references. The build rejects system icons that use them; third-party SVG outside the profile still renders as far as plutosvg goes, never crashes
- [ ] Parser hardening for untrusted SVG (app icons arrive inside executables): the bounds in `svg-profile.json` `limits` (Q64), and a pixel-size cap; fuzz the parser on the host
- [ ] `svg_render(svg, len, px, scale, theme, out_surface)` into a `gfx_surface_t`, applying the theme's token colours (§4)
- [ ] Unit tests: every system icon renders without error at 16 and 256 px; a reference image comparison against the host renderer (`rsvg-convert`) within a small tolerance; malformed and hostile inputs are rejected cleanly
- [ ] Commit: `"gfx: SVG runtime -- vendored plutovg + plutosvg, clipPath, system SVG profile"`

**Test checkpoint:** the desktop draws every system icon from SVG at 100%, 150% and 200% scale, crisp at each; a malformed SVG from a test EIF is rejected without a crash. Test on: QEMU TCG, VirtualBox, bare metal.

## 8. Icon Engine (Build-Time Generator and Validator)

> **Spawned-by:** root

Moving to Impossible Brand ([brand split design](https://github.com/rizonesoft/ImpossibleBrand/blob/main/docs/specs/2026-11-25-brand-split-design.md)): the OS keeps the interface and Impossible Brand builds the art tools. §12 registers the source and marks this section `**Moved:** impossible-brand`, after which its items are kept in Impossible Brand's roadmap; until then this section is not built here, which is why it depends on §12.

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

Moving to Impossible Brand ([brand split design](https://github.com/rizonesoft/ImpossibleBrand/blob/main/docs/specs/2026-11-25-brand-split-design.md)): the OS keeps the interface and Impossible Brand builds the art tools. §12 registers the source and marks this section `**Moved:** impossible-brand`, after which its items are kept in Impossible Brand's roadmap; until then this section is not built here, which is why it depends on §12.

**Design:** [`icons.md#what-happens-when-an-icon-is-missing`](../../docs/design/icons.md#what-happens-when-an-icon-is-missing), [`icons.md#drafts-awaiting-polish`](../../docs/design/icons.md#drafts-awaiting-polish)

Decided 2026-09-28: when any section, the unattended run included, needs an icon that does not exist, a draft is generated in the same change instead of shipping the generic fallback; polish comes later and persists. The spec rule already binds today (drafts are hand-drawn SVGs until §8 exists); this section automates detection and generation so a missing icon cannot slip through. It does NOT re-cover §8's parts, modes, determinism or validation; it adds the fourth state (draft) and the trigger.

- [x] Icon reference inventory: a host scanner lists every icon a build can ask for (taken over and shipped by §11, `scripts/brand_contract.py`)
  - Shipped as the `icons` array of `docs/design/brand-contract.json`; EIF app icons are `02-kernel-core/TODO-20` §12
  - Engine consumption of the inventory stays with this section's remaining items (pending)
- [ ] Missing-icon check: every inventoried name must have a source (hand-authored SVG or engine definition) and a spec row; ships as a lint WARN, promoted to ERROR once the corpus is clean
  - Refusal control: a fixture that references `icon_get_by_name("no_such_icon")` fails; a name with both a source and a row passes
- [ ] Draft mode in the engine (a fourth state beside hand-authored, generated, per-size override): `status: draft` in the icon's definition, generated from the nearest parts and a named metaphor, passing every §8 validation
- [ ] `icon-engine draft <name> --metaphor <part>[,<overlay>] --needed-by <todo#section>` writes the definition, the spec row and the Drafts awaiting polish row in one step, so the run needs no hand drawing
- [ ] No part fits: `icon-engine part new <name>` scaffolds a draft parametric part, so a draft icon composes a new reusable part instead of a one-off drawing
  - The part follows token colours, top-left light and the small-size rules
  - Floor when no credible part can be drawn: the tile-plus-glyph draft (standard rounded tile, category colour, primitive line glyph)
  - Draft parts get a drafts row; polishing a part re-generates every icon that uses it (§8 fingerprints)
  - Refusal control: a draft icon whose definition inlines raw paths instead of referencing a part fails validation, so one-offs cannot bypass the library
- [ ] Promotion: polishing a draft (a definition edit, or a hand edit that makes it hand-authored) removes its drafts row; the check refuses a drafts row whose icon is no longer a draft, and a draft with no row
- [ ] The /design page and the docs coverage page show drafts with a "draft" badge, so what still needs polish is visible without reading the table
- [ ] Unit tests: inventory finds all four reference kinds; missing check pass and fail; draft generation is deterministic and validates; promotion clears the row; a hand edit of a draft is never overwritten
- [ ] Commit: `"tools: missing-icon check and draft icons -- inventory, draft mode, promotion"`

**Test checkpoint:** adding `icon_get_by_name("printer")` with no source fails the check naming `printer`; `icon-engine draft printer --metaphor document,badge` then produces a validated SVG, a spec row and a drafts row, and the check passes; a second run changes nothing; hand-editing the draft marks it hand-authored and removes the drafts row. Test on: WSL2 dev host.

## 10. Automatic Icon Polish and Promotion

> **Spawned-by:** root

Moving to Impossible Brand ([brand split design](https://github.com/rizonesoft/ImpossibleBrand/blob/main/docs/specs/2026-11-25-brand-split-design.md)): the OS keeps the interface and Impossible Brand builds the art tools. §12 registers the source and marks this section `**Moved:** impossible-brand`, after which its items are kept in Impossible Brand's roadmap; until then this section is not built here, which is why it depends on §12.

**Design:** [`icons.md#what-happens-when-an-icon-is-missing`](../../docs/design/icons.md#what-happens-when-an-icon-is-missing), [`icons.md#small-size-rules`](../../docs/design/icons.md#small-size-rules)

Decided 2026-11-14 (owner direction: no manual icon work, and icons are finished to a premium standard): a draft is polished and promoted by the run itself, in the section that needed it. No person has to review, approve or edit an icon; a person may reject one.

- [ ] Write the scoring rubric in `docs/design/icons.md` ("How is an icon judged?"), each point with a pass rule
  - Grid and optical size, single top-left light, palette tokens only, stroke and corner consistency with the set
  - Legible at 16, 20, 24, 32 and 48 pixels in light and dark, and distinct from every other icon at 16 pixels
  - Reads as its object without its label, and matches the neighbours it is shown beside
- [ ] `icon-engine score <name>`: runs the mechanical checks (grid, palette, profile, contrast against both themes, pixel distance from every sibling at 16 pixels) and writes `build/icons/<name>.score.json`
- [ ] `icon-engine sheet <name>`: renders one review sheet (every size, light and dark, beside the finished icons of the surface it belongs to) to `build/icons/<name>.sheet.png`
- [ ] Polish pass in the run: read the sheet and the mechanical score, judge the icon against the rubric, revise its definition or parts, and re-score, for at most 5 rounds
- [ ] Promotion: an icon that passes every rubric point becomes `status: generated`, its drafts row is removed and the score is recorded beside its row in The set
  - One that fails the last round stays a draft with the failing points written in its drafts row
- [ ] A section that draws a surface is not done while an icon it added is a draft: `icon-engine drafts --needed-by <todo#section>` must print none before the section's commit, unless the drafts row records a failed final round
- [ ] Rejection without editing: `icon-engine reject <name> "<reason>"` returns a promoted icon to draft with the reason as an extra rubric point, and the next polish pass must satisfy it
- [ ] Icon gallery page: `gh-pages/design/icons.html`, generated by the site build from the icon set and `build/icons/*.score.json`, published at impossibleos.co/design/icons.html on every push
  - Every icon at every size in light and dark, with its state (draft, generated, hand-authored), its score per rubric point and the section that needed it; drafts and rejected icons listed first
  - The same file opens locally from `_site/design/icons.html` after `python3 scripts/site/build.py`, so it can be read without the network
- [ ] Unit tests: an off-palette icon and an off-grid icon each fail scoring and name the point; a passing icon is promoted and its row removed; a rejected icon returns to draft and keeps the reason
- [ ] Commit: `"icons: automatic polish, scoring and promotion of draft icons"`

**Test checkpoint:** `icon-engine score` on the fixture off-palette icon exits 1 naming `palette`; on a passing fixture it exits 0 and `icon-engine drafts` no longer lists it. `python3 scripts/site/build.py --check` passes with the rubric section present.

## 11. Icon and Cursor Contract for Impossible Brand

> **Spawned-by:** root

**Design:** [`icons.md#how-are-icons-rendered`](../../docs/design/icons.md#how-are-icons-rendered), [`icons.md#the-set`](../../docs/design/icons.md#the-set)

Impossible Brand draws the art; the OS owns what the art must satisfy ([brand split design](https://github.com/rizonesoft/ImpossibleBrand/blob/main/docs/specs/2026-11-25-brand-split-design.md#what-is-the-contract-this-repository-validates-against)). This section turns that interface into one generated file, `docs/design/brand-contract.json`, which Impossible Brand copies at a named OS commit (`tools/contract.py pull`) and validates every icon against. It also takes over the icon reference inventory from §9, because the inventory reads OS code and stays here when §9 moves.

- [x] System SVG profile as data in `docs/design/svg-profile.json`, cited by `icons.md#how-are-icons-rendered`
  - Elements with their attributes, `forbidden`, the `paint` grammar (`none`, hex, `var(--token)`, local gradient), `references` and `prolog`
  - `geometry_cost` and `limits` (`use_depth` 8, `nesting` 32, expanded `elements` 4096 and `path_commands` 65536; Q64)
- [x] Icon reference inventory (taken over from §9) in `scripts/brand_contract.py` `generate()`, keyed (name, provider)
  - Reads `system_icon_t` numerically, `icon_names[]`, `icon_codepoints[]`, `accepted_icons[]`, `icon_get_by_name` literals, The set
  - Colour entries carry `sources`, `used_for`, `lookup`, `required`, `wired` and `missing`; glyph entries need a non-zero codepoint
  - EIF app icons: none exist yet (`build-eif.py` writes no metadata); `02-kernel-core/TODO-20` §12 owns adding them
- [x] `scripts/brand_contract.py` writes `docs/design/brand-contract.json`: sorted keys, LF, no timestamps
  - Lists in a fixed order: icons glyph then colour, each by name; cursor shapes by enum slot
  - `limits` read from source: `ires_sizes` (incl. the `uint8_t` wire field), `name_bytes`, `pack_icons`, `name_table`, `ires_bytes`
  - `tokens` with per-theme values in SVG order (`#RRGGBBAA`) and `fallback_theme`; a token missing from one theme is refused
  - `cursors`: `CURSOR_GUEST_DIR` (the Makefile variable the install recipe uses), the loader's frame rule (Q65), shapes, files
- [x] Refusals name what is wrong: enum holes, same-half duplicates, glyphs without a codepoint, names over a limit, sizes outside 1..limit
  - Also refused: a full pack over the loader's byte limit, a required icon without a source, a packer list not ending in one `NULL`
  - And: any shipped SVG or cursor the validators reject, and a C initializer entry that is not exactly one literal
- [x] Behaviour pinned, data parsed (redesign after round 3, the regex class reopened three times): `BEHAVIOUR_PINS`
  - `xcur_parse`, `xcur_load`, `cursor_init`, `ires_load`, `icon_get_by_name`, irespack `main`, `is_known_icon`, `find_size_index` pinned
  - Name tables, enums (resolved to numeric slots), size and file lists and `#define` values parsed with strict grammars
  - Whitelist rule (rounds 5, 6): a form the reader does not model is refused, never read approximately
  - Exact array declarations; IRES structs identical in packer and loader and pinned; each macro `#define`d once, unconditionally
  - One top-level `:=` per Make variable; single plain literals (only `\\` escapes); unpadded paint; no CDATA
  - Round 7: macro values one literal or fully parenthesised; arithmetic `+` and `*` only; `IRES_COLOR_COUNT` exact; no Make conditionals
  - Round 8: extent structs pinned, decimal sizes, 31-bit intermediates, wire fields fit, no continuations; the compiled-value fix is §17
- [x] Host validators Impossible Brand mirrors: `validate_svg`, `validate_xcursor`, `resolve_paint` (no renderer needed)
  - Geometry read by linear left-to-right scanners, checked on every element; unprefixed elements only; unique temp file
  - Reference costs by an explicit-stack walk; every start tag read by the raw scanners; ASCII numbers; file mode kept
  - Linear by construction: one paint-independent cost summary per element, forward comment scan, no range built from a count
  - Shipped OS sources keep literal colours (the converter renders them as written); conditional literal calls refused
  - Pass 56 (Q66): a padded Make value refused, a declared encoding must be UTF-8, the gate needs all of its artifacts
- [x] `--check` names the first stale JSON path; `--staged` reads the index (`git grep --cached`)
  - Lint Check 30 dispatches `scripts/brand_contract_gate.sh` on one snapshot; a contract without gate or checker fails; `--check` compares bytes
- [x] Tests: `scripts/tests/test_brand_contract.py` (84), registered and collected in `scripts/test-tooling.sh`
  - Mutation controls run: dropping the duplicate-id, slot-fit, single-literal or paint-server charge check each fails its test
- [x] Commit: `"brand: generated icon and cursor contract for Impossible Brand"`

**Test checkpoint:** `python3 scripts/brand_contract.py --check` exits 0 on the committed tree; a fixture adding `icon_get_by_name("printer")` publishes `printer` as a `colour` entry with `missing: [irespack, kernel_table]` and makes `--check` exit 1 naming `$.icons`; `python3 -m pytest scripts/tests/test_brand_contract.py` passes. Test on: WSL2 dev host.

> **Test runner:** `python3 -m pytest -q scripts/tests/test_brand_contract.py` -- 84 passed (host only; no kernel source changed).

> **Notes:**
> - Shipped `scripts/brand_contract.py`, `docs/design/svg-profile.json` and the generated `docs/design/brand-contract.json`.
> - Check 30 runs `--check` (`--staged` under the commit gate); generation needs no renderer.
> - Impossible Brand pulls the contract from `docs/design/brand-contract.json` on `origin/main` (its D01 T01 §1 probe).
> - Canonical doc: `docs/design/icons.md#how-are-icons-rendered`; decisions Q64 and Q65 in `todo/answers.md`.
> - Moving the converter, asset validator and installers onto the contract is §16; kernel loader and packer hardening is §15.

## 12. Brand Import Kind -- Explicit Resource Mappings

> **Spawned-by:** root

**Design:** [`icons.md#the-set`](../../docs/design/icons.md#the-set)

Impossible Brand publishes the art as a pack pinned by commit ([brand split design](https://github.com/rizonesoft/ImpossibleBrand/blob/main/docs/specs/2026-11-25-brand-split-design.md#what-is-the-brand-pack)). `scripts/imports.py` today accepts one destination folder under `src/` per source and replaces it whole, so it cannot bring art into `resources/` without putting fonts and boot files at risk. This section adds a `brand` kind whose destinations are explicit, non-overlapping paths under `resources/`, and registers Impossible Brand as a source.

- [ ] `brand` kind in `scripts/imports.py` and `todo/imports.json`: a source of this kind carries `paths`, an object mapping a top-level path of the pack to one destination under `resources/`
  - The mappings: `icons/src` to `resources/icons/src`, `icons/color` to `resources/icons/color`, `cursors` to `resources/cursors`, `identity/png` to `resources/logo`, `identity/logo.svg`, the wordmarks, `identity/src` and `identity/shortcuts` to `resources/brand`, `wallpapers` to `resources/backgrounds`
  - Check 42 refuses a destination outside `resources/`, two mappings that overlap, a destination that holds `resources/fonts` or `resources/boot`, and a copy whose tree differs from the pinned one at any mapped destination
  - `--pull impossible-brand` replaces only the mapped destinations; every other file under `resources/` keeps its bytes
- [ ] Register `impossible-brand` (repository `ImpossibleBrand`, kind `brand`) with this section as its import section, carrying `**Import:** impossible-brand`
- [ ] Mark §8, §9 and §10 `**Moved:** impossible-brand` with their rows under `moved` in `todo/imports.json`
  - As the app sections were closed behind their import on 2026-11-23; their items are then kept in Impossible Brand's roadmap
  - The inventory item of §9 stays here with §11
- [ ] Unit tests for the brand kind: upgrade isolation, refused mappings, and a build that still packs
  - An upgrade fixture with a changed icon and a changed cursor leaves `resources/fonts`, `resources/boot` and `resources/system` byte-identical
  - An overlapping mapping and a mapping into `resources/fonts` are each refused; after the pull the build still produces `icons.ires` and the boot logo PNGs
- [ ] Commit: `"imports: brand kind with explicit resource mappings, Impossible Brand registered"`

**Test checkpoint:** `python3 scripts/imports.py --check` exits 0 with `impossible-brand` registered; the upgrade fixture test passes and fails if `--pull` is changed to replace `resources/` whole. Test on: WSL2 dev host.

## 13. Switch-over to the Brand Pack

> **Spawned-by:** root

**Design:** [`icons.md#what-happens-when-an-icon-is-missing`](../../docs/design/icons.md#what-happens-when-an-icon-is-missing), [`icons.md#adding-or-changing-an-icon`](../../docs/design/icons.md#adding-or-changing-an-icon)

The art was seeded into Impossible Brand from OS commit `75c468c32` on 2026-11-25 ([brand split design](https://github.com/rizonesoft/ImpossibleBrand/blob/main/docs/specs/2026-11-25-brand-split-design.md#in-what-order), phase 4). This section makes the pinned pack the only source of the OS's art, keeps only the interface half of the icon spec here, and routes a missing icon through Impossible Brand's pinned engine instead of drawing it in this repository.

- [ ] Three-way reconciliation of every art path against the seed (`75c468c32`), recording the reconciled OS commit in `todo/imports.json`
  - Each OS change since the seed, including deletions and renames, is carried into Impossible Brand without overwriting work done there
  - If the OS art changes again before the pull, reconcile again first
- [ ] Pull the pack (`--pull impossible-brand`, §12) so the mapped destinations become the imported copies
  - A hand edit of OS art then fails Check 42 and is made in Impossible Brand instead
  - `resources/logo.svg` and `gh-pages/logo.svg` are derived from the imported logo by the site build, never copied by hand
- [ ] Split `docs/design/icons.md` into the interface half, which stays, and a link to Impossible Brand's `docs/icons.md` for the art
  - Stays: names, provider, the SVG profile, tokens, sizes, rendering, and how a name is wired into the shell
  - Goes to Impossible Brand: grid, palette, light, small-size rules and the drawings
- [ ] Missing-icon flow through the pinned Impossible Brand engine, drafting into `resources/brand-drafts/` and requesting in `docs/design/brand-requests.json`
  - The §11 inventory finds a `colour` name the pinned pack lacks; the engine from the pinned commit drafts it from existing parts only, and the build packs it like any icon
  - The request records name, kind, what, metaphor, needed by and the draft digest
  - Sync on a pin move: an unmodified draft the pack now holds as finished is removed with its request in the same commit; an edited draft is kept and reported, never deleted
  - The check fails a draft that duplicates a finished pack icon and still matches its digest
- [ ] Retire `scripts/convert-icons.sh` for Impossible Brand's renderer; until then sources keep literal colours (§11)
- [ ] Update the `CLAUDE.md` paragraph on original artwork and `.claude/skills/` icon guidance to point at Impossible Brand
- [ ] Unit tests: a fixture missing `printer` produces a validated draft and one request; a pin move that brings `printer` removes the unmodified draft and its request; an edited draft survives the same move
- [ ] Commit: `"brand: the OS art comes from the Impossible Brand pack"`

**Test checkpoint:** after the pull, `bash scripts/build.sh` ends `=== BUILD OK ===` and the desktop shows the same icons and cursors as before at 100% and 200%; `python3 scripts/imports.py --check` and `python3 scripts/site/build.py --check` exit 0. Test on: WSL2 dev host, QEMU TCG.

## 14. Adopt Redesigned Defaults from the Brand Pack

> **Spawned-by:** root

**Design:** [`icons.md#what-about-cursors`](../../docs/design/icons.md#what-about-cursors), [brand split design](https://github.com/rizonesoft/ImpossibleBrand/blob/main/docs/specs/2026-11-25-brand-split-design.md#what-is-the-brand-pack)

The pinned Impossible Brand pack names which of its assets are the shipped defaults, so a redesign reaches users by moving the pin, never by hand-copying files. The pack's manifest carries a `defaults` map: the wallpaper pair (light and dark), the cursor theme, and later sounds or other themed sets. Today the default wallpaper is the literal `C:\Impossible\Web\Wallpaper\default.jpg`, written by `src/kernel/registry.c` into `HKLM\SYSTEM\Theme\Wallpaper` and `HKU\Default\Desktop\Wallpaper` and read by `src/desktop/desktop.c`; the cursor is one fixed Adwaita set named in `src/kernel/drivers/cursor.c` `cursor_filenames[]`, with no theme setting at all.

- [ ] Import reads the pack's `defaults` map when the pin moves (§12) and refuses a default that names an asset missing from the pack
- [ ] Shipped default wallpaper comes from `defaults.wallpaper` (light and dark pair)
  - `registry.c` seeds `HKLM\SYSTEM\Theme\Wallpaper` and the default user's value from a generated header, not a literal
  - The light or dark member follows the theme, as `08-graphics-ui/TODO-03-theme-system.md` specifies
- [ ] Shipped default cursor theme comes from `defaults.cursor_theme`, stored as `HKLM\SYSTEM\Theme\CursorTheme`
  - Three themes ship: "Windows 11 style" (the new default), "Mac style" and "Linux style" (Adwaita)
  - Each theme is a folder under `C:\Impossible\System\Cursors\<theme>\`
- [ ] Cursor loader reads the theme folder named by `CursorTheme`, falling back to the default theme, then to the built-in cursors
  - Owned in `src/kernel/drivers/cursor.c` (`cursor_filenames[]` becomes a per-theme path); -> XREF: §15 for the hardened parser it loads through
- [ ] Upgrade keeps an explicit user choice: only an unset value or a fresh install takes the new default
  - A value the user set (wallpaper or cursor theme) is never overwritten by a pin move
- [ ] Settings > Personalization lists every pack wallpaper pair and every cursor theme, so earlier defaults stay selectable
  - -> XREF: `09-desktop-shell/TODO-11-control-panel.md` `desk.cpl` wallpaper picker and `main.cpl` cursor theme picker
- [ ] Unit tests: a fixture pack with a new default wallpaper changes the shipped default, leaves an explicit choice alone, and keeps the old wallpaper in the picker list; a missing default asset fails the import
- [ ] Commit: `"brand: shipped defaults follow the pack's defaults map"`

**Test checkpoint:** with a fixture pack whose `defaults.wallpaper` names a new pair, a fresh image boots to that wallpaper, an image whose user set a wallpaper keeps it after the upgrade, and the picker lists both pairs; a fixture whose default names a missing file makes `python3 scripts/imports.py --pull impossible-brand` exit 1 naming it. Test on: QEMU TCG.

## 15. Asset Loader and Packer Integrity

> **Spawned-by:** section 11 (review)
> **User impact:** Every desktop boot, the cursor loader frees an unrelated physical frame (it passes a frame number to pmm_free_frame) and leaks its own buffer, so memory still in use can be handed out again and crash or corrupt the system unpredictably.

**Design:** [`icons.md#how-are-icons-rendered`](../../docs/design/icons.md#how-are-icons-rendered)

The plan review of §11 (2026-11-25, eight passes) found that every boundary an asset crosses on its way to the screen trusts its input: the Xcursor parser, the cursor temp-buffer release, the IRES loader, `irespack` and the `sysroot-icons` recipe. Fixing them one case at a time kept reopening the same class, so this section closes it the way the repository closes untrusted-byte parsers: each parser becomes a pure validator with a host fuzz target, and the build steps around them fail loudly.

- [ ] Xcursor parser as a pure validator `xcur_validate` in `src/kernel/drivers/cursor.c`, fuzzed by `tools/fuzz/fuzz_xcursor.c`
  - Header span first, then `toc_count <= (file_size - header_size) / 12`, then every chunk and payload by remaining length, never wrapping `a + b > size`
  - Only frames with `w * h` within the slot capacity and the hotspot inside the frame are ranked; the nominal size closest to `XCUR_TARGET_SIZE` wins
  - A file with no fitting frame returns -1 and the shape keeps its built-in cursor
  - Distance by defined unsigned arithmetic (today `(int32_t)subtype` lets high-bit nominals rank as near sizes)
  - Same commit: §11's published `cursors.rules.selection` becomes `closest_fitting` and its nominal ceiling is lifted
- [ ] Cursor buffers through `pmm_alloc_pages_hhdm` and `pmm_free_contiguous(phys, pages)` on every exit of `xcur_load`
  - Replaces `pmm_free_frame(((uintptr_t)buf / 4096) + p)`, which passed a frame number where a byte address is expected
  - That clears an unrelated frame's bit and leaks the buffer on every desktop boot
- [ ] IRES loader validation as a pure `ires_validate` in `src/kernel/icon_store.c`, fuzzed by `tools/fuzz/fuzz_ires.c`
  - The read must match the header's `file_size`; size table, index, terminated names and every pixel span are checked against those bytes
  - Loader state is published only after validation succeeds; the 32 MiB bound becomes the named `IRES_MAX_FILE_BYTES`
- [ ] `tools/irespack.c` output integrity
  - Writes a sibling temporary file, checks every write and `fclose`, and renames over the destination only on success
  - Refuses a discovered PNG that fails to decode or whose dimensions differ from its size directory; an absent variant stays legal
  - `_Static_assert(SIZE_COUNT <= MAX_SIZES)`; refuses an archive over `IRES_MAX_FILE_BYTES` and a name table of 65536 bytes or more
- [ ] `tools/mkfs-ixfs.c` copies each file through one descriptor: `open`, `fstat` on it, read exactly that length, fail on a short read (today `stat(path)` then `fopen(path)`)
- [ ] Unit tests (kernel, desktop category): a 32x32-only cursor with a 576-pixel slot is refused and a sentinel past the slot is unchanged; a 32 frame listed before a 24 frame picks 24; an out-of-range hotspot picks the valid alternate
- [ ] More unit tests: an out-of-range `toc_count`, a header past the end and a chunk near `UINT32_MAX` refused; `xcur_load` frees its extent on success and on a short file; a truncated IRES index and a truncated final payload refused
- [ ] Host tests (pytest, `tools/irespack.c` built into tmp_path): write failure keeps the old archive and exits non-zero; corrupt PNG, wrong-size PNG and over-limit archive refused (the `sysroot-icons` recipe itself is §16's)
- [ ] Commit: `"assets: cursor and IRES loaders validated and fuzzed; irespack fails loudly"`

**Test checkpoint:** `bash tools/fuzz/run.sh` includes `fuzz_xcursor` and `fuzz_ires` and finds nothing in its run; `make test-desktop` passes the cursor and IRES cases; `python3 -m pytest scripts/tests/test_irespack.py` passes; the smoke matrix boots with the shipped cursors and icons. Test on: WSL2 dev host, QEMU TCG and KVM.

## 16. Verified Icon Render and Install Pipeline on the Contract

> **Spawned-by:** section 11 (split)

**Design:** [`icons.md#how-are-icons-rendered`](../../docs/design/icons.md#how-are-icons-rendered), [`icons.md#adding-or-changing-an-icon`](../../docs/design/icons.md#adding-or-changing-an-icon)

§11 publishes the contract; this section makes every consumer in the OS read it, so none keeps its own copy of a list or a limit. A complete draft was built and reviewed during §11 on 2026-11-25 and taken out of that commit to keep §11 to the contract (operator scope decision): `scripts/icon_render.py` (converter, verifier, `install`, `install-cursors`) and its 24 tests, with the review findings below still open. Its §11 plan passes (41) and first round are the design record.

**Needs:** operator

The one check that needs the operator: the pinned CI image (`ghcr.io/rizonesoft/impossible-os-ci`) gains `librsvg2-bin` and its new digest is pinned in `build.yml` and `release.yml` before `rsvg-convert` becomes a required tool (operator scope decision, 2026-11-25: §11 makes nothing require it). Everything else in this section builds without it.

- [ ] `scripts/icon_render.py render`: full canonical render or `--src DIR --out DIR` into a separate root; `convert-icons.sh` calls it
  - Every source read once, validated with `brand_contract.validate_svg` (literal colours only), rendered from those bytes on stdin
  - All renders staged before any rename; the PNG namespace `<size>/*.png` is generated output pruned to the expected set; stamp last
- [ ] `verify` (sources, namespace, stamp digests, required names, sizes from the contract) used by Check 30's render-stamp check
- [ ] `sysroot-icons` calls `install`: locked, fresh snapshot verified, packed, archive validated, renamed into place with no-follow writes
  - Temporary file created relative to the verified destination descriptor, never in a shared staging directory
  - The descriptor walk opens the sysroot component itself with `O_NOFOLLOW` and rejects `.` and `..`
  - `validate_ires` refuses a size count outside 1..`ires_sizes` and a version other than the loader's `IRES_VERSION`
- [ ] `sysroot-cursors` calls `install-cursors`: sources read once and validated, required names replaced (symlink or directory) without following, the directory pruned to the exact set
- [ ] Neither asset target depends on `.dirs-stamp` (the installers create their own destination chains)
- [ ] `tools/validate-assets.py` drops its own cursor list and checks for `brand_contract.validate_xcursor` with the contract's files
- [ ] `rsvg-convert` (`librsvg2-bin`) in `scripts/ci/toolchain-packages.txt` first; once the image digest is pinned, a required tool in `scripts/setup.sh`
- [ ] `scripts/overnight/tests/test_staged_dash_check.py` writes its own binary fixture instead of copying `resources/icons/color/64/settings.png`
- [ ] Unit tests: the 24 of the §11 draft (render, verify, both installers, the real `make` targets from a clean build directory), plus a symlinked `build/sysroot`, a symlinked staging path and an oversized IRES size table
- [ ] Commit: `"assets: the converter, asset validator and sysroot installers read the brand contract"`

**Test checkpoint:** `bash scripts/convert-icons.sh` reproduces the committed `SOURCES.sha256` byte for byte; `make sysroot-icons sysroot-cursors BUILD_DIR=<fresh>` installs exactly `icons.ires` and the contract's 11 cursors; the smoke test logs `IRES loaded` and `11/11` cursors. Test on: WSL2 dev host, QEMU TCG.

## 17. Compiler-Evaluated Brand Contract Inputs

> **Spawned-by:** section 11 (review)
> **User impact:** Nothing today: the contract reader refuses any C form it does not model, so a developer who writes one gets a refusal at lint, never a wrong contract; this section removes that friction and the digest pins.

**Design:** [`icons.md#how-are-icons-rendered`](../../docs/design/icons.md#how-are-icons-rendered)

§11 reads its inputs out of C and Make text. Eight review rounds (2026-11-25) each found another place where C compiles a construct differently from how a host parser reads it (token substitution, octal and suffixed literals, integer width, line splicing, conditionals, array extents), and §11 closed each by refusing every form its reader does not model and pinning the behaviour it describes. That holds, but every new kernel or packer edit near those constructs now needs a pin refresh. This section replaces the text reader with values the compiler itself evaluated, so the class is closed by construction.

- [ ] An extractor translation unit per side, compiled with the flags `compile_commands.json` records for `src/kernel/icon_store.c`, `src/kernel/drivers/cursor.c` and `tools/irespack.c`
  - It places every value the contract publishes (enum values, array extents and contents, struct sizes and offsets, macro values) in a data section
  - `scripts/brand_contract.py` reads that section from the object with `llvm-objcopy`, never from source text
- [ ] Remove `BEHAVIOUR_PINS`, `LAYOUT_PINS`, `EXTENT_PINS` and the whitelist grammar where the compiled values make them redundant; keep the pins only for selection rules no constant can express
- [ ] Unit tests: every §11 mutation case (octal size, unparenthesised macro, wrapped multiplication, continued comment, narrowed extent) yields the compiled value or a refusal, never a stale contract
- [ ] Commit: `"brand: contract inputs come from the compiler, not from source text"`

**Test checkpoint:** `python3 scripts/brand_contract.py --check` exits 0 with the extractor in place, and the §11 mutation suite passes with the whitelist grammar removed. Test on: WSL2 dev host.

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
