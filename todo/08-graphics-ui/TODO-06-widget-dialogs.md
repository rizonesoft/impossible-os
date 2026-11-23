---
schema_version: 1
id: widget-dialogs
domain: 08-graphics-ui
status: active
title: "TODO-06 -- Extended Widget Library: Complex Controls & Dialogs"
---

# TODO-06 -- Extended Widget Library: Complex Controls & Dialogs

> **Goal:** Add the 7 complex controls and the complete dialog system needed to build the File Manager, Notepad, Registry Editor, and Control Panel: ListView (details + icon-grid), TreeView (hierarchical expand/collapse), Toolbar (icon buttons + overflow), MenuBar (horizontal menu + popup dropdowns), StatusBar (multi-pane), GroupBox + Separator (visual grouping), Tooltip (hover popup + animation), and the full dialog system (Win32-compatible `MessageBox`, file Open/Save, input, color picker, `SYS_MSGBOX` syscall).

> [!IMPORTANT]
> `CTRL_MAX_PER_WINDOW = 32` means each control counts against the per-window slot budget -- ListView and TreeView must store row/node data **outside** the `struct control` union. Use `pmm_alloc_contiguous()` for data arrays > 4 KB (e.g., more than ~170 rows @ 24 bytes each). Each control stores only a pointer + metadata in the union; the data buffer is freed in a new `ctrl_destroy(wh, id)` destructor. Dropdown floating popup mechanism (z_order=9999, borderless `wm_create_window`) is established in TODO-05 §5 -- use the same pattern for Toolbar overflow, MenuBar dropdowns, and Tooltip popups. `anim_mgr_add()` from TODO-04 drives the Tooltip fade-in. All colors via `theme_get()` -- no hardcoded hex. `tools/convert_icon.py` exists and handles the icon-to-C-array conversion for msgbox icons. The legacy SYS_ table uses 1-26 and 33-48 today (`include/kernel/sched/syscall.h`), and `07-networking/TODO-02-dns-sockets.md` §7 will take the next 12 numbers for sockets, so `SYS_MSGBOX` takes the next free number at implementation time, not a fixed one (allocation policy: `02-kernel-core/TODO-12-native-api-ssdt.md` §33). Complete sections in order: GroupBox → StatusBar → Toolbar → Tooltip → ListView → TreeView → MenuBar → Dialog system.

## Inputs

- `include/desktop/controls.h` -- extend `enum ctrl_type` with 7 new values; add `ctrl_destroy(wh, id)` destructor; `CTRL_MAX_PER_WINDOW = 32` applies
- `src/desktop/controls.c` -- extend `ctrl_draw_all/handle_mouse/handle_key` dispatch tables
- `include/gfx.h` -- `gfx_fill_rounded_rect`, `gfx_fill_circle`, `gfx_fill_rect_alpha`, `gfx_drop_shadow` for new control rendering
- `include/desktop/theme.h` (TODO-03) -- `theme_get()` for all colors; `accent_hover`, `surface_variant`, `border`, `shadow`
- `include/kernel/gfx/anim_mgr.h` (TODO-04) -- `anim_mgr_add()` + `gfx_ease_decelerate` for Tooltip fade-in tween
- `include/desktop/wm.h` -- `wm_create_window()` + `z_order` overlay for Toolbar overflow, MenuBar dropdowns, Tooltip; `wm_create_window()` for modal dialog
- `include/kernel/sched/syscall.h` -- extend with `SYS_MSGBOX` (next free number) for user-mode MessageBox access
- `scripts/convert-icons.sh` + `resources/icons/src/` -- message box icons join the original icon set (`docs/design/icons.md`)
- → XREF: `08-graphics-ui/TODO-05-widget-library-core.md` -- `CTRL_DROPDOWN` overlay pattern + `CTRL_SCROLLBAR` used by ListView/TreeView; must be complete before this TODO starts
- Related (no stable XREF target): `08-graphics-ui/TODO-09` §1 (context menu engine) -- MenuBar popup dropdown may be refactored to share context menu engine once it exists; this TODO implements a self-contained popup
- -> XREF: `TODO-19-fluent-component-library.md §1,§12,§13,§14` -- authoring contract, and the menu flyout, app bar button and dialog frame these controls share

## Outcome

- `CTRL_LISTVIEW/TREEVIEW/TOOLBAR/MENUBAR/STATUSBAR/GROUPBOX/SEPARATOR/TOOLTIP` in `enum ctrl_type`.
- ListView in details mode: sortable columns, multi-select; icon-grid mode with 32/48 px icons.
- TreeView: expand/collapse with indented nodes, scrollable, keyboard navigation.
- Toolbar: flat icon buttons, toggle state, separator, overflow `>>` chevron.
- MenuBar: top-level menus, popup dropdown, keyboard Alt-navigation, accelerators.
- StatusBar: stretchy + fixed-width panes, text + icon per pane.
- Tooltip per `docs/design/controls.md#tooltip`: appears after `THEME_MOTION_TOOLTIP_DELAY_MS` (400) of hover; applied to all chrome buttons.
- `MessageBox()` Win32-compatible (exact `MB_*`/`ID*` constants), modal overlay, embedded icons.
- `dialog_file_open/save`, `dialog_input`, `dialog_color` dialog implementations.
- `SYS_MSGBOX` (next free legacy number); `msgbox` shell command.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                | Depends On                                                                                   | Status |
| --- | :---: | ------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------- | :----: |
| 💎  |   6   | §6 GroupBox + Separator -- visual-only border + line controls; zero interaction            | T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   5   | §5 StatusBar -- multi-pane bottom bar; text + icon per pane                                | §6 (GroupBox establishes the visual-only control extension pattern), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   3   | §3 Toolbar -- flat icon buttons, toggle state, separator, overflow `>>` chevron popup      | TODO-05 overlay pattern (z_order=9999 from dropdown) re-used for chevron, T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   7   | §7 Tooltip -- 400 ms hover delay, themed layer popup, applied to all chrome                | §3 toolbar (toolbar buttons are first tooltip recipients); TODO-04 `anim_mgr_add()`, T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   1   | §1 ListView -- details mode (columns + sort + multi-select) + icon-grid mode + scrollbar   | §7 tooltip (list items receive tooltips); external data buffer pattern, T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   2   | §2 TreeView -- hierarchical nodes, expand/collapse, indent, scrollbar, keyboard nav        | §1 ListView (same external data + scrollbar integration pattern), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   4   | §4 MenuBar -- horizontal menu bar, popup dropdown, Alt-navigation, accelerators            | §3 toolbar (popup uses same z_order overlay); TODO-05 dropdown pattern, T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   8   | §8 Dialog system -- `MessageBox`, file Open/Save, input dialog, color picker, `SYS_MSGBOX` | §1+§2 (file dialog uses ListView + TreeView); §3 (nav toolbar), T19 §27, `11-apps/TODO-15` §1 |  [ ]   |
| 💎  |   9   | §9 Info bar control -- severity strip with glyph, action, close                            | §8, T19 §27, `11-apps/TODO-15` §1 |  [ ]   |

---

## 1. ListView

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings ListView"`

## 2. TreeView

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings TreeView"`

## 3. Toolbar

**Design:** [`controls.md#button`](../../docs/design/controls.md#button), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Toolbar"`

## 4. MenuBar

**Design:** [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings MenuBar"`

## 5. StatusBar

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings StatusBar"`

## 6. GroupBox + Separator

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings GroupBox + Separator"`

## 7. Tooltip Integration

**Design:** [`controls.md#tooltip`](../../docs/design/controls.md#tooltip)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] Apply to WM buttons: in `wm.c` window creation: `tooltip_register(close_btn_id, "Close")`, `"Minimize"`, `"Maximize"`
- [ ] `void tooltip_init(void)` called from `desktop_init()`; `void tooltip_shutdown(void)` (no-op for now)
- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Tooltip Integration"`

## 8. Dialog System

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] `#define SYS_MSGBOX` at the next free number in `include/kernel/sched/syscall.h`; `sys_msgbox()` handler in `syscall.c` dispatches to `MessageBox()` on desktop thread (post to desktop event queue)
- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Dialog System"`

---

## 9. Info Bar Control

> **Spawned-by:** root

**Design:** [`controls.md#info-bar`](../../docs/design/controls.md#info-bar), [`controls.md#status-colours`](../../docs/design/controls.md#status-colours)

**Moved:** impossible-sdk

Built in Impossible SDK, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until `TODO-19` §27 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as `TODO-19` §27 brings it
- [ ] Commit: `"imports: impossible-sdk brings Info Bar Control"`

**Test checkpoint:** the controls gallery shows all four severities in light and dark with correct glyphs and fills; a long message wraps and grows the bar; close removes it and the owner receives the notification. Test on: QEMU TCG, VirtualBox.

## OS Comparison


| ⭐  | Feature                                          | 🪟 Win11                                                        | 🐧 Linux                                                       | 🚀 Impossible OS                                                           |
| --- | ------------------------------------------------ | --------------------------------------------------------------- | -------------------------------------------------------------- | -------------------------------------------------------------------------- |
| 💎  | ListView                                         | ✅ WinUI3 `ListView`/`GridView`; Win32 `LVM_SORTITEMS`; virtual | ✅ GTK `GtkTreeView`/`GtkIconView`; Qt `QListView`; virtual    | ⬜ §1 -- DFS-rebuilt visible list; shell-sort; `pmm_alloc_contiguous`      |
| 💎  | TreeView                                         | ✅ WinUI3 `TreeView`; Win32 `WC_TREEVIEW`; `TVN_ITEMEXPANDING`  | ✅ GTK `GtkTreeView` with `GtkTreeStore`; Qt                   | ⬜ §2 -- 512-node `pmm_alloc_contiguous` slab; DFS visible-list            |
| 💎  | Toolbar                                          | ✅ Win32 `WC_TOOLBAR`; WinUI3 `CommandBar`; overflow            | ✅ GTK `GtkToolbar`; Qt `QToolBar`; overflow                   | ⬜ §3 -- z_order=9999 overlay for overflow popup                           |
| 💎  | MenuBar                                          | ✅ Win32 `HMENU`; WinUI3 `MenuBar`; full                        | ✅ GTK `GtkMenuBar`; Qt `QMenuBar`; Alt-key                    | ⬜ §4 -- self-contained popup (z_order overlay); TODO-07                   |
| 💎  | StatusBar -- stretchy + fixed panes, text + icon | ✅ Win32 `WC_STATUSBAR`; `SB_SETTEXT`; multiple parts           | ✅ GTK `GtkStatusbar`; Qt `QStatusBar`; permanent              | ⬜ §5 -- 8 panes; `width=0` = stretchy                                     |
| 💎  | GroupBox + Separator                             | ✅ Win32 `BS_GROUPBOX`; `WS_GROUP` frame; `SS_ETCHEDHORZ`       | ✅ GTK `GtkFrame`; Qt `QGroupBox`; `QFrame`                    | ⬜ §6 -- card with heading (`controls.md#cards-and-settings-rows`)         |
| 💎  | Tooltip                                          | ✅ WinUI3 `ToolTipService`; Win32 `WC_TOOLTIP`; 500             | ✅ GTK `gtk_widget_set_tooltip_text`; Qt `setToolTip`; default | ⬜ §7 -- global `tooltip_tick()` in compositor; 400 ms delay, themed layer |
| 💎  | MessageBox                                       | ✅ `MessageBoxW` exact same constants; modal                    | ✅ GTK `gtk_message_dialog_new`; Qt `QMessageBox`; parent      | ⬜ §8 -- exact Win32 MB_*/ID* values; `smoke` scrim                        |
| ⭐  | Color picker                                     | ✅ Windows color dialog; Settings accent                        | ✅ GTK `GtkColorChooserDialog`; Qt `QColorDialog`; hue         | ⬜ §8 -- `⭐` hue wheel rasterized at                                      |
| 💎  | File Open/Save dialog                            | ✅ `GetOpenFileNameW`; IFileOpenDialog Shell API; breadcrumb    | ✅ GTK `GtkFileChooserDialog`; Qt `QFileDialog`; bookmarks     | ⬜ §8 -- TreeView left + ListView right                                    |
| ⭐  | `SYS_MSGBOX`                                     | ✅ User-mode `MessageBoxW` via `user32.dll`; `msg`              | ❌ No kernel-level MessageBox syscall; all                     | ⬜ §8 -- `⭐` kernel-dispatched MessageBox via syscall                     |

> **After §1–§8:** Impossible OS has a production-quality widget library and dialog system entirely in kernel-native code. The `⭐` differentiators are: (1) `dialog_color()` rasterizes the hue wheel at init time -- no external color picker library needed; (2) `SYS_MSGBOX` (next free number) lets any user-mode program pop a MessageBox with a single syscall -- a pattern Linux has no equivalent for (GTK/Qt are userspace-only).

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] ListView details: `ctrl_listview_add_column` + 20 items; click column header → sorts; Shift-click selects range; icon-grid mode shows icons + labels in grid
- [ ] TreeView: 3-level hierarchy; click ▸ → expands; child nodes indented; Up/Down keys navigate; double-click calls `on_activate`
- [ ] Toolbar: 5 buttons; button 3 is toggle; click toggles accent background; overflow `>>` appears when window narrow; chevron click shows overflow popup
- [ ] MenuBar: File menu with 3 items + separator + accelerator text; Alt key activates; underlines appear; arrow keys navigate; Escape closes; item click calls `cb`
- [ ] StatusBar: 3 panes (stretchy + 2 fixed); `ctrl_statusbar_set_pane_text(…, 0, "3 items")` updates first pane in QEMU
- [ ] GroupBox: a card (`card_bg`, 1 px `stroke_card`, radius 8) headed "General" in the body strong style; Separator: 1 px horizontal line visible
- [ ] Tooltip: hover over close button 400 ms → themed tooltip appears below the pointer with "Close"; mouse move → popup disappears; appears again on next hover
- [ ] MessageBox: `MessageBox(-1, "Hello", "Test", MB_YESNO | MB_ICONQUESTION)` → centered dialog with question icon, Yes/No buttons; Enter selects default; Escape cancels; returns IDYES/IDNO
- [ ] File open dialog: `dialog_file_open("*.txt", "C:\\")` → TreeView shows directory tree; ListView shows files; click folder in TreeView → ListView updates; select file + OK → returns path
- [ ] Color picker: `dialog_color(0xFF0078D4)` → hue ring + SV square appear; drag in ring → changes hue; live preview updates; hex TextBox shows hex color; OK returns ARGB32
- [ ] `msgbox "Test" "Hello" ok` → dialog appears in QEMU; close → exit code 1 (IDOK)
- [ ] `SYS_MSGBOX` in `syscall.h` (next free number); user-mode call triggers desktop-thread MessageBox
- [ ] Commit: `"desktop: complete widget library -- ListView/TreeView/Toolbar/MenuBar/StatusBar/GroupBox/Tooltip/dialogs"`
