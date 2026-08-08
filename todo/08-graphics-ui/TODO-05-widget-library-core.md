---
schema_version: 1
id: widget-library-core
domain: 08-graphics-ui
status: active
title: "TODO-05 -- Extended Widget Library: Core Controls"
---

# TODO-05 -- Extended Widget Library: Core Controls

> **Goal:** Add the 6 most-needed missing controls to `controls.c`: Checkbox, Radio Button, Dropdown/ComboBox, Slider/TrackBar, Progress Bar, and Tab Strip. These are required by every Settings applet and app UI. All new controls use `theme_get()->field` from day one. The indeterminate progress bar uses the animation engine tween from TODO-02. The dropdown floating popup uses the WM `z_order` overlay mechanism.

> [!IMPORTANT]
> **Already implemented** -- do not re-implement: `CTRL_BUTTON`, `CTRL_LABEL`, `CTRL_TEXTBOX`, `CTRL_SCROLLBAR`; `ctrl_draw_all()`, `ctrl_handle_mouse()`, `ctrl_handle_key()`, `ctrl_set_text/get_text/set_enabled`. `gfx_fill_rounded_rect(s, x, y, w, h, radius, color)` and `gfx_fill_circle(s, cx, cy, r, color)` are in `include/gfx.h`. `z_order` field is in `struct wm_window` -- set to a high value for dropdown overlay. All new controls must use `theme_get()->field` (never hardcoded hex); `CTRL_COLOR_*` constants in `controls.h` will be removed by TODO-01 migration -- do not add new ones. TODO-02 `anim_mgr_add()` drives the indeterminate progress bar tween. `CTRL_MAX_PER_WINDOW = 32` is the per-window control limit -- counts against all 6 new types combined with existing controls. Complete sections in order: Checkbox → Radio → Slider → Progress Bar → Dropdown → Tab Strip → Theming → Accessibility.

## Inputs

- `include/desktop/controls.h` -- `enum ctrl_type`, `struct control`, `CTRL_MAX_PER_WINDOW=32`; extend with 6 new type enum values and new API declarations
- `src/desktop/controls.c` -- extend `ctrl_draw_all()`, `ctrl_handle_mouse()`, `ctrl_handle_key()` dispatch tables for new types
- `include/gfx.h` -- `gfx_fill_rounded_rect()`, `gfx_fill_circle()`, `gfx_fill_rect_alpha()` for new control rendering
- `include/desktop/theme.h` (TODO-01) -- `theme_get()->button_bg/hover/pressed/accent/border/foreground/foreground_muted/surface/scrollbar_track/scrollbar_thumb` -- all colors for new controls
- `include/kernel/gfx/anim_mgr.h` (TODO-02) -- `anim_mgr_add()`, `gfx_tween_start()` for indeterminate progress bar animation
- `include/desktop/wm.h` -- `wm_create_window()` + `z_order` field for dropdown floating popup overlay
- → XREF: `08-graphics-ui/TODO-03-theme-system.md` -- prerequisite; `theme_get()` must be live and `CTRL_COLOR_*` migration done before new controls paint correctly
- → XREF: `08-graphics-ui/TODO-04-animation-engine.md` -- prerequisite for §3 indeterminate progress bar; `anim_mgr_add()` must be available
- Related (no stable XREF target): `08-graphics-ui/TODO-05-*` (advanced widgets) -- ListView, TreeView, Tooltip, Dialog use these 6 controls as building blocks; Tab Strip is consumed by Settings applet immediately
- → XREF: `08-graphics-ui/TODO-07-ui-accessibility-automation-ime.md §1-§3` -- §8 name/role helpers are only the seed; full semantic-tree and provider ownership lives there

## Outcome

- `CTRL_CHECKBOX`, `CTRL_RADIO`, `CTRL_DROPDOWN`, `CTRL_SLIDER`, `CTRL_PROGRESSBAR`, `CTRL_TABSTRIP` added to `enum ctrl_type`.
- Full `create/set/get/draw/handle` implementation for all 6 types in `controls.c`.
- Dropdown popup renders as a high-z-order borderless overlay window; click outside closes it.
- Indeterminate progress bar animates via `anim_mgr_add()` with a looping tween.
- All controls use `theme_get()` tokens; zero hardcoded hex.
- `ctrl_get_accessible_name/role()` stubs for all 8 control types (4 existing + 6 new).

## Implementation Order

| ⭐  | Order | Deliverable                                                                                  | Depends On                                                                | Status |
| --- | :---: | -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Checkbox -- `CTRL_CHECKBOX`, 16×16 box + checkmark glyph, states, hover/focus             | Existing `gfx_fill_rounded_rect`; `theme_get()` (TODO-01)                 |  [ ]   |
| 💎  |   2   | §2 Radio button -- `CTRL_RADIO`, 16×16 circle + dot, group mutual exclusion                  | §1 (same struct extension pattern; group_id field added alongside)        |  [ ]   |
| 💎  |   3   | §4 Slider -- `CTRL_SLIDER`, track + thumb drag, horiz/vert, real-time callback               | §1 (same `on_change` callback type established in §1)                     |  [ ]   |
| 💎  |   4   | §5 Progress bar -- `CTRL_PROGRESSBAR`, determinate + indeterminate tween mode                | §3 slider (determinate fill is same pattern); TODO-02 `anim_mgr_add()`    |  [ ]   |
| 💎  |   5   | §3 Dropdown -- `CTRL_DROPDOWN`, floating popup via WM z-order overlay, keyboard navigation   | All of §1–4 done (dropdown is most complex; isolated until others stable) |  [ ]   |
| 💎  |   6   | §6 Tab strip -- `CTRL_TABSTRIP`, tab headers, accent underline, keyboard arrow navigation    | §5 dropdown (all input-capture patterns established)                      |  [ ]   |
| 💎  |   7   | §7 Theming -- confirm all new controls use `theme_get()` only; remove any CTRL_COLOR_* usage | §6 (all controls must exist before audit)                                 |  [ ]   |
| ⭐  |   8   | §8 Accessibility stubs -- `ctrl_get_accessible_name/role()` for all 8 types                  | §7 (all control types must be registered before role table is complete)   |  [ ]   |

---

## 1. Checkbox `[Sonnet]`

`CTRL_CHECKBOX` type. 16×16 rounded square (`gfx_fill_rounded_rect`, 3 px radius). Checkmark via Fluent icon codepoint when checked. Label to the right. States: unchecked/checked/disabled. Hover: accent at 20% opacity overlay. Focus: 1 px accent border.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> Add `CTRL_CHECKBOX` to `enum ctrl_type`. Checkbox union field in `struct control`: `struct { char text[CTRL_TEXT_MAX]; uint8_t checked; void (*on_change)(int ctrl_id, int checked); }`. Checkmark glyph: use Fluent icon codepoint `✓` (U+2713) rendered via `boot_font_render` or `gfx_draw_fluent_icon`; fallback: draw two line segments manually if font not available at draw time. Hover overlay: `gfx_fill_rect_alpha(s, bx-1, by-1, 18, 18, theme_get()->accent, 51)` (51 = 20% of 255). Focus: `gfx_draw_rect(s, bx-1, by-1, 18, 18, theme_get()->accent)`. Disabled: use `theme_get()->foreground_muted` for box border and text; no hover/focus response.

- [ ] `CTRL_CHECKBOX` in `enum ctrl_type` in `controls.h`
- [ ] Union field: `struct { char text[CTRL_TEXT_MAX]; uint8_t checked; void (*on_change)(int, int); } checkbox;`
- [ ] `int ctrl_create_checkbox(int wh, uint32_t x, uint32_t y, const char *text, int checked, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_set_checked(int wh, int id, int checked)` + `int ctrl_get_checked(int wh, int id)`
- [ ] Draw case in `ctrl_draw_all()`: `gfx_fill_rounded_rect` box (16×16, r=3, theme surface or accent if checked); checkmark glyph if checked; hover accent overlay; focus accent border; label text right of box
- [ ] Mouse handler: click on box (or label) toggles `checked`; calls `on_change`
- [ ] Commit: `"controls: CTRL_CHECKBOX -- box+checkmark, checked/hover/focus/disabled states"`

## 2. Radio Button `[Sonnet]`

`CTRL_RADIO` type. 16×16 circle (`gfx_fill_circle`). Inner filled dot (8 px) when selected. `group_id` field -- selecting one radio auto-deselects all others in the same window with the same `group_id`. Label to the right. Same hover/focus states as checkbox.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_RADIO` union field: `struct { char text[CTRL_TEXT_MAX]; int group_id; uint8_t selected; void (*on_change)(int ctrl_id, int selected); } radio;`. Mutual exclusion: in the mouse click handler for a radio button click, iterate all controls in the same window with `type == CTRL_RADIO && ctrl->radio.group_id == clicked->radio.group_id`; set each `selected = 0`; then set clicked `selected = 1`; call each one's `on_change` that changed state. Draw: outer circle border (`gfx_fill_circle` with theme border color, then inner `gfx_fill_circle` with surface color to create ring); when selected: inner dot `gfx_fill_circle` at 8 px radius with `theme_get()->accent`. Disabled: `theme_get()->foreground_muted` for all elements.

- [ ] `CTRL_RADIO` in `enum ctrl_type` in `controls.h`
- [ ] Union field: `struct { char text[CTRL_TEXT_MAX]; int group_id; uint8_t selected; void (*on_change)(int, int); } radio;`
- [ ] `int ctrl_create_radio(int wh, uint32_t x, uint32_t y, const char *text, int group_id, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_set_radio_selected(int wh, int id, int selected)` + `int ctrl_get_radio_selected(int wh, int id)`
- [ ] `int ctrl_get_radio_group_selection(int wh, int group_id)` → ctrl_id of currently selected radio in group, or -1
- [ ] Draw case: outer ring (16×16, `gfx_fill_circle` border then surface inner); inner dot when selected; hover overlay; focus border
- [ ] Mouse handler: on click → deselect all in same group → select this one → `on_change` callbacks
- [ ] Commit: `"controls: CTRL_RADIO -- circle+dot, group_id mutual exclusion, hover/focus/disabled"`

## 3. Slider / TrackBar `[Sonnet]`

`CTRL_SLIDER` type. Thin rounded-rect track (4 px tall for horiz, 4 px wide for vert). Accent-colored filled portion (track left of thumb). Circle thumb (12 px radius). Horizontal and vertical orientations. Drag thumb → real-time `on_change`. Click track → jump value.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_SLIDER` union field: `struct { int32_t min_val, max_val, value; uint8_t orientation; void (*on_change)(int ctrl_id, int32_t value); uint8_t dragging; } slider;`. Track fill: width = `(value - min) * track_w / (max - min)` px from left; fill color `theme_get()->accent`; unfilled track `theme_get()->scrollbar_track`. Thumb position: `thumb_cx = x + fill_width`; draw `gfx_fill_circle(s, thumb_cx, track_cy, 6, theme_get()->accent)`; thumb hover: `theme_get()->accent_hover`. Mouse-down on thumb area (within 8 px): set `dragging = 1`; mouse-move while dragging: `new_val = min + (mouse_x - x) * (max - min) / track_w`; clamp; call `on_change`. Mouse-down on track outside thumb: compute nearest value; set; call `on_change`. `CTRL_SLIDER_HORIZ = 0`, `CTRL_SLIDER_VERT = 1` in `controls.h`.

- [ ] `CTRL_SLIDER` + `CTRL_SLIDER_HORIZ=0` + `CTRL_SLIDER_VERT=1` in `controls.h`
- [ ] Union field as above
- [ ] `int ctrl_create_slider(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int32_t min, int32_t max, int32_t value, uint8_t orientation, void (*on_change)(int, int32_t))` → ctrl_id
- [ ] `void ctrl_slider_set_value(int wh, int id, int32_t v)` + `int32_t ctrl_slider_get_value(int wh, int id)`
- [ ] Draw case: track (rounded rect, 4 px); filled portion (accent); thumb circle at computed position; thumb hover/focus
- [ ] Mouse-down/move/up handler: dragging logic; track-click jump
- [ ] Commit: `"controls: CTRL_SLIDER -- track+thumb, drag, track-click, horiz/vert, on_change callback"`

## 4. Progress Bar `[Sonnet]`

`CTRL_PROGRESSBAR` type. Rounded-rect track + accent fill. Determinate mode: `ctrl_set_progress(handle, id, 0–100)` updates fill width. Indeterminate mode: 40 px accent highlight block slides L→R via `anim_mgr_add()` looping tween.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_PROGRESSBAR` union field: `struct { uint8_t value; uint8_t mode; gfx_tween_t slide_tween; int32_t slide_pos; } progressbar;`. `PROGRESSBAR_DETERMINATE=0`, `PROGRESSBAR_INDETERMINATE=1`. Determinate draw: fill `value * w / 100` px with accent; subtle gradient: accent at left, `accent_hover` tint at right (2 px `gfx_fill_rect` gradient overlay). Indeterminate: on mode switch: `gfx_tween_start(&pb->slide_tween, 0, ctrl_w, 1200, GFX_EASE_IN_OUT_QUAD)`; set `on_complete` to restart the tween (loop); `anim_mgr_add(&pb->slide_tween)`; draw: fill whole track with `theme_get()->scrollbar_track`; overlay 40 px accent rect at `slide_tween.current`. Indeterminate mode when `anim_mgr` not yet initialized (pre-TODO-02): fall back to determinate fill at 50%.

- [ ] `CTRL_PROGRESSBAR` in `enum ctrl_type`; `PROGRESSBAR_DETERMINATE=0`, `PROGRESSBAR_INDETERMINATE=1` in `controls.h`
- [ ] Union field as above (includes `gfx_tween_t` embedded directly -- no heap allocation)
- [ ] `int ctrl_create_progressbar(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h)` → ctrl_id
- [ ] `void ctrl_set_progress(int wh, int id, uint8_t pct)` -- clamp 0–100; set `value`
- [ ] `void ctrl_progressbar_set_mode(int wh, int id, uint8_t mode)`: if switching to INDETERMINATE: start slide tween + `anim_mgr_add()`; if switching to DETERMINATE: `anim_mgr_cancel()` the tween
- [ ] Draw case: rounded rect track (`theme_get()->scrollbar_track`, h=8, r=4); determinate: accent fill + gradient overlay; indeterminate: 40 px accent rect at `slide_tween.current`
- [ ] Commit: `"controls: CTRL_PROGRESSBAR -- determinate fill+gradient, indeterminate looping tween"`

## 5. Dropdown / ComboBox `[Opus]`

`CTRL_DROPDOWN` type. Text field + ▼ chevron button. Click opens a floating popup list rendered as a high-z-order borderless `wm_create_window()` overlay. Up to 64 items. Keyboard: up/down navigate, Enter select, Escape close. Click outside popup closes it.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the floating popup is a novel overlay mechanism: a transient borderless window at `z_order = 9999` that must (1) extend beyond the parent window's client area, (2) capture all mouse input while open, and (3) close when the user clicks anywhere outside its bounds. Implementation strategy: when opening, call `wm_create_window(NULL, abs_x, abs_y, w, popup_h, WM_FLAG_VISIBLE)` with no decoration flags; set `win->z_order = 9999`; set `win->flags &= ~WM_FLAG_DECORATED`; store `popup_wh` in dropdown union. Each frame: `ctrl_draw_all(popup_wh)` renders the list items as labels. `ctrl_handle_mouse()` for the parent: if popup open and click is outside popup bounds (`wm_hit_test(popup_wh, cx, cy) == 0`): close popup. Close = `wm_destroy_window(popup_wh); popup_wh = -1`. Keyboard in `ctrl_handle_key()`: if any dropdown has popup open, send keys to it first (Tab/Escape/Enter/arrows). Item storage: `char items[64][CTRL_TEXT_MAX]` + `int item_count` + `int selected_idx` in the union.

- [ ] `CTRL_DROPDOWN` in `enum ctrl_type` in `controls.h`
- [ ] Union field: `struct { char items[64][CTRL_TEXT_MAX]; int item_count; int selected_idx; int hovered_idx; int popup_wh; } dropdown;`
- [ ] `int ctrl_create_dropdown(int wh, uint32_t x, uint32_t y, uint32_t w, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_dropdown_add_item(int wh, int id, const char *text)` -- append to items[]; bump item_count; cap at 64
- [ ] `int ctrl_dropdown_get_selection(int wh, int id)` → `selected_idx`
- [ ] `void ctrl_dropdown_set_selection(int wh, int id, int idx)` -- set selected_idx; call on_change
- [ ] `ctrl_dropdown_open(wh, id)`: compute absolute screen coords of control; `wm_create_window(NULL, ax, ay+h, w, min(item_count,8)*24, WM_FLAG_VISIBLE)`; set z_order=9999; no decoration; render item rows with `ctrl_create_label` or direct draw
- [ ] `ctrl_dropdown_close(wh, id)`: `wm_destroy_window(popup_wh)`; `popup_wh = -1`
- [ ] Draw case: text field with selected item; ▼ chevron (Fluent icon or `▼` U+25BC) right-aligned
- [ ] Mouse handler: click chevron or field → `ctrl_dropdown_open`; click outside popup → `ctrl_dropdown_close`; click item → set selection + close
- [ ] Key handler: if popup open: Up/Down move `hovered_idx`; Enter selects; Escape closes
- [ ] Commit: `"controls: CTRL_DROPDOWN -- floating popup overlay, z_order=9999, keyboard nav, 64 items"`

## 6. Tab Strip `[Sonnet]`

`CTRL_TABSTRIP` type. Row of tab header buttons at the top of a pane. Active tab: 2 px accent underline + `theme_get()->surface_variant` background. Inactive: flat, hover highlight. Keyboard: Left/Right arrows navigate. `on_change` callback fires on tab switch.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_TABSTRIP` union field: `struct { struct { char label[64]; uint32_t icon_id; } tabs[16]; int tab_count; int active_idx; void (*on_change)(int ctrl_id, int tab_idx); } tabstrip;`. Tab width: distribute `ctrl_w / tab_count` equally; each tab is a clickable region. Active underline: `gfx_fill_rect(s, tab_x, tab_y + tab_h - 2, tab_w, 2, theme_get()->accent)`. Active background: `gfx_fill_rect(s, tab_x, tab_y, tab_w, tab_h, theme_get()->surface_variant)`. Inactive + hovered: `theme_get()->button_hover`. Icon: if `icon_id != 0`: render Fluent icon codepoint to left of label text. Tab strip height: 36 px standard. On change: call `on_change(id, new_active_idx)`; the app is responsible for showing/hiding the corresponding content panel.

- [ ] `CTRL_TABSTRIP` in `enum ctrl_type` in `controls.h`
- [ ] Union field as above (max 16 tabs per strip)
- [ ] `int ctrl_create_tabstrip(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_tabstrip_add_tab(int wh, int id, const char *label, uint32_t icon_id)` -- append; cap at 16
- [ ] `void ctrl_tabstrip_remove_tab(int wh, int id, int tab_idx)` -- remove and shift; update active_idx if needed
- [ ] `void ctrl_tabstrip_set_active(int wh, int id, int tab_idx)` -- set active_idx; call on_change
- [ ] `int ctrl_tabstrip_get_active(int wh, int id)` → active_idx
- [ ] Draw case: iterate tabs; per tab: background fill (active/hover/normal); label text centered; icon if set; active underline 2 px accent
- [ ] Mouse handler: click on tab → `ctrl_tabstrip_set_active()`
- [ ] Key handler: Left/Right arrows on focused tabstrip → navigate; Enter/Space → same as click
- [ ] Commit: `"controls: CTRL_TABSTRIP -- tab headers, accent underline, icon support, keyboard arrows"`

## 7. Theming `[Sonnet]`

Audit all 6 new controls and confirm zero hardcoded hex colors. All color references use `theme_get()->field`. Remove any `CTRL_COLOR_*` constants if still present after TODO-01 migration. Existing controls (Button, Label, TextBox, ScrollBar) already migrated by TODO-01.

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

> [!NOTE]
> This section is an audit pass, not new code. The 6 new controls were written against `theme_get()` from the start. This section verifies: (1) `rg "0x[0-9A-Fa-f]{6}" src/desktop/controls.c` returns zero results outside comments; (2) `CTRL_COLOR_*` constants are removed from `controls.h` (replaced by TODO-01); (3) every new `_draw` case uses only `theme_get()->` references. If any slip-through literal is found: fix it here.

- [ ] `rg "0x[0-9A-Fa-f]{6}" src/desktop/controls.c` → zero results (excluding comments)
- [ ] `CTRL_COLOR_*` constants absent from `controls.h` (removed by TODO-01 migration)
- [ ] Each new control type's draw code references: `theme_get()->button_bg/hover/pressed/accent/accent_hover/border/foreground/foreground_muted/surface/surface_variant/scrollbar_track/scrollbar_thumb` as appropriate
- [ ] Commit: `"controls: theming audit -- all new controls use theme_get() only, CTRL_COLOR_* removed"`

## 8. Accessibility Stubs `[Sonnet]`

`ctrl_get_accessible_name(wh, id)` returns the control's label text or a descriptive string. `ctrl_get_accessible_role(id)` returns the control type as a plain-text string. Covers all 8 control types (4 existing + 6 new). Foundation for a future screen reader.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> These are stubs -- no screen reader exists yet. The goal is to establish the API so future screen reader code can call `ctrl_get_accessible_name/role()` without needing to know control type internals. Role strings match ARIA role names for forward compatibility: `"button"`, `"label"`, `"textbox"`, `"scrollbar"`, `"checkbox"`, `"radio"`, `"combobox"`, `"slider"`, `"progressbar"`, `"tab"`. Name: for controls with text labels return the label; for scrollbars/sliders/progressbars return `"N%"` or `"N/M"` numeric description.

- [ ] `const char* ctrl_get_accessible_name(int wh, int id)` -- switch on `type`: BUTTON/LABEL/TEXTBOX → `ctrl->button.text`; CHECKBOX → `ctrl->checkbox.text`; RADIO → `ctrl->radio.text`; DROPDOWN → selected item text; SLIDER → `"N"` (value as string); PROGRESSBAR → `"N%"`; TABSTRIP → active tab label; SCROLLBAR → `""`
- [ ] `const char* ctrl_get_accessible_role(int wh, int id)` -- switch on type: return ARIA role string
- [ ] Declarations in `include/desktop/controls.h`
- [ ] Commit: `"controls: accessibility stubs -- ctrl_get_accessible_name/role() for all 8 control types"`

---

## OS Comparison


| ⭐  | Feature             | 🪟 Win11                                                 | 🐧 Linux                                                                | 🚀 Impossible OS                                                                |
| --- | ------------------- | -------------------------------------------------------- | ----------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| 💎  | Checkbox            | ✅ WinUI3 `CheckBox`; BS_CHECKBOX; full state            | ✅ GTK `GtkCheckButton`; Qt `QCheckBox`; full                           | ⬜ §1 -- `gfx_fill_rounded_rect` + Fluent checkmark glyph                       |
| 💎  | Radio button        | ✅ WinUI3 `RadioButton`; BS_RADIOBUTTON; `WM_COMMAND` on | ✅ GTK `GtkRadioButton`; `..._from_widget` group link                   | ⬜ §2 -- `gfx_fill_circle` outer ring + inner                                   |
| 💎  | Dropdown / ComboBox | ✅ WinUI3 `ComboBox`; Win32 `CBS_DROPDOWN`; popup        | ✅ GTK `GtkComboBox`; Qt `QComboBox`; popup                             | ⬜ §5 -- `wm_create_window` at z_order=9999; borderless; click-outside-to-close |
| 💎  | Slider / TrackBar   | ✅ WinUI3 `Slider`; Win32 `TRACKBAR_CLASS`; TBS_VERT     | ✅ GTK `GtkScale`; Qt `QSlider`; both                                   | ⬜ §3 -- accent fill track + circle                                             |
| 💎  | Progress bar        | ✅ WinUI3 `ProgressBar`; Win32 PBS_MARQUEE for           | ✅ GTK `GtkProgressBar`; `gtk_progress_bar_pulse()` for indeterminate   | ⬜ §4 -- indeterminate uses `anim_mgr_add()` looping tween                      |
| 💎  | Tab strip           | ✅ WinUI3 `TabView`; Win32 `WC_TABCONTROL`; keyboard     | ✅ GTK `GtkNotebook`; Qt `QTabWidget`; arrow                            | ⬜ §6 -- 16 tabs max; 2 px                                                      |
| ⭐  | Theming             | ✅ WinUI3 resource brush system; Win32                   | ⚠️ GTK CSS variables per widget;                                        | ⬜ §7 -- `⭐` new controls never had                                            |
| 💎  | Accessibility stubs | ✅ UIA (UI Automation); `IUIAutomationElement`; full     | ✅ ATK/AT-SPI2; `AtkObject::get_name/get_role`; full accessibility tree | ⬜ §8 -- plain-text name/role stubs; ARIA role                                  |

> **After §1–§8:** Impossible OS has the complete set of controls needed for every settings applet, dialog box, and app panel. The `⭐` theming advantage is that all 6 new controls are written against `theme_get()` natively -- they never had hardcoded hex colors, unlike Win32 legacy controls which require a `WM_CTLCOLOR*` redirection chain to theme, and unlike GTK which has a parallel CSS variable system alongside older hardcoded GDK colors.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Checkbox: create window + `ctrl_create_checkbox`; click → `checked` toggles; `on_change` fires; visual states match hover/focus/disabled in QEMU
- [ ] Radio: 3 radio buttons same `group_id`; click one → others deselect; `ctrl_get_radio_group_selection` returns correct ctrl_id
- [ ] Slider: drag thumb → `on_change` fires with new value; click track left/right of thumb → value jumps; vertical orientation works
- [ ] Progress bar determinate: `ctrl_set_progress(wh, id, 75)` → 75% fill visible in QEMU; gradient overlay visible
- [ ] Progress bar indeterminate: `ctrl_progressbar_set_mode(… INDETERMINATE)` → 40 px block slides L→R continuously; switching back to DETERMINATE stops animation
- [ ] Dropdown: click field → popup appears above parent window; 8 items visible; Up/Down highlight; Enter selects; popup closes; selected text shows in field
- [ ] Dropdown: click anywhere outside popup → popup closes
- [ ] Tab strip: click tab 2 → `on_change(id, 1)` fires; accent underline on tab 2; Left/Right arrows on focused tabstrip navigate
- [ ] `rg "0x[0-9A-Fa-f]{6}" src/desktop/controls.c` → zero results (confirm after §7 audit)
- [ ] `ctrl_get_accessible_role(wh, checkbox_id)` returns `"checkbox"`; `ctrl_get_accessible_name(wh, slider_id)` returns the slider's current value as string
- [ ] Commit: `"controls: complete widget library core -- checkbox/radio/dropdown/slider/progressbar/tabstrip"`
