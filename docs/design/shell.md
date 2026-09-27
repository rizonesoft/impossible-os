<!-- docs: covers=todo/08-graphics-ui/TODO-10-taskbar.md,todo/08-graphics-ui/TODO-11-startmenu-tray-notifications.md,todo/08-graphics-ui/TODO-08-window-manager.md,todo/06-desktop-foundation/TODO-05-desktop-shell.md,todo/06-desktop-foundation/TODO-06-desktop-icons.md,todo/09-desktop-shell/TODO-09-file-manager.md order=2 -->
# Shell Specification

This page specifies the desktop shell pixel by pixel: the desktop, taskbar, Start menu, quick settings, notifications and calendar, context menus, window chrome and File Explorer. Every number is a token in [`tokens.json`](tokens.json) and a `THEME_*` constant in the generated `include/desktop/theme_tokens.h`; the token name is given in `code`. All sizes are at 100% scale and multiply by the display scale factor once DPI scaling lands.

The [interactive mockup](https://impossibleos.co/design/) is the visual reference for everything below.

## Materials

Shell surfaces are acrylic: the wallpaper and windows behind are blurred, tinted and given a faint grain. Windows use mica: the wallpaper alone, heavily blurred and desaturated, tinted toward the window background. Both are implemented by `gfx_acrylic()` and `gfx_mica()` in `src/kernel/gfx/`.

| Surface | Material | Dark tint | Light tint | Blur radius | Grain |
| --- | --- | --- | --- | --- | --- |
| Taskbar | acrylic `material.*.taskbar` | `#1C1C1C` at 184/255 | `#EEEEEE` at 176/255 | 24 | 6% |
| Start menu | acrylic `material.*.start` | `#202020` at 196/255 | `#F3F3F3` at 190/255 | 30 | 6% |
| Flyouts (quick settings, calendar) | acrylic `material.*.flyout` | `#2C2C2C` at 204/255 | `#FCFCFC` at 196/255 | 30 | 6% |
| Context menus | acrylic `material.*.menu` | `#2C2C2C` at 214/255 | `#FCFCFC` at 214/255 | 20 | 4% |
| Window background | mica `material.*.mica` | `#202020` at 204/255 | `#F3F3F3` at 204/255 | wallpaper-only | none |

The blur radius is the box-blur radius passed to `gfx_acrylic()`. Three box passes approximate a Gaussian about 1.7 times as wide, which is how the web mockup converts it. Frosted surfaces are rendered into a per-surface cache and recomposited only when the region behind them changes, so a static desktop costs no blur work per frame.

**Transparency off.** When `EnableTransparency` is `0` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`, every material draws its tint at full opacity with no blur and no grain.

## Desktop

- The wallpaper fills the screen (`Fill` mode by default). The default is the "impossible bloom": `resources/backgrounds/bloom-dark.jpg` in dark mode and `bloom-light.jpg` in light mode.
- Desktop icons sit in a column-major grid from the top-left, `size.desktop_margin` (8) from the edges. Each cell is `size.desktop_cell_width` x `size.desktop_cell_height` (76 x 86) with a `size.desktop_icon` (48) icon, 6 px from the top of the cell, and a two-line label in `type.caption` (12/16).
- Labels are white (`desktop_label`) with a soft dark shadow (`desktop_label_shadow`) in both themes, as on Windows 11.
- Hover draws a 12% white fill with a 16% white 1 px stroke. Selection uses `selection_fill` and `selection_stroke`. Radius `radius.control` (4).
- Default icons, in order: This PC, Recycle Bin, the user folder, Network and Control Panel. Recycle Bin switches between `recycle_bin_empty` and `recycle_bin_full`.

## Taskbar

- Height `size.taskbar_height` (48), full width, docked to the bottom, with a 1 px top border in `stroke_divider`.
- **Centre group**, centred on the screen: Start, the search box, Task view, then pinned and running apps. Buttons are `size.taskbar_button` (40) square with `size.taskbar_gap` (4) between them and `radius.taskbar_button` (4) corners; icons are `size.taskbar_icon` (24).
- **Start button** shows the `start` icon, the logo mark. Pressing it scales the icon to 86% for the press duration.
- **Search box** is 196 x 36 with a `radius.search_box` (18) pill, `control_fill` background and `stroke_control` outline, a search glyph and the word "Search" in `text_secondary`. Below 1024 px screen width it collapses to a 40 px search button.
- **Running indicator:** a pill `size.taskbar_indicator_height` (3) tall, 2 px above the bottom edge. Running but not focused: 6 px wide in `taskbar_indicator_idle`. Focused: 16 px wide in `taskbar_indicator` (the accent), and the button gets the hover fill. The width animates over `motion.normal` (167 ms) with the decelerate curve.
- **Right side:** an overflow chevron (28 x 40), then the system tray cluster (network, volume and battery glyphs in one 40 px tall button that opens quick settings), then the clock (time over date in `type.caption`, right-aligned) with the notification bell, which opens notifications and the calendar.

## Start menu

- `size.start_width` x `size.start_height` (640 x 720, clamped to the space above the taskbar), centred horizontally, `size.start_offset_bottom` (12) above the taskbar. Radius `radius.overlay` (8), shadow `elevation.start`, 1 px `stroke_surface` outline.
- **Open:** fades in over `motion.normal` while sliding up `size.start_slide` (48) px over `motion.start_open` (250 ms) with the decelerate curve. **Close:** reverses over `motion.start_close` (167 ms) with the accelerate curve. The Windows key toggles it; Escape or a click outside closes it.
- **Search box** at the top: full width minus 32 px padding each side, `size.start_search_height` (36), pill radius, a 1 px accent line along the bottom edge while focused.
- **Pinned:** a "Pinned" heading in `type.body_strong` with an "All" pill button on the right, then a grid of `size.start_pinned_cols` x `size.start_pinned_rows` (6 x 3) tiles. Each tile is `size.start_tile_width` x `size.start_tile_height` (96 x 84) with a `size.start_tile_icon` (32) icon over a `type.caption` label, radius `radius.start_tile` (4). Extra pins page vertically.
- **Recommended:** heading plus "More" pill, then a two-column list of up to six recent items: 32 px icon, name in `type.body`, a relative time in `type.caption` `text_secondary`.
- **Footer:** `size.start_footer_height` (64) tall with a 1 px `stroke_divider` top border and a slightly darker band (8% black in dark mode, 35% white in light mode). The user's avatar (32 px circle) and name on the left, the power button (40 px) on the right, which opens Sleep, Shut down and Restart.

## Quick settings

- `size.flyout_width` (360) wide, anchored 12 px from the right screen edge and 12 px above the taskbar, `material.*.flyout`, `elevation.flyout`.
- A 3 x 2 grid of toggles (Wi-Fi, Bluetooth, Airplane mode, Energy saver, Night light, Accessibility). Each toggle is a 96 x 48 button over a `type.caption` label. On: accent fill with `text_on_accent`. Off: `control_fill` with a `stroke_control` outline.
- Brightness and volume sliders: 4 px track in `text_tertiary`, filled part in the accent, 20 px thumb with an accent centre.
- Footer, 48 px, same band as the Start footer: battery level on the left; edit and Settings buttons on the right.

## Notifications and calendar

Same width, anchor and material as quick settings. A "Notifications" header with "Clear all", the notification list or a "No new notifications" empty state, then the month calendar: a month title with previous and next buttons, and a 7-column grid of 40 px day cells. Today is an accent circle with `text_on_accent`; days outside the month use `text_tertiary`.

## Context menus

- `size.context_menu_width` (256) wide, 4 px inner padding, `material.*.menu`, `elevation.flyout`, radius `radius.overlay` (8).
- Items are `size.context_menu_item_height` (32) tall: a 16 px glyph, 12 px gap, label in `type.body`, and either a submenu chevron or a shortcut hint in `text_secondary` on the right. Hover uses `subtle_fill_hover` with `radius.control`.
- Separators are 1 px `stroke_divider` lines that run to the menu edges.
- The desktop menu reads: View, Sort by, Refresh | New | Display settings, Personalize | Open in Terminal, Show more options (Shift+F10).

## Window chrome

- Corner radius `radius.window` (8), a 1 px `stroke_surface` outline, shadow `elevation.window_active` when focused and `elevation.window_inactive` otherwise. Maximized windows drop the radius and shadow.
- The title bar is `size.caption_height` (32) for classic windows; File Explorer-style windows extend it to 40 to hold tabs. It draws mica when active and `window_bg_inactive` when not.
- Caption buttons are `size.caption_button_width` (46) wide and fill the caption height, with 10 px glyphs (`size.caption_glyph`) at 1 px stroke. Hover: `subtle_fill_hover`. Close hover: `caption_close_hover` (`#C42B1C`) with a white glyph.
- The resize grab zone is `size.resize_margin` (5) outside the visible edge.

## File Explorer

- **Title bar (40 px):** tabs. The active tab is 220 px minimum, `layer_bg` fill, `radius.overlay` top corners, 16 px icon, title in `type.caption` and a close glyph; then a new-tab button and the caption buttons.
- **Address row (48 px):** back, forward, up and refresh (36 x 32 each), a breadcrumb field with the location icon, then a 260 px search field.
- **Command bar (48 px):** an accent "New" button, then cut, copy, paste, rename, share and delete, a separator, then Sort and View menus and an overflow button. A 1 px `stroke_divider` line closes it.
- **Body:** a 220 px navigation pane (Desktop, Downloads, Documents, Pictures, Music, Videos, a separator, then This PC, drives and Network) beside the content area. The selected nav item gets a 3 px accent bar on its left edge. The content area shows "Devices and drives" as 48 px drive icons with a 6 px usage bar in the accent colour and free space in `type.caption`, then "Folders" as a grid of 48 px folder icons.
- **Status bar:** 24 px, item count in `type.caption` `text_secondary`.

## Accessibility

- Focus is a 2 px `focus_outer` ring outside a 1 px `focus_inner` ring, drawn on keyboard focus only.
- Text on acrylic must meet WCAG AA (4.5:1) against the worst-case region of the default wallpaper; the tint opacities above are chosen to guarantee that.
- High contrast mode replaces every token with the system high-contrast colours; the mapping is owned by the [theme system](../../todo/08-graphics-ui/TODO-03-theme-system.md).
- Every motion respects the reduced-animation setting by jumping straight to the final state.
