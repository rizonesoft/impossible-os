---
schema_version: 1
id: fluent-component-library
domain: 08-graphics-ui
status: active
title: "TODO-19 -- Fluent Component Library"
---

# TODO-19 -- Fluent Component Library

> **Goal:** Supply every control a 1:1 Windows 11 interface is assembled from, each built once on the UI framework engines and used everywhere. The existing widget files in this folder own the check box, radio button, slider, progress bar, combo box, tab strip, scroll bar, list view, tree view, toolbar, menu bar, status bar, tooltip, dialog system and info bar. This TODO owns the rest of the modern control set (buttons beyond the basic one, toggle switch, text and number entry, pickers, flyouts, command bar, content dialog, navigation view, breadcrumb, tab view, settings cards, grid and flip views, split views, title bar, media transport) plus the contract every control meets, a gallery app that shows them all, and a conformance suite that checks every state of every control against the inventory.

> [!IMPORTANT]
> **Current state:** Measured 2026-11-14. [`src/desktop/controls.c`](../../src/desktop/controls.c) implements `CTRL_BUTTON`, `CTRL_LABEL`, `CTRL_TEXTBOX` and `CTRL_SCROLLBAR` only. [`docs/design/controls.md`](../../docs/design/controls.md) specifies button, toggle switch, check box, radio, slider, text boxes, date picker, combo box, lists, tabs, progress, scroll bar, tooltip, dialog, menus, info bar and settings cards; it has no entry yet for number box, auto-suggest, colour picker, rating, flyout, command bar, teaching tip, navigation view, breadcrumb, tab view, expander, grid and flip views, split view, selector bar or title bar. No gallery app and no control conformance test exists.

> [!NOTE]
> **Owner direction, 2026-11-14:** controls match Windows 11 in size, shape, states and keyboard behaviour, measured from the inventory, and follow one rule each where the original has several. All artwork and code is original; nothing is taken from Microsoft's control libraries. The control list itself is checked against the `controls` baseline of the inventory.

## Inputs

- [`docs/design/controls.md`](../../docs/design/controls.md) -- the control spec; each section here extends it first when its control is missing
- [`docs/design/tokens.json`](../../docs/design/tokens.json) -- sizes, radii, colours and motion every template resolves through resources
- [`src/desktop/controls.c`](../../src/desktop/controls.c) -- the existing button and text box, superseded by §2 and §4 on the new engine
- -> XREF: `TODO-18-ui-framework-engines.md §1,§5,§7,§9,§11,§14,§24` -- element tree, styles and states, input, commands, markup, popups and automation peers every control is built on
- -> XREF: `TODO-17-win11-parity-inventory.md §5,§49` -- the `controls` baseline this library is complete against, and the measured state matrix each template implements
- -> XREF: `TODO-05-widget-library-core.md §1,§2,§3,§4,§5,§6,§9` -- check box, radio, slider, progress bar, combo box, tab strip and overlay scroll bar stay owned there and adopt the contract of §1
- -> XREF: `TODO-06-widget-dialogs.md §1,§2,§3,§4,§5,§7,§8,§9` -- list view, tree view, toolbar, menu bar, status bar, tooltip, dialog system and info bar stay owned there and adopt the contract of §1
- -> XREF: `TODO-02-text-font-internationalization.md §4` -- caret, selection and composition services the text controls of §4 to §7 use
- -> XREF: `D10 TB §2` -- the classic common control classes §24 maps onto these controls

## Outcome

- Every control of the inventory's `controls` baseline exists as an element type with a template, or is recorded as excluded with a reason.
- Every control meets one contract: states, keyboard, pointer, touch, automation, right-to-left, both densities, light, dark and contrast.
- One dialog frame, one flyout family, one command surface and one settings row replace the several generations Windows 11 carries.
- A gallery app shows every control in every state and is the fixture the conformance suite and the visual regression tests read.
- Classic Win32 control classes render through these controls, so an old-style dialog and a new page look the same.

## Control Quick Reference

| Family      | Controls                                                                  | Sections          |
| ----------- | ------------------------------------------------------------------------- | ----------------- |
| Buttons     | Button variants, toggle switch                                            | §2, §3            |
| Text entry  | Text, password and search box, number box, auto-suggest, rich text, links | §4-§7             |
| Pickers     | Date, time, calendar, colour                                              | §8, §9            |
| Status      | Progress ring, rating, person picture, info badge                         | §10, §11          |
| Surfaces    | Flyouts, command bar, content dialog, teaching tip                        | §12-§14           |
| Navigation  | Navigation view, breadcrumb, tab view, selector bar, title bar            | §15-§17, §21, §22 |
| Collections | Expander and settings cards, grid, items and flip views, split views      | §18-§20           |
| Media       | Media transport controls                                                  | §23               |

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On           | Status |
| --- | :---: | ------------------------------------------------- | -------------------- | :----: |
| ⭐  |   1   | §1 Control authoring contract                     | T18 §1,§5,§7,§11,§24 |  [ ]   |
| 💎  |   2   | §2 Button family                                  | §1, T18 §9           |  [ ]   |
| 💎  |   3   | §3 Toggle switch                                  | §1                   |  [ ]   |
| 💎  |   4   | §4 Text box family                                | §1, T02 §4           |  [ ]   |
| 💎  |   5   | §5 Number box                                     | §4                   |  [ ]   |
| 💎  |   6   | §6 Auto-suggest box                               | §4, T18 §14          |  [ ]   |
| 💎  |   7   | §7 Rich text display and hyperlink                | §1, T02 §3           |  [ ]   |
| 💎  |   8   | §8 Date, time and calendar pickers                | §1, T18 §14          |  [ ]   |
| 💎  |   9   | §9 Colour picker                                  | §1, §5               |  [ ]   |
| 💎  |  10   | §10 Progress ring                                 | §1                   |  [ ]   |
| 💎  |  11   | §11 Rating, person picture and info badge         | §1                   |  [ ]   |
| 💎  |  12   | §12 Flyout and menu flyout                        | §1, T18 §9,§14       |  [ ]   |
| 💎  |  13   | §13 Command bar and command bar flyout            | §2, §12              |  [ ]   |
| ⭐  |  14   | §14 Content dialog and teaching tip               | §2, T18 §14          |  [ ]   |
| 💎  |  15   | §15 Navigation view                               | §1, T18 §3,§15       |  [ ]   |
| 💎  |  16   | §16 Breadcrumb bar                                | §12, T18 §15         |  [ ]   |
| 💎  |  17   | §17 Tab view                                      | §2, T18 §8           |  [ ]   |
| 💎  |  18   | §18 Expander, settings card and settings expander | §1, §3               |  [ ]   |
| 💎  |  19   | §19 Grid view, items view and flip view           | §1, T18 §4,§10       |  [ ]   |
| 💎  |  20   | §20 Split view and two-pane view                  | §1, T18 §3           |  [ ]   |
| 💎  |  21   | §21 Selector bar and segmented control            | §1                   |  [ ]   |
| ⭐  |  22   | §22 Title bar control                             | §2, §6               |  [ ]   |
| 💎  |  23   | §23 Media transport controls                      | §2, T18 §12          |  [ ]   |
| ⭐  |  24   | §24 Classic control parity                        | §2, §4, §8, §12      |  [ ]   |
| ⭐  |  25   | §25 Controls gallery app                          | §2, §4, §12, §15     |  [ ]   |
| ⭐  |  26   | §26 State matrix conformance suite                | §25, T17 §49         |  [ ]   |

> 💎 = parity work: the control exists in Windows 11 and in the Linux toolkits.
> ⭐ = exclusive work: one contract for every control, one dialog frame, classic and modern controls sharing one look, and a conformance suite measured against a reference.

---

## 1. Control Authoring Contract

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

The definition of done for a control, written once, so twenty-five sections do not each invent their own.

- [ ] Write `docs/design/control-contract.md`: the list every control meets, with the check that proves each point
  - A registered element type and a template in `src/desktop/ui/controls/<name>.iui`, with no colour, size or duration literal (resources only)
  - The common, focus and selection visual states of the engine, with no state logic of its own
  - Keyboard: reachable by Tab, operated by the keys in the inventory's shortcut table, with an access key where it has a label
  - Pointer, touch and pen: a hit target of at least 40 by 40 units for touch, and the same result from each input kind
  - An automation peer with role, name, value, state and actions, and the `inventory` id of the record it implements
  - Right-to-left mirroring, the compact density, and the light, dark and contrast themes
  - A gallery page (§25) and a row in the state matrix (§26)
- [ ] Add `scripts/ui/check-control.py <name>`: verifies the mechanical points (template exists, no literals, peer registered, gallery page exists, matrix row exists) and prints one line per failed point
- [ ] Add the control template for new controls, `src/desktop/ui/controls/_template/`: the `.c`, `.h`, `.iui`, gallery page and test stubs with the contract's order
- [ ] Add a short adoption item to the still-open sections of `TODO-05` and `TODO-06` naming this contract, without changing what those sections own
- [ ] Wire `check-control.py --all` into `scripts/lint.sh` for staged files under `src/desktop/ui/controls/`
- [ ] Commit: `"controls: authoring contract, checker and template"`

**Test checkpoint:** `python3 scripts/ui/check-control.py _template` prints `control _template: 7 of 7 points` and exits 0; removing the gallery stub makes it exit 1 naming `gallery page`. `bash scripts/lint.sh` passes.

## 2. Button Family

**Design:** [`controls.md#button`](../../docs/design/controls.md#button)

- [ ] Add to `controls.md` the variants it lacks, from the inventory's measurements: accent, standard, subtle, hyperlink, repeat, toggle, split, drop-down, and icon-only
- [ ] Button base: content (text, icon, or both with a spacing rule), command binding, click on pointer release inside, on Space release and on Enter press
- [ ] Accent, standard and subtle styles as three styles of one type, not three types
- [ ] Repeat button: fires on press, then after the initial delay, then at the repeat interval, both from system metrics
- [ ] Toggle button: checked, unchecked and optional indeterminate, with the checked state styles
- [ ] Split button and drop-down button: a primary part and a chevron part that opens a flyout (§12); Alt-Down and F4 open it; the two parts have separate hover and pressed states
- [ ] Hyperlink button: link colours in rest, hover, pressed and visited, and activation through the app model's URI path
- [ ] Default and cancel roles: Enter invokes the default button of the scope and Esc the cancel button, drawn with the default emphasis
- [ ] Replace `CTRL_BUTTON` call sites with the new type and delete the old paint path
- [ ] Commit: `"controls: button family"`

**Test checkpoint:** `test_ctl_button` passes: Space press then release invokes once, a pointer release outside the button invokes nothing, a repeat button held 1 second fires more than twice, and the split button's chevron opens its flyout without invoking the primary command. `check-control.py button` reports 7 of 7.

## 3. Toggle Switch

**Design:** [`controls.md#toggle-switch`](../../docs/design/controls.md#toggle-switch)

- [ ] Track and knob template at the measured sizes, with the knob growing on hover and stretching on press as recorded
- [ ] On and off content: the state word beside the switch ("On", "Off" from resources), optional, placed before or after by flow direction
- [ ] Toggle on click, on Space, and by dragging the knob past the midpoint; a drag that ends before the midpoint returns
- [ ] Knob travel animates with the recorded duration and curve; reduce-motion snaps
- [ ] Header slot above the switch for a label, wired as the automation name
- [ ] Two-way binding of the `is_on` property, and a `toggled` event raised once per change
- [ ] Commit: `"controls: toggle switch"`

**Test checkpoint:** `test_ctl_toggle` passes: Space toggles, a drag of 60 percent of the track toggles and a drag of 40 percent does not, and the peer reports the toggle action and the on state. `check-control.py toggle-switch` reports 7 of 7.

## 4. Text Box Family

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box)

- [ ] Text box on the text services of `TODO-02 §4`: caret, selection by keyboard, pointer and touch handles, word and line moves, the one editing key table of the inventory
- [ ] The field chrome: the bottom accent line on focus, placeholder text, header and description slots, the clear button that shows when focused and non-empty
- [ ] Clipboard, undo and redo as the standard commands of the command engine, and the built-in context menu (cut, copy, paste, undo, select all) as one command group
- [ ] Constraints: maximum length, read-only, input scope (number, email, URL) for the touch keyboard, multi-line with wrapping and its own scroll viewer
- [ ] Password box: masked characters, the reveal button held to show, no copy or cut, and the buffer cleared with a volatile wipe on destroy
- [ ] Search box: the leading search icon, the clear button, a query-submitted event on Enter, and Esc clearing the text
- [ ] Composition: IME composition text shown inline with its underline, through `TODO-07 §4`
- [ ] Replace `CTRL_TEXTBOX` call sites with the new type and delete the old paint path
- [ ] Commit: `"controls: text box, password box and search box"`

**Test checkpoint:** `test_ctl_textbox` passes: typing, Shift-arrow selection and Ctrl-X then Ctrl-V round-trip the text; a 5 character limit refuses the sixth; the password box's peer never exposes its value and copy is disabled. `check-control.py text-box` reports 7 of 7.

## 5. Number Box

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box)

- [ ] Add the number box entry to `controls.md` from the inventory's measurements before building
- [ ] Value, minimum, maximum, small and large step, and a wrap option; text is parsed on commit and reformatted through the locale number format
- [ ] Spin buttons: hidden, inline (beside the text) or compact (a flyout on focus), stepping by the small step and repeating on hold
- [ ] Keys: Up and Down step small, Page Up and Page Down step large, wheel steps when focused
- [ ] Simple expressions on commit (`2+3*4`), evaluated with integer or fixed-point arithmetic and rejected when malformed
- [ ] Invalid input restores the last valid value and raises the validation state with a message slot
- [ ] Commit: `"controls: number box"`

**Test checkpoint:** `test_ctl_numberbox` passes: Up from 9 with maximum 10 gives 10 and a second Up stays at 10, `2+3*4` commits as 14, and `abc` restores the previous value with the invalid state set. `check-control.py number-box` reports 7 of 7.

## 6. Auto-Suggest Box

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box), [`controls.md#combo-box-and-drop-down`](../../docs/design/controls.md#combo-box-and-drop-down)

- [ ] Add the auto-suggest entry to `controls.md` from the inventory's measurements before building
- [ ] A text box with a suggestion list in a popup below it, sized to the box's width, opening after text changes by user input
- [ ] Suggestions come from a bound observable list the owner fills on a text-changed event that says whether the user or the program changed the text
- [ ] Keys: Down and Up move through suggestions and fill the box with the highlighted one, Enter submits the highlighted suggestion or the typed query, Esc closes the list then clears
- [ ] Events: suggestion chosen and query submitted, each once
- [ ] The query icon button at the end of the box, and an item template slot for rich suggestions
- [ ] Debounce: the text-changed event for user input is raised after the typing pause recorded in the inventory, so a provider is not called per keystroke
- [ ] Commit: `"controls: auto-suggest box"`

**Test checkpoint:** `test_ctl_autosuggest` passes: typing opens the list, Down then Enter raises suggestion-chosen then query-submitted once each, and a programmatic text change opens nothing. `check-control.py auto-suggest-box` reports 7 of 7.

## 7. Rich Text Display and Hyperlink

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

- [ ] Text block: the type ramp styles as named styles (caption, body, body strong, subtitle, title, display), wrapping, trimming with an ellipsis, maximum lines, and line height from tokens
- [ ] Inline runs: bold, italic, underline, colour, and inline hyperlinks, laid out through the paragraph engine of `TODO-02 §3`
- [ ] Selectable text: an opt-in selection with pointer, keyboard and the copy command
- [ ] Trimmed text shows its full content in a tooltip, automatically
- [ ] Inline hyperlink: focusable, activated by Enter and click, with its own automation peer inside the text
- [ ] Access key marking: the underlined letter of a label, shown per the access key setting
- [ ] Commit: `"controls: text block, inline runs and hyperlinks"`

**Test checkpoint:** `test_ctl_textblock` passes: a long string in a 100 unit block with a two-line maximum ends in an ellipsis and reports trimmed, Tab reaches an inline hyperlink and Enter activates it, and selection plus copy yields the plain text. `check-control.py text-block` reports 7 of 7.

## 8. Date, Time and Calendar Pickers

**Design:** [`controls.md#date-picker`](../../docs/design/controls.md#date-picker)

- [ ] Add the calendar view, calendar date picker and time picker entries to `controls.md` from the inventory's measurements before building
- [ ] Calendar view: the month grid (6 by 7), month, year and decade zoom levels with their transitions, first day of week and day names from the locale, today marked, selection single, multiple or range
- [ ] Calendar keys: arrows move by day and week, Page Up and Down by month, Ctrl-Up and Ctrl-Down change zoom level, Home and End go to month start and end
- [ ] Calendar date picker: a button showing the formatted date that opens the calendar view in a flyout
- [ ] Date picker: looping selector columns for day, month and year in locale order, opened in a flyout with accept and cancel
- [ ] Time picker: looping columns for hour, minute and the day period, 12 or 24 hour by locale, with a minute increment
- [ ] Looping selector part shared by both: wheel, drag with snap, arrow keys, and typing a value
- [ ] Minimum and maximum dates, blackout dates, and a null (unset) value with placeholder text
- [ ] Commit: `"controls: calendar view, date and time pickers"`

**Test checkpoint:** `test_ctl_calendar` passes: Right from the 31st moves to the 1st of the next month and changes the displayed month, a date before the minimum cannot be selected, and the time picker in a 24 hour locale has no day period column. `check-control.py calendar-view` reports 7 of 7.

## 9. Colour Picker

**Design:** [`controls.md#slider`](../../docs/design/controls.md#slider)

- [ ] Add the colour picker entry to `controls.md` from the inventory's measurements before building
- [ ] Spectrum: a box or ring of hue and saturation with a draggable selector, plus a value slider and an optional alpha slider over a checkerboard
- [ ] Text entry: red, green and blue or hue, saturation and value number boxes (§5), a hex box, and the model switch between them
- [ ] Preview: the new colour beside the previous one
- [ ] Keys: arrows move the spectrum selector by one step and by a large step with Ctrl
- [ ] The selector reports a colour name through its automation peer (nearest named colour), so the control is usable without sight
- [ ] Conversions between colour models in fixed-point, with a round-trip test, in `src/desktop/ui/controls/colour_math.c`
- [ ] Commit: `"controls: colour picker"`

**Test checkpoint:** `test_ctl_colour` passes: converting 4,096 sampled colours RGB to HSV and back changes no channel by more than 1, typing a hex value moves the selector, and the peer's value is a colour name. `check-control.py colour-picker` reports 7 of 7.

## 10. Progress Ring

**Design:** [`controls.md#progress`](../../docs/design/controls.md#progress)

- [ ] Determinate ring: an arc from 0 to 100 percent at the measured sizes and stroke widths, with rounded ends
- [ ] Indeterminate ring: the looping arc animation with the recorded cycle, driven on the compositor so it stays smooth while the UI thread works
- [ ] States: running, paused and error colours, matching the progress bar of `TODO-05 §4`
- [ ] Reduce-motion replaces the spinning arc with a pulsing static ring
- [ ] An `is_active` property that removes the visual and stops the animation when false, costing nothing while hidden
- [ ] Reuse the boot splash spinner's arc rasteriser where it fits, without linking shell code into the boot path
- [ ] Commit: `"controls: progress ring"`

**Test checkpoint:** `test_ctl_progressring` passes: at 25 percent the arc spans 90 degrees, an inactive ring schedules no animation ticks, and the peer reports the range value. `check-control.py progress-ring` reports 7 of 7.

## 11. Rating, Person Picture and Info Badge

**Design:** [`controls.md#status-colours`](../../docs/design/controls.md#status-colours)

- [ ] Add the rating, person picture and info badge entries to `controls.md` from the inventory's measurements before building
- [ ] Rating: a row of items (five by default), set by click, drag and arrow keys, with a clear gesture, a read-only mode and a placeholder value
- [ ] Person picture: a circular image, or initials derived from a display name, or a generic glyph, with an optional badge slot
- [ ] Info badge: a dot, a number (capped display such as "99+") or an icon, in the attention, informational, success, caution and critical colours
- [ ] Badge placement helper: attaches a badge to the corner of another element (a navigation item, a taskbar button)
- [ ] Automation: the rating exposes a range value; a badge's number is appended to its host's name
- [ ] Commit: `"controls: rating, person picture and info badge"`

**Test checkpoint:** `test_ctl_badges` passes: Right three times on an empty rating gives 3, a display name of two words yields two initials, and a badge value of 120 displays the capped form. `check-control.py rating` reports 7 of 7.

## 12. Flyout and Menu Flyout

**Design:** [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus), [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)

One menu for every place a menu appears: context menus, drop-down buttons, the menu bar's popups and the system menu.

- [ ] Flyout: arbitrary content in a popup of the popup layer, attached to a target with a placement, light dismiss, and focus moved in on open and restored on close
- [ ] Menu flyout: items built from a command group; item kinds are command, toggle, radio (in a group), submenu and separator
- [ ] Item layout from the inventory: icon column, label, key hint column, submenu chevron, with the columns aligned across all items of one menu
- [ ] Keys: arrows move, Right opens a submenu, Left closes it, Enter and Space invoke, a letter invokes by access key, Home and End jump, typing cycles matching items
- [ ] Submenu timing: opens on hover after the recorded delay, and stays open while the pointer moves toward it
- [ ] Scrolling: a menu taller than the work area scrolls with edge buttons
- [ ] Context request: the context menu key, Shift-F10, right click and press-and-hold all raise one context-requested event with a position
- [ ] Move the existing context menu engine (`TODO-09 §1`) onto this control as its renderer, keeping its public call
- [ ] Commit: `"controls: flyout and menu flyout"`

**Test checkpoint:** `test_ctl_menu` passes: a three-level menu opens with Right and closes one level per Left, an access key invokes its item, a disabled command's item is skipped by arrow keys, and Shift-F10 raises one context request. `check-control.py menu-flyout` reports 7 of 7.

## 13. Command Bar and Command Bar Flyout

**Design:** [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus), [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

- [ ] Add the command bar entry to `controls.md` from the inventory's measurements before building
- [ ] Command bar: primary commands as app bar buttons in a row, secondary commands in the overflow menu, both from one command group
- [ ] App bar button, toggle button and separator: icon with optional label, the label position (right, bottom, collapsed), and the compact form
- [ ] Dynamic overflow: when width runs out, primary commands move to the overflow menu in the declared order and return when room comes back
- [ ] Keys: arrow keys move between buttons inside the bar, Tab leaves it, and the overflow opens with Enter, Space or Alt-Down
- [ ] Command bar flyout: the floating form with a primary row and a secondary list, shown on selection or context request
- [ ] Content slot at the start of the bar for a custom element (a search box, a path)
- [ ] The toolbar of `TODO-06 §3` and this bar share the app bar button type, so there is one toolbar button in the system
- [ ] Commit: `"controls: command bar and command bar flyout"`

**Test checkpoint:** `test_ctl_commandbar` passes: narrowing the bar from 600 to 200 units moves commands to overflow in declared order with none lost, widening restores them, and a command disabled in the group is disabled in both places. `check-control.py command-bar` reports 7 of 7.

## 14. Content Dialog and Teaching Tip

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog)

One dialog frame for the whole system, the rule the consistency ledger sets for the three dialog generations.

- [ ] Content dialog: title, content, and up to three buttons (primary, secondary, close) in one fixed order and alignment, with a default button
- [ ] The frame's measurements (padding, title style, button row, maximum and minimum width, corner radius, shadow) come from the ledger's dialog rule and nothing else
- [ ] Modal to its window through the modal layer, with the smoke overlay, focus trap, Esc mapped to close and Enter to the default button
- [ ] A window-level form for a dialog with no owner window, sharing the same template
- [ ] Message helper: `ui_message(owner, icon_role, title, text, buttons)` returning the chosen button, the one call every "are you sure" uses
- [ ] Move the dialog system of `TODO-06 §8` (message box, file dialogs) onto this frame, keeping its public calls and the seven button sets of the inventory
- [ ] Teaching tip: a non-modal callout attached to a target with a tail, or centred with none, with title, subtitle, content, an action button and a close button
- [ ] Teaching tip placement and light-dismiss mode, and automatic dismissal when its target leaves the screen
- [ ] Commit: `"controls: content dialog and teaching tip"`

**Test checkpoint:** `test_ctl_dialog` passes: Enter returns the default button, Esc returns close, Tab cycles only inside the dialog, focus returns to the opener, and a message box request for each of the seven button sets renders in the one frame. `check-control.py content-dialog` reports 7 of 7.

## 15. Navigation View

**Design:** [`shell.md#settings-and-control-panel-frame`](../../docs/design/shell.md#settings-and-control-panel-frame)

- [ ] Add the navigation view entry to `controls.md` from the inventory's measurements before building
- [ ] The pane: menu items with icon and label, group headers, separators, footer items, a settings item, and a header slot for search or an account
- [ ] Display modes: left expanded, left compact (icons only, expanding on hover or the menu button), left minimal (hidden behind the menu button) and top; the mode follows width breakpoints
- [ ] Hierarchy: items with children expand in place, with the expand chevron and indent
- [ ] Selection: one selected item with the animated selection indicator moving between items
- [ ] The content area hosts a frame of the navigation framework; selecting an item navigates, and back navigation updates the selection
- [ ] The back button and the pane toggle button in the title area, both commands
- [ ] Keys: arrows move in the pane, Enter selects, Right and Left expand and collapse, F6 moves between pane and content
- [ ] Info badge slot on items (§11)
- [ ] Commit: `"controls: navigation view"`

**Test checkpoint:** `test_ctl_navview` passes: at a width below the first breakpoint the pane is minimal and at a width above the second it is expanded, selecting an item navigates the frame once, and back restores the previous selection. `check-control.py navigation-view` reports 7 of 7.

## 16. Breadcrumb Bar

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

- [ ] Add the breadcrumb bar entry to `controls.md` from the inventory's measurements before building
- [ ] Items from a bound list, separated by chevrons, the last item styled as current and not clickable
- [ ] Overflow: when items do not fit, the leading ones collapse into an ellipsis item that opens a menu flyout of the hidden ones
- [ ] Item click raises item-clicked with the index; the owner truncates the path
- [ ] Explorer form: each chevron opens a menu of the sibling folders at that level, and a click on empty space switches to the editable path box
- [ ] Keys: Left and Right move between items, Enter activates, Alt-Down opens a chevron's menu
- [ ] Right-to-left reverses the item order and flips the chevrons
- [ ] Commit: `"controls: breadcrumb bar"`

**Test checkpoint:** `test_ctl_breadcrumb` passes: six items in a width that fits three show the ellipsis plus the last three, the ellipsis menu lists the hidden items in order, and clicking the second item raises item-clicked with index 1. `check-control.py breadcrumb-bar` reports 7 of 7.

## 17. Tab View

**Design:** [`controls.md#tabs`](../../docs/design/controls.md#tabs)

The document tab strip of File Explorer, Notepad and the terminal. The simple tab strip stays with `TODO-05 §6`.

- [ ] Add the tab view entry to `controls.md` from the inventory's measurements before building
- [ ] Tab items with icon, title, a close button (always, on hover, or only on the selected tab) and the selected tab's shape joining the content area
- [ ] The add button after the last tab, and the tab strip header and footer slots for title bar integration
- [ ] Width modes: equal, size to content, and compact (only the selected tab shows its title); tabs shrink to a minimum, then the strip scrolls with edge buttons
- [ ] Drag to reorder with the other tabs sliding aside; drag out of the strip raises a tear-out request, and a drop from another window raises a drop request
- [ ] Keys: Ctrl-Tab and Ctrl-Shift-Tab cycle, Ctrl and a digit select by position, Ctrl-W or Ctrl-F4 close, Ctrl-T adds
- [ ] A tab's unsaved indicator and its context menu (close, close others, close to the right) as a command group
- [ ] Commit: `"controls: tab view"`

**Test checkpoint:** `test_ctl_tabview` passes: closing the selected tab selects its right neighbour (or the left when it was last), dragging tab 1 past tab 3 reorders the bound list, and Ctrl-Tab wraps from the last tab to the first. `check-control.py tab-view` reports 7 of 7.

## 18. Expander, Settings Card and Settings Expander

**Design:** [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

The row every Settings page is made of, built once.

- [ ] Expander: a header and a content area that opens downward or upward with the recorded motion; Space and Enter toggle; the chevron rotates
- [ ] Settings card: icon, title, description, and a control slot at the end; the whole card is clickable when it navigates, with a trailing chevron
- [ ] Settings expander: a settings card as the header of an expander whose content is a list of nested cards with the inset layout
- [ ] Card states and the group layout: spacing between cards, the group heading above, and corner rounding for first, middle and last cards in a joined group
- [ ] Narrow layout: below the recorded width, the control slot wraps under the text
- [ ] Automation: a card exposes its title as the name of the control inside it, so a screen reader reads "Night light, toggle, off"
- [ ] Generated use: the settings platform's generic page instantiates these from setting definitions with no per-setting code
- [ ] Commit: `"controls: expander, settings card and settings expander"`

**Test checkpoint:** `test_ctl_settingscard` passes: a toggle inside a card takes the card's title as its automation name, the expander's content is absent from the tab order while collapsed, and at the narrow width the control slot arranges below the description. `check-control.py settings-card` reports 7 of 7.

## 19. Grid View, Items View and Flip View

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

The list view and tree view stay with `TODO-06 §1,§2`. These are the tile and paging collections.

- [ ] Items view: a selectable, virtualized collection over the items repeater with a pluggable layout (stack, grid, wrap) and one selection model
- [ ] Selection model shared with the list view: none, single, multiple and extended, with Ctrl and Shift click, Ctrl-A, Space to toggle, and selection check boxes on hover or touch
- [ ] Grid view: tiles in a wrapping grid with item templates, group headers (sticky while scrolling), and incremental loading when the end nears
- [ ] Item container states: rest, hover, pressed, selected, selected and hover, focused, drag source and drop target, from the state matrix
- [ ] Reorder and drag: drag to reorder within the view, and drag items out as a drag source
- [ ] Flip view: one item at a time with previous and next buttons on hover, swipe on touch, arrow keys, and a pips pager beneath it
- [ ] Pips pager and semantic zoom: page dots with previous and next; a zoomed-out view of group headers that jumps to a group (the Start menu's letter grid)
- [ ] Commit: `"controls: items view, grid view and flip view"`

**Test checkpoint:** `test_ctl_gridview` passes: Shift-click selects a contiguous range across rows, a 50,000 item grid view holds fewer than 200 live containers, and the flip view's Right key advances one item and stops at the last. `check-control.py grid-view` reports 7 of 7.

## 20. Split View and Two-Pane View

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout)

- [ ] Add the split view entry to `controls.md` from the inventory's measurements before building
- [ ] Split view: a pane and content with display modes overlay, inline, compact overlay and compact inline, an open and closed state, and left or right placement
- [ ] Pane open and close motion, and light dismiss for the overlay modes
- [ ] Two-pane view: two panes side by side or stacked by available width, with a priority pane shown alone when only one fits
- [ ] Resizable pane: a splitter between pane and content with minimum and maximum widths, double-click to reset, and arrow keys when the splitter is focused
- [ ] Details and preview pane use in File Explorer: the pane remembers its width per window kind in the layout store
- [ ] Focus: opening an overlay pane moves focus into it, closing returns it
- [ ] Commit: `"controls: split view and two-pane view"`

**Test checkpoint:** `test_ctl_splitview` passes: in overlay mode a click on the content closes the pane, in inline mode it does not, and the two-pane view shows only the priority pane below its breakpoint. `check-control.py split-view` reports 7 of 7.

## 21. Selector Bar and Segmented Control

**Design:** [`controls.md#tabs`](../../docs/design/controls.md#tabs)

- [ ] Add the selector bar and segmented control entries to `controls.md` from the inventory's measurements before building
- [ ] Selector bar: a row of text or icon items with the selection underline that slides between items, used to switch views inside a page
- [ ] Segmented control: joined buttons with one selected (or several in multi-select mode), with first, middle and last corner shapes
- [ ] Radio buttons group helper: a labelled group of radio buttons with arrow key movement and one tab stop, for settings lists
- [ ] Keys: arrows move and select, Home and End jump, one tab stop for the whole bar
- [ ] Automation: the bar is a tab list or a radio group by use, and items report selected
- [ ] Commit: `"controls: selector bar and segmented control"`

**Test checkpoint:** `test_ctl_selector` passes: Right moves selection and raises selection-changed once, the bar is one tab stop, and the peer of the selected item reports selected. `check-control.py selector-bar` reports 7 of 7.

## 22. Title Bar Control

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome)

One title bar for every window, the rule the consistency ledger sets for the four heights Windows 11 ships.

- [ ] A title bar element an app places at the top of its window: back button, pane toggle button, icon, title, subtitle, a centre content slot (a search box, tabs) and a trailing slot before the caption buttons
- [ ] Two heights only, standard and tall (when it holds a search box or tabs), from the ledger's title bar rule
- [ ] Drag regions: everything not interactive drags the window; double-click maximizes; right-click and Alt-Space open the system menu as a menu flyout
- [ ] Caption buttons drawn by the window manager over the reserved trailing inset, so they are identical in every window; the maximize button's hover opens the snap layout flyout
- [ ] Active and inactive appearance, the backdrop showing through, and the accent-on-title-bars option
- [ ] Classic windows get the same bar from the window manager with icon and title filled from the window, so no window has a different title bar
- [ ] The window manager decorations of `TODO-08 §2` render through this control's template
- [ ] Commit: `"controls: title bar"`

**Test checkpoint:** `test_ctl_titlebar` passes: a press on the title text starts a window move while a press on the search box does not, the two heights are the only values the property accepts, and a classic window and a markup window report equal caption button rectangles. `check-control.py title-bar` reports 7 of 7.

## 23. Media Transport Controls

**Design:** [`controls.md#slider`](../../docs/design/controls.md#slider)

- [ ] Add the media transport entry to `controls.md` from the inventory's measurements before building
- [ ] The control row: play or pause, stop, previous, next, skip back and forward, the seek slider with elapsed and remaining time, volume with mute, and full screen
- [ ] The compact form (one row) and the overflow menu for controls that do not fit: playback speed, audio track, subtitles, repeat, cast
- [ ] Seek slider extras: buffered range, a thumbnail or time tooltip while hovering, and keyboard seeking by small and large steps
- [ ] Auto-hide after pointer inactivity during playback, shown again on movement, key or touch
- [ ] Bound to a media session interface, so the video player, the media player and the quick settings media card drive one implementation
- [ ] Media keys and the system media session: play, pause, next and previous from the keyboard reach the active session
- [ ] Commit: `"controls: media transport controls"`

**Test checkpoint:** `test_ctl_transport` passes: Space toggles play and pause on the bound session, narrowing the row moves secondary controls to the overflow menu, and seeking by keyboard changes the session position by the small step. `check-control.py media-transport` reports 7 of 7.

## 24. Classic Control Parity

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

Old-style programs get the same controls as new ones, so a property sheet and a Settings page look alike.

> [!TIP]
> In Windows 11 a classic dialog keeps its older buttons, scroll bars, fonts and light theme. Here each classic control class is a thin adapter over the modern control.

- [ ] Write the class map in `docs/design/classic-map.md`: each classic window class of the `classic-controls` baseline, the modern control that renders it, and the style and notification differences the adapter covers
- [ ] Adapters for the standard classes: button (push, check, radio, group box), edit, static, list box, combo box, scroll bar
- [ ] Adapters for the common control classes with a modern equivalent: list view, tree view, tab control, tool bar, status bar, progress bar, track bar, tooltips, header, date and time picker, month calendar, link
- [ ] The classes with no modern control are built here on the engine: up-down (spin), IP address field, hot key field, property sheet and wizard frame, rebar, pager, animation control
- [ ] Property sheet frame: tabs across the top, the page area, and the OK, Cancel and Apply row in the one dialog frame; wizards with Back, Next and Finish
- [ ] Classic windows follow the theme: dark mode, accent and contrast apply to every adapter with no opt-in by the program
- [ ] Record each adapter against its export rows in the common controls export table of `D10 TB`, so the API surface and the renderer stay in step
- [ ] Commit: `"controls: classic control classes rendered by the modern controls"`

**Test checkpoint:** `test_ctl_classic` passes: a classic push button and a markup button of the same size produce equal pixels in light and in dark, a property sheet with three pages switches pages with Ctrl-Tab, and every class in `classic-map.md` resolves to a registered element type. `check-control.py --all` exits 0.

## 25. Controls Gallery App

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout)

An app that shows every control, for people to browse and for tests to read.

- [ ] `src/apps/gallery/`: a navigation view with one page per control, grouped by the families of the Control Quick Reference
- [ ] Each page shows the control in every variant, with switches for disabled, right-to-left, compact density, and the light, dark and contrast themes
- [ ] A state strip per control: the control frozen in each visual state (rest, hover, pressed, focused, disabled, selected), for capture without injected input
- [ ] Each page shows the markup that produced it, read from the compiled page's source
- [ ] A deep link per page and per state (`gallery:<control>/<state>`), used by the conformance suite and the visual regression tests
- [ ] The gallery is the fixture for `TODO-17 §51`: its pages carry inventory ids so the layout check has a surface from the first control onward
- [ ] Retire `src/desktop/gallery.c` once its content is covered, or record in this section why it stays
- [ ] Commit: `"apps: controls gallery"`

**Test checkpoint:** booting with the gallery as the startup app shows the button page; `gallery:button/pressed` opens the button page with the pressed strip scrolled into view; the serial log shows `gallery: N control pages`. Test on: QEMU TCG and KVM.

## 26. State Matrix Conformance Suite

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

Every control, in every state and theme, compared with the measured reference. A failing cell is a named, reproducible defect.

- [ ] `scripts/ui/state-matrix.py`: for each control, state, theme (light, dark, contrast) and scale (100, 150), opens the gallery deep link under the headless compositor and captures the control's rectangle and its automation node
- [ ] Structural comparison against the inventory's state records: size, corner radius, border, and the fill, border and text colour roles, within the tolerances of the layout check
- [ ] Contrast comparison: text against its fill meets 4.5 to 1 (3 to 1 for large text and non-text parts) in every cell, reusing the checker of `D00 T05 §14`
- [ ] Keyboard walk: each gallery page is traversed by Tab and arrows, and every interactive element must receive focus and show the focus visual
- [ ] Output `build/ui/state-matrix.json` and a one-line summary `matrix: N cells, M differ`, with the first 20 differing cells named as control, state, theme and scale
- [ ] The suite joins the tooling pack as a nested suite, and a new control without a matrix row fails `check-control.py`
- [ ] Commit: `"test: control state matrix conformance suite"`

**Test checkpoint:** `python3 scripts/ui/state-matrix.py --control button` prints `matrix: 42 cells, 0 differ` (7 states by 3 themes by 2 scales) and exits 0; changing the button's corner radius resource by 1 makes it exit 1 naming every button cell. Test on: QEMU TCG with the headless compositor.

---

## OS Comparison

| ⭐  | Feature                                 | 🪟 Win11                   | 🐧 Linux                 | 🚀 Impossible OS      |
| --- | --------------------------------------- | -------------------------- | ------------------------ | --------------------- |
| 💎  | Modern control set                      | ✅ WinUI controls          | ✅ GTK 4, libadwaita, Qt | ⬜ Planned -- §2-§23  |
| 💎  | Navigation view, tab view, command bar  | ✅ Present                 | ✅ Present               | ⬜ Planned -- §13-§17 |
| ⭐  | One dialog frame system-wide            | ❌ Three generations       | ⚠️ Per toolkit           | ⬜ Planned -- §14     |
| ⭐  | One title bar for every window          | ❌ Four heights            | ⚠️ Client or server side | ⬜ Planned -- §22     |
| ⭐  | Classic controls render as modern       | ❌ Old look, often no dark | ⚠️ Theme dependent       | ⬜ Planned -- §24     |
| ⭐  | One contract checked for every control  | ❌ None published          | ❌ None                  | ⬜ Planned -- §1      |
| ⭐  | Every state checked against a reference | ❌ None                    | ❌ None                  | ⬜ Planned -- §26     |

> **After §1-§11:** forms can be built: buttons, switches, text and number entry, pickers and status. **After §12-§23:** whole app frames can be built: menus, command bars, dialogs, navigation, tabs, settings pages and collections. **After §24-§26:** classic programs look the same as new ones, and every control is proven against the Windows 11 measurements in every state.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_controls()` in `src/kernel/test/test_runner.c`, category `TEST_CAT_DESKTOP`. Each control's tests build the control in an in-memory tree and inject input; they never call live desktop init.

- [ ] Create `src/kernel/test/test_controls.c` with one group per control, each asserting the cases named in that section's test checkpoint:
  - `test_ctl_button`: Space release invokes once; a release outside invokes nothing
  - `test_ctl_toggle`: a 60 percent drag toggles and a 40 percent drag does not
  - `test_ctl_textbox`: a 5 character limit refuses the sixth; the password peer exposes no value
  - `test_ctl_numberbox`: `2+3*4` commits as 14; `abc` restores the last value
  - `test_ctl_menu`: Left closes exactly one submenu level
  - `test_ctl_dialog`: Enter returns the default button and Esc returns close
  - `test_ctl_tabview`: closing the selected tab selects the right neighbour
- [ ] Register in `test_runner_init()`: `test_register_controls()`
- [ ] Author `scripts/debug/desktop/run-controls-tests.bat`
- [ ] Host tests for `scripts/ui/check-control.py` and `scripts/ui/state-matrix.py` under `scripts/ui/tests/`, wired into `scripts/test-tooling.sh`
- [ ] Commit: `"test: control library suite"`

## Verification

- [ ] `bash scripts/build.sh clean` then `tail -1 build/build.log` shows `=== BUILD OK ===`
- [ ] `bash scripts/test.sh SUITE=desktop` passes with `test_controls` listed
- [ ] `python3 scripts/ui/check-control.py --all` exits 0 and lists every control of the inventory's `controls` baseline as built or excluded
- [ ] `python3 scripts/ui/state-matrix.py` prints `matrix: N cells, 0 differ`
- [ ] `bash scripts/test-smoke-matrix.sh` passes with the gallery present; the serial log contains `gallery: N control pages`
- [ ] QEMU WHPX at 2 CPUs and VirtualBox: the gallery opens every page with no fault and no missing-resource warning in the serial log
- [ ] Bare metal: touch targets, press-and-hold and the looping selectors are checked on a touch display (VM pointer input differs)
