---
schema_version: 1
id: accessibility
domain: 10-platform-services
status: active
title: "TODO-06 -- Accessibility Features"
---

# TODO-06 -- Accessibility Features

> **Goal:** Ensure Impossible OS is usable for users with visual, motor, and cognitive needs -- a requirement for any production-grade OS. The theme system, DPI layer, and animation engine are foundations; this TODO wires eight accessibility features and their unified Control Panel applet on top of them.

> [!IMPORTANT]
> **Already exists**: `cursor_image_t { images[CURSOR_MAX_SIZES=8] }` + `cursor_draw()` in `cursor.h` -- size struct present; no `cursor_set_size()` yet. Keyboard driver supports Shift/Ctrl/Alt modifier tracking (`keyboard.h` comment confirms). `gfx_blit()` for magnifier. `system_get_ticks()`. **Forward deps**: `theme_reload(theme_id)` + `WM_THEME_CHANGED` broadcast + `THEME_FLUENT_HIGH_CONTRAST` preset (08-graphics-ui theme system). `WM_DPI_CHANGED` + DPI scale factor broadcast (08-graphics-ui DPI). `anim_set_enabled(bool)` / `g_anim_enabled` (08-graphics-ui animation engine). `fb_get_backbuffer()` for magnifier source (framebuffer driver). **Nothing from this TODO needs a new syscall** -- all features are kernel/desktop layer. Note on color blind LUT: 3×3 color matrix applied at `fb_flush()` stage to compositor output; new function `fb_set_color_matrix()` needed in framebuffer driver.

## Inputs

- `include/desktop/wm.h` -- WM message loop; add `WM_DPI_CHANGED` broadcast; mouse event injection for §6 -- §2, §6
- `include/cursor.h` -- `cursor_image_t`, `CURSOR_MAX_SIZES`, `cursor_draw()` -- §4 large cursor
- `include/kernel/drivers/keyboard.h` -- modifier key tracking; extend for sticky key latch -- §3
- `include/kernel/drivers/framebuffer.h` -- `fb_lock_compositor()`, `fb_get_backbuffer()` -- §5 magnifier source; add `fb_set_color_matrix()` for §7 LUT
- `include/gfx.h` -- `gfx_blit()`, `gfx_fill_rect()`, `gfx_put_pixel()` -- §5 magnifier zoom blit
- `include/registry.h` -- `HKLM\SYSTEM\Accessibility\*`, `HKCU\Software\Impossible\Display\ScaleFactor` -- §1-8 all Registry keys
- `include/kernel/timer.h` -- `system_get_ticks()` -- §3 sticky key timing, §5 magnifier follow rate
- `include/desktop/controls.h` (TODO-05) -- `CTRL_SLIDER`, `CTRL_CHECKBOX`, `CTRL_DROPDOWN` -- §8 ease.cpl
- `include/cpl.h` (TODO-11) -- `CPlApplet_t`, `NEWCPLINFO` -- §8 ease.cpl
- `include/desktop/notification.h` (TODO-09) -- `notify_send()` -- §3 sticky keys audio + visual cue
- → XREF: `08-graphics-ui/TODO-03-theme-system.md §8, §9` -- theme system (`theme_reload()`, `WM_THEME_CHANGED`, `THEME_FLUENT_HIGH_CONTRAST`); §1 depends on that
- → XREF: `08-graphics-ui/TODO-09` -- DPI scale factor + `WM_DPI_CHANGED` broadcast; §2 depends on that
- → XREF: `08-graphics-ui/TODO-04-animation-engine.md` -- animation engine (`anim_set_enabled()`); §7 reduced motion depends on that
- → XREF: `09-desktop-shell/TODO-11 §5` -- `ease.cpl` stub registered in Control Panel; §8 implements it
- → XREF: `D00 T05 §4, §11` -- the desktop UI test framework reuses `mouse_event_inject()` (§6) for input injection and record/replay; do not redefine the primitive in the test framework

## Outcome

- `accessibility_set_high_contrast(on)` → `theme_reload(THEME_FLUENT_HIGH_CONTRAST)` + Registry.
- `accessibility_set_dpi(pct)` → scale factor override + `WM_DPI_CHANGED` all windows.
- Sticky keys: 5× Shift latch; visual indicator in tray; modifier names shown when latched.
- `cursor_set_size(px)` → reload cursor images at 32/48/64/96 px; XOR/solid color option.
- Screen magnifier: Win+Plus/Minus zoom 2×–8×; compositor blit zoom overlay; cursor-tracking.
- Mouse keys: Numpad → mouse cursor; Alt+Shift+Numlock toggle; configurable speed.
- Reduced motion: `anim_set_enabled(0)` + instant transitions; color blind LUT stretch.
- `ease.cpl` one-page applet: all 8 toggles + sliders; instant Registry apply.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                          | Depends On                                                                                   | Status |
| --- | :---: | ---------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §7 Reduced motion -- `HKLM\SYSTEM\Accessibility\ReducedMotion`; `anim_set_enabled(0)` gate          | `anim_set_enabled()` (08-graphics-ui); `registry_set/get()`                                 |  [ ]   |
| 💎  |   2   | §1 High contrast -- `THEME_FLUENT_HIGH_CONTRAST` preset; `accessibility_set_high_contrast()`; Registry      | `theme_reload(THEME_FLUENT_HIGH_CONTRAST)` + `WM_THEME_CHANGED` (08-graphics-ui theme)            |  [ ]   |
| 💎  |   3   | §2 Large text/DPI -- `accessibility_set_dpi(pct)` scale override; `WM_DPI_CHANGED` broadcast        | `WM_DPI_CHANGED` + DPI scale infrastructure (08-graphics-ui DPI TODO)                      |  [ ]   |
| 💎  |   4   | §3 Sticky keys -- 5× Shift detection; modifier latch; tray indicator; `HKLM\SYSTEM\Accessibility\StickyKeys` | keyboard modifier hook in `keyboard.h`; tray area (TODO-07)                       |  [ ]   |
| 💎  |   5   | §4 Large cursor -- `cursor_set_size(px)` 32/48/64/96; color options; Registry                        | `cursor_image_t` (exists); cursor loader (TODO-02 file assoc / cursor theme)               |  [ ]   |
| ⭐  |   6   | §5 Screen magnifier -- `Win+Plus/Minus` zoom 2×–8×; compositor `gfx_blit` zoom overlay; cursor track | `fb_get_backbuffer()` + `gfx_blit()` (exist); `fb_lock_compositor()` (exists)             |  [ ]   |
| 💎  |   7   | §6 Mouse keys -- Numpad → mouse; Alt+Shift+Numlock toggle; speed config                              | keyboard driver modifier state; WM mouse event injection                                    |  [ ]   |
| 💎  |   8   | §8 `ease.cpl` -- one-page applet; all 8 toggles+sliders; instant apply                               | §1-7 all APIs; `CTRL_SLIDER/CHECKBOX/DROPDOWN`; `include/cpl.h` (TODO-11)                 |  [ ]   |

---

## 1. High Contrast Mode `[Sonnet]`

High contrast uses the theme system's high-contrast preset (`THEME_FLUENT_HIGH_CONTRAST`, owned by `08-graphics-ui/TODO-03 §9`, which replaces every design token per `docs/design/shell.md#accessibility`); this TODO defines no palette of its own. `accessibility_set_high_contrast(enabled)` → `theme_reload(THEME_FLUENT_HIGH_CONTRAST)` + `WM_THEME_CHANGED` broadcast. Registry `HKLM\SYSTEM\Accessibility\HighContrast`. Toggle in `ease.cpl` and via the Accessibility tile of quick settings (`08-graphics-ui/TODO-09 §8`).

**Files:** `src/kernel/accessibility.c` (new), `include/kernel/accessibility.h` (new)

> [!NOTE]
> → XREF: `08-graphics-ui/TODO-03-theme-system.md §8` -- `theme_reload(int theme_id)` and `WM_THEME_CHANGED` live there; the high-contrast preset `THEME_FLUENT_HIGH_CONTRAST` is generated by `08-graphics-ui/TODO-03 §9`. `accessibility_set_high_contrast(int on)`: `registry_set("HKLM\\SYSTEM\\Accessibility\\HighContrast", on ? "1" : "0")`; `theme_reload(on ? THEME_FLUENT_HIGH_CONTRAST : THEME_DEFAULT)` → broadcasts `WM_THEME_CHANGED` to all windows. All widgets re-read theme tokens on `WM_THEME_CHANGED` (this is the theme system contract from `08-graphics-ui`). **Quick settings**: the Accessibility tile of the quick settings flyout (`08-graphics-ui/TODO-09 §8`) exposes the high contrast toggle, which calls `accessibility_set_high_contrast(!g_high_contrast)`; there is no separate tray icon. **Load on boot**: kernel init reads `HKLM\SYSTEM\Accessibility\HighContrast` → if `"1"` → `theme_reload(THEME_FLUENT_HIGH_CONTRAST)` before desktop launch.

- [ ] `include/kernel/accessibility.h`: global state `g_accessibility_t { int high_contrast, dpi_pct, sticky_keys, cursor_size_px, magnifier_zoom, mouse_keys, reduced_motion, color_blind_mode; }`, all `accessibility_set_*()` prototypes
- [ ] `src/kernel/accessibility.c`: `accessibility_init()` -- read all `HKLM\SYSTEM\Accessibility\*` Registry keys at boot
- [ ] `accessibility_set_high_contrast(on)` -- `registry_set()` + `theme_reload()`; read back on WM init
- [ ] Use the theme system's `THEME_FLUENT_HIGH_CONTRAST` preset (`08-graphics-ui/TODO-03 §9`); no second palette is defined here
- [ ] `accessibility_init()` called from kernel init sequence before desktop launch
- [ ] Accessibility tile in quick settings (`08-graphics-ui/TODO-09 §8`) opens the accessibility quick list with the high contrast toggle; no separate tray icon
- [ ] Commit: `"accessibility: high_contrast -- theme system high-contrast preset, theme_reload(), boot restore"`

## 2. Large Text / DPI Override `[Sonnet]`

`accessibility_set_dpi(pct)` forces scale factor (125/150/200/250/300%) via `HKCU\Software\Impossible\Display\ScaleFactor`; overrides display-detected DPI. Broadcasts `WM_DPI_CHANGED` to all open windows. `HKLM\SYSTEM\Accessibility\DPIScale`.

**Files:** extend `src/kernel/accessibility.c`

> [!NOTE]
> → XREF: `08-graphics-ui/TODO-09` -- DPI scale infrastructure lives there; `WM_DPI_CHANGED` + global `g_dpi_scale` live there. `accessibility_set_dpi(int pct)`: validate pct in `{100, 125, 150, 200, 250, 300}`; `registry_set("HKCU\\Software\\Impossible\\Display\\ScaleFactor", pct_str)` + `registry_set("HKLM\\SYSTEM\\Accessibility\\DPIScale", pct_str)`; call DPI layer's `dpi_set_scale(pct)` → broadcasts `WM_DPI_CHANGED` to all windows (each window recalculates its layout on receipt). Preset levels in `ease.cpl` shown as: 100% (Default), 125% (Medium), 150% (Large), 200% (Larger), 250% (Extra Large), 300% (Huge). **On boot**: `accessibility_init()` reads `DPIScale` → if present + non-zero → `dpi_set_scale()` before window creation.

- [ ] `accessibility_set_dpi(int pct)` -- validate preset levels; `registry_set()` both keys; `dpi_set_scale(pct)` call
- [ ] `WM_DPI_CHANGED` broadcast: all open windows receive and recalculate layout (contract from TODO-09 DPI system)
- [ ] Boot restore: `accessibility_init()` reads `DPIScale` → `dpi_set_scale()` before any window opens
- [ ] Preset labels: 100/125/150/200/250/300% → Combo box in `ease.cpl`
- [ ] Commit: `"accessibility: large_text_dpi -- dpi_set_scale override, WM_DPI_CHANGED broadcast, boot restore"`

## 3. Sticky Keys `[Sonnet]`

5× Shift activates sticky keys (visual + audio cue). Modifier keys (Shift/Ctrl/Alt/Win) latch after press+release until next non-modifier key. Tray indicator: modifier names shown when latched. `HKLM\SYSTEM\Accessibility\StickyKeys` toggle.

**Files:** extend `src/kernel/drivers/keyboard.c`, extend `src/kernel/accessibility.c`

> [!NOTE]
> **5× Shift detection**: add `g_shift_press_count` + `g_shift_last_ticks` counters in keyboard ISR. On each Left/Right Shift keydown: if `system_get_ticks() - g_shift_last_ticks < 2 * TICKS_PER_SEC`: `g_shift_press_count++`; else: `g_shift_press_count = 1`; update `g_shift_last_ticks`. If `g_shift_press_count >= 5` AND `registry_get("HKLM\\SYSTEM\\Accessibility\\StickyKeys") == "1"`: `sticky_keys_activate()` -- play activation sound (`audio_play_file(SOUND_CLICK)`) + show tray notification "Sticky Keys On". **Modifier latch**: `g_sticky_modifier_state { lshift, rshift, lctrl, rctrl, lalt, ralt, lwin }` bit flags. On modifier keydown: set latch bit. On next non-modifier keydown: apply latched modifiers to key event → clear all latch bits after one use. **Visual indicator**: tray icon shows latched modifier labels "Shift Ctrl" etc.; updated on each latch state change. **Toggle shortcut**: pressing Shift 5× also toggles off sticky keys if already active. **Registry default**: `StickyKeys=0` (off by default; user enables in ease.cpl).

- [ ] `g_shift_press_count`, `g_shift_last_ticks` counters in `keyboard.c`; 5× detection in keydown handler
- [ ] `g_sticky_modifier_state` bit flags; latch on modifier press; clear after first non-modifier key use
- [ ] `sticky_keys_activate()` -- set `g_sticky_active=1`; audio cue `audio_play_file()`; tray notification
- [ ] `sticky_keys_deactivate()` -- clear all latch bits; tray notification "Sticky Keys Off"
- [ ] Visual tray indicator: update `g_tray_sticky_label` string ("Shift", "Ctrl+Alt", etc.) on latch change
- [ ] Registry `StickyKeys` read in `accessibility_init()`; write in `accessibility_set_sticky_keys(on)`
- [ ] 5× Shift also toggles off if already active
- [ ] Commit: `"accessibility: sticky_keys -- 5x Shift activate, modifier latch, tray indicator, audio cue"`

## 4. Large Cursor `[Sonnet]`

**Design:** n/a -- cursors are the existing Adwaita set; a Windows 11 cursor redesign is not part of the design system yet

`cursor_set_size(px)`: sizes 32/48/64/96 px. Reload cursor images at requested size (scale from source Xcur or fallback scale). Color options: white+black border (default), solid black, inverted XOR. `HKLM\SYSTEM\Accessibility\CursorSize` + `CursorColor`.

**Files:** extend `src/desktop/cursor.c`, extend `src/kernel/accessibility.c`

> [!NOTE]
> `cursor_image_t` has `images[CURSOR_MAX_SIZES=8]` -- multi-size is already structurally supported. `cursor_set_size(int px)`: validate `px` in `{32, 48, 64, 96}`; set `g_cursor_size = px`; reload all cursor types at that size: for each `CURSOR_*` type, if the `.xcur` source has a frame at that size → use it; else `image_scale(&scaled, &base_image, px, px, SCALE_BILINEAR)` → store in `cursor.images[size_idx]`. **Cursor color**: `CURSOR_COLOR_WHITE=0` (white fill + black 1-px border, default), `CURSOR_COLOR_BLACK=1` (fill every white pixel with black, invert border), `CURSOR_COLOR_INVERTED=2` (XOR composited: each pixel = `~screen_pixel` at cursor coords). For XOR: in `cursor_draw()`, instead of `gfx_blit_alpha()`, use `gfx_xor_blit()` stub. `cursor_set_color(int mode)`: `g_cursor_color_mode = mode`; `registry_set("HKLM\\SYSTEM\\Accessibility\\CursorColor", ...)`. Boot restore: `accessibility_init()` reads `CursorSize` (default 32) + `CursorColor` (default 0) → `cursor_set_size()` + `cursor_set_color()`.

- [ ] `cursor_set_size(int px)` -- validate 32/48/64/96; reload all cursor type images at that size; `image_scale()` if no native size
- [ ] `g_cursor_size` global in `cursor.c`; `cursor_draw()` uses `g_cursor_size` to select `images[]` index
- [ ] `cursor_set_color(int mode)` -- WHITE/BLACK/INVERTED; BLACK: invert white pixels in loaded images; INVERTED: `gfx_xor_blit()` in `cursor_draw()`
- [ ] `gfx_xor_blit(dst, x, y, src)` stub in `gfx.c` -- `dst[x+i][y+j] ^= src[i][j]` for each non-transparent pixel
- [ ] Registry: `HKLM\SYSTEM\Accessibility\CursorSize` + `CursorColor`; boot restore in `accessibility_init()`
- [ ] Commit: `"accessibility: large_cursor -- cursor_set_size 32/48/64/96, color WHITE/BLACK/XOR, boot restore"`

## 5. Screen Magnifier `[Sonnet]`

**Design:** [`shell.md#magnifier`](../../docs/design/shell.md#magnifier)

Win+Plus zoom in (2×/4×/6×/8×), Win+Minus zoom out, Win+Escape close. Magnifier overlay: tracks cursor, copies `gfx_blit` source region from compositor backbuffer at zoom factor to corner/edge panel. `HKLM\SYSTEM\Accessibility\MagnifierZoom`.

**Files:** `src/desktop/magnifier.c` (new), `include/desktop/magnifier.h` (new)

> [!NOTE]
> **Overlay window**: lens mode per `docs/design/shell.md#magnifier`: a 400 x 300 (`THEME_SIZE_MAGNIFIER_LENS_*`) lens following the pointer, radius 8, 2 px accent border, flyout elevation; a compact flyout-acrylic toolbar at the top centre (zoom out, level, zoom in, view menu Full screen / Lens / Docked, settings) that fades after 3 s; `WM_FLAG_NO_TASKBAR | WM_FLAG_TOPMOST | WM_FLAG_NO_RESIZE`; `g_zoom = 2` (levels: 2/4/6/8). **Source region**: centered on cursor `(mx, my)`: `src_w = overlay_w / g_zoom`; `src_h = overlay_h / g_zoom`; `src_x = mx - src_w/2`; `src_y = my - src_h/2`; clamp to screen bounds. **Nearest-neighbour zoom blit**: for `row = 0..overlay_h`: for `col = 0..overlay_w`: `src_pixel = compositor_backbuf[(src_y + row/g_zoom) * screen_w + (src_x + col/g_zoom)]`; write to overlay surface. This runs every compositor frame when magnifier is active. **Cursor follow**: magnifier updates each compositor tick while `g_magnifier_active`. **Win+Plus**: `g_zoom = min(g_zoom * 2, 8)`. **Win+Minus**: `g_zoom = max(g_zoom / 2, 2)`. **Win+Escape**: `magnifier_close()` → `wm_destroy_window()`. **View modes** (`docs/design/shell.md#magnifier`): Lens (default, follows the pointer), Full screen, Docked (top third of the screen, 2 px accent separator). `HKLM\SYSTEM\Accessibility\MagnifierZoom` (save/restore).

- [ ] `void magnifier_open(void)` -- `wm_create_window()` topmost lens `THEME_SIZE_MAGNIFIER_LENS_WIDTH` x `THEME_SIZE_MAGNIFIER_LENS_HEIGHT` (400 x 300) per `docs/design/shell.md#magnifier`; set `g_magnifier_active=1`
- [ ] `void magnifier_render(void)` -- `fb_get_backbuffer()` source region → nearest-neighbour zoom → blit to overlay surface; called each compositor frame
- [ ] Source region calc: `src_x = mx - (overlay_w/2)/g_zoom`; `src_y = my - (overlay_h/2)/g_zoom`; clamp
- [ ] Win+Plus hotkey → `g_zoom = min(g_zoom*2, 8)`; Win+Minus → `g_zoom = max(g_zoom/2, 2)`
- [ ] Win+Escape → `magnifier_close()` + `g_magnifier_active=0` + Registry save
- [ ] Magnifier follows cursor (re-computes source rect each frame from `g_mouse_x/y`)
- [ ] `HKLM\SYSTEM\Accessibility\MagnifierZoom` persist; boot restore if `g_magnifier_active` was set
- [ ] Commit: `"accessibility: magnifier -- Win+Plus/Minus zoom 2-8x, compositor blit overlay, cursor track"`

## 6. Mouse Keys `[Sonnet]`

Numpad controls mouse cursor: 4/6=L/R, 2/8=D/U, 7/9/1/3=diagonals, 5=click, +=double-click, 0=mouse-down, .=mouse-up. Activated by Alt+Shift+Numlock. Movement speed configurable. `HKLM\SYSTEM\Accessibility\MouseKeys`.

**Files:** extend `src/kernel/drivers/keyboard.c`, extend `src/kernel/accessibility.c`

> [!NOTE]
> `g_mouse_keys_active` flag toggled by `Alt+Shift+Numlock` in keyboard modifier state check. When active: intercept numpad scancodes (Numpad0–9, NumpadDecimal, NumpadPlus) BEFORE normal key dispatch; map to `mouse_event_inject(dx, dy, buttons)`. **Movement**: `dx/dy = ±g_mousekeys_speed` (default 4 px/step); each numpad keydown adds delta to `g_mouse_x/y` (clamp to screen bounds); generate `WM_MOUSE_MOVE` synthetic event. **Acceleration**: if key held (keyrepeat): multiply speed by `1.5` every 500 ms (max 3× base speed). **Click**: Numpad5 → `WM_MOUSE_DOWN + WM_MOUSE_UP`; NumpadPlus → two click pairs (double-click); Numpad0 → `WM_MOUSE_DOWN` (hold); NumpadDecimal → `WM_MOUSE_UP` (release hold). **Speed config**: `HKLM\SYSTEM\Accessibility\MouseKeysSpeed` (2/4/8/16 px); `HKLM\SYSTEM\Accessibility\MouseKeys` toggle. **Visual cue**: `notify_send("Mouse Keys On/Off", ...)` on toggle. **`mouse_event_inject(dx, dy, buttons)`**: new function that pushes a synthetic mouse event into the WM event queue as if real hardware generated it.

- [ ] `g_mouse_keys_active` flag; `Alt+Shift+Numlock` toggle in keyboard ISR
- [ ] `mouse_event_inject(int dx, int dy, int buttons)` -- push synthetic WM mouse event
- [ ] Numpad intercept: map Numpad4/6/8/2/7/9/1/3 → `dx/dy`; Numpad5/+/0/. → button events
- [ ] Movement speed `g_mousekeys_speed` from `HKLM\SYSTEM\Accessibility\MouseKeysSpeed`; key-repeat acceleration
- [ ] `notify_send("Mouse Keys On")` / `"Off"` on toggle
- [ ] Registry: `MouseKeys` toggle + `MouseKeysSpeed` persist; boot restore
- [ ] Commit: `"accessibility: mouse_keys -- Numpad mouse control, Alt+Shift+Numlock toggle, synthetic events"`

## 7. Reduced Motion & Color Blind Mode `[Sonnet]`

`HKLM\SYSTEM\Accessibility\ReducedMotion=1` → `anim_set_enabled(0)` in animation engine; all transitions instant. Stretch: color blind simulation via `fb_set_color_matrix()` -- 3×3 LUT applied at compositor output stage for Deuteranopia/Protanopia/Tritanopia.

**Files:** extend `src/kernel/accessibility.c`; extend `src/kernel/drivers/framebuffer.c` for color matrix

> [!NOTE]
> `accessibility_set_reduced_motion(int on)`: `registry_set("HKLM\\SYSTEM\\Accessibility\\ReducedMotion", ...)` + `anim_set_enabled(!on)` -- call from `08-graphics-ui` animation engine. **Color blind stretch**: `fb_set_color_matrix(float mat[9])` -- applied per-pixel during `fb_flush()` by multiplying `[R, G, B]` × mat. Three presets:
> - **Deuteranopia** (green-blind): `[[0.367, 0.861, -0.228], [0.280, 0.673, 0.047], [-0.012, 0.043, 0.969]]`
> - **Protanopia** (red-blind): `[[0.152, 1.053, -0.205], [0.115, 0.786, 0.099], [-0.004, -0.048, 1.052]]`
> - **Tritanopia** (blue-blind): `[[1.256, -0.077, -0.179], [-0.078, 0.931, 0.148], [0.005, 0.691, 0.304]]`
> `fb_set_color_matrix(NULL)` disables (identity matrix). Registry `HKLM\SYSTEM\Accessibility\ColorBlindMode` (0=off, 1=Deuteranopia, 2=Protanopia, 3=Tritanopia). Integer arithmetic approximation (fixed-point ×256 then shift) to avoid float in `fb_flush()` hot path.

- [ ] `accessibility_set_reduced_motion(int on)` -- `registry_set()` + `anim_set_enabled(!on)` call
- [ ] Boot restore: `accessibility_init()` reads `ReducedMotion` → `anim_set_enabled()` immediately
- [ ] `fb_set_color_matrix(int mat_q8[9])` -- fixed-point `×256` color matrix in `framebuffer.c`; applied in `fb_flush()` hot path
- [ ] Three color blind presets as `int[9]` constants (Q8 fixed-point)
- [ ] `accessibility_set_color_blind_mode(int mode)` -- 0/1/2/3; `fb_set_color_matrix()` + Registry
- [ ] Boot restore: read `ColorBlindMode` → apply matrix before first frame
- [ ] Commit: `"accessibility: reduced_motion + color_blind -- anim_set_enabled, fb color matrix, 3 CB presets"`

## 8. `ease.cpl` -- Accessibility Settings Applet `[Sonnet]`

**Design:** [`shell.md#accessibility`](../../docs/design/shell.md#accessibility)

One-page Control Panel applet with all 8 toggles: High Contrast, Large Text, Sticky Keys, Large Cursor (slider), Magnifier, Mouse Keys, Reduced Motion, Color Blind mode. Linked to each Registry key. Changes apply instantly.

**Files:** `src/apps/control/applets/ease.c` (new)

> [!NOTE]
> The page renders inside the Settings and Control Panel frame (`docs/design/shell.md#settings-and-control-panel-frame`) as settings rows (`docs/design/controls.md#cards-and-settings-rows`), no fixed-size surface. On/off settings use toggle switches (`docs/design/controls.md#toggle-switch`), choices use combo boxes, ranges use sliders: **High Contrast**: toggle switch "High Contrast" → `accessibility_set_high_contrast()`. **Large Text**: combo box "100% / 125% / 150% / 200% / 250% / 300%" → `accessibility_set_dpi()`. **Sticky Keys**: toggle switch → `accessibility_set_sticky_keys()`; on-change: if enabling → simulate 5× Shift activation flow. **Large Cursor**: `CTRL_SLIDER(32, 96)` with tick marks at 32/48/64/96 → `cursor_set_size()`; below it: combo box "White / Black / Inverted" → `cursor_set_color()`. **Magnifier**: toggle switch + combo box "2× / 4× / 6× / 8×" initial zoom → `magnifier_open()` or `magnifier_close()`. **Mouse Keys**: toggle switch → `accessibility_set_mouse_keys()`; speed `CTRL_SLIDER(2, 16)` → `registry_set("MouseKeysSpeed")`. **Reduced Motion**: toggle switch → `accessibility_set_reduced_motion()`. **Color Blind Mode**: combo box "Off / Deuteranopia / Protanopia / Tritanopia" → `accessibility_set_color_blind_mode()`. All controls initialized from current `g_accessibility` state. Changes apply immediately (no Apply button needed). Keyboard: Tab cycles between controls; Space flips toggle switches; Enter accepts dropdown.

- [ ] `ease.c` implementing `CPlApplet()` `CPL_INIT/INQUIRE/DBLCLK/STOP`
- [ ] Populate all controls from `g_accessibility` struct on `CPL_DBLCLK`
- [ ] Toggle switch High Contrast → immediate `accessibility_set_high_contrast()` on toggle
- [ ] Combo box DPI → `accessibility_set_dpi(pct)` on change
- [ ] Toggle switch Sticky Keys → `accessibility_set_sticky_keys(on)`
- [ ] `CTRL_SLIDER(32, 96)` cursor size → `cursor_set_size(px)` on drag; Combo box color → `cursor_set_color()`
- [ ] Toggle switch Magnifier + zoom dropdown → `magnifier_open/close()` + `g_zoom = selected`
- [ ] Toggle switch Mouse Keys + speed slider → `accessibility_set_mouse_keys()` + `registry_set("MouseKeysSpeed")`
- [ ] Toggle switch Reduced Motion → `accessibility_set_reduced_motion()`
- [ ] Combo box Color Blind → `accessibility_set_color_blind_mode()`
- [ ] Commit: `"ease.cpl: accessibility settings page -- settings rows, toggle switches, instant apply"`

---

## OS Comparison


| ⭐  | Feature                   | 🪟 Win11                                           | 🐧 Linux                                                                        | 🚀 Impossible OS                                                          |
| --- | ------------------------- | -------------------------------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| 💎  | High contrast mode        | ✅ High Contrast themes (4 presets);               | ✅ GNOME/KDE high contrast theme; GTK                                           | ⬜ §1 -- `THEME_FLUENT_HIGH_CONTRAST` preset wired via `WM_THEME_CHANGED` |
| 💎  | Large text / DPI override | ✅ Display Settings → Scale: 100–350%;             | ✅ GNOME: text-scaling-factor; KDE: Force Font                                  | ⬜ §2 -- `dpi_set_scale()` + `WM_DPI_CHANGED` broadcast; all              |
| 💎  | Sticky keys               | ✅ Sticky Keys: 5× Shift toggle;                   | ✅ GNOME/KDE: Accessibility → Sticky Keys;                                      | ⬜ §3 -- in-keyboard-ISR 5× Shift detect; `g_sticky_modifier_state`       |
| 💎  | Large cursor              | ✅ Mouse Pointer size 1–15 (up                     | ✅ GNOME/KDE: cursor size setting (16–96                                        | ⬜ §4 -- `cursor_set_size()` with `image_scale()` fallback; XOR           |
| ⭐  | Screen magnifier          | ✅ Magnifier: Win+Plus; docked/full/lens modes; up | ✅ GNOME Magnifier (built-in compositor); KMagnifier;                           | ⬜ §5 -- `⭐` in-kernel compositor nearest-neighbour zoom                 |
| 💎  | Mouse keys                | ✅ Mouse Keys in Ease of                           | ✅ XKB `MouseKeys`; `xdotool`; GNOME Mouse                                      | ⬜ §6 -- in-ISR numpad intercept + `mouse_event_inject()`                 |
| 💎  | Reduced motion            | ✅ Settings → Accessibility → Visual               | ✅ GNOME: `reduced-motion` preference; KDE: animation                           | ⬜ §7 -- single `anim_set_enabled(0)` gate; all window                    |
| ⭐  | Color blind simulation    | ✅ No built-in color blind simulation;             | ✅ GNOME: Color Filters (Deuteranopia/Protanopia/Tritanopia/Desaturation); KWin | ⬜ §7 -- (stretch) -- ; `⭐` fixed-point                                  |
| 💎  | `ease.cpl`                | ✅ Ease of Access Center; Settings                 | ✅ GNOME Accessibility Settings; KDE Accessibility;                             | ⬜ §8 -- single-page applet with all 8                                    |

> **After §1–§8:** Impossible OS meets production accessibility requirements matching Windows 11 Ease of Access. The `⭐` differentiators: the screen magnifier runs as a native compositor zoom layer (no separate DWM magnification API hook, zero extra processes); the color blind simulation is applied as a fixed-point 3×3 matrix at the `fb_flush()` output stage (transforms every pixel at the hardware boundary rather than per-widget, so it is universal and cannot be bypassed by any app).

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] ease.cpl → toggle High Contrast → desktop colors change instantly to black/white/yellow
- [ ] ease.cpl → DPI 150% → all windows, text, and controls enlarge; switch back → revert
- [ ] Press Shift 5 times rapidly → "Sticky Keys On" notification; press Ctrl → tray shows "Ctrl"; press A → Ctrl+A fires; latch clears
- [ ] ease.cpl → Cursor size 64 → cursor visibly larger; color "Inverted" → cursor XOR-composited with screen
- [ ] Win+Plus → magnifier overlay appears top-right; cursor movement → source region tracks; Win+Plus again → 4× zoom; Win+Escape → magnifier closes
- [ ] Alt+Shift+Numlock → "Mouse Keys On"; Numpad8 → cursor moves up; Numpad5 → click event fires
- [ ] ease.cpl → Reduced Motion ON → open/close a window → no animation (instant appear/disappear)
- [ ] Color blind mode: ease.cpl → Deuteranopia → screenshot shows green-red shift in compositor output
- [ ] All settings persist across reboot (`accessibility_init()` restores all from Registry)
- [ ] Commit: `"accessibility: high-contrast, large-text, sticky-keys, large-cursor, magnifier, mouse-keys, reduced-motion, ease.cpl -- complete"`
