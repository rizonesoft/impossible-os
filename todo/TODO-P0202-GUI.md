# P0202 — GUI & Visual Foundation (Hub)

> **Goal:** Build a complete, Windows 11-quality desktop shell with a full widget
> toolkit, modern visuals, and rich interactivity.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

> **Note:** Items marked `[x]` were completed in earlier phases.

---

## 1. Theme System *(from Phase 02 §9.1)*

> Foundation: every visual element references the theme instead of hardcoded hex colors.

**Prompt:** Centralize all UI colors into a `theme_t` struct with named fields: background, foreground, accent, border, shadow, titlebar_active, titlebar_inactive, button_bg, button_hover, selection, error, warning. Load theme colors from the Registry under `HKCU\Software\Impossible\Theme\*`. Provide two built-in presets: Dark (dark backgrounds, light text, blue accent) and Light (light backgrounds, dark text). Every drawing function in `desktop.c`, `wm.c`, and `controls.c` must reference `theme_get()->field` instead of hardcoded hex colors. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: theme system"`.


- [ ] Define `theme_t` struct with all UI colors (bg, fg, accent, border, shadow, titlebar, button states, etc.)
- [ ] Create `include/desktop/theme.h` and `src/desktop/theme.c`
- [ ] Load theme from Registry (`HKCU\Software\Impossible\Theme\*`)
- [ ] Built-in Dark mode preset (default)
- [ ] Built-in Light mode preset
- [ ] All drawing functions in `desktop.c`, `wm.c`, `controls.c` reference `theme_get()->field`
- [ ] Apply accent color to focused controls, active title bars, selection highlights
- [ ] Commit: `"desktop: theme system"`

---

## 2. Widget & Control Library

> **See [TODO-P0204-Controls.md](TODO-P0204-Controls.md)** — Basic controls (Button, Label,
> TextBox, ScrollBar) + 13 extended widgets (Checkbox, Radio, Dropdown, Slider,
> ProgressBar, Tabs, ListView, TreeView, Toolbar, MenuBar, StatusBar, GroupBox,
> Tooltip) + Dialog system.

---

## 3. Animation Engine

> **See [TODO-P0205-Animation.md](TODO-P0205-Animation.md)** — Tween engine with easing
> functions + window transition animations.

---

## 4. Window Manager Enhancements

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §1** — Minimize/maximize,
> keyboard & edge snapping, snap layouts.

---

## 5. Context Menu System

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §2** — Generic context menu
> engine, desktop right-click, file right-click.

---

## 6. Taskbar

> **See [TODO-P0304-Taskbar.md](TODO-P0304-Taskbar.md)** — Window list, button context menu,
> Aero Peek.

---

## 7. Start Menu

> **See [TODO-P0303-Start-Menu.md](TODO-P0303-Start-Menu.md)** — Layout, data loading,
> interaction, search.

---

## 8. System Tray & Notifications

> **See [TODO-P0305-Notifications.md](TODO-P0305-Notifications.md)** — Tray icons, toasts,
> notification center.

---

## 9. Desktop Icons & Shortcuts

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §3** — Icon grid, shortcuts,
> drag reorder.

---

## 10. Keyboard Shortcuts & Task Switching

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §4** — Hotkey manager,
> Alt+Tab, Win+R run dialog.

---

## 11. Drag and Drop

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §5** — Drag state, visual
> feedback, file/text drag.

---

## 12. Quick Settings Panel

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §6** — Toggle grid, sliders.

---

## 13. Screenshot Capture

> **See [TODO-P0509-Utility-Apps.md](TODO-P0509-Utility-Apps.md) §3** — Screenshot tool
> (PrtSc, Alt+PrtSc, Win+Shift+S, region select).

---

## 14. DPI Scaling

### 14.1 DPI System

- [ ] Create `include/dpi.h` with `DPI(px)` macro: `(pixels * scale / 100)`
- [ ] `dpi_get_scale()`, `dpi_auto_detect()` (≥3840→200%, ≥2560→150%, else 100%)
- [ ] Store in Registry: `HKLM\SYSTEM\Display\Scale`, `HKLM\SYSTEM\Display\AutoScale`
- [ ] Commit: `"display: DPI scaling system"`

### 14.2 DPI-Aware UI

- [ ] `desktop.c`: taskbar height `DPI(48)`, button padding, menu sizes
- [ ] `wm.c`: title bar `DPI(32)`, borders, corner radius, button sizes
- [ ] `controls.c`: scrollbar width `DPI(16)`, minimum click target `DPI(32)`
- [ ] Font sizes: `font_get(FONT_UI, DPI(14))`
- [ ] Commit: `"desktop: DPI-aware layout"`

---

## 15. Virtual Desktops

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §7** — Up to 8 workspaces,
> Ctrl+Win shortcuts, taskbar filtering.

---

## 16. Night Light

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §8** — Blue light filter
> with scheduling.

---

## 17. Focus / Do Not Disturb

> **See [TODO-P0301-Desktop-Shell.md](TODO-P0301-Desktop-Shell.md) §9** — Notification
> suppression modes.

---

## 18. Boot Splash Screen

> **See [TODO-P0307-Boot-Splash.md](TODO-P0307-Boot-Splash.md)** — Logo, progress bar,
> boot menu.

---

## 19. Screensaver & Lock Screen

> **See [TODO-P0306-Screensaver.md](TODO-P0306-Screensaver.md)** — Idle detection,
> screensavers, lock screen.

---

## 20. Desktop Widgets

- [ ] Create `src/desktop/widgets.c`
- [ ] Widget API: `widget_fn(msg, surface, ctx)` — WGT_INIT/RENDER/TICK/CLOSE
- [ ] Widget manager: load, position, update
- [ ] Draggable positioning, semi-transparent background
- [ ] Built-in: Clock, CPU Meter, RAM Monitor, Calendar, Quick Notes
- [ ] Commit: `"desktop: widget framework + built-in widgets"`

---

## 21. Display & Resolution

### 21.1 Dynamic Resolution

- [ ] `display_enum_modes()` — query VESA/VBE modes
- [ ] `display_get_mode()` — return current resolution
- [ ] *(Stretch)* `display_set_mode(w, h)` — runtime change (requires virtio-gpu)
- [ ] Commit: `"display: resolution management"`

### 21.2 Multi-Monitor (Stretch)

- [ ] *(Stretch)* `struct monitor` (id, resolution, position, DPI, framebuffer)
- [ ] *(Stretch)* Virtual desktop coordinate space

---

## 22. Software OpenGL

- [ ] Port TinyGL (~5000 lines, Zlib license) to Impossible OS framebuffer
- [ ] Basic OpenGL 1.1: `glBegin/glEnd`, vertices, colors, textures, z-buffer
- [ ] Test: rotating cube
- [ ] Commit: `"gfx: TinyGL software OpenGL 1.1"`

---

## Already Completed ✅

- [x] **2D Compositing Library** — surfaces, primitives, alpha blending, gradients, blur, Mica, Acrylic, shadows, reveal highlight, SIMD optimization *(Phase 02 §1)*
- [x] **TrueType Font System** — stb_truetype, font manager, glyph caching, replaced bitmap font *(Phase 02 §2)*
- [x] **Runtime Image Decoding** — stb_image, JPEG/PNG wallpaper, image scaling *(Phase 02 §3)*
- [x] **System Icon Store** — IRES format, Fluent UI font icons, ICO loader, file type mapping *(Phase 02 §4)*
- [x] **Cursor Manager** — Adwaita X11 cursors, 11 shapes, embedded fallbacks *(Phase 02 §5.1–5.2)*
- [x] **Context-Aware Cursor Switching** — wm/desktop context, resize/move/hand cursors *(Phase 02 §5.3)*
- [x] **Dirty Rectangle Compositor** — partial redraws, fb_swap_rect *(implemented)*
- [x] **Basic Window Manager** — create, move, resize, close, title bar, focus *(Phase 01)*
- [x] **Basic Taskbar** — start button, clock, window buttons *(Phase 01)*
- [x] **Basic Controls** — Button, Label, TextBox, ScrollBar *(implemented)*

---

## Priority Order

| Priority | Section | File |
|----------|---------|------|
| 🔴 P0 | Theme System | This file §1 |
| 🔴 P0 | Core Controls | `P0204-Controls.md` |
| 🔴 P0 | Tween Engine | `P0205-Animation.md` |
| 🔴 P0 | Minimize/Maximize | `P0301-Desktop-Shell.md` |
| 🔴 P0 | Context Menu Engine | `P0301-Desktop-Shell.md` |
| 🟠 P1 | Advanced Controls | `P0204-Controls.md` |
| 🟠 P1 | Taskbar Window List | `P0304-Taskbar.md` |
| 🟠 P1 | Start Menu | `P0303-Start-Menu.md` |
| 🟠 P1 | Desktop Icons | `P0301-Desktop-Shell.md` |
| 🟠 P1 | Keyboard Shortcuts | `P0301-Desktop-Shell.md` |
| 🟡 P2 | Dialogs | `P0204-Controls.md` |
| 🟡 P2 | Window Animations | `P0205-Animation.md` |
| 🟡 P2 | Window Snapping | `P0301-Desktop-Shell.md` |
| 🟡 P2 | Notifications | `P0305-Notifications.md` |
| 🟡 P2 | Boot Splash | `P0307-Boot-Splash.md` |
| 🟢 P3 | Drag and Drop | `P0301-Desktop-Shell.md` |
| 🟢 P3 | DPI Scaling | This file §14 |
| 🟢 P3 | Virtual Desktops | `P0301-Desktop-Shell.md` |
| 🔵 P4 | Screensaver/Lock | `P0306-Screensaver.md` |
| 🔵 P4 | Desktop Widgets | This file §20 |
| 🔵 P4 | Display/Resolution | This file §21 |
| 🔵 P4 | Software OpenGL | This file §22 |
