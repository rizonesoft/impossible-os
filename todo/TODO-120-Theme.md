# Theme System

> **Goal:** Centralize all UI colors into a theme struct so every visual element
> uses themed colors instead of hardcoded hex values.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

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

> **See [TODO-130-Controls.md](TODO-130-Controls.md)** — Basic controls (Button, Label,
> TextBox, ScrollBar) + 13 extended widgets (Checkbox, Radio, Dropdown, Slider,
> ProgressBar, Tabs, ListView, TreeView, Toolbar, MenuBar, StatusBar, GroupBox,
> Tooltip) + Dialog system.
