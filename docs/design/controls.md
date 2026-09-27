<!-- docs: covers=todo/06-desktop-foundation/TODO-04-control-library.md,todo/08-graphics-ui/TODO-05-widget-library-core.md,todo/08-graphics-ui/TODO-06-widget-dialogs.md order=3 -->
# Controls Specification

Every control in Impossible OS looks and behaves like its Windows 11 (WinUI) counterpart. This page fixes the geometry, colours and states for each one so that the control library, the Win32 common controls and every app draw the same thing. Values are tokens in [`tokens.json`](tokens.json) and `THEME_*` constants in `include/desktop/theme_tokens.h`; sizes are at 100% scale and pass through `DPI_SCALE()`.

The [controls gallery](https://impossibleos.co/design/controls.html) renders every control below in dark and light, generated from the same tokens; the [desktop mockup](https://impossibleos.co/design/) shows them in context.

## Which rules apply to every control?

- **Height and shape.** Standard controls are `size.control_height` (32) tall with `radius.control` (4) corners and a horizontal padding of `size.control_padding_x` (12). Glyphs are `size.control_glyph` (16).
- **Fills by state.** Rest `control_fill`, hover `control_fill_hover`, pressed `control_fill_pressed`, disabled `control_fill_disabled`; the outline is `stroke_control`. Pressed text drops to `text_secondary`; disabled text is `text_disabled`.
- **Accent controls** (the default button of a dialog, a checked box, an on toggle) use `accent`, `accent_hover` and `accent_pressed` with `text_on_accent`.
- **Focus** is the two-ring keyboard focus visual from the [shell accessibility rules](shell.md#accessibility), 3 px outside the control, never shown for mouse focus.
- **Text** is `type.body` (14/20) unless stated.
- **Motion.** State changes cross-fade over `motion.fast` (83 ms); larger transitions (expanding panels, page changes) use `motion.normal` (167 ms) or `motion.slow` (250 ms) with `motion.ease_standard`. Reduced animation removes them.
- **Typography.** The UI face is Selawik (`type.family_ui`, aliased as Segoe UI); code, terminals and monospaced data use Cascadia Code (`type.family_mono`). Use the ramp by role: `caption` 12/16 (labels, timestamps), `body` 14/20 (default), `body_strong` 14/20 semibold (headings inside panels), `body_large` 18/24, `subtitle` 20/28 (dialog titles), `title` 28/36 (page titles), `title_large` 40/52, `display` 68/92 (lock-screen style hero text).
- **Spacing.** Everything sits on the 4 px grid (`spacing.xxs` 2 through `spacing.xxl` 32); gaps between related controls are `spacing.s` (8), between groups `spacing.xl` (24).
- **Elevation.** Controls use `elevation.control`, cards `elevation.card`, tooltips `elevation.tooltip`; flyouts, Start and windows use the levels given in the [shell specification](shell.md).

## Status colours

Status is never colour alone: pair it with a glyph (success check, caution triangle, critical cross, info circle) and text. Use `status_success`, `status_caution`, `status_critical` and `status_info` for glyphs and short labels, and the `*_bg` tokens for tinted backgrounds such as info bars. Destructive actions are never red buttons: they are standard buttons that open a confirmation dialog whose default button is the safe choice.

## Button

| Variant | Rest | Hover | Pressed |
| --- | --- | --- | --- |
| Standard | `control_fill`, `stroke_control` | `control_fill_hover` | `control_fill_pressed`, `text_secondary` |
| Accent | `accent` | `accent_hover` | `accent_pressed` |
| Subtle (toolbar, caption) | transparent | `subtle_fill_hover` | `subtle_fill_pressed` |

Minimum width `size.control_min_width` (96) for text buttons; icon-only buttons are square. A split button separates its chevron with a 1 px `stroke_divider`. **Toggle buttons** (for example Bold in a toolbar) look like their variant when off and like an accent button when on (`accent` fill, `text_on_accent` glyph). Toolbars and command bars are `size.command_bar_height` (48) tall; there is no other toolbar height.

## Toggle switch

Track `size.toggle_width` x `size.toggle_height` (40 x 20), fully rounded (`radius.toggle`, 10). Off: 1 px `control_strong_stroke` outline, knob `size.toggle_knob` (12) in `toggle_knob_off`. On: `accent` fill, knob in `text_on_accent`. Hover grows the knob to `size.toggle_knob_hover` (14); pressed stretches it to 17 x 14. The knob slides over `motion.normal` with the decelerate curve. The On/Off label sits 12 px to the right.

## Check box and radio button

- **Check box:** `size.check_box` (20) square, `radius.check_box` (4). Unchecked: `control_fill` with a 1 px `control_strong_stroke`. Checked and indeterminate: `accent` fill with a check or dash glyph in `text_on_accent`. Label 8 px to the right.
- **Radio button:** `size.radio` (20) circle. Unchecked: 1 px `control_strong_stroke`. Checked: `accent` fill with a `size.radio_dot` (12) dot in `text_on_accent`; the dot grows to 14 on hover and shrinks to 10 on press.

## Slider

Track `size.slider_track` (4) tall, fully rounded, `control_strong_stroke`; the filled part is `accent`. The thumb is a `size.slider_thumb` (20) circle in `window_bg` with a 1 px `stroke_control` ring and a `size.slider_thumb_inner` (12) accent centre that grows to 14 on hover and shrinks to 10 on press. A tooltip shows the value while dragging.

## Text box, password box and search box

32 tall, `radius.control`, `control_fill` with `stroke_control`, and a bottom underline of `size.text_box_underline` (1) in `control_underline`. Focused: fill `control_fill_input_active` and a `size.text_box_underline_focused` (2) accent underline. Placeholder text is `text_secondary`. A clear button (16 px glyph) appears at the right when the box has text and focus. The password box adds a reveal button; the search box a search glyph. Pill search boxes in the shell use `radius.search_box` (18) instead.

## Date picker

A 32 px standard button showing the date in the user's short format with a 16 px calendar glyph at the right. Clicking opens a flyout calendar identical to the [taskbar calendar](shell.md#notifications-and-calendar) (month title with previous/next, 7-column grid of 40 px cells, today as an accent ring, the selected date as an accent circle with `text_on_accent`). A date range is two pickers labelled "From" and "To", 8 px apart; the "To" calendar disables days before "From".

## Combo box and drop-down

32 tall, standard button fills, a 12 px chevron 11 px from the right edge. The list opens as a menu-material flyout (`material.*.menu`, radius 8, `elevation.flyout`) aligned so the selected item sits over the box. Items are `size.list_item_height` (32); the selected item shows a `size.selection_pill_width` x `size.selection_pill_height` (3 x 16) accent pill at its left edge.

## List, tree and grid views

Rows are `size.list_item_height` (32) with `radius.control` hover and selection fills (`subtle_fill_hover`, `subtle_fill_pressed`), 4 px inset from the view edges. Selection adds the 3 x 16 accent pill at the left edge. Tree views indent 16 px per level with a 12 px chevron that rotates 90 degrees over `motion.fast`. Grid view tiles (Explorer icon view) use the same fills with 48 px icons and a two-line caption.

## Tabs

Tab strips are `size.tab_height` (32) inside a 40 px bar. The selected tab uses `layer_bg` with `radius.overlay` (8) top corners and connects to the content below; unselected tabs are transparent with a 1 px `stroke_divider` between them, and show `subtle_fill_hover` on hover. A close glyph appears on the selected and hovered tab. A new-tab button follows the last tab.

## Progress

- **Progress bar:** a `size.progress_track` (1) track in `control_strong_stroke` and a `size.progress_bar` (3) indicator in `accent`, rounded. Indeterminate: a 30% segment sweeps left to right over 2 s. Paused: `text_tertiary`; error: `caption_close_hover`.
- **Progress ring:** 32 px, 3 px stroke arc in `accent` rotating once per 1.3 s.

## Scroll bar

Collapsed: a `size.scrollbar_collapsed` (2) line in `control_strong_stroke`, drawn only while scrolling or hovering the view. On pointer approach it expands to `size.scrollbar_expanded` (6) with rounded ends and arrow glyphs at each end, over `motion.fast`.

## Tooltip

Appears after `motion.tooltip_delay` (400 ms) of hover, below the pointer. `layer_bg` over `window_bg`, 1 px `stroke_surface`, `radius.tooltip` (4), `elevation.tooltip`, text `type.caption` (12/16), max width `size.tooltip_max_width` (320), padding 8 x 5.

## Dialog

A centred card between `size.dialog_min_width` (320) and `size.dialog_max_width` (548) wide, `radius.dialog` (8), `window_bg`, `elevation.window_active`, over a `smoke` scrim that covers the owner window (or the screen for system dialogs). Content padding `size.dialog_padding` (24): title in `type.subtitle` (20/28), body in `type.body`. The footer is `size.dialog_footer_height` (80) with a `layer_bg` band and right-aligned buttons: the default action is an accent button, the rest are standard; equal widths, 8 px apart. Escape invokes Cancel; Enter the default button. Message boxes show a `size.message_icon` (32) status glyph left of the text (error, warning, information, question) in the matching status colour per [status colours](#status-colours); they are glyphs, not colour icons.

## Menu bar and menus

Classic menu bars (Win32 apps) are 32 tall with subtle buttons. Every drop-down, context and overflow menu uses the [context menu spec](shell.md#context-menus): 256 wide, 4 px padding, 32 px items, 16 px glyphs, shortcut hints in `text_secondary`, menu acrylic.

## Info bar

A full-width strip at the top of a page or dialog: `radius.control`, the matching `*_bg` status token, a 16 px status glyph, a title in `type.body_strong`, a message, and an optional action button and close glyph on the right. Height grows with the message; minimum 48.

## Cards and settings rows

Cards use `card_bg`, a 1 px `stroke_card` and `radius.card` (8), padding 16. A settings row is at least `size.settings_card_min_height` (64): 20 px glyph, a title in `type.body` with an optional `type.caption` description in `text_secondary`, and the control right-aligned.
