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
- `include/desktop/theme.h` (TODO-01) -- `theme_get()->colors.<token>` (design colour tokens) + `THEME_SIZE_*` / `THEME_RADIUS_*` -- all colours and geometry for new controls, per `docs/design/controls.md`
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
| 💎  |   1   | §1 Checkbox -- `CTRL_CHECKBOX`, 20 px box radius 4, accent checked state, focus rings         | Existing `gfx_fill_rounded_rect`; `theme_get()` (TODO-01)                 |  [ ]   |
| 💎  |   2   | §2 Radio button -- `CTRL_RADIO`, 20 px circle + 12 px dot, group mutual exclusion             | §1 (same struct extension pattern; group_id field added alongside)        |  [ ]   |
| 💎  |   3   | §4 Slider -- `CTRL_SLIDER`, track + thumb drag, horiz/vert, real-time callback               | §1 (same `on_change` callback type established in §1)                     |  [ ]   |
| 💎  |   4   | §5 Progress bar -- `CTRL_PROGRESSBAR`, determinate + indeterminate tween mode                | §3 slider (determinate fill is same pattern); TODO-02 `anim_mgr_add()`    |  [ ]   |
| 💎  |   5   | §3 Dropdown -- `CTRL_DROPDOWN`, floating popup via WM z-order overlay, keyboard navigation   | All of §1–4 done (dropdown is most complex; isolated until others stable) |  [ ]   |
| 💎  |   6   | §6 Tab strip -- `CTRL_TABSTRIP`, tab headers, accent underline, keyboard arrow navigation    | §5 dropdown (all input-capture patterns established)                      |  [ ]   |
| 💎  |   7   | §7 Theming -- confirm all new controls use `theme_get()` only; remove any CTRL_COLOR_* usage | §6 (all controls must exist before audit)                                 |  [ ]   |
| ⭐  |   8   | §8 Accessibility stubs -- `ctrl_get_accessible_name/role()` for all 8 types                  | §7 (all control types must be registered before role table is complete)   |  [ ]   |

---

## 1. Checkbox `[Sonnet]`

**Design:** [`controls.md#check-box-and-radio-button`](../../docs/design/controls.md#check-box-and-radio-button)

**Owner of:** the work planned in `06-desktop-foundation/TODO-04 §1`, which is superseded there so the shell has one implementation.

`CTRL_CHECKBOX` type per `docs/design/controls.md#check-box-and-radio-button`: a `THEME_SIZE_CHECK_BOX` (20) square with `THEME_RADIUS_CHECK_BOX` (4) corners. Unchecked: `control_fill` with a 1 px `control_strong_stroke`. Checked and indeterminate: `accent` fill with a check or dash glyph in `text_on_accent`. Hover and pressed use `control_fill_hover` / `control_fill_pressed` (checked: `accent_hover` / `accent_pressed`). Disabled: `control_fill_disabled` and `text_disabled`. Label 8 px to the right in the body style. Focus: the two-ring keyboard focus visual (2 px `focus_outer` ring outside a 1 px `focus_inner` ring, 3 px outside the control, keyboard focus only). State changes cross-fade over `THEME_MOTION_FAST_MS`.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> Add `CTRL_CHECKBOX` to `enum ctrl_type`. Checkbox union field in `struct control`: `struct { char text[CTRL_TEXT_MAX]; uint8_t checked; void (*on_change)(int ctrl_id, int checked); }`. Checkmark glyph: use Fluent icon codepoint `✓` (U+2713) rendered via `boot_font_render` or `gfx_draw_fluent_icon`; fallback: draw two line segments manually if font not available at draw time. Hover and pressed: swap the box fill as above (no accent tint overlay). Focus: `ctrl_draw_focus_ring(s, bx, by, 20, 20, radius)` draws the two rings. Disabled: `control_fill_disabled` box, `text_disabled` label and glyph; no hover/focus response.

- [ ] `CTRL_CHECKBOX` in `enum ctrl_type` in `controls.h`
- [ ] Union field: `struct { char text[CTRL_TEXT_MAX]; uint8_t checked; void (*on_change)(int, int); } checkbox;`
- [ ] `int ctrl_create_checkbox(int wh, uint32_t x, uint32_t y, const char *text, int checked, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_set_checked(int wh, int id, int checked)` + `int ctrl_get_checked(int wh, int id)`
- [ ] Draw case in `ctrl_draw_all()`: 20 x 20 box, radius 4 (`THEME_SIZE_CHECK_BOX`, `THEME_RADIUS_CHECK_BOX`); unchecked `control_fill` + `control_strong_stroke`, checked `accent` + glyph in `text_on_accent`; hover/pressed fills; label 8 px right
- [ ] `void ctrl_draw_focus_ring(gfx_surface_t *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r)`: shared two-ring keyboard focus visual used by every control
- [ ] Mouse handler: click on box (or label) toggles `checked`; calls `on_change`
- [ ] Commit: `"controls: CTRL_CHECKBOX -- box+checkmark, checked/hover/focus/disabled states"`

## 2. Radio Button `[Sonnet]`

**Design:** [`controls.md#check-box-and-radio-button`](../../docs/design/controls.md#check-box-and-radio-button)

**Owner of:** the work planned in `06-desktop-foundation/TODO-04 §1`, which is superseded there so the shell has one implementation.

`CTRL_RADIO` type per `docs/design/controls.md#check-box-and-radio-button`: a `THEME_SIZE_RADIO` (20) circle. Unselected: 1 px `control_strong_stroke` ring on `control_fill`. Selected: `accent` fill with a `THEME_SIZE_RADIO_DOT` (12) dot in `text_on_accent`; the dot grows to 14 on hover and shrinks to 10 on press. `group_id` field -- selecting one radio auto-deselects all others in the same window with the same `group_id`. Label 8 px to the right. Focus uses `ctrl_draw_focus_ring()`.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_RADIO` union field: `struct { char text[CTRL_TEXT_MAX]; int group_id; uint8_t selected; void (*on_change)(int ctrl_id, int selected); } radio;`. Mutual exclusion: in the mouse click handler for a radio button click, iterate all controls in the same window with `type == CTRL_RADIO && ctrl->radio.group_id == clicked->radio.group_id`; set each `selected = 0`; then set clicked `selected = 1`; call each one's `on_change` that changed state. Draw: unselected ring `control_strong_stroke` over `control_fill`; selected `accent` disc with the `text_on_accent` dot (12, 14 hover, 10 pressed); disabled `control_fill_disabled` + `text_disabled`.

- [ ] `CTRL_RADIO` in `enum ctrl_type` in `controls.h`
- [ ] Union field: `struct { char text[CTRL_TEXT_MAX]; int group_id; uint8_t selected; void (*on_change)(int, int); } radio;`
- [ ] `int ctrl_create_radio(int wh, uint32_t x, uint32_t y, const char *text, int group_id, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_set_radio_selected(int wh, int id, int selected)` + `int ctrl_get_radio_selected(int wh, int id)`
- [ ] `int ctrl_get_radio_group_selection(int wh, int group_id)` → ctrl_id of currently selected radio in group, or -1
- [ ] Draw case: 20 px circle; unselected ring, selected accent disc with 12 px dot (14 hover, 10 pressed); focus via `ctrl_draw_focus_ring()`
- [ ] Mouse handler: on click → deselect all in same group → select this one → `on_change` callbacks
- [ ] Commit: `"controls: CTRL_RADIO -- circle+dot, group_id mutual exclusion, hover/focus/disabled"`

## 3. Slider / TrackBar `[Sonnet]`

**Design:** [`controls.md#slider`](../../docs/design/controls.md#slider)

`CTRL_SLIDER` type per `docs/design/controls.md#slider`: a `THEME_SIZE_SLIDER_TRACK` (4) track, fully rounded, in `control_strong_stroke`, with the filled part in `accent`. Thumb: a `THEME_SIZE_SLIDER_THUMB` (20) circle in `window_bg` with a 1 px `stroke_control` ring and a `THEME_SIZE_SLIDER_THUMB_INNER` (12) accent centre that grows to 14 on hover and shrinks to 10 on press. A tooltip shows the value while dragging. Horizontal and vertical orientations. Drag thumb → real-time `on_change`. Click track → jump value.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_SLIDER` union field: `struct { int32_t min_val, max_val, value; uint8_t orientation; void (*on_change)(int ctrl_id, int32_t value); uint8_t dragging; } slider;`. Track fill: width = `(value - min) * track_w / (max - min)` px from left; fill `accent`; unfilled track `control_strong_stroke`. Thumb at `thumb_cx = x + fill_width`: 20 px `window_bg` disc + `stroke_control` ring + 12 px `accent` centre (14 hover, 10 pressed). Mouse-down on thumb area (within 8 px): set `dragging = 1`; mouse-move while dragging: `new_val = min + (mouse_x - x) * (max - min) / track_w`; clamp; call `on_change`. Mouse-down on track outside thumb: compute nearest value; set; call `on_change`. `CTRL_SLIDER_HORIZ = 0`, `CTRL_SLIDER_VERT = 1` in `controls.h`.

- [ ] `CTRL_SLIDER` + `CTRL_SLIDER_HORIZ=0` + `CTRL_SLIDER_VERT=1` in `controls.h`
- [ ] Union field as above
- [ ] `int ctrl_create_slider(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int32_t min, int32_t max, int32_t value, uint8_t orientation, void (*on_change)(int, int32_t))` → ctrl_id
- [ ] `void ctrl_slider_set_value(int wh, int id, int32_t v)` + `int32_t ctrl_slider_get_value(int wh, int id)`
- [ ] Draw case: 4 px track (`control_strong_stroke`), accent fill, 20 px thumb with 12 px accent centre (hover 14, pressed 10), value tooltip while dragging, focus ring
- [ ] Mouse-down/move/up handler: dragging logic; track-click jump
- [ ] Commit: `"controls: CTRL_SLIDER -- track+thumb, drag, track-click, horiz/vert, on_change callback"`

## 4. Progress Bar `[Sonnet]`

**Design:** [`controls.md#progress`](../../docs/design/controls.md#progress)

**Owner of:** the work planned in `06-desktop-foundation/TODO-04 §2`, which is superseded there so the shell has one implementation.

`CTRL_PROGRESSBAR` type per `docs/design/controls.md#progress`: a `THEME_SIZE_PROGRESS_TRACK` (1) track in `control_strong_stroke` and a `THEME_SIZE_PROGRESS_BAR` (3) indicator in `accent`, rounded. Determinate: `ctrl_set_progress(handle, id, 0–100)` sets the indicator width. Indeterminate: a segment 30% of the track width sweeps left to right every 2 s. Paused state: `text_tertiary`; error state: `caption_close_hover`.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_PROGRESSBAR` union field: `struct { uint8_t value; uint8_t mode; gfx_tween_t slide_tween; int32_t slide_pos; } progressbar;`. `PROGRESSBAR_DETERMINATE=0`, `PROGRESSBAR_INDETERMINATE=1`. Determinate draw: 1 px track centred vertically, 3 px accent bar of `value * w / 100` px, rounded ends. Indeterminate: on mode switch `gfx_tween_start(&pb->slide_tween, -w*3/10, w, 2000, GFX_EASE_STANDARD)` looping via `on_complete`; draw the 3 px accent segment of width `w*3/10` at `slide_tween.current`, clipped to the track. Indeterminate mode when `anim_mgr` not yet initialized (pre-TODO-02): fall back to determinate fill at 50%.

- [ ] `CTRL_PROGRESSBAR` in `enum ctrl_type`; `PROGRESSBAR_DETERMINATE=0`, `PROGRESSBAR_INDETERMINATE=1` in `controls.h`
- [ ] Union field as above (includes `gfx_tween_t` embedded directly -- no heap allocation)
- [ ] `int ctrl_create_progressbar(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h)` → ctrl_id
- [ ] `void ctrl_set_progress(int wh, int id, uint8_t pct)` -- clamp 0–100; set `value`
- [ ] `void ctrl_progressbar_set_mode(int wh, int id, uint8_t mode)`: if switching to INDETERMINATE: start slide tween + `anim_mgr_add()`; if switching to DETERMINATE: `anim_mgr_cancel()` the tween
- [ ] Draw case: 1 px `control_strong_stroke` track + 3 px `accent` bar (determinate) or sweeping 30% segment (indeterminate); paused `text_tertiary`, error `caption_close_hover`; no gradient
- [ ] Commit: `"controls: CTRL_PROGRESSBAR -- 1 px track, 3 px accent bar, sweeping indeterminate segment, paused/error states"`

## 5. Dropdown / ComboBox `[Opus]`

**Design:** [`controls.md#combo-box-and-drop-down`](../../docs/design/controls.md#combo-box-and-drop-down)

**Owner of:** the work planned in `06-desktop-foundation/TODO-04 §4`, which is superseded there so the shell has one implementation.

`CTRL_DROPDOWN` type per `docs/design/controls.md#combo-box-and-drop-down`: a 32 px (`THEME_SIZE_CONTROL_HEIGHT`) field with standard button fills and a 12 px chevron 11 px from the right edge. Click opens the list as a menu-material flyout (`mat.menu`, radius 8, `THEME_ELEV_FLYOUT_*`) aligned so the selected item sits over the field; items are `THEME_SIZE_LIST_ITEM_HEIGHT` (32) with a 3 x 16 accent pill on the selected one; the flyout is a high-z-order borderless `wm_create_window()` overlay. Up to 64 items. Keyboard: up/down navigate, Enter select, Escape close. Click outside popup closes it.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the floating popup is a novel overlay mechanism: a transient borderless window at `z_order = 9999` that must (1) extend beyond the parent window's client area, (2) capture all mouse input while open, and (3) close when the user clicks anywhere outside its bounds. Implementation strategy: when opening, call `wm_create_window(NULL, abs_x, abs_y, w, popup_h, WM_FLAG_VISIBLE)` with no decoration flags; set `win->z_order = 9999`; set `win->flags &= ~WM_FLAG_DECORATED`; store `popup_wh` in dropdown union. Each frame: `ctrl_draw_all(popup_wh)` renders the list items as labels. `ctrl_handle_mouse()` for the parent: if popup open and click is outside popup bounds (`wm_hit_test(popup_wh, cx, cy) == 0`): close popup. Close = `wm_destroy_window(popup_wh); popup_wh = -1`. Keyboard in `ctrl_handle_key()`: if any dropdown has popup open, send keys to it first (Tab/Escape/Enter/arrows). Item storage: `char items[64][CTRL_TEXT_MAX]` + `int item_count` + `int selected_idx` in the union.

- [ ] `CTRL_DROPDOWN` in `enum ctrl_type` in `controls.h`
- [ ] Union field: `struct { char items[64][CTRL_TEXT_MAX]; int item_count; int selected_idx; int hovered_idx; int popup_wh; } dropdown;`
- [ ] `int ctrl_create_dropdown(int wh, uint32_t x, uint32_t y, uint32_t w, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_dropdown_add_item(int wh, int id, const char *text)` -- append to items[]; bump item_count; cap at 64
- [ ] `int ctrl_dropdown_get_selection(int wh, int id)` → `selected_idx`
- [ ] `void ctrl_dropdown_set_selection(int wh, int id, int idx)` -- set selected_idx; call on_change
- [ ] `ctrl_dropdown_open(wh, id)`: compute absolute screen coords of control
  - `wm_create_window(NULL, ax, ay - selected_idx*32 - 4, w, min(item_count,10)*32 + 8, WM_FLAG_VISIBLE)` (selected item over the field, 4 px padding)
  - set z_order=9999
  - no decoration
  - menu acrylic
  - 32 px rows with the selection pill
- [ ] `ctrl_dropdown_close(wh, id)`: `wm_destroy_window(popup_wh)`; `popup_wh = -1`
- [ ] Draw case: 32 px field with standard fills (`control_fill`, hover, pressed), selected item text, 12 px chevron 11 px from the right edge
- [ ] Mouse handler: click chevron or field → `ctrl_dropdown_open`; click outside popup → `ctrl_dropdown_close`; click item → set selection + close
- [ ] Key handler: if popup open: Up/Down move `hovered_idx`; Enter selects; Escape closes
- [ ] Commit: `"controls: CTRL_DROPDOWN -- floating popup overlay, z_order=9999, keyboard nav, 64 items"`

## 6. Tab Strip `[Sonnet]`

**Design:** [`controls.md#tabs`](../../docs/design/controls.md#tabs)

`CTRL_TABSTRIP` type per `docs/design/controls.md#tabs`: `THEME_SIZE_TAB_HEIGHT` (32) tabs inside a 40 px bar. The selected tab uses `layer_bg` with `THEME_RADIUS_OVERLAY` (8) top corners and connects to the content below; unselected tabs are transparent with a 1 px `stroke_divider` between them and `subtle_fill_hover` on hover. A close glyph shows on the selected and hovered tab; an optional new-tab button follows the last tab. Keyboard: Left/Right arrows navigate. `on_change` callback fires on tab switch.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_TABSTRIP` union field: `struct { struct { char label[64]; uint32_t icon_id; } tabs[16]; int tab_count; int active_idx; void (*on_change)(int ctrl_id, int tab_idx); } tabstrip;`. Tab width: distribute `ctrl_w / tab_count` equally; each tab is a clickable region. Selected: `gfx_fill_rounded_rect_top(s, tab_x, tab_y, tab_w, 32, 8, layer_bg)` joined to the content area (no underline). Unselected + hovered: `subtle_fill_hover`. Dividers: 1 px `stroke_divider` between unselected tabs. Icon: if `icon_id != 0`: 16 px icon left of the label. Bar height: 40 px, tabs 32 px. On change: call `on_change(id, new_active_idx)`; the app is responsible for showing/hiding the corresponding content panel.

- [ ] `CTRL_TABSTRIP` in `enum ctrl_type` in `controls.h`
- [ ] Union field as above (max 16 tabs per strip)
- [ ] `int ctrl_create_tabstrip(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void (*on_change)(int, int))` → ctrl_id
- [ ] `void ctrl_tabstrip_add_tab(int wh, int id, const char *label, uint32_t icon_id)` -- append; cap at 16
- [ ] `void ctrl_tabstrip_remove_tab(int wh, int id, int tab_idx)` -- remove and shift; update active_idx if needed
- [ ] `void ctrl_tabstrip_set_active(int wh, int id, int tab_idx)` -- set active_idx; call on_change
- [ ] `int ctrl_tabstrip_get_active(int wh, int id)` → active_idx
- [ ] Draw case: 40 px bar; selected tab `layer_bg` with 8 px top radius joined to content; unselected transparent with dividers, hover `subtle_fill_hover`; icon + label; close glyph on selected/hovered
- [ ] Mouse handler: click on tab → `ctrl_tabstrip_set_active()`
- [ ] Key handler: Left/Right arrows on focused tabstrip → navigate; Enter/Space → same as click
- [ ] Commit: `"controls: CTRL_TABSTRIP -- Windows 11 tabs, selected layer tab, dividers, close glyph, keyboard arrows"`

## 7. Theming `[Sonnet]`

**Design:** [`index.md#how-does-this-relate-to-the-theme-system`](../../docs/design/index.md#how-does-this-relate-to-the-theme-system), [`shell.md#materials`](../../docs/design/shell.md#materials)

Audit all 6 new controls and confirm zero hardcoded hex colors. All color references use `theme_get()->field`. Remove any `CTRL_COLOR_*` constants if still present after TODO-01 migration. Existing controls (Button, Label, TextBox, ScrollBar) already migrated by TODO-01.

**Files:** `src/desktop/controls.c`, `include/desktop/controls.h`

> [!NOTE]
> This section is an audit pass, not new code. The 6 new controls were written against `theme_get()` from the start. This section verifies: (1) `rg "0x[0-9A-Fa-f]{6}" src/desktop/controls.c` returns zero results outside comments; (2) `CTRL_COLOR_*` constants are removed from `controls.h` (replaced by TODO-01); (3) every new `_draw` case uses only `theme_get()->` references. If any slip-through literal is found: fix it here.

- [ ] `rg "0x[0-9A-Fa-f]{6}" src/desktop/controls.c` → zero results (excluding comments)
- [ ] `CTRL_COLOR_*` constants absent from `controls.h` (removed by TODO-01 migration)
- [ ] Each control's draw code uses only design tokens
  - `theme_get()->colors.control_fill*`, `subtle_fill_*`, `accent*`, `text_*`, `stroke_*`, `control_strong_stroke`, `focus_*`, and `THEME_SIZE_*` / `THEME_RADIUS_*` for geometry (`docs/design/controls.md`)
- [ ] Commit: `"controls: theming audit -- all new controls use theme_get() only, CTRL_COLOR_* removed"`

## 8. Accessibility Stubs `[Sonnet]`

**Design:** [`shell.md#accessibility`](../../docs/design/shell.md#accessibility)

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
