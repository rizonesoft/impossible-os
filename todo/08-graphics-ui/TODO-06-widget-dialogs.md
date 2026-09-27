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
> `CTRL_MAX_PER_WINDOW = 32` means each control counts against the per-window slot budget -- ListView and TreeView must store row/node data **outside** the `struct control` union. Use `pmm_alloc_contiguous()` for data arrays > 4 KB (e.g., more than ~170 rows @ 24 bytes each). Each control stores only a pointer + metadata in the union; the data buffer is freed in a new `ctrl_destroy(wh, id)` destructor. Dropdown floating popup mechanism (z_order=9999, borderless `wm_create_window`) is established in TODO-04 §5 -- use the same pattern for Toolbar overflow, MenuBar dropdowns, and Tooltip popups. `anim_mgr_add()` from TODO-02 drives the Tooltip fade-in. All colors via `theme_get()` -- no hardcoded hex. `tools/convert_icon.py` exists and handles the icon-to-C-array conversion for msgbox icons. SYS_ table currently ends at `SYS_MUNMAP=38`; socket syscalls 39–50 are allocated in TODO-02-dns-sockets.md; `SYS_MSGBOX = 51`. Complete sections in order: GroupBox → StatusBar → Toolbar → Tooltip → ListView → TreeView → MenuBar → Dialog system.

## Inputs

- `include/desktop/controls.h` -- extend `enum ctrl_type` with 7 new values; add `ctrl_destroy(wh, id)` destructor; `CTRL_MAX_PER_WINDOW = 32` applies
- `src/desktop/controls.c` -- extend `ctrl_draw_all/handle_mouse/handle_key` dispatch tables
- `include/gfx.h` -- `gfx_fill_rounded_rect`, `gfx_fill_circle`, `gfx_fill_rect_alpha`, `gfx_drop_shadow` for new control rendering
- `include/desktop/theme.h` (TODO-01) -- `theme_get()` for all colors; `accent_hover`, `surface_variant`, `border`, `shadow`
- `include/kernel/gfx/anim_mgr.h` (TODO-02) -- `anim_mgr_add()` + `gfx_ease_decelerate` for Tooltip fade-in tween
- `include/desktop/wm.h` -- `wm_create_window()` + `z_order` overlay for Toolbar overflow, MenuBar dropdowns, Tooltip; `wm_create_window()` for modal dialog
- `include/kernel/sched/syscall.h` -- extend with `SYS_MSGBOX = 51` for user-mode MessageBox access
- `scripts/convert-icons.sh` + `resources/icons/src/` -- message box icons join the original icon set (`docs/design/icons.md`)
- → XREF: `08-graphics-ui/TODO-05-widget-library-core.md` -- `CTRL_DROPDOWN` overlay pattern + `CTRL_SCROLLBAR` used by ListView/TreeView; must be complete before this TODO starts
- Related (no stable XREF target): `08-graphics-ui/TODO-07-*` (context menu) -- MenuBar popup dropdown may be refactored to share context menu engine once it exists; this TODO implements a self-contained popup

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
- `SYS_MSGBOX = 51`; `msgbox` shell command.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                   | Depends On                                                                          | Status |
| --- | :---: | --------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §6 GroupBox + Separator -- visual-only border + line controls; zero interaction               | Nothing; standalone                                                                 |  [ ]   |
| 💎  |   2   | §5 StatusBar -- multi-pane bottom bar; text + icon per pane                                   | §6 (GroupBox establishes the visual-only control extension pattern)                 |  [ ]   |
| 💎  |   3   | §3 Toolbar -- flat icon buttons, toggle state, separator, overflow `>>` chevron popup         | TODO-04 overlay pattern (z_order=9999 from dropdown) re-used for chevron            |  [ ]   |
| 💎  |   4   | §7 Tooltip -- 400 ms hover delay, themed layer popup, applied to all chrome                   | §3 toolbar (toolbar buttons are first tooltip recipients); TODO-02 `anim_mgr_add()` |  [ ]   |
| 💎  |   5   | §1 ListView -- details mode (columns + sort + multi-select) + icon-grid mode + scrollbar      | §4 tooltip (list items receive tooltips); external data buffer pattern              |  [ ]   |
| 💎  |   6   | §2 TreeView -- hierarchical nodes, expand/collapse, indent, scrollbar, keyboard nav           | §5 ListView (same external data + scrollbar integration pattern)                    |  [ ]   |
| 💎  |   7   | §4 MenuBar -- horizontal menu bar, popup dropdown, Alt-navigation, accelerators               | §3 toolbar (popup uses same z_order overlay); TODO-04 dropdown pattern              |  [ ]   |
| 💎  |   8   | §8 Dialog system -- `MessageBox`, file Open/Save, input dialog, color picker, `SYS_MSGBOX=51` | §5+§6 (file dialog uses ListView + TreeView); §3 (nav toolbar); §7 modal            |  [ ]   |

---

## 1. ListView `[Opus]`

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

**Owner of:** the work planned in `06-desktop-foundation/TODO-04 §3`, which is superseded there so the shell has one implementation.

`CTRL_LISTVIEW`: details mode -- column headers + sortable rows + multi-select (Ctrl+click, Shift+click) + integrated vertical scrollbar. Rows are `THEME_SIZE_LIST_ITEM_HEIGHT` (32) with `radius.control` (4) hover and selection fills (`subtle_fill_hover`, `subtle_fill_pressed`) inset 4 px from the view edges, and a 3 x 16 accent pill at the left edge of the selected row (`docs/design/controls.md#list-tree-and-grid-views`). Icon-grid mode -- 48 px icons + two-line caption, same fills. Single-click select, double-click activate. `ctrl_listview_set_view_mode(DETAILS|ICONS)`.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend), `src/desktop/ctrl_listview.c` (new)

> [!NOTE]
> This is `[Opus]` -- ListView is the most complex control: it has virtual rendering (only visible rows drawn), dual view modes, multi-select, column resizing, and a sort algorithm -- none of which exist elsewhere in Impossible OS controls. **External data**: `ctrl_listview_data_t` (pointer in union): `{ columns[16]; rows[]; int row_count; int col_count; int selected[]; int sel_count; int scroll_offset; int view_mode; int sort_col; int sort_asc; }`. For > 4 KB data (> 170 rows @ 24 bytes): allocate via `pmm_alloc_contiguous()`; otherwise `kmalloc`. **Virtual rendering**: draw only rows where `y = header_h + (row_idx - scroll_offset) * ROW_H` falls within the control's bounds; `ROW_H = THEME_SIZE_LIST_ITEM_HEIGHT` (32) for details, tile width `THEME_SIZE_EXPLORER_TILE_WIDTH` (96) for the icon grid. **Sort**: on column header click: toggle `sort_asc`; `qsort`-style insertion sort on the rows array (max 1024 rows; `O(n log n)` shell sort). **Multi-select**: track `last_clicked_row`; Shift-click: select range `[last, clicked]`; Ctrl-click: toggle one. **Column resize**: drag the header divider; update `column.width`; clip text if needed.

- [ ] `CTRL_LISTVIEW` + `LISTVIEW_DETAILS=0`, `LISTVIEW_ICONS=1` in `controls.h`
- [ ] `ctrl_listview_data_t` with columns, rows, selection, scroll, sort state in `ctrl_listview.c`; pointer stored in `ctrl->listview.data`
- [ ] `ctrl_destroy(wh, id)` destructor: free `data` buffer; add to `ctrl_draw_all/handle_*` dispatch
- [ ] `int ctrl_create_listview(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h)` → ctrl_id
- [ ] `void ctrl_listview_add_column(int wh, int id, const char *title, uint32_t width)`
- [ ] `int ctrl_listview_add_item(int wh, int id, uint32_t icon_id, const char *text, const char *sub[], int nsub, void *user_ptr)` → row_idx
- [ ] `void ctrl_listview_clear(int wh, int id)` -- free rows; reset scroll + selection
- [ ] `void ctrl_listview_set_view_mode(int wh, int id, uint8_t mode)`
- [ ] `int ctrl_listview_get_selection(int wh, int id, int *out_indices, int max)` → count
- [ ] Draw: header row (column labels in `text_secondary`, sort chevron); 32 px virtual rows; selection fill + 3 x 16 accent pill; integrated scroll bar per `docs/design/controls.md#scroll-bar`
- [ ] Mouse: header click → sort toggle; row click → select; double-click → `on_activate(idx, user_ptr)`; header divider drag → resize column
- [ ] Key: Up/Down navigate; Enter activate; Ctrl+A select all; Delete → `on_delete(indices[], count)`
- [ ] Icon-grid mode: `THEME_SIZE_EXPLORER_TILE_WIDTH` (96) px tiles; icon 48 px centred; two-line caption below (ellipsis on the second line); hover/selection fills with `radius.control`
- [ ] Commit: `"controls: CTRL_LISTVIEW -- details+icon-grid, column sort, multi-select, virtual render"`

## 2. TreeView `[Opus]`

**Design:** [`controls.md#list-tree-and-grid-views`](../../docs/design/controls.md#list-tree-and-grid-views)

`CTRL_TREEVIEW`: hierarchical list with ▸/▾ expand triangles. Each node: icon, label, parent ptr, children list, expanded flag. 16 px per-level indentation; a 12 px chevron that rotates 90 degrees over `THEME_MOTION_FAST_MS` when expanding; 32 px rows with the list-view hover/selection fills and accent pill (`docs/design/controls.md#list-tree-and-grid-views`). Click chevron → toggle. Arrow keys: up/down move, right expand, left collapse. Integrated vertical scrollbar.

**Files:** `src/desktop/ctrl_treeview.c` (new), `include/desktop/controls.h` (extend)

> [!NOTE]
> This is `[Opus]` -- tree rendering requires a flattened visible-node list rebuilt on expand/collapse, which is a novel data structure for Impossible OS. **Node storage**: `ctrl_treeview_node_t { uint32_t icon_id; char label[128]; int parent_idx; int children[32]; int child_count; int expanded; uint8_t level; void *user_ptr; }`. Max 512 nodes per tree; allocated via `pmm_alloc_contiguous(512 * sizeof(ctrl_treeview_node_t))`. **Visible list**: `int visible_nodes[512]` + `int visible_count`; rebuilt by `tv_rebuild_visible(data)` which does a DFS traversal emitting only nodes whose ancestors are all expanded. Scroll offset in visible-node units. **Add node**: `ctrl_treeview_add_node(wh, id, parent_node, icon_id, label, user_ptr)` → node_idx; appends to parent's `children[]`; calls `tv_rebuild_visible()`. **Indent**: draw at `x + node.level * 16`. **Expand triangle**: ▸ (U+25B8) for collapsed with children; ▾ (U+25BE) for expanded; blank for leaf. Triangle click: toggle `expanded`; `tv_rebuild_visible()`; `wm_mark_dirty()`.

- [ ] `CTRL_TREEVIEW` in `controls.h`
- [ ] `ctrl_treeview_node_t` + `ctrl_treeview_data_t` in `ctrl_treeview.c`; pointer in `ctrl->treeview.data`
- [ ] `int ctrl_create_treeview(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h)` → ctrl_id
- [ ] `int ctrl_treeview_add_node(int wh, int id, int parent_node, uint32_t icon_id, const char *label, void *user_ptr)` → node_idx; -1 for root (parent_node = -1)
- [ ] `void ctrl_treeview_remove_node(int wh, int id, int node_idx)` -- remove + re-parent children to grandparent; rebuild visible
- [ ] `void ctrl_treeview_expand(int wh, int id, int node_idx)` / `ctrl_treeview_collapse()`
- [ ] `void ctrl_treeview_set_selected(int wh, int id, int node_idx)` + `int ctrl_treeview_get_selected(int wh, int id)`
- [ ] `void ctrl_treeview_clear(int wh, int id)` -- free all nodes + rebuild
- [ ] Draw: virtual render from `visible_nodes[scroll_offset..]`; 32 px rows; indent = `level × 16`; 12 px rotating chevron; icon (16 px); label; selection fill + 3 x 16 accent pill
- [ ] Mouse: click triangle area → toggle; click label → select + `on_select(node_idx, user_ptr)`; double-click → `on_activate()`
- [ ] Key: Up/Down move; Right expand; Left collapse; Enter activate
- [ ] Commit: `"controls: CTRL_TREEVIEW -- hierarchical nodes, expand/collapse, DFS visible list, indent"`

## 3. Toolbar `[Sonnet]`

**Design:** [`controls.md#button`](../../docs/design/controls.md#button), [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

`CTRL_TOOLBAR`: horizontal strip of flat icon buttons (hover highlight, pressed state, toggle state). Separators (1 px vertical line). Overflow `>>` chevron when buttons don't fit. `ctrl_toolbar_add_button(h, id, icon, tooltip, cb, is_toggle)`, `ctrl_toolbar_add_separator(h, id)`, `ctrl_toolbar_set_active(h, id, btn_idx, state)`.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_TOOLBAR` union: `struct { struct { uint32_t icon_id; char tooltip[64]; void (*cb)(int); uint8_t is_toggle; uint8_t active; uint8_t is_sep; } buttons[32]; int btn_count; int overflow_idx; int overflow_wh; }`. Buttons are subtle buttons (`docs/design/controls.md#button`): transparent at rest, `subtle_fill_hover` / `subtle_fill_pressed`, `radius.control`, 16 px glyphs. Overflow: when `x + BTN_W > ctrl_w - CHEVRON_W`, set `overflow_idx`; the `...` button at the right edge opens a menu flyout per `docs/design/shell.md#context-menus` (256 px wide, 4 px padding, 32 px items, menu acrylic, radius 8) at z_order=9999 listing the overflowed buttons with their labels. Button width: `BTN_W = 36` px for icon-only, wider for icon + label.

- [ ] `CTRL_TOOLBAR` in `controls.h`; `BTN_W=36` constant
- [ ] Union field as above
- [ ] `int ctrl_create_toolbar(int wh, uint32_t x, uint32_t y, uint32_t w)` → ctrl_id; height = `THEME_SIZE_COMMAND_BAR_HEIGHT` (48) fixed (`docs/design/controls.md#button`: no other toolbar height)
- [ ] `int ctrl_toolbar_add_button(int wh, int id, uint32_t icon_id, const char *tooltip, void (*cb)(int), uint8_t is_toggle)` → btn_idx
- [ ] `void ctrl_toolbar_add_separator(int wh, int id)` -- adds separator entry
- [ ] `void ctrl_toolbar_set_active(int wh, int id, int btn_idx, uint8_t state)` -- set toggle active state
- [ ] Draw: subtle buttons (hover `subtle_fill_hover`, pressed `subtle_fill_pressed`); separator 1 px `stroke_divider` 24 px tall; `...` overflow button; toggled-on buttons keep the `subtle_fill_pressed` fill
- [ ] Mouse: hit-test per button; click → `cb(btn_idx)`; toggle: flip `active`; chevron click → overflow popup
- [ ] Commit: `"controls: CTRL_TOOLBAR -- icon buttons, toggle, separator, overflow chevron popup"`

## 4. MenuBar `[Sonnet]`

**Design:** [`controls.md#menu-bar-and-menus`](../../docs/design/controls.md#menu-bar-and-menus)

`CTRL_MENUBAR`: horizontal bar at window top, 32 px tall with subtle buttons (`docs/design/controls.md#menu-bar-and-menus`). Top-level items (File/Edit/View/Help). Click → popup dropdown list (z_order overlay). Keyboard: Alt activates, arrows navigate, underline-letter accelerators. Each item: label, accelerator text, checkmark, submenu indicator, disabled/checked/separator types.

**Files:** `src/desktop/controls.c` (extend), `src/desktop/ctrl_menubar.c` (new), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_MENUBAR` structure: `{ struct menu_def { char label[64]; struct menu_item { char label[64]; char accel[16]; void (*cb)(void); uint8_t type; uint8_t checked; } items[32]; int item_count; } menus[8]; int menu_count; int active_menu; int popup_wh; }`. `type`: `ITEM_NORMAL=0`, `ITEM_SEPARATOR=1`, `ITEM_CHECKED=2`, `ITEM_SUBMENU=3`, `ITEM_DISABLED=4`. Popup: the drop-down is the shared menu per `docs/design/shell.md#context-menus` -- `wm_create_window(NULL, hdr_ax, bar_abs_y + 32, 256, 8 + item_count*32, WM_FLAG_VISIBLE)` at z_order=9999, menu acrylic, radius 8, `THEME_ELEV_FLYOUT_*`, 32 px items with 16 px glyph, label, shortcut hint in `text_secondary`, full-width 1 px `stroke_divider` separators. Keyboard Alt: set `alt_active = 1`; draw underlined access keys.

- [ ] `CTRL_MENUBAR` + item type constants in `controls.h`
- [ ] `ctrl_menubar_data_t` in `ctrl_menubar.c`; pointer in union
- [ ] `int ctrl_create_menubar(int wh)` → ctrl_id; height=32 (`THEME_SIZE_CONTROL_HEIGHT`), width=parent window width
- [ ] `int ctrl_menubar_add_menu(int wh, int id, const char *label)` → menu_idx
- [ ] `int ctrl_menubar_add_item(int wh, int id, int menu_idx, const char *label, const char *accel, void (*cb)(void))` → item_idx
- [ ] `void ctrl_menubar_add_separator(int wh, int id, int menu_idx)`
- [ ] `void ctrl_menubar_set_checked(int wh, int id, int menu_idx, int item_idx, uint8_t checked)`
- [ ] `void ctrl_menubar_set_enabled(int wh, int id, int menu_idx, int item_idx, uint8_t enabled)`
- [ ] Draw: bar on the window background; menu headers as subtle buttons (`subtle_fill_hover`, open menu `subtle_fill_pressed`); Alt-mode underlines
- [ ] Mouse + keyboard handling: click header → open popup; item click → call `cb`; keyboard Alt, arrows, Escape
- [ ] Commit: `"controls: CTRL_MENUBAR -- popup dropdown, Alt-navigation, accelerators, checked/separator items"`

## 5. StatusBar `[Sonnet]`

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

`CTRL_STATUSBAR`: 24 px bar at window bottom (matches the File Explorer status bar, `docs/design/shell.md#file-explorer`), text in the caption style (12/16) and `text_secondary`. 1–8 panes; first pane stretchy (remaining width), others fixed-width. `ctrl_statusbar_add_pane(wh, id, width, text)`, `ctrl_statusbar_set_pane_text/icon(wh, id, pane_idx, …)`.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> `CTRL_STATUSBAR` union: `struct { struct { uint32_t width; char text[128]; uint32_t icon_id; } panes[8]; int pane_count; }`. First pane has `width=0` meaning stretch. Layout: stretchy pane width = `ctrl_w - sum_of_fixed_widths`. Draw each pane: 1 px `stroke_divider` between panes; icon (16 px) if set; text in `text_secondary` at 12 px; background `window_bg`. StatusBar always renders at the bottom edge of its parent control rect.

- [ ] `CTRL_STATUSBAR` in `controls.h`; statusbar height `STATUSBAR_H = 24`
- [ ] Union field as above
- [ ] `int ctrl_create_statusbar(int wh)` → ctrl_id; placed at window bottom; width = window client width
- [ ] `int ctrl_statusbar_add_pane(int wh, int id, uint32_t width, const char *text)` → pane_idx; `width=0` = stretchy first pane
- [ ] `void ctrl_statusbar_set_pane_text(int wh, int id, int pane_idx, const char *text)`
- [ ] `void ctrl_statusbar_set_pane_icon(int wh, int id, int pane_idx, uint32_t icon_id)`
- [ ] Draw case: layout pass (compute stretchy width); per-pane fill + border + icon + text
- [ ] Commit: `"controls: CTRL_STATUSBAR -- multi-pane, stretchy first pane, icon+text per pane"`

## 6. GroupBox + Separator `[Sonnet]`

**Design:** [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

`CTRL_GROUPBOX`: draws as a card per `docs/design/controls.md#cards-and-settings-rows` (`card_bg`, 1 px `stroke_card`, `THEME_RADIUS_CARD` (8), padding 16) with its label as a body-strong heading 8 px above the card, not a Win32 border cutout. `CTRL_SEPARATOR`: 1 px horizontal or vertical line in `stroke_divider`. Purely visual; no interaction; no callbacks.

**Files:** `src/desktop/controls.c` (extend), `include/desktop/controls.h` (extend)

> [!NOTE]
> GroupBox render: `gfx_draw_string(label, x, y, text_primary, body_strong)`; card at `y + 28`: `gfx_fill_rounded_rect(s, x, y+28, w, h-28, 8, card_bg)` + 1 px `stroke_card` outline; children lay out from the 16 px padding. Separator: `gfx_fill_rect(s, x, y, w, 1, stroke_divider)` for horizontal; `(x, y, 1, h, stroke_divider)` for vertical. Both are `read-only` -- `ctrl_handle_mouse/key` ignores them.

- [ ] `CTRL_GROUPBOX`, `CTRL_SEPARATOR` in `controls.h`; `SEPARATOR_HORIZ=0`, `SEPARATOR_VERT=1`
- [ ] `int ctrl_create_groupbox(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *label)` → ctrl_id
- [ ] `int ctrl_create_separator(int wh, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t orientation)` → ctrl_id
- [ ] Draw cases: GroupBox heading + card; Separator 1 px `stroke_divider` line
- [ ] Mouse/key handlers: no-op for both types
- [ ] Commit: `"controls: CTRL_GROUPBOX + CTRL_SEPARATOR -- card grouping with heading, divider line"`

## 7. Tooltip Integration `[Sonnet]`

**Design:** [`controls.md#tooltip`](../../docs/design/controls.md#tooltip)

`tooltip_register(ctrl_id, text)`: hover `THEME_MOTION_TOOLTIP_DELAY_MS` (400) → show the tooltip below the pointer per `docs/design/controls.md#tooltip`: `layer_bg` over `window_bg` (so it follows light/dark), 1 px `stroke_surface`, `THEME_RADIUS_TOOLTIP` (4), `THEME_ELEV_TOOLTIP_*` shadow, caption text (12/16) in `text_primary`, max width `THEME_SIZE_TOOLTIP_MAX_WIDTH` (320), padding 8 x 5. Fades in over `THEME_MOTION_FAST_MS` (83) with `gfx_ease_decelerate` via `anim_mgr_add()`. Auto-hide on mouse-leave or after 5 s. Apply to all window chrome buttons, toolbar buttons, and tray icons.

**Files:** `src/desktop/tooltip.c` (new), `include/desktop/tooltip.h` (new), `src/desktop/wm.c` (extend)

> [!NOTE]
> Tooltip manager is global (not per-window). `static struct { int ctrl_id; int wh; char text[256]; uint64_t hover_start_tick; int popup_wh; gfx_tween_t fade_tween; } g_tooltip;`. `tooltip_tick()` called from `wm_composite()` before drawing (after `anim_mgr_tick()`): check if any window has a hovered control; compare `system_get_ticks()` vs `hover_start_tick`; if elapsed >= `THEME_MOTION_TOOLTIP_DELAY_MS` and popup not shown: open popup. Popup: `wm_create_window(NULL, cursor_x, cursor_y + 20, text_w + 16, 26, WM_FLAG_VISIBLE)` at z_order=10000 (below the pointer, clamped on-screen); draw the themed layer, stroke, radius 4 and tooltip shadow; start the fade tween.

- [ ] `tooltip_register(int ctrl_id, const char *text)` + `tooltip_unregister(int ctrl_id)` in `src/desktop/tooltip.c`
- [ ] `tooltip_tick()` in `tooltip.c`; called from `wm_composite()` just after `anim_mgr_tick()`
- [ ] Fade-in: `gfx_tween_t fade_tween` with `gfx_ease_decelerate`, `THEME_MOTION_FAST_MS` (83); connected to popup `win->anim.opacity`
- [ ] Auto-hide: on mouse move (position change): `hover_start_tick = current_ticks`; if popup showing: close it
- [ ] Apply to WM buttons: in `wm.c` window creation: `tooltip_register(close_btn_id, "Close")`, `"Minimize"`, `"Maximize"`
- [ ] Apply to Toolbar buttons: `ctrl_toolbar_add_button` stores `tooltip` text; `tooltip_register` called on add
- [ ] `void tooltip_init(void)` called from `desktop_init()`; `void tooltip_shutdown(void)` (no-op for now)
- [ ] Commit: `"desktop: tooltip system -- 400 ms hover, themed layer popup, auto-hide, wired to WM+toolbar buttons"`

## 8. Dialog System `[Opus]`

**Design:** [`controls.md#dialog`](../../docs/design/controls.md#dialog)

Win32-compatible `MessageBox()` (exact `MB_*`/`ID*` constants, 32 px status glyphs in the status colours (`docs/design/controls.md#dialog`), modal dimming, word-wrap). `dialog_file_open/save` (TreeView + ListView + TextBox + Dropdown). `dialog_input`. `dialog_color` (hue wheel + SV square + hex TextBox). `SYS_MSGBOX = 51` syscall. `msgbox` shell command.

**Files:** `src/desktop/dialogs.c` (new), `include/desktop/dialogs.h` (new), icon assets + build pipeline (new), `include/kernel/sched/syscall.h` (extend)

> [!NOTE]
> This is `[Opus]` -- the dialog system has three novel aspects for Impossible OS: (1) **modal overlay**: `wm_create_window()` for the dialog, then dim the owner window (or the whole screen for system dialogs) with the `smoke` scrim token (`theme_get()->colors.smoke`, `#4D000000`) re-applied each compositor frame while modal is open; close = `wm_destroy_window(modal_wh)` + undim; (2) **color picker hue wheel**: rasterize a 128 px diameter hue circle at init time (iterate pixels, compute HSV angle for each pixel, convert to RGB); SV square (128×128): for current hue H, each pixel is `HSV(H, x/128, y/128)` → RGB; mouse-drag in either → update color; (3) **word-wrapped MessageBox body**: implement `dialog_text_measure_wrap(text, max_w, font_size)` → line_count and `dialog_text_draw_wrap()` to render. `MessageBox` Win32 constants: `MB_OK=0x0`, `MB_OKCANCEL=0x1`, `MB_ABORTRETRYIGNORE=0x2`, `MB_YESNOCANCEL=0x3`, `MB_YESNO=0x4`, `MB_RETRYCANCEL=0x5`; icons `MB_ICONERROR=0x10`, `MB_ICONWARNING=0x30`, `MB_ICONINFO=0x40`, `MB_ICONQUESTION=0x20`; `MB_SYSTEMMODAL=0x1000` dims parent. Returns `IDOK=1`, `IDCANCEL=2`, `IDABORT=3`, `IDRETRY=4`, `IDIGNORE=5`, `IDYES=6`, `IDNO=7`. SYS_MSGBOX: user-mode syscall `int SYS_MSGBOX(const char *text, const char *caption, uint32_t uType)` → return ID; handler in `sys_msgbox()` in `syscall.c` calls `MessageBox()` on the desktop thread.

- [ ] **Icon assets**
  - error, warning, info and question icons are added to the original icon set first (`resources/icons/src/`, specified in `docs/design/icons.md`), then rendered by `scripts/convert-icons.sh` and packed like the other system icons
  - no separate `assets/` tree
- [ ] `include/desktop/dialogs.h`: `MB_*` constants (exact Win32 values), `ID*` return codes, all dialog function signatures
- [ ] `int MessageBox(int parent_wh, const char *text, const char *caption, uint32_t uType)` in `dialogs.c`: parse `uType` for button set + icon
  - create a centred dialog per `docs/design/controls.md#dialog`: width between `THEME_SIZE_DIALOG_MIN_WIDTH` (320) and `THEME_SIZE_DIALOG_MAX_WIDTH` (548), `THEME_RADIUS_DIALOG` (8), `window_bg`, `THEME_ELEV_WINDOW_ACTIVE_*`, over the `smoke` scrim
  - 24 px padding, caption as the title in the subtitle style (20/28), a `THEME_SIZE_MESSAGE_ICON` (32) status glyph beside the body text
  - `dialog_text_draw_wrap()`
  - an 80 px footer band in `layer_bg` with right-aligned equal-width buttons 8 px apart, the default button accent (reuse `ctrl_create_button`)
  - keyboard: Enter=default, Escape=cancel, Tab cycle
  - block until button clicked
  - return ID
- [ ] `dialog_text_measure_wrap(text, max_w)` → line_count + line_array; `dialog_text_draw_wrap()` renders
- [ ] `char* dialog_file_open(const char *filter, const char *default_dir)` → path or NULL: 600×400 window; left TreeView (directory tree) + right ListView (files, details mode) + TextBox (filename) + Dropdown (file type filter); OK/Cancel buttons; directory change in TreeView refreshes ListView via `vfs_readdir()`; returns heap-allocated path (`pmm_alloc_contiguous(MAX_PATH)`)
- [ ] `char* dialog_file_save(const char *filter, const char *default_name)` → path or NULL: same layout as open; TextBox pre-filled with `default_name`; warns if file exists (nested `MessageBox`)
- [ ] `char* dialog_input(const char *title, const char *prompt, const char *default_text)` → text or NULL: 400×150; label + `ctrl_create_textbox` + OK/Cancel
- [ ] `uint32_t dialog_color(uint32_t initial)` → ARGB32 or 0 on cancel: 300×320; rasterize hue ring + SV square at init; mouse-drag updates `H/S/V`; live preview rect; hex `ctrl_create_textbox`; OK/Cancel
- [ ] `#define SYS_MSGBOX 51` in `include/kernel/sched/syscall.h`; `sys_msgbox()` handler in `syscall.c` dispatches to `MessageBox()` on desktop thread (post to desktop event queue)
- [ ] Shell command: `msgbox "title" "text" [ok|yesno|okcancel]` → prints returned ID to stdout
- [ ] Commit: `"desktop/dialogs: MessageBox Win32-compat, file open/save, input, color picker, SYS_MSGBOX=51"`

---

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
| ⭐  | `SYS_MSGBOX = 51`                                | ✅ User-mode `MessageBoxW` via `user32.dll`; `msg`              | ❌ No kernel-level MessageBox syscall; all                     | ⬜ §8 -- `⭐` kernel-dispatched MessageBox via syscall                     |

> **After §1–§8:** Impossible OS has a production-quality widget library and dialog system entirely in kernel-native code. The `⭐` differentiators are: (1) `dialog_color()` rasterizes the hue wheel at init time -- no external color picker library needed; (2) `SYS_MSGBOX = 51` lets any user-mode program pop a MessageBox with a single syscall -- a pattern Linux has no equivalent for (GTK/Qt are userspace-only).

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
- [ ] `SYS_MSGBOX = 51` in `syscall.h`; user-mode call triggers desktop-thread MessageBox
- [ ] Commit: `"desktop: complete widget library -- ListView/TreeView/Toolbar/MenuBar/StatusBar/GroupBox/Tooltip/dialogs"`
