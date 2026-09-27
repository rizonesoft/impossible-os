<!-- docs: covers=todo/08-graphics-ui/TODO-10-taskbar.md,todo/08-graphics-ui/TODO-11-startmenu-tray-notifications.md,todo/08-graphics-ui/TODO-08-window-manager.md,todo/06-desktop-foundation/TODO-05-desktop-shell.md,todo/06-desktop-foundation/TODO-06-desktop-icons.md,todo/09-desktop-shell/TODO-09-file-manager.md,todo/08-graphics-ui/TODO-09-desktop-shell-features.md,todo/08-graphics-ui/TODO-12-clock-time.md order=2 -->
# Shell Specification

This page specifies the desktop shell pixel by pixel: the desktop, taskbar, Start menu, quick settings, notifications and calendar, context menus, window chrome and File Explorer. Every number is a token in [`tokens.json`](tokens.json) and a `THEME_*` constant in the generated `include/desktop/theme_tokens.h`; the token name is given in `code`. All sizes are at 100% scale and multiply by the display scale factor once DPI scaling lands.

The [interactive mockup](https://impossibleos.co/design/) is the visual reference for everything below. Each surface opens directly by URL hash (`#start`, `#quick`, `#cal`, `#ctx`, `#toast`, `#alttab`, `#snap`, `#taskview`, `#lock`, `#signin`, `#boot`, `#oobe`, `#settings`, `#osd`); add `?shot` for a deterministic render and `?theme=light` for the light theme. Those renders are the references the shell verification items compare against.

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

**Terminal.** The terminal window uses mica by default like every window; when the user lowers its background opacity below 100% it switches to acrylic with the `menu` material values at that opacity. This is the only window that may be acrylic.

**Flyout and menu motion.** Every flyout (quick settings, notifications and calendar, clipboard history, overflow, hover previews) and every menu opens by fading in while rising `size.flyout_rise` (12) px over `motion.normal` (167 ms, decelerate) and closes by fading out over `motion.fast` (83 ms). Start uses its own larger motion (see [Start menu](#start-menu)).

**Transparency off.** When `EnableTransparency` is `0` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize`, every material draws its tint at full opacity with no blur and no grain.

## Desktop

- The wallpaper fills the screen (`Fill` mode by default). The default is the "impossible bloom": `resources/backgrounds/bloom-dark.jpg` in dark mode and `bloom-light.jpg` in light mode.
- Desktop icons sit in a column-major grid from the top-left, `size.desktop_margin` (8) from the edges. Each cell is `size.desktop_cell_width` x `size.desktop_cell_height` (76 x 86) with a `size.desktop_icon` (48) icon, 6 px from the top of the cell, and a two-line label in `type.caption` (12/16).
- Labels are white (`desktop_label`) with a soft dark shadow (`desktop_label_shadow`) in both themes, as on Windows 11.
- Hover draws a 12% white fill with a 16% white 1 px stroke. Selection uses `selection_fill` and `selection_stroke`. Radius `radius.control` (4).
- Icon sizes (View menu): Small `size.desktop_icon_small` (32) in `desktop_cell_small_*` (64 x 70) cells, Medium (default) 48 in 76 x 86 cells, Large `size.desktop_icon_large` (96) in `desktop_cell_large_*` (120 x 132) cells. Icons snap to the grid ("Align icons to grid" on by default).
- Shortcuts carry a shortcut arrow overlay at the icon's bottom-left: a square `size.shortcut_overlay_ratio_pct` (33%) of the icon size, white with a 1 px `stroke_surface` and radius 2, holding a curved arrow glyph in `accent`.
- Default icons, in order: This PC, Recycle Bin, the user folder, Network and Control Panel. Recycle Bin switches between `recycle_bin_empty` and `recycle_bin_full`.

## Taskbar

- Height `size.taskbar_height` (48), full width, docked to the bottom, with a 1 px top border in `stroke_divider`.
- **Centre group**, centred on the screen: Start, the search box, Task view, then pinned and running apps. Buttons are `size.taskbar_button` (40) square with `size.taskbar_gap` (4) between them and `radius.taskbar_button` (4) corners; icons are `size.taskbar_icon` (24). Hover and the running-focused state draw `subtle_fill_hover` with a 1 px inset `stroke_control` outline.
- **Start button** shows the `start` icon, the logo mark. Pressing it scales the icon to 86% for the press duration.
- **Search box** is 196 x 36 with a `radius.search_box` (18) pill, `control_fill` background and `stroke_control` outline, a search glyph and the word "Search" in `text_secondary`. Below 1024 px screen width it collapses to a 40 px search button.
- **Running indicator:** a pill `size.taskbar_indicator_height` (3) tall, 2 px above the bottom edge. Running but not focused: 6 px wide in `taskbar_indicator_idle`. Focused: 16 px wide in `taskbar_indicator` (the accent), and the button gets the hover fill. The width animates over `motion.normal` (167 ms) with the decelerate curve.
- **Badges:** a taskbar button can carry a count or alert badge at its icon's top-right corner: a pill `size.badge_height` (16) tall and at least `size.badge_min_width` (16) wide, `badge_fill` with `text_on_accent`, 11 px semibold digits, "99+" above 99. An alert badge uses `status_critical` with a "!" glyph. A progress state draws a 3 px bar along the button's bottom edge in the accent (paused `status_caution`, error `status_critical`).
- **Attention:** a window that requests attention (`FlashWindow`) turns its button fill `taskbar_attention_fill` and its indicator pill full-width `taskbar_attention_indicator` until focused; no blinking.
- **Hover preview:** resting on a running app's button for `motion.taskbar_preview_delay` (400 ms) opens a menu-acrylic flyout above it with one `size.taskbar_preview_width` x `size.taskbar_preview_height` (200 x 120) thumbnail per window, each with the window title and a close glyph; hovering a thumbnail for `motion.peek_delay` (500 ms) peeks that window by fading all others to 10%.
- **Show desktop:** the last `size.show_desktop_width` (8) px at the far right of the taskbar; clicking it minimizes everything, hovering it peeks at the desktop.
- **Taskbar settings** (Personalisation > Taskbar): Alignment (Centre, default; Left), Search (Hide; Search icon only; Search icon and label; Search box, default), Task view (on by default), Automatically hide the taskbar (off). Height and position are fixed: 48 px at the bottom.
- **Right side:** an overflow chevron (28 x 40) that opens hidden tray icons, then, only when more than one keyboard layout is installed, a layout button (40 tall, three-letter code such as "ENG" in `type.caption`; click opens a menu of layouts), then the system tray cluster (network, volume and battery glyphs in one 40 px tall button that opens quick settings), then the clock (time over date in `type.caption`, right-aligned) with the notification bell, which opens notifications and the calendar.

## Start menu

- `size.start_width` x `size.start_height` (640 x 720, clamped to the space above the taskbar), centred horizontally, `size.start_offset_bottom` (12) above the taskbar. Radius `radius.overlay` (8), shadow `elevation.start`, 1 px `stroke_surface` outline.
- **Open:** fades in over `motion.normal` while sliding up `size.start_slide` (48) px over `motion.start_open` (250 ms) with the decelerate curve. **Close:** reverses over `motion.start_close` (167 ms) with the accelerate curve. The Windows key toggles it; Escape or a click outside closes it.
- **Search box** at the top: full width minus 32 px padding each side, `size.start_search_height` (36), pill radius, a 1 px accent line along the bottom edge while focused.
- **Pinned:** a "Pinned" heading in `type.body_strong` with an "All" pill button on the right, then a grid of `size.start_pinned_cols` x `size.start_pinned_rows` (6 x 3) tiles. Each tile is `size.start_tile_width` x `size.start_tile_height` (96 x 84) with a `size.start_tile_icon` (32) icon over a `type.caption` label, radius `radius.start_tile` (4). Extra pins page vertically.
- **Search results:** typing replaces the Pinned and Recommended regions below the search box with a two-column results view: left, the matches grouped by type (Apps, Settings, Documents) in 32 px rows with the matched text in `accent`; right, a "Best match" card for the selected result (64 px icon, name in `type.subtitle`, type in `type.caption`, and Open plus context actions as standard buttons). No results shows "No results for \"<query>\"" in `text_secondary`.
- **Recommended:** heading plus "More" pill, then a two-column list of up to six recent items: 32 px icon, name in `type.body`, a relative time in `type.caption` `text_secondary`.
- **Footer:** `size.start_footer_height` (64) tall with a 1 px `stroke_divider` top border and a slightly darker band (8% black in dark mode, 35% white in light mode). The user's avatar (32 px circle) and name on the left, the power button (40 px) on the right, which opens Sleep, Shut down and Restart.

## Quick settings

- `size.flyout_width` (360) wide, anchored 12 px from the right screen edge and 12 px above the taskbar, `material.*.flyout`, `elevation.flyout`.
- A 3 x 2 grid of toggles (Wi-Fi, Bluetooth, Airplane mode, Energy saver, Night light, Accessibility). Each toggle is a 96 x 48 button over a `type.caption` label. On: accent fill with `text_on_accent`. Off: `control_fill` with a `stroke_control` outline.
- Brightness and volume sliders: 4 px track in `text_tertiary`, filled part in the accent, 20 px thumb with an accent centre.
- Footer, 48 px, same band as the Start footer: battery level on the left; edit and Settings buttons on the right.

## Notifications and calendar

Same width, anchor and material as quick settings. A "Notifications" header with a bell toggle for Do not disturb (bell with a slash and `text_secondary` label "Do not disturb" when on) and "Clear all", the notification list or a "No new notifications" empty state, then the month calendar: a month title with previous and next buttons, and a 7-column grid of 40 px day cells. Today is an accent circle with `text_on_accent`; days outside the month use `text_tertiary`. Below the calendar, a Focus row: a minutes stepper (standard buttons) and a "Focus" standard button that starts a focus session (Do not disturb on, a countdown in the clock area).

## Context menus

- `size.context_menu_width` (256) wide, 4 px inner padding, `material.*.menu`, `elevation.flyout`, radius `radius.overlay` (8).
- Items are `size.context_menu_item_height` (32) tall: a 16 px glyph, 12 px gap, label in `type.body`, and either a submenu chevron or a shortcut hint in `text_secondary` on the right. Hover uses `subtle_fill_hover` with `radius.control`.
- Separators are 1 px `stroke_divider` lines that run to the menu edges.
- **File and folder menus** start with a `size.context_menu_icon_row_height` (40) row of icon-only subtle buttons (Cut, Copy, Rename, Share, Delete; Paste when applicable), 32 px each with tooltips, separated from the items below by a 1 px `stroke_divider`. "Show more options" opens the classic full-length menu.
- The desktop menu reads: View, Sort by, Refresh | New | Display settings, Personalize | Open in Terminal, Show more options (Shift+F10).

## Window chrome

- Corner radius `radius.window` (8), a 1 px `stroke_surface` outline, shadow `elevation.window_active` when focused and `elevation.window_inactive` otherwise. Maximized windows drop the radius and shadow.
- The title bar is `size.caption_height` (32) for classic windows; File Explorer-style windows extend it to 40 to hold tabs. It draws mica when active and `window_bg_inactive` when not.
- Caption buttons are `size.caption_button_width` (46) wide and fill the caption height, with 10 px glyphs (`size.caption_glyph`) at 1 px stroke. Hover: `subtle_fill_hover`. Close hover: `caption_close_hover` (`#C42B1C`) with a white glyph.
- The resize grab zone is `size.resize_margin` (5) outside the visible edge.
- **Window motion.** Open: scale from `size.window_open_scale_pct` (96%) to 100% while fading in over `motion.slow` (250 ms, decelerate). Close: the reverse over `motion.normal` (167 ms, accelerate). Minimize: shrink and move toward the window's taskbar button while fading over `motion.slow`; restore from the taskbar reverses it. Maximize and restore morph the window bounds over `motion.normal` with `motion.ease_standard`. Reduced animation makes every transition instant. Win32 `AnimateWindow` requests (`AW_BLEND`, `AW_SLIDE` with a direction, `AW_CENTER`) are honoured for compatibility with the requested effect over `motion.normal` and `motion.ease_standard`, regardless of the duration the app passes.

## File Explorer

- **Title bar (40 px):** tabs. The active tab is 220 px minimum, `layer_bg` fill, `radius.overlay` top corners, 16 px icon, title in `type.caption` and a close glyph; then a new-tab button and the caption buttons.
- **Address row (48 px):** back, forward, up and refresh (36 x 32 each), a breadcrumb field with the location icon, then a 260 px search field.
- **Command bar (48 px):** an accent "New" button, then cut, copy, paste, rename, share and delete, a separator, then Sort and View menus and an overflow button. A 1 px `stroke_divider` line closes it.
- **Body:** a 220 px navigation pane (Desktop, Downloads, Documents, Pictures, Music, Videos, a separator, then This PC, drives and Network) beside the content area. The selected nav item gets a 3 px accent bar on its left edge. The content area shows "Devices and drives" as 48 px drive icons with a 6 px usage bar in the accent colour and free space in `type.caption`, then "Folders" as a grid of 48 px folder icons.
- **Views:** icon view tiles are `size.explorer_tile_width` (96) wide with a 48 px icon 10 px from the top, a 6 px gap and a two-line caption; details view has a `size.explorer_header_height` (32) sortable column header (Name 240, Date modified 160, Type 120, Size 80 right-aligned, all resizable) and 32 px rows with no alternating fills, selection per [lists](controls.md#list-tree-and-grid-views).
- **Status bar:** 24 px, item count in `type.caption` `text_secondary`.

## Display scaling

Every size in this specification is at 100% and multiplies by the display scale. The default scale follows the screen: 100% below 2560 px wide, 150% from 2560, 200% from 3840; the user can choose 100% to 300% in 25% steps in Settings > System > Display. Icons pick the nearest rendered size at or above the scaled size; blur radii scale too.

## Toast notifications

- `size.toast_width` (364) wide, flyout acrylic, `radius.overlay`, `elevation.flyout`, anchored 12 px from the right edge and 12 px above the taskbar; new toasts stack upward `size.toast_gap` (12) apart, at most three visible.
- Layout: app icon (16) and app name in `type.caption` `text_secondary` on the top row with a close glyph on hover; title in `type.body_strong`; body in `type.body`, up to three lines; optional action buttons (standard buttons, equal width) along the bottom.
- Slides in from the right over `motion.toast_in` (250 ms, decelerate), stays `motion.toast_dwell` (5 s, paused while hovered), then fades out and moves into the notification center.

## Alt+Tab

- A full-width row of window thumbnails centred on the screen over a menu-acrylic panel (`radius.overlay`, `elevation.start`), 24 px padding.
- Each thumbnail is `size.alt_tab_thumb_height` (158) tall, width by aspect ratio, `radius.control`, with the app icon (16) and title in `type.caption` above it. The selected window has a 2 px `focus_outer` ring 4 px outside the thumbnail.
- Appears after 100 ms of Alt+Tab held; cycles with Tab and Shift+Tab; Escape cancels.

## Snap layouts

- Hovering the maximize button for `motion.snap_hover_delay` (500 ms), or pressing Win+Z, opens a menu-acrylic flyout below the button with six layouts: halves, 2/3 + 1/3, thirds, quarters, 1/2 + two quarters, 1/4 + 1/2 + 1/4.
- Each layout is a `size.snap_flyout_tile` (72) wide miniature with `radius.control` zones in `control_fill`; the hovered zone fills with `accent`. Dragging a window to a screen edge previews the zone as a translucent accent-tinted rectangle with `radius.window`.

## Task View

- Win+Tab or the taskbar button dims the desktop with `smoke` over a strong blur of the wallpaper and lays out every window as a thumbnail grid (same thumbnail rules as Alt+Tab), with a close button on hover.
- A strip along the bottom shows the virtual desktops as 16:9 cards with names and a "New desktop" card; the current desktop has the accent ring.

## Lock and sign-in screens

- **Lock screen:** the wallpaper at full brightness; time centred in the top third at `size.lock_clock` (96) semibold, the date below in `type.subtitle`; network and battery glyphs bottom right. Any key or click lifts the image with an upward slide over `motion.slow` to reveal sign-in.
- **Sign-in:** the wallpaper under a strong acrylic blur; a `size.login_avatar` (192) circular avatar, the user name in `type.title`, then a `size.login_field_width` (296) password box with an accent submit arrow button inside its right end. Other users are listed bottom left as 48 px avatars; accessibility, network and power buttons bottom right.

## Boot splash

- Black background; the logo mark at `size.boot_logo` (128, 256 on 2160p and above) centred at 40% of the screen height; a `size.boot_spinner` (40) dot-ring spinner in white centred at 70%. No text unless boot fails, which switches to the boot error screen. Verbose boot (`boot.conf` verbose mode) adds a `size.boot_progress_width` (240) wide, `size.progress_bar` (3) tall accent progress bar 48 px below the spinner, driven by the boot progress milestones; normal boot shows no bar. The splash fades to black over 8 steps before the desktop appears.

## First-run setup

- Operating system setup (the installer and the first-boot experience) is full screen: the dark bloom wallpaper under acrylic; a centred card 800 x 600 (`radius.overlay`, `window_bg`, `elevation.start`) with an illustration or icon on the left third and the step on the right: title in `type.title`, body in `type.body`, controls per [controls](controls.md), and an accent Next button bottom right with Back as a standard button.
- App installers are NOT full screen: they are normal windows of `size.app_installer_width` x `size.app_installer_height` (640 x 480) with the window chrome and the same step layout (icon or illustration left third, step content right, Next and Back bottom right).

## Settings and Control Panel frame

- A window with the standard chrome. Left: a `size.nav_pane_width` (280) navigation pane with the user card on top, a search box, and `size.nav_item_height` (36) items (16 px glyph, label, `radius.control` hover, 3 x 16 accent pill on the selected item).
- Right: the page title in `type.title`, then settings rows per the [cards and settings rows](controls.md#cards-and-settings-rows) spec, grouped under `type.body_strong` headings, 4 px apart within a group and 24 px between groups.

## Volume and brightness overlay

- Changing volume or brightness with keys shows a `size.osd_width` x `size.osd_height` (196 x 48) flyout-acrylic pill centred horizontally 24 px above the taskbar: glyph, a slider track per [slider](controls.md#slider) without a thumb, and the value. It fades out 2 s after the last change.

## App window layout

Every app window uses the [window chrome](#window-chrome). Below the title bar, an app that has commands shows a `size.command_bar_height` (48) command bar of subtle buttons (16 px glyph plus label where space allows, separators between groups, overflow "..." at the end), like File Explorer. Document and canvas apps (Paint, Photos, WordPad) centre their canvas on `layer_bg` with 24 px of breathing room and put zoom and page state in a `size.status_bar_height` (24) status bar in `type.caption`. Side panels are 280-320 px, separated by a 1 px `stroke_divider`. Classic Win32 menu bars use the [menu bar](controls.md#menu-bar-and-menus) spec. App-specific colours (sticky notes use `note_*`) are tokens, never literals. Keypad apps (Calculator) use standard buttons that grow to fill the window on a 2 px gap, never smaller than `size.keypad_button_min` (40) square; operator keys are subtle, the equals key is accent.

## Clipboard history

Win+V opens a `size.clipboard_width` (360) wide, up to `size.clipboard_max_height` (480) tall flyout-acrylic panel beside the text caret (or 12 px above the taskbar at the right when there is no caret), radius 8, `elevation.flyout`. A header with "Clipboard" and "Clear all", then one card per item (text clipped to three lines, or an image thumbnail) with a "..." menu for Pin and Delete. Enter pastes the selected item.

## Snipping toolbar

Print Screen (or Win+Shift+S) dims the screen with `smoke` and shows a `size.snip_toolbar_height` (48) flyout-acrylic toolbar centred 12 px from the top: capture modes as toggle buttons (Rectangle, Window, Full screen, Freeform), then a close button. The selection draws with `selection_fill` and a 1 px `selection_stroke`. The captured image raises a toast per [toast notifications](#toast-notifications).

## Magnifier

Win+Plus starts the magnifier in lens mode: a `size.magnifier_lens_width` x `size.magnifier_lens_height` (400 x 300) lens following the pointer, `radius.overlay`, a 2 px `accent` border, `elevation.flyout`. A compact flyout-acrylic toolbar (zoom out, zoom level, zoom in, view menu for Full screen / Lens / Docked, settings) appears at the top centre and fades after 3 s without interaction. Docked mode takes the top third of the screen, separated by a 2 px accent line.

## Accessibility

- Focus is a 2 px `focus_outer` ring outside a 1 px `focus_inner` ring, drawn on keyboard focus only.
- Text on acrylic must meet WCAG AA (4.5:1) against the worst-case region of the default wallpaper; the tint opacities above are chosen to guarantee that.
- High contrast mode replaces every token with the system high-contrast colours; the mapping is owned by the [theme system](../../todo/08-graphics-ui/TODO-03-theme-system.md).
- Every motion respects the reduced-animation setting by jumping straight to the final state.
