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
- -> XREF: `TODO-17-win11-parity-inventory.md §5,§14` -- the `controls` baseline this library is complete against, and the measured state matrix each template implements
- -> XREF: `TODO-05-widget-library-core.md §1,§2,§3,§4,§5,§6,§9` -- check box, radio, slider, progress bar, combo box, tab strip and overlay scroll bar stay owned there and adopt the contract of §1
- -> XREF: `TODO-06-widget-dialogs.md §1,§2,§3,§4,§5,§7,§8,§9` -- list view, tree view, toolbar, menu bar, status bar, tooltip, dialog system and info bar stay owned there and adopt the contract of §1
- -> XREF: `TODO-02-text-font-internationalization.md §4` -- caret, selection and composition services the text controls of §4 to §7 use
- -> XREF: `D10 TB §2` -- the classic common control classes §24 maps onto these controls
- -> XREF: `D11 T05 §15` -- Recto, the bundled PDF app, uses the tab view of §17 and the title bar of §22

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

| ⭐  | Order | Deliverable                                       | Depends On                                      | Status |
| --- | :---: | ------------------------------------------------- | ----------------------------------------------- | :----: |
| ⭐  |   1   | §1 Control authoring contract                     | §27, T18 §1,§5,§7,§11,§24, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   2   | §2 Button family                                  | §27, §1, T18 §9, `11-apps/TODO-15` §1           |  [ ]   |
| 💎  |   3   | §3 Toggle switch                                  | §27, §1, `11-apps/TODO-15` §1                   |  [ ]   |
| 💎  |   4   | §4 Text box family                                | §27, §1, T02 §4, `11-apps/TODO-15` §1           |  [ ]   |
| 💎  |   5   | §5 Number box                                     | §27, §4, `11-apps/TODO-15` §1                   |  [ ]   |
| 💎  |   6   | §6 Auto-suggest box                               | §27, §4, T18 §14, `11-apps/TODO-15` §1          |  [ ]   |
| 💎  |   7   | §7 Rich text display and hyperlink                | §27, §1, T02 §3, `11-apps/TODO-15` §1           |  [ ]   |
| 💎  |   8   | §8 Date, time and calendar pickers                | §27, §1, T18 §14, `11-apps/TODO-15` §1          |  [ ]   |
| 💎  |   9   | §9 Colour picker                                  | §27, §1, §5, `11-apps/TODO-15` §1               |  [ ]   |
| 💎  |  10   | §10 Progress ring                                 | §27, §1, `11-apps/TODO-15` §1                   |  [ ]   |
| 💎  |  11   | §11 Rating, person picture and info badge         | §27, §1, `11-apps/TODO-15` §1                   |  [ ]   |
| 💎  |  12   | §12 Flyout and menu flyout                        | §27, §1, T18 §9,§14, `11-apps/TODO-15` §1       |  [ ]   |
| 💎  |  13   | §13 Command bar and command bar flyout            | §27, §2, §12, `11-apps/TODO-15` §1              |  [ ]   |
| ⭐  |  14   | §14 Content dialog and teaching tip               | §27, §2, T18 §14, `11-apps/TODO-15` §1          |  [ ]   |
| 💎  |  15   | §15 Navigation view                               | §27, §1, T18 §3,§15, `11-apps/TODO-15` §1       |  [ ]   |
| 💎  |  16   | §16 Breadcrumb bar                                | §27, §12, T18 §15, `11-apps/TODO-15` §1         |  [ ]   |
| 💎  |  17   | §17 Tab view                                      | §27, §2, T18 §8, `11-apps/TODO-15` §1           |  [ ]   |
| 💎  |  18   | §18 Expander, settings card and settings expander | §27, §1, §3, `11-apps/TODO-15` §1               |  [ ]   |
| 💎  |  19   | §19 Grid view, items view and flip view           | §27, §1, T18 §4,§10, `11-apps/TODO-15` §1       |  [ ]   |
| 💎  |  20   | §20 Split view and two-pane view                  | §27, §1, T18 §3, `11-apps/TODO-15` §1           |  [ ]   |
| 💎  |  21   | §21 Selector bar and segmented control            | §27, §1, `11-apps/TODO-15` §1                   |  [ ]   |
| ⭐  |  22   | §22 Title bar control                             | §27, §2, §6, `11-apps/TODO-15` §1               |  [ ]   |
| 💎  |  23   | §23 Media transport controls                      | §27, §2, T18 §12, `11-apps/TODO-15` §1          |  [ ]   |
| ⭐  |  24   | §24 Classic control parity                        | §27, §2, §4, §8, §12, `11-apps/TODO-15` §1      |  [ ]   |
| ⭐  |  25   | §25 Controls gallery app                          | §27, §2, §4, §12, §15, `11-apps/TODO-15` §1     |  [ ]   |
| ⭐  |  26   | §26 State matrix conformance suite                | §27, §25, T17 §14, `11-apps/TODO-15` §1         |  [ ]   |
| 💎  |  27   | §27 Impossible UI on the engines                  | T18 §1,§2,§5,§7,§24, `11-apps/TODO-15` §1       |  [ ]   |

> 💎 = parity work: the control exists in Windows 11 and in the Linux toolkits.
> ⭐ = exclusive work: one contract for every control, one dialog frame, classic and modern controls sharing one look, and a conformance suite measured against a reference.

---

## 1. Control Authoring Contract

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Control Authoring Contract"`

**Test checkpoint:** `python3 scripts/ui/check-control.py _template` prints `control _template: 7 of 7 points` and exits 0; removing the gallery stub makes it exit 1 naming `gallery page`. `bash scripts/lint.sh` passes.

## 2. Button Family

**Design:** [`controls.md#button`](../../docs/design/controls.md#button)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Button Family"`

**Test checkpoint:** `test_ctl_button` passes: Space press then release invokes once, a pointer release outside the button invokes nothing, a repeat button held 1 second fires more than twice, and the split button's chevron opens its flyout without invoking the primary command. `check-control.py button` reports 7 of 7.

## 3. Toggle Switch

**Design:** [`controls.md#toggle-switch`](../../docs/design/controls.md#toggle-switch)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Toggle Switch"`

**Test checkpoint:** `test_ctl_toggle` passes: Space toggles, a drag of 60 percent of the track toggles and a drag of 40 percent does not, and the peer reports the toggle action and the on state. `check-control.py toggle-switch` reports 7 of 7.

## 4. Text Box Family

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Text Box Family"`

**Test checkpoint:** `test_ctl_textbox` passes: typing, Shift-arrow selection and Ctrl-X then Ctrl-V round-trip the text; a 5 character limit refuses the sixth; the password box's peer never exposes its value and copy is disabled. `check-control.py text-box` reports 7 of 7.

## 5. Number Box

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Number Box"`

**Test checkpoint:** `test_ctl_numberbox` passes: Up from 9 with maximum 10 gives 10 and a second Up stays at 10, `2+3*4` commits as 14, and `abc` restores the previous value with the invalid state set. `check-control.py number-box` reports 7 of 7.

## 6. Auto-Suggest Box

**Design:** [`controls.md#text-box-password-box-and-search-box`](../../docs/design/controls.md#text-box-password-box-and-search-box), [`controls.md#combo-box-and-drop-down`](../../docs/design/controls.md#combo-box-and-drop-down)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Auto-Suggest Box"`

**Test checkpoint:** `test_ctl_autosuggest` passes: typing opens the list, Down then Enter raises suggestion-chosen then query-submitted once each, and a programmatic text change opens nothing. `check-control.py auto-suggest-box` reports 7 of 7.

## 7. Rich Text Display and Hyperlink

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Rich Text Display and Hyperlink"`

**Test checkpoint:** `test_ctl_textblock` passes: a long string in a 100 unit block with a two-line maximum ends in an ellipsis and reports trimmed, Tab reaches an inline hyperlink and Enter activates it, and selection plus copy yields the plain text. `check-control.py text-block` reports 7 of 7.

## 8. Date, Time and Calendar Pickers

**Design:** [`controls.md#date-picker`](../../docs/design/controls.md#date-picker)

**Inventory:** `win11.flyouts.calendar-year`, `win11.flyouts.calendar-decade`, `win11.flyouts.notifications.calendar-zoom`, `win11.flyouts.notifications.calendar-year-columns`, `win11.flyouts.notifications.calendar-year-rows`, `win11.flyouts.notifications.calendar-year-item`, `win11.flyouts.notifications.calendar-year-pitch`, `win11.flyouts.notifications.calendar-decade-columns`, `win11.flyouts.notifications.calendar-decade-rows`, `win11.flyouts.notifications.calendar-decade-item`, `win11.flyouts.notifications.calendar-decade-pitch`

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Date, Time and Calendar Pickers"`

**Test checkpoint:** `test_ctl_calendar` passes: Right from the 31st moves to the 1st of the next month and changes the displayed month, a date before the minimum cannot be selected, and the time picker in a 24 hour locale has no day period column. `check-control.py calendar-view` reports 7 of 7.

## 9. Colour Picker

**Design:** [`controls.md#slider`](../../docs/design/controls.md#slider)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Colour Picker"`

**Test checkpoint:** `test_ctl_colour` passes: converting 4,096 sampled colours RGB to HSV and back changes no channel by more than 1, typing a hex value moves the selector, and the peer's value is a colour name. `check-control.py colour-picker` reports 7 of 7.

## 10. Progress Ring

**Design:** [`controls.md#progress`](../../docs/design/controls.md#progress)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Progress Ring"`

**Test checkpoint:** `test_ctl_progressring` passes: at 25 percent the arc spans 90 degrees, an inactive ring schedules no animation ticks, and the peer reports the range value. `check-control.py progress-ring` reports 7 of 7.

## 11. Rating, Person Picture and Info Badge

**Design:** [`controls.md#status-colours`](../../docs/design/controls.md#status-colours)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Rating, Person Picture and Info Badge"`

**Test checkpoint:** `test_ctl_badges` passes: Right three times on an empty rating gives 3, a display name of two words yields two initials, and a badge value of 120 displays the capped form. `check-control.py rating` reports 7 of 7.

## 12. Flyout and Menu Flyout

**Design:** [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus), [`shell.md#context-menus`](../../docs/design/shell.md#context-menus)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Flyout and Menu Flyout"`

**Test checkpoint:** `test_ctl_menu` passes: a three-level menu opens with Right and closes one level per Left, an access key invokes its item, a disabled command's item is skipped by arrow keys, and Shift-F10 raises one context request. `check-control.py menu-flyout` reports 7 of 7.

## 13. Command Bar and Command Bar Flyout

**Design:** [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus), [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Command Bar and Command Bar Flyout"`

**Test checkpoint:** `test_ctl_commandbar` passes: narrowing the bar from 600 to 200 units moves commands to overflow in declared order with none lost, widening restores them, and a command disabled in the group is disabled in both places. `check-control.py command-bar` reports 7 of 7.

## 14. Content Dialog and Teaching Tip

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Content Dialog and Teaching Tip"`

**Test checkpoint:** `test_ctl_dialog` passes: Enter returns the default button, Esc returns close, Tab cycles only inside the dialog, focus returns to the opener, and a message box request for each of the seven button sets renders in the one frame. `check-control.py content-dialog` reports 7 of 7.

## 15. Navigation View

**Design:** [`shell.md#settings-and-control-panel-frame`](../../docs/design/shell.md#settings-and-control-panel-frame)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Navigation View"`

**Test checkpoint:** `test_ctl_navview` passes: at a width below the first breakpoint the pane is minimal and at a width above the second it is expanded, selecting an item navigates the frame once, and back restores the previous selection. `check-control.py navigation-view` reports 7 of 7.

## 16. Breadcrumb Bar

**Design:** [`shell.md#file-explorer`](../../docs/design/shell.md#file-explorer)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Breadcrumb Bar"`

**Test checkpoint:** `test_ctl_breadcrumb` passes: six items in a width that fits three show the ellipsis plus the last three, the ellipsis menu lists the hidden items in order, and clicking the second item raises item-clicked with index 1. `check-control.py breadcrumb-bar` reports 7 of 7.

## 17. Tab View

**Design:** [`controls.md#tabs`](../../docs/design/controls.md#tabs)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Tab View"`

**Test checkpoint:** `test_ctl_tabview` passes: closing the selected tab selects its right neighbour (or the left when it was last), dragging tab 1 past tab 3 reorders the bound list, and Ctrl-Tab wraps from the last tab to the first. `check-control.py tab-view` reports 7 of 7.

## 18. Expander, Settings Card and Settings Expander

**Design:** [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Expander, Settings Card and Settings Expander"`

**Test checkpoint:** `test_ctl_settingscard` passes: a toggle inside a card takes the card's title as its automation name, the expander's content is absent from the tab order while collapsed, and at the narrow width the control slot arranges below the description. `check-control.py settings-card` reports 7 of 7.

## 19. Grid View, Items View and Flip View

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Grid View, Items View and Flip View"`

**Test checkpoint:** `test_ctl_gridview` passes: Shift-click selects a contiguous range across rows, a 50,000 item grid view holds fewer than 200 live containers, and the flip view's Right key advances one item and stops at the last. `check-control.py grid-view` reports 7 of 7.

## 20. Split View and Two-Pane View

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Split View and Two-Pane View"`

**Test checkpoint:** `test_ctl_splitview` passes: in overlay mode a click on the content closes the pane, in inline mode it does not, and the two-pane view shows only the priority pane below its breakpoint. `check-control.py split-view` reports 7 of 7.

## 21. Selector Bar and Segmented Control

**Design:** [`controls.md#tabs`](../../docs/design/controls.md#tabs)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Selector Bar and Segmented Control"`

**Test checkpoint:** `test_ctl_selector` passes: Right moves selection and raises selection-changed once, the bar is one tab stop, and the peer of the selected item reports selected. `check-control.py selector-bar` reports 7 of 7.

## 22. Title Bar Control

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Title Bar Control"`

**Test checkpoint:** `test_ctl_titlebar` passes: a press on the title text starts a window move while a press on the search box does not, the two heights are the only values the property accepts, and a classic window and a markup window report equal caption button rectangles. `check-control.py title-bar` reports 7 of 7.

## 23. Media Transport Controls

**Design:** [`controls.md#slider`](../../docs/design/controls.md#slider)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Media Transport Controls"`

**Test checkpoint:** `test_ctl_transport` passes: Space toggles play and pause on the bound session, narrowing the row moves secondary controls to the overflow menu, and seeking by keyboard changes the session position by the small step. `check-control.py media-transport` reports 7 of 7.

## 24. Classic Control Parity

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Classic Control Parity"`

**Test checkpoint:** `test_ctl_classic` passes: a classic push button and a markup button of the same size produce equal pixels in light and in dark, a property sheet with three pages switches pages with Ctrl-Tab, and every class in `classic-map.md` resolves to a registered element type. `check-control.py --all` exits 0.

## 25. Controls Gallery App

**Design:** [`shell.md#app-window-layout`](../../docs/design/shell.md#app-window-layout)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Controls Gallery App"`

**Test checkpoint:** booting with the gallery as the startup app shows the button page; `gallery:button/pressed` opens the button page with the pressed strip scrolled into view; the serial log shows `gallery: N control pages`. Test on: QEMU TCG and KVM.

## 26. State Matrix Conformance Suite

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings State Matrix Conformance Suite"`

**Test checkpoint:** `python3 scripts/ui/state-matrix.py --control button` prints `matrix: 42 cells, 0 differ` (7 states by 3 themes by 2 scales) and exits 0; changing the button's corner radius resource by 1 makes it exit 1 naming every button cell. Test on: QEMU TCG with the headless compositor.

## 27. Impossible UI on the Engines

> **Spawned-by:** root

**Design:** n/a -- connects imported code, it designs nothing

Impossible UI holds the controls of Impossible SDK, drawn from the values of WinUI and proved on Windows against the real one and against the inventory of `TODO-17`. The engines stay here in `TODO-18`. It arrives with `11-apps/TODO-15` §1, which is pulled again whenever a section below it needs more of it, and each of those sections then shows its own behaviour on the copy.

- [ ] Build it with the flags of this kernel and of user programs, and run its tests here
- [ ] Run its controls on the engines of `TODO-18`: element tree, layout, states, input, popups and automation; what a control needs of an engine and the engine lacks becomes an item of that engine section
- [ ] Read the inventory ids it gives each element through the transport of `TODO-07 §6`, so that `TODO-17 §16` can compare
- [ ] Keep what Impossible SDK builds from in step with this repository: the inventory's control records, state and metric catalogues (`docs/design/inventory/`, `TODO-17 §14`), `controls.md` and `tokens.json`, which it copies at a pinned commit
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose impossible-sdk`
- [ ] Commit: `"ui: Impossible UI runs on the engines"`

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
