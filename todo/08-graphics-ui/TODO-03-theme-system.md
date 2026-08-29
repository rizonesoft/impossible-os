---
schema_version: 1
id: theme-system
domain: 08-graphics-ui
status: active
title: "TODO-03 -- Theme System"
---

# TODO-03 -- Theme System

> **Goal:** Centralize all UI colors from hardcoded hex literals into a `theme_t` token struct that every drawing call references. Provide Dark and Light built-in presets, Registry persistence, live hot-reload via `WM_THEME_CHANGED` broadcast, computed accent hover/pressed variants, and theme-aware shadow rendering. This is the P0 prerequisite before any other UI work can produce correct visual output -- every subsequent graphics-ui and desktop-shell TODO assumes `theme_get()` is live.

> [!IMPORTANT]
> `gfx_drop_shadow(s, x, y, w, h, radius, corner_radius, offset_x, offset_y, color)` and `gfx_acrylic(s, x, y, w, h, tint, opacity, blur_radius)` exist in `include/gfx.h` and are ready to be wired to theme tokens. Hardcoded `0xRRGGBB` hex colors are confirmed in `src/desktop/desktop.c`, `wm.c`, `controls.c`, `terminal.c`, and `gallery.c`. The Registry API (`RegGetValue`, `RegSetValueEx`, `HKCU`) is available in `include/registry.h`. The desktop is single-threaded -- `theme_get()` needs no locking. Complete sections in order: struct → singleton → presets → registry → accent → migration → shadow → hot-reload; each section has a commit gate.

## Inputs

- `include/gfx.h` -- `gfx_drop_shadow(… color)`, `gfx_acrylic(… tint, opacity …)`, `gfx_mica(… tint)` -- callers to be updated in §8
- `include/registry.h` -- `RegGetValue()`, `RegSetValueEx()`, `HKCU` handle -- used in §4 (Registry load)
- `src/desktop/desktop.c`, `wm.c`, `controls.c`, `terminal.c`, `gallery.c` -- confirmed sources of hardcoded hex colors to be replaced in §6 (migration)
- `include/desktop/wm.h` -- WM message constants and window list -- extended in §7 to add `WM_THEME_CHANGED` and `wm_post_message_all()`
- Related (no stable XREF target): `08-graphics-ui/TODO-02-*` (future controls TODO) -- must use `theme_get()->field` from day one; this TODO is a hard prerequisite for all graphics-ui work
- Related (no stable XREF target): `09-desktop-shell/TODO-01-*` (future desktop shell TODO) -- Start Menu, taskbar, compositor all depend on theme tokens being live

## Outcome

- `include/desktop/theme.h` exports `theme_t` with 21 named color tokens; `theme_get()` returns singleton.
- `src/desktop/theme.c` provides Dark + Light presets, Registry load, `theme_reload()`, computed accent variants.
- Zero hardcoded `0xRRGGBB` literals remain in `desktop.c`, `wm.c`, `controls.c`, `terminal.c`, `gallery.c`.
- `gfx_drop_shadow` called with `theme_get()->shadow` everywhere.
- Hot-reload: `theme_reload()` re-reads Registry and broadcasts `WM_THEME_CHANGED` → all windows redraw in the same compositor frame.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                   | Depends On                                                              | Status |
| --- | :---: | --------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 `theme_t` struct -- 21 named token fields in `include/desktop/theme.h`                    | Nothing; standalone header                                              |  [ ]   |
| 💎  |   2   | §2 `theme_get()` singleton -- static global + inline accessor; opaque to callers              | §1 struct definition                                                    |  [ ]   |
| 💎  |   3   | §3 Built-in presets -- `THEME_DARK` + `THEME_LIGHT` `const theme_t` constants               | §1 struct                                                               |  [ ]   |
| 💎  |   4   | §4 Registry load -- `theme_load()` reads `HKCU\Software\Impossible\Theme\*`, selects preset  | §3 presets (load picks one then overrides individual fields)            |  [ ]   |
| 💎  |   5   | §5 Custom accent -- computed `accent_hover` + `accent_pressed` derived at load time           | §4 (accent DWORD is the load-time input)                                |  [ ]   |
| 💎  |   6   | §6 Migration pass -- replace all hardcoded hex colors in five desktop source files            | §2 `theme_get()` must be callable before any file is migrated           |  [ ]   |
| 💎  |   7   | §7 Shadow rendering -- wire `gfx_drop_shadow` + `gfx_acrylic` calls to `theme_get()` tokens  | §6 (migration establishes the pattern; shadow is the last holdout)      |  [ ]   |
| ⭐  |   8   | §8 Hot-reload -- `theme_reload()` + `WM_THEME_CHANGED` broadcast + per-window redraw         | §6 (redraw only correct after migration; §7 for shadow redraw to work)  |  [ ]   |
| 💎  |   9   | §9 Fluent token corpus + Win11 personalization contract -- generated tokens, Microsoft Registry keys, Selawik + Fluent icons | §1 struct; §4 load path (keys move); §8 reload broadcast |  [ ]   |

---

## 1. `theme_t` Struct `[Sonnet]`

Define 21 named color token fields covering every semantic UI color role. Store in `include/desktop/theme.h`. All fields are `uint32_t` ARGB32 (`0xAARRGGBB`).

**Files:** `include/desktop/theme.h` (new)

> [!NOTE]
> Keep the struct a plain POD -- no function pointers, no padding. All colors are `uint32_t ARGB32`. Group fields semantically: surface colors, foreground colors, accent colors, component-specific colors. Forward-declare the struct only in this header; all other desktop files `#include "desktop/theme.h"`.

- [ ] `typedef struct theme_t { ... } theme_t;` in `include/desktop/theme.h` with `#pragma once`
- [ ] Surface group: `uint32_t background, surface, surface_variant`
- [ ] Foreground group: `uint32_t foreground, foreground_muted`
- [ ] Accent group: `uint32_t accent, accent_hover, accent_pressed`
- [ ] Border/shadow group: `uint32_t border, shadow`
- [ ] Titlebar group: `uint32_t titlebar_active, titlebar_inactive`
- [ ] Button group: `uint32_t button_bg, button_hover, button_pressed`
- [ ] State group: `uint32_t selection, error, warning, success`
- [ ] Scrollbar group: `uint32_t scrollbar_track, scrollbar_thumb`
- [ ] Total: 21 fields; add `uint8_t mode` (0=dark, 1=light) as metadata field for hot-reload logic
- [ ] Commit: `"desktop/theme: theme_t struct -- 21 color tokens + mode field"`

## 2. `theme_get()` Singleton `[Sonnet]`

Static global `theme_t g_theme` in `theme.c`. `theme_get()` returns `const theme_t*`. `theme_init()` called once from desktop startup before any drawing occurs.

**Files:** `src/desktop/theme.c` (new), `include/desktop/theme.h` (extend)

> [!NOTE]
> Desktop is single-threaded -- no spinlock needed on `g_theme`. `theme_get()` is a one-liner; mark `static inline` in the header so call sites compile to a single register load. `theme_init()` is called once from `desktop_init()` (before any window is drawn); it calls `theme_load()` (§4) and falls back to `THEME_DARK` if Registry is unavailable.

- [ ] `static theme_t g_theme;` in `src/desktop/theme.c`
- [ ] `void theme_init(void)` -- calls `theme_load()` (§4); if load fails: `g_theme = THEME_DARK`
- [ ] `static inline const theme_t* theme_get(void)` in `include/desktop/theme.h` -- returns `&g_theme`
- [ ] Call `theme_init()` from `desktop_init()` before first compositor frame
- [ ] Commit: `"desktop/theme: theme_get() singleton, theme_init() called from desktop_init()"`

## 3. Built-in Presets `[Sonnet]`

Two `const theme_t` constants: `THEME_DARK` and `THEME_LIGHT`. Both use `#0078D4` accent. Declared as `extern const theme_t THEME_DARK, THEME_LIGHT` in the header; defined in `theme.c`.

**Files:** `src/desktop/theme.c` (extend), `include/desktop/theme.h` (extend)

> [!NOTE]
> Encode colors as `0xFF000000 | 0xRRGGBB` (fully opaque ARGB32). The `shadow` token carries alpha -- dark mode: `0xB4000000` (70% opacity black); light mode: `0x50000000` (31% opacity black). Accent hover/pressed are computed in §5, not hard-coded here; leave them set to `0` in the const presets (they get filled in by `theme_load()`).

- [ ] `THEME_DARK`: `background=0xFF1C1C1C`, `surface=0xFF2C2C2C`, `surface_variant=0xFF3A3A3A`, `foreground=0xFFFFFFFF`, `foreground_muted=0xFF9D9D9D`, `accent=0xFF0078D4`, `border=0xFF454545`, `shadow=0xB4000000`, `titlebar_active=0xFF1C1C1C`, `titlebar_inactive=0xFF2C2C2C`, `button_bg=0xFF3A3A3A`, `button_hover=0xFF4A4A4A`, `button_pressed=0xFF2A2A2A`, `selection=0xFF0078D4`, `error=0xFFCC2929`, `warning=0xFFD98400`, `success=0xFF107C10`, `scrollbar_track=0xFF2C2C2C`, `scrollbar_thumb=0xFF555555`, `mode=0`
- [ ] `THEME_LIGHT`: `background=0xFFF3F3F3`, `surface=0xFFFFFFFF`, `surface_variant=0xFFEAEAEA`, `foreground=0xFF000000`, `foreground_muted=0xFF666666`, `accent=0xFF0078D4`, `border=0xFFD1D1D1`, `shadow=0x50000000`, `titlebar_active=0xFFEEEEEE`, `titlebar_inactive=0xFFF3F3F3`, `button_bg=0xFFE5E5E5`, `button_hover=0xFFD5D5D5`, `button_pressed=0xFFC5C5C5`, `selection=0xFF0078D4`, `error=0xFFCC2929`, `warning=0xFFD98400`, `success=0xFF107C10`, `scrollbar_track=0xFFF0F0F0`, `scrollbar_thumb=0xFFB0B0B0`, `mode=1`
- [ ] `extern const theme_t THEME_DARK;` and `extern const theme_t THEME_LIGHT;` in `include/desktop/theme.h`
- [ ] Commit: `"desktop/theme: THEME_DARK + THEME_LIGHT built-in presets"`

## 4. Registry Load `[Sonnet]`

`theme_load()` reads `HKCU\Software\Impossible\Theme\Mode` (0=dark, 1=light), `AccentColor` DWORD, `TitlebarActiveColor`, `TitlebarInactiveColor`. Selects the preset, then overrides individual fields if Registry values are present.

**Files:** `src/desktop/theme.c` (extend)

> [!NOTE]
> Pattern: start with a full copy of `THEME_DARK` or `THEME_LIGHT` (based on `Mode`); then call `RegGetValue` for each optional override key; if the call succeeds (returns `ERROR_SUCCESS`): write the field. If Registry is unavailable (boot before registry init): fall back to `THEME_DARK` silently and log `[theme] registry unavailable, using dark preset`. Use `REG_DWORD` type for all color values.

- [ ] `void theme_load(void)` in `src/desktop/theme.c`:
  - [ ] Open `HKCU\Software\Impossible\Theme` with `RegOpenKeyEx()`
  - [ ] Read `Mode` DWORD → select `THEME_DARK` or `THEME_LIGHT` as base; `g_theme = base_preset`
  - [ ] Read `AccentColor` DWORD → if present: `g_theme.accent = value`
  - [ ] Read `TitlebarActiveColor` DWORD → if present: `g_theme.titlebar_active = value`
  - [ ] Read `TitlebarInactiveColor` DWORD → if present: `g_theme.titlebar_inactive = value`
  - [ ] On any `RegOpenKeyEx` failure: `g_theme = THEME_DARK`; log and return
  - [ ] `RegCloseKey()` after all reads
- [ ] Registry paths above are superseded by the Win11 personalization contract in §9: read the Microsoft keys first, `HKCU\Software\Impossible\Theme\*` only for Impossible-specific overrides
- [ ] Log: `[theme] loaded mode=%s accent=#%06X`
- [ ] Commit: `"desktop/theme: theme_load() -- Registry HKCU read, preset selection, field overrides"`

## 5. Custom Accent Color `[Sonnet]`

After `theme_load()` sets `g_theme.accent`, compute `accent_hover` and `accent_pressed` by clamped lightening and darkening. Done once at load time -- no runtime recomputation.

**Files:** `src/desktop/theme.c` (extend)

> [!NOTE]
> Lightening/darkening formula operates per channel on the RGB bytes of the ARGB32 value. Keep alpha channel (`0xFF`) from the source accent. Clamp each channel to `[0, 255]`. Call `theme_compute_accent_variants()` at the end of `theme_load()` -- and again inside `theme_reload()` (§8) after re-reading the accent.

- [ ] `static void theme_compute_accent_variants(void)` in `src/desktop/theme.c`:
  - [ ] Extract `r`, `g`, `b` from `g_theme.accent`
  - [ ] `accent_hover`: add `0x10` to each channel; clamp to 255; recombine with `0xFF` alpha
  - [ ] `accent_pressed`: subtract `0x18` from each channel; clamp to 0; recombine with `0xFF` alpha
- [ ] Call `theme_compute_accent_variants()` at the end of `theme_load()`
- [ ] Commit: `"desktop/theme: computed accent_hover/accent_pressed variants from loaded accent"`

## 6. Migration Pass `[Sonnet]`

Replace all hardcoded `0xRRGGBB` / `0xAARRGGBB` color literals in `desktop.c`, `wm.c`, `controls.c`, `terminal.c`, and `gallery.c` with `theme_get()->field` references. Add a `static_assert`-style comment guard: after migration, any remaining literal triggers a TODO comment for review.

**Files:** `src/desktop/desktop.c`, `src/desktop/wm.c`, `src/desktop/controls.c`, `src/desktop/terminal.c`, `src/desktop/gallery.c`

> [!NOTE]
> Mapping guide for common literals found in the codebase:
> - Window background fills → `theme_get()->background`
> - Control surface fills (button bg, panel bg) → `theme_get()->surface` or `theme_get()->button_bg`
> - Titlebar background (focused) → `theme_get()->titlebar_active`
> - Titlebar background (unfocused) → `theme_get()->titlebar_inactive`
> - Text/label color → `theme_get()->foreground`
> - Muted/secondary text → `theme_get()->foreground_muted`
> - Accent highlights, focused rings, selection → `theme_get()->accent`
> - Button hover state → `theme_get()->button_hover`
> - Button pressed state → `theme_get()->button_pressed`
> - Scrollbar track → `theme_get()->scrollbar_track`
> - Scrollbar thumb → `theme_get()->scrollbar_thumb`
> - Border/outline → `theme_get()->border`
>
> Do not change `terminal.c` terminal-emulator ANSI colors (those are VT100 palette, not UI theme). Only replace UI chrome colors (cursor, selection bg, scrollbar, border).

- [ ] `#include "desktop/theme.h"` added to all five files
- [ ] `desktop.c`: replace background fills, desktop surface, selection highlight
- [ ] `wm.c`: replace titlebar_active, titlebar_inactive, close/min/max button states, window border, resize handle
- [ ] `controls.c`: replace button_bg, button_hover, button_pressed, label foreground, textbox bg + border, scrollbar track + thumb, checkbox/radio fill, progress bar accent
- [ ] `terminal.c`: replace terminal chrome (border, scrollbar, cursor color) -- keep ANSI VT100 palette literals untouched
- [ ] `gallery.c`: replace background, toolbar bg, selection rect
- [ ] After all replacements: `rg "0x[0-9A-Fa-f]{6}" src/desktop/` should return zero results outside ANSI color tables and known intentional literals (leave a `/* intentional: ANSI VT100 */` comment for those)
- [ ] Commit: `"desktop/theme: migration -- replace all hardcoded hex colors with theme_get()->field"`

## 7. Theme-Aware Shadow Rendering `[Sonnet]`

Update all `gfx_drop_shadow()` and `gfx_acrylic()` call sites to pass `theme_get()->shadow` and `theme_get()->surface` respectively, so shadow depth and tint respond to dark/light mode.

**Files:** `src/desktop/wm.c`, `src/desktop/controls.c`, `src/desktop/desktop.c`

> [!NOTE]
> `gfx_drop_shadow(s, x, y, w, h, radius, corner_radius, offset_x, offset_y, color)` -- pass `theme_get()->shadow` for `color`. `gfx_acrylic(s, x, y, w, h, tint, opacity, blur_radius)` -- pass `theme_get()->surface` for `tint`; opacity stays as-is (window-specific). `gfx_mica(s, x, y, w, h, wallpaper, tint)` -- pass `theme_get()->surface` for `tint`. Dark mode shadow: `0xB4000000` (opaque-ish black); light mode: `0x50000000` (lighter) -- already encoded in the presets (§3), so this section is purely a call-site wiring task.

- [ ] `wm.c`: `gfx_drop_shadow(…, theme_get()->shadow)` for window shadow
- [ ] `wm.c`: `gfx_acrylic(…, theme_get()->surface, opacity, radius)` for dialog acrylic background
- [ ] `controls.c`: any `gfx_drop_shadow` calls (e.g., tooltip shadow, dropdown shadow) → `theme_get()->shadow`
- [ ] `desktop.c`: `gfx_mica(…, theme_get()->surface)` for desktop Mica background
- [ ] Commit: `"desktop/theme: wire gfx_drop_shadow+acrylic+mica to theme shadow/surface tokens"`

## 8. Hot-Reload `[Sonnet]`

`theme_reload()` re-reads Registry and recomputes all fields. Broadcasts `WM_THEME_CHANGED` to all open windows via new `wm_post_message_all()`. Each window marks itself dirty and redraws on the next compositor frame.

**Files:** `src/desktop/theme.c` (extend), `include/desktop/wm.h` (extend), `src/desktop/wm.c` (extend)

> [!NOTE]
> `wm_post_message_all(msg, wparam, lparam)` iterates `wm_state.windows[0..WM_MAX_WINDOWS]`; for each visible window: sets `win->dirty = 1` (or calls `wm_invalidate(win)`). The compositor's existing per-frame dirty check already redraws dirty windows -- no new compositor logic is needed. `WM_THEME_CHANGED` message constant: add to `wm.h` after the last `WM_` define. `theme_reload()` is called by: (a) Registry change notification (future), (b) Control Panel color picker (future), (c) shell command `theme dark` / `theme light`. Add `theme` shell command to `src/shell/shell.c` commands table.

- [ ] `#define WM_THEME_CHANGED  0x0020` in `include/desktop/wm.h` (after existing WM_ constants)
- [ ] `void wm_post_message_all(uint32_t msg, uint64_t wparam, uint64_t lparam)` in `src/desktop/wm.c`: iterate window array; call `wm_invalidate()` on each visible window
- [ ] `void theme_reload(void)` in `src/desktop/theme.c`: `theme_load()` → `theme_compute_accent_variants()` → `wm_post_message_all(WM_THEME_CHANGED, 0, 0)`
- [ ] Shell command: `theme dark` → `g_theme = THEME_DARK; theme_compute_accent_variants(); wm_post_message_all(…)`; `theme light` → same with `THEME_LIGHT`; `theme status` → log current mode + accent hex
- [ ] Log: `[theme] reloaded -- mode=%s accent=#%06X, broadcasting WM_THEME_CHANGED`
- [ ] Commit: `"desktop/theme: hot-reload -- theme_reload(), WM_THEME_CHANGED broadcast, theme shell cmd"`

---

## 9. Fluent Token Corpus and Win11 Personalization Contract `[Opus]`

> **Spawned-by:** root

Two things make the desktop read as Windows 11 rather than merely Fluent-shaped: the numbers come from Microsoft's own Fluent resource dictionaries instead of being hand-picked, and the theme state lives where Win32 apps already look for it. The WinUI 3 source (`microsoft/microsoft-ui-xaml`, MIT, LICENSE file verified 2026-08-29) ships the light/dark/high-contrast brush tables, type ramp, corner radii, control heights and easing curves as XAML resource dictionaries. Per CLAUDE.md "Vendor-First Evaluation" that is authoritative DATA to generate from, never code to port: WinUI 3 itself sits on WinRT, `Microsoft.UI.Composition` and DirectX, none of which exist here, and running WinUI 3 apps is a compatibility tier (`12-user-platform-sdk/TODO-07 §13` → XREF), not this section.

**Files:** `tools/fluent-tokens/` (new host tool), `include/desktop/theme_fluent.h` (generated, checked in), `src/desktop/theme.c` (extend), `resources/fonts/`, `resources/icons/`

- [ ] `tools/fluent-tokens/gen-tokens.py` -- host tool over a pinned checkout of the WinUI 3 theme resource dictionaries; emits `include/desktop/theme_fluent.h`
  - Emits `const theme_t THEME_FLUENT_DARK / THEME_FLUENT_LIGHT / THEME_FLUENT_HIGH_CONTRAST`, plus corner radius (4/8 px), control height (32 px), 4 px spacing grid, and the standard easing/duration tables consumed by `08-graphics-ui/TODO-04`
  - Upstream commit pinned in `tools/fluent-tokens/CORPUS-VERSION`; regenerate only via `make fluent-tokens`; the generated header is checked in so the kernel build never touches the corpus
  - `THEME_DARK` / `THEME_LIGHT` (§3) become aliases of the generated presets and every hand-authored hex value leaves `theme.c`
  - Rows in `src/libs/PROVENANCE.md` + `CREDITS.md` naming the corpus (MIT) and the pinned commit, same commit as the generator
- [ ] Win11 personalization Registry contract -- `theme_load()` (§4) reads the Microsoft paths as the source of truth, because Win32 apps read them directly to pick dark mode and accent
  - `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`: `AppsUseLightTheme`, `SystemUsesLightTheme`, `EnableTransparency`
  - `HKCU\Software\Microsoft\Windows\DWM`: `AccentColor`, `ColorizationColor`, `ColorPrevalence`
  - `HKCU\Control Panel\Desktop`: `WallPaper`; `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent`: `AccentPalette`
  - `HKCU\Software\Impossible\Theme\*` keeps only Impossible-specific overrides (titlebar colors); `Mode` / `AccentColor` there are dropped and the §4 items updated to match
  - `theme_reload()` (§8) fires on a change to any of these keys so the `WM_SETTINGCHANGE("ImmersiveColorSet")` consumers in `12-user-platform-sdk/TODO-05 §9` (→ XREF) see exactly one broadcast
- [ ] Fonts and icons -- ship the permissive equivalents of the Segoe family; no Microsoft font file is ever redistributed
  - `Selawik` (SIL OFL 1.1, LICENSE verified 2026-08-29) as the UI face, registered under the `Segoe UI` / `Segoe UI Variable` aliases in the font catalog (`08-graphics-ui/TODO-02 §1` → XREF; vendored engine in `TODO-02 §7`)
  - `fluentui-system-icons` (MIT, LICENSE verified 2026-08-29) rasterized into the icon atlas as the `Segoe Fluent Icons` / `Segoe MDL2 Assets` glyph set, same codepoints
  - Alias table: a Win32 `CreateFont("Segoe UI")` resolves to the shipped face; PROVENANCE + CREDITS rows for both with the pinned upstream tag
- [ ] Log: `[theme] fluent corpus <sha> mode=%s accent=#%06X source=win11-keys`
- [ ] Commit: `"desktop/theme: Fluent token corpus generator, Win11 personalization Registry contract, Selawik + Fluent icon assets"`

**Test checkpoint:** `THEME_DARK.background` equals the generated `SolidBackgroundFillColorBase` dark value byte for byte; writing `AppsUseLightTheme=1` under the Microsoft key and running `theme reload` switches the desktop to light with no Impossible-specific key present; `CreateFont("Segoe UI")` measures text identically to `CreateFont("Selawik")`. Test on: QEMU TCG + KVM; bare metal.

---

## OS Comparison


| ⭐  | Feature                                                         | 🪟 Win11                                                               | 🐧 Linux                                          | 🚀 Impossible OS                                                          |
| --- | --------------------------------------------------------------- | ---------------------------------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------------------------- |
| 💎  | Semantic color token struct                                     | ✅ `COLORREF` + `GetSysColor()` + WinUI3                               | ✅ GTK `GtkStyleContext`; CSS custom properties   | ⬜ §1 -- `theme_t` 21-field POD; inline `theme_get()`                     |
| 💎  | Dark + Light built-in presets                                   | ✅ Dark/Light system theme; auto-switches at                           | ✅ GTK prefers-color-scheme; GNOME night mode     | ⬜ §3 -- `THEME_DARK` + `THEME_LIGHT` `const theme_t`                     |
| 💎  | Registry-backed persistence                                     | ✅ `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize` | ✅ `dconf`/`gsettings` key-value store; INI files | ⬜ §4 -- `HKCU\Software\Impossible\Theme\Mode` + `AccentColor` + titlebar |
| 💎  | Custom accent color                                             | ✅ Settings → Personalization → Accent                                 | ✅ KDE/GNOME accent color pickers; GTK            | ⬜ §5 -- `accent_hover = accent +0x101010` (clamped)                      |
| 💎  | Migration -- zero hardcoded hex colors in UI source             | ✅ WinUI3 resource brush system; no                                    | ✅ GTK CSS variables; theme engine                | ⬜ §6 -- `rg "0x[0-9A-Fa-f]{6}" src/desktop/` → zero                      |
| 💎  | Theme-aware shadow + acrylic                                    | ✅ Shadow elevation system in WinUI3;                                  | ✅ GNOME uses elevation system; KDE               | ⬜ §7 -- `gfx_drop_shadow(…, theme_get()->shadow)` -- `0xB4000000` dark   |
| ⭐  | Hot-reload with zero app restart                                | ✅ Windows redraws all windows live                                    | ⚠️ GTK/Qt apps reload themes live;                | ⬜ §8 -- `⭐` kernel-level broadcast: `wm_post_message_all()` dirty-marks |
| 💎  | Fluent tokens from Microsoft's corpus + Win11 Registry contract | ✅ WinUI3 resource dictionaries; `Themes\Personalize` + `DWM` keys     | ⚠️ libadwaita named colors; no cross-toolkit key  | ⬜ §9 -- generated `theme_fluent.h`; Microsoft keys; Selawik + icons      |

> **After §1–§8:** Impossible OS has a fully kernel-native theme system with zero external dependencies. The `⭐` hot-reload advantage over Linux is that `wm_post_message_all()` operates at the kernel compositor level -- every window is dirty-marked in a single pass before the next frame, so the entire desktop repaints atomically in one compositor tick regardless of how many windows are open. GTK and Qt apps on Linux each maintain their own theming subscriptions and redraw at different times.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `theme_get()` called before first window draw in QEMU serial log: `[theme] loaded mode=dark accent=#0078D4`
- [ ] `THEME_DARK.background == 0xFF1C1C1C` and `THEME_LIGHT.background == 0xFFF3F3F3` (verify in debugger or serial dump)
- [ ] `accent_hover = 0xFF1088E4` (i.e., `0078D4 + 0x101010`); `accent_pressed = 0xFF0060BC` (i.e., `0078D4 − 0x181818`) -- verify computed values
- [ ] After §6 migration: `rg "0x[0-9A-Fa-f]{6}" src/desktop/wm.c src/desktop/controls.c src/desktop/desktop.c` returns zero non-ANSI-palette hits
- [ ] Window drop shadows visibly lighter in Light mode than Dark mode (QEMU screenshot comparison)
- [ ] Hot-reload: run `theme light` shell command → entire desktop redraws to light palette without restart; `[theme] reloaded` in serial log; run `theme dark` → reverts
- [ ] Commit: `"desktop/theme: complete theme system -- tokens, presets, registry, migration, hot-reload"`
