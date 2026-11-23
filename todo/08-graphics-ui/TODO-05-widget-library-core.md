---
schema_version: 1
id: widget-library-core
domain: 08-graphics-ui
status: active
title: "TODO-05 -- Extended Widget Library: Core Controls"
---

# TODO-05 -- Extended Widget Library: Core Controls

> **Goal:** Add the 6 most-needed missing controls to `controls.c`: Checkbox, Radio Button, Dropdown/ComboBox, Slider/TrackBar, Progress Bar, and Tab Strip. These are required by every Settings applet and app UI. All new controls use `theme_get()->field` from day one. The indeterminate progress bar uses the animation engine tween from TODO-04. The dropdown floating popup uses the WM `z_order` overlay mechanism.

> [!IMPORTANT]
> **Already implemented** -- do not re-implement: `CTRL_BUTTON`, `CTRL_LABEL`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR`; `ctrl_draw_all()`, `ctrl_handle_mouse()`, `ctrl_handle_key()`, `ctrl_set_text/get_text/set_enabled`. `gfx_fill_rounded_rect(s, x, y, w, h, radius, color)` and `gfx_fill_circle(s, cx, cy, r, color)` are in `include/gfx.h`. `z_order` field is in `struct wm_window` -- set to a high value for dropdown overlay. All new controls must use `theme_get()->field` (never hardcoded hex); `CTRL_COLOR_*` constants in `controls.h` will be removed by TODO-03 migration -- do not add new ones. TODO-04 `anim_mgr_add()` drives the indeterminate progress bar tween. `CTRL_MAX_PER_WINDOW = 32` is the per-window control limit -- counts against all 6 new types combined with existing controls. Complete sections in order: Checkbox → Radio → Slider → Progress Bar → Dropdown → Tab Strip → Theming → Accessibility.

## Inputs

- `include/desktop/controls.h` -- `enum ctrl_type`, `struct control`, `CTRL_MAX_PER_WINDOW=32`; extend with 6 new type enum values and new API declarations
- `src/desktop/controls.c` -- extend `ctrl_draw_all()`, `ctrl_handle_mouse()`, `ctrl_handle_key()` dispatch tables for new types
- `include/gfx.h` -- `gfx_fill_rounded_rect()`, `gfx_fill_circle()`, `gfx_fill_rect_alpha()` for new control rendering
- `include/desktop/theme.h` (TODO-03) -- `theme_get()->colors.<token>` (design colour tokens) + `THEME_SIZE_*` / `THEME_RADIUS_*` -- all colours and geometry for new controls, per `docs/design/controls.md`
- `include/kernel/gfx/anim_mgr.h` (TODO-04) -- `anim_mgr_add()`, `gfx_tween_start()` for indeterminate progress bar animation
- `include/desktop/wm.h` -- `wm_create_window()` + `z_order` field for dropdown floating popup overlay
- → XREF: `08-graphics-ui/TODO-03-theme-system.md` -- prerequisite; `theme_get()` must be live and `CTRL_COLOR_*` migration done before new controls paint correctly
- → XREF: `08-graphics-ui/TODO-04-animation-engine.md` -- prerequisite for §4 indeterminate progress bar; `anim_mgr_add()` must be available
- Related (no stable XREF target): `08-graphics-ui/TODO-06-widget-dialogs.md` (advanced widgets) -- ListView, TreeView, Tooltip, Dialog use these 6 controls as building blocks; Tab Strip is consumed by Settings applet immediately
- → XREF: `08-graphics-ui/TODO-07-ui-accessibility-automation-ime.md §1-§3` -- §8 name/role helpers are only the seed; full semantic-tree and provider ownership lives there
- -> XREF: `TODO-19-fluent-component-library.md §1` -- the control authoring contract these widgets adopt; the rest of the modern control set lives there

## Outcome

- `CTRL_CHECKBOX`, `CTRL_RADIO`, `CTRL_DROPDOWN`, `CTRL_SLIDER`, `CTRL_PROGRESSBAR`, `CTRL_TABSTRIP` added to `enum ctrl_type`.
- Full `create/set/get/draw/handle` implementation for all 6 types in `controls.c`.
- Dropdown popup renders as a high-z-order borderless overlay window; click outside closes it.
- Indeterminate progress bar animates via `anim_mgr_add()` with a looping tween.
- All controls use `theme_get()` tokens; zero hardcoded hex.
- `ctrl_get_accessible_name/role()` stubs for all 8 control types (4 existing + 6 new).

## Implementation Order

| ⭐  | Order | Deliverable                                                                                  | Depends On                                                                         | Status |
| --- | :---: | -------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Checkbox -- `CTRL_CHECKBOX`, 20 px box radius 4, accent checked state, focus rings        | Existing `gfx_fill_rounded_rect`; `theme_get()` (TODO-03), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   2   | §2 Radio button -- `CTRL_RADIO`, 20 px circle + 12 px dot, group mutual exclusion            | §1 (same struct extension pattern; group_id field added alongside), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   3   | §3 Slider -- `CTRL_SLIDER`, track + thumb drag, horiz/vert, real-time callback               | §1 (same `on_change` callback type established in §1), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   4   | §4 Progress bar -- `CTRL_PROGRESSBAR`, determinate + indeterminate tween mode                | §3 slider (determinate fill is same pattern); TODO-04 `anim_mgr_add()`, T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   5   | §5 Dropdown -- `CTRL_DROPDOWN`, floating popup via WM z-order overlay, keyboard navigation   | All of §1–4 done (dropdown is most complex; isolated until others stable), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   6   | §6 Tab strip -- `CTRL_TABSTRIP`, tab headers, accent underline, keyboard arrow navigation    | §5 dropdown (all input-capture patterns established), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   7   | §7 Theming -- confirm all new controls use `theme_get()` only; remove any CTRL_COLOR_* usage | §6 (all controls must exist before audit), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| ⭐  |   8   | §8 Accessibility stubs -- `ctrl_get_accessible_name/role()` for all 8 types                  | §7 (all control types must be registered before role table is complete), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   9   | §9 Overlay scroll bar -- collapse/expand on hover                                            | §7, T19 §27, `11-apps/TODO-15` §1 |  [ ]   |

---

## 1. Checkbox

**Design:** [`controls.md#check-box-and-radio-button`](../../docs/design/controls.md#check-box-and-radio-button)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Checkbox"`

## 2. Radio Button

**Design:** [`controls.md#check-box-and-radio-button`](../../docs/design/controls.md#check-box-and-radio-button)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Radio Button"`

## 3. Slider / TrackBar

**Design:** [`controls.md#slider`](../../docs/design/controls.md#slider)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Slider / TrackBar"`

## 4. Progress Bar

**Design:** [`controls.md#progress`](../../docs/design/controls.md#progress)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Progress Bar"`

## 5. Dropdown / ComboBox

**Design:** [`controls.md#combo-box-and-drop-down`](../../docs/design/controls.md#combo-box-and-drop-down)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Dropdown / ComboBox"`

## 6. Tab Strip

**Design:** [`controls.md#tabs`](../../docs/design/controls.md#tabs)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Tab Strip"`

## 7. Theming

**Design:** [`index.md#how-does-this-relate-to-the-theme-system`](../../docs/design/index.md#how-does-this-relate-to-the-theme-system), [`shell.md#materials`](../../docs/design/shell.md#materials)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Theming"`

## 8. Accessibility Stubs

**Design:** [`shell.md#accessibility`](../../docs/design/shell.md#accessibility)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Accessibility Stubs"`

---

## 9. Overlay Scroll Bar (Collapse and Expand on Hover)

> **Spawned-by:** root

**Design:** [`controls.md#scroll-bar`](../../docs/design/controls.md#scroll-bar)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Overlay Scroll Bar (Collapse and Expand on Hover)"`

**Test checkpoint:** in the controls gallery a long list shows no bar at rest, a 2 px line while scrolling, and a 6 px bar with arrows on hover; the list's text never reflows. Test on: QEMU TCG, VirtualBox.

## OS Comparison


| ⭐  | Feature             | 🪟 Win11                                                 | 🐧 Linux                                                                | 🚀 Impossible OS                                                                |
| --- | ------------------- | -------------------------------------------------------- | ----------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| 💎  | Checkbox            | ✅ WinUI3 `CheckBox`; BS_CHECKBOX; full state            | ✅ GTK `GtkCheckButton`; Qt `QCheckBox`; full                           | ⬜ §1 -- `gfx_fill_rounded_rect` + Fluent checkmark glyph                       |
| 💎  | Radio button        | ✅ WinUI3 `RadioButton`; BS_RADIOBUTTON; `WM_COMMAND` on | ✅ GTK `GtkRadioButton`; `..._from_widget` group link                   | ⬜ §2 -- `gfx_fill_circle` outer ring + inner                                   |
| 💎  | Dropdown / ComboBox | ✅ WinUI3 `ComboBox`; Win32 `CBS_DROPDOWN`; popup        | ✅ GTK `GtkComboBox`; Qt `QComboBox`; popup                             | ⬜ §5 -- `wm_create_window` at z_order=9999; borderless; click-outside-to-close |
| 💎  | Slider / TrackBar   | ✅ WinUI3 `Slider`; Win32 `TRACKBAR_CLASS`; TBS_VERT     | ✅ GTK `GtkScale`; Qt `QSlider`; both                                   | ⬜ §3 -- accent fill track + circle                                             |
| 💎  | Progress bar        | ✅ WinUI3 `ProgressBar`; Win32 PBS_MARQUEE for           | ✅ GTK `GtkProgressBar`; `gtk_progress_bar_pulse()` for indeterminate   | ⬜ §4 -- indeterminate uses `anim_mgr_add()` looping tween                      |
| 💎  | Tab strip           | ✅ WinUI3 `TabView`; Win32 `WC_TABCONTROL`; keyboard     | ✅ GTK `GtkNotebook`; Qt `QTabWidget`; arrow                            | ⬜ §6 -- 16 tabs max; Windows 11 selected-tab shape                             |
| ⭐  | Theming             | ✅ WinUI3 resource brush system; Win32                   | ⚠️ GTK CSS variables per widget;                                        | ⬜ §7 -- `⭐` new controls never had                                            |
| 💎  | Accessibility stubs | ✅ UIA (UI Automation); `IUIAutomationElement`; full     | ✅ ATK/AT-SPI2; `AtkObject::get_name/get_role`; full accessibility tree | ⬜ §8 -- plain-text name/role stubs; ARIA role                                  |

> **After §1–§8:** Impossible OS has the complete set of controls needed for every settings applet, dialog box, and app panel. The `⭐` theming advantage is that all 6 new controls are written against `theme_get()` natively -- they never had hardcoded hex colors, unlike Win32 legacy controls which require a `WM_CTLCOLOR*` redirection chain to theme, and unlike GTK which has a parallel CSS variable system alongside older hardcoded GDK colors.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Checkbox: create window + `ctrl_create_checkbox`; click → `checked` toggles; `on_change` fires; visual states match hover/focus/disabled in QEMU
- [ ] Radio: 3 radio buttons same `group_id`; click one → others deselect; `ctrl_get_radio_group_selection` returns correct ctrl_id
- [ ] Slider: drag thumb → `on_change` fires with new value; click track left/right of thumb → value jumps; vertical orientation works
- [ ] Progress bar determinate: `ctrl_set_progress(wh, id, 75)` → 75% of the 3 px accent bar visible over the 1 px track in QEMU
- [ ] Progress bar indeterminate: `ctrl_progressbar_set_mode(… INDETERMINATE)` → a segment 30% of the track width sweeps left to right every 2 s (`docs/design/controls.md#progress`); switching back to DETERMINATE stops animation
- [ ] Dropdown: click field → popup appears above parent window; 8 items visible; Up/Down highlight; Enter selects; popup closes; selected text shows in field
- [ ] Dropdown: click anywhere outside popup → popup closes
- [ ] Tab strip: click tab 2 → `on_change(id, 1)` fires; tab 2 drawn as the selected `layer_bg` tab; Left/Right arrows on focused tabstrip navigate
- [ ] `rg "0x[0-9A-Fa-f]{6}" src/desktop/controls.c` → zero results (confirm after §7 audit)
- [ ] `ctrl_get_accessible_role(wh, checkbox_id)` returns `"checkbox"`; `ctrl_get_accessible_name(wh, slider_id)` returns the slider's current value as string
- [ ] Commit: `"controls: complete widget library core -- checkbox/radio/dropdown/slider/progressbar/tabstrip"`
