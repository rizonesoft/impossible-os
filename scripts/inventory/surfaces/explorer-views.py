"""Surface module: the File Explorer items view, its view modes and columns.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 12 (File
Explorer views and columns). Writes docs/design/inventory/explorer/views.json:

    python3 scripts/inventory/capture.py --surface explorer --file views --module explorer-views --prepare explorer:sample

The window is the frame module's (shared/explorer_window.py), on the fixture's
sample tree (fixtures/explorer.ps1). Published scenes walk the details view with
its header, the default columns of each of the five folder kinds, the header's
menu, a column's filter list, the Choose Details dialog, the special folders
(Home, Gallery, This PC, Network, the Recycle Bin, a search, a library, a
compressed folder, an empty folder) and a grouped view.

Measured scenes, not published: each of the eight view modes in the normal and
the compact layout (cell, label and gap sizes, read off the items), a new
folder's placeholder names, and an item's info tip. METRICS turns them into
records. The Sort and Group by menus are the frame's (explorer/frame.json,
sort-menu); the selection model and the drag feedback are hand records.
"""
import collections
import time

import capture_core as core
from explorer_window import (
    DATE_VALUE, HOME, SAMPLE, SETTLE, _root, at, close_all, close_menu, descendants, esc_twice, focused, fresh, in_bar, item,
    items,
    menu, nothing, opened, press_button, window)

SURFACE = "explorer"
ROOT = "views"
# Among the frame record's children: its parts run from 0, its metrics from 200
# and its known-folders group is 300 (explorer-frame.py).
VIEWS_ORDER = 400

# The view modes, in the View menu's order, with the key that sets each.
MODES = [("extra-large-icons", "Extra large icons", "ctrl+shift+1"), ("large-icons", "Large icons", "ctrl+shift+2"),
         ("medium-icons", "Medium icons", "ctrl+shift+3"), ("small-icons", "Small icons", "ctrl+shift+4"),
         ("list", "List", "ctrl+shift+5"), ("details", "Details", "ctrl+shift+6"), ("tiles", "Tiles", "ctrl+shift+7"),
         ("content", "Content", "ctrl+shift+8")]
ICON_MODES = {"extra-large-icons", "large-icons", "medium-icons"}       # the label under the icon
DETAILS = "ctrl+shift+6"
# The folder kinds the fixture types through desktop.ini, each with its own default columns.
FOLDER_KINDS = [("general", "General items"), ("documents", "Documents items"), ("pictures", "Pictures items"),
         ("music", "Music items"), ("videos", "Videos items")]
ONE_LINE = "Notes"                      # one word: its label is one line in every mode (fixtures/explorer.ps1)
LONG = "Sample file with a long name that wraps onto two lines under its icon"
BIN = "shell:RecycleBinFolder"
GALLERY = "shell:::{e88865ea-0e1c-4e20-9aa6-edcd0212c87c}"
LIBRARY = "C:\\Users\\ref\\AppData\\Roaming\\Microsoft\\Windows\\Libraries\\Sample library.library-ms"
SEARCH_TEXT = "sample"
SEARCH_WAIT = 6                         # s for the search of the sample tree to list its results
TIP_WAIT = 6                            # s an info tip may take to appear under a resting pointer
NEW_NAMES = 2                           # new folders made in the empty folder: the pattern shows on the second
NEW_WANT = ["New folder", "New folder (2)"]
MENU_SEARCH_DEPTH = 14                  # levels from the window to a submenu's entries
GROUP_WAIT = 6                          # s for the view to show or drop its groups
COMPACT_TRIES = 3                       # menu tries to switch compact view
POPUP_TRIES = 20                        # half-second looks for a classic popup menu
MENU_CLASS = "#32768"                   # a classic Win32 popup menu, top-level
OWNED_SKIP = ("ShellTabWindowClass", "Microsoft.UI.Content.DesktopChildSiteBridge", "TITLE_BAR_SCAFFOLDING_WINDOW_CLASS")

# A column's filter button is one of identical siblings beside the column
# headers; its place is its column's place, which the column records hold.
IDENTITY = [({"automationId": "DropDown", "nth": n}, f"filter-{n}") for n in range(1, 13)]
# Choose Details labels its list and its width box with a Static of the same name.
IDENTITY += [({"className": "Static", "name": "Details:"}, "details-label"),
             ({"className": "SysListView32", "name": "Details:"}, "details"),
             ({"className": "Static", "name": "Width of selected column (in pixels):"}, "column-width-label"),
             ({"className": "Edit", "name": "Width of selected column (in pixels):"}, "column-width")]
VALUES = [DATE_VALUE]
LABELS = [(DATE_VALUE, "Date"), ({"controlType": "Image", "className": "Image"}, "Icon"),
          ({"automationId": "HomeListView"}, "Home"), ({"automationId": "ItemsView"}, "Gallery"),
          ({"className": "UIViewHeader"}, "Header"),
          # Elements that report no name: labelled by what they are, not their developer id.
          ({"controlType": "TitleBar"}, "Title bar"),
          ({"automationId": "QuickAccessGridView"}, "Quick access items"), ({"automationId": "GroupCheckbox"}, "Select all"),
          ({"automationId": "PivotsView"}, "Tabs"), ({"automationId": "TopNavMenuItemsHost"}, "Tab list"),
          ({"automationId": "PART_ScrollPresenter"}, "Scroll area")]

_m = {}                                 # measured by the unpublished scenes, read by METRICS


# ------------------------------------------------------------- helpers --

def set_mode(ctx, chord):
    ctx.keys(chord)
    time.sleep(SETTLE * 2)


def focus_item(ctx):
    """Put the focus on the one-word item with a Ctrl+click, in the details
    view, where every row is on screen. The mode keys reach the view only while
    the list has the focus (2026-11-19: pressed after a menu closed, they went
    nowhere), and the focused item shows its whole name, so it must not be the
    long-named one. Ctrl keeps a click on a selected item from starting a rename."""
    details(ctx)
    ctx.hold("ctrl")
    try:
        ctx.click(item(ctx, ONE_LINE))
    finally:
        ctx.release("ctrl")
    time.sleep(0.5)


def details(ctx):
    set_mode(ctx, DETAILS)


def header(ctx):
    return ctx.find(within=items(ctx), className="UIViewHeader", depth=2)


def column(ctx, name):
    return ctx.find(within=header(ctx), controlType="SplitButton", name=name, depth=2)


def owned_windows(ctx):
    """The windows the File Explorer window owns, from one look for it: none
    while it is not there (the caller's poll has its own deadline)."""
    try:
        return descendants(ctx, window(ctx, wait=0), 1)
    except core.Incomplete:
        return []


def owned(ctx, wait=10, **spec):
    """A window the File Explorer window owns (a dialog, a drop-down): UI
    Automation lists it under its owner, not the desktop, and in the raw view
    only (2026-11-19, Choose Details)."""
    deadline = time.monotonic() + wait
    while True:
        # One look at the owner per poll: its own 20 s wait would outlast this one.
        for k in owned_windows(ctx):
            if k.class_name not in OWNED_SKIP and core.matches(k, spec):
                return k
        if time.monotonic() > deadline:
            raise core.Incomplete(f"no window owned by File Explorer matches {spec} within {wait} s")
        time.sleep(0.5)


def popup(ctx):
    """The classic popup menu open on screen (the header's menu, 2026-11-19)."""
    for _ in range(POPUP_TRIES):
        for w in ctx.live.children(ctx.live.uia.GetRootElement()):
            if w.class_name == MENU_CLASS:
                return w
        time.sleep(0.5)
    raise core.Incomplete("no popup menu opened")


def header_menu(ctx):
    x, y, w, h = column(ctx, "Type").bounds
    ctx.click_at(x + w // 3, y + h // 2, button="right")
    time.sleep(SETTLE)
    _root["el"] = popup(ctx)


def open_header_menu(ctx):
    details(ctx)
    header_menu(ctx)


def open_filter(ctx):
    """A column's filter button shows under the pointer; the list it opens is a
    drop-down window the File Explorer window owns (2026-11-19)."""
    details(ctx)
    x, y, w, h = column(ctx, "Name").bounds
    ctx.hover_point(x + w // 2, y + h // 2)
    time.sleep(1)
    ctx.click(ctx.find(within=column(ctx, "Name"), automationId="DropDown", depth=2))
    time.sleep(SETTLE)
    _root["el"] = owned(ctx, className="DropDown")


def open_choose_details(ctx):
    details(ctx)
    header_menu(ctx)
    # "More..." is the menu's last entry: Up from the open menu reaches it.
    ctx.keys("up")
    ctx.keys("enter")
    _root["el"] = owned(ctx, wait=30, className="#32770", name="Choose Details")


def cancel(ctx):
    ctx.click(ctx.find(within=_root["el"], controlType="Button", name="Cancel", depth=2))
    time.sleep(SETTLE)


def view_root(ctx):
    return items(ctx)


def search(ctx):
    ctx.click(in_bar(ctx, automationId="FileExplorerSearchBox"))
    time.sleep(0.5)
    for k in SEARCH_TEXT:
        ctx.keys(k)
    ctx.keys("enter")
    time.sleep(SEARCH_WAIT)


def search_options(ctx):
    search(ctx)
    press_button(controlType="Button", name="Search options")(ctx)


def command_bar(ctx):
    return in_bar(ctx, automationId="FileExplorerCommandBar")


def home_list(ctx):
    return ctx.find(within=window(ctx), automationId="HomeListView", depth=12)


def gallery(ctx):
    return ctx.find(within=window(ctx), automationId="ItemsView", depth=12)


def group_by(ctx, name):
    ctx.click(in_bar(ctx, controlType="Button", name="Sort"))
    m = menu(ctx)
    ctx.click(ctx.find(within=m, name="Group by", depth=3))
    time.sleep(SETTLE)
    # The Sort menu has entries of the same names (sort by Type); the Group by
    # submenu opens beside it, so its entry is the one furthest right (2026-11-19).
    ctx.click(shown_entry(ctx, name, rightmost=True))
    time.sleep(SETTLE * 2)


def groups(ctx, want, wait=GROUP_WAIT):
    """Wait until the folder shows groups (want=True) or none; True when it
    does. Only groups on screen count: an ungrouped folder's groups can stay
    in the tree offscreen while the view redraws."""
    deadline = time.monotonic() + wait
    while True:
        shown = any(k.class_name == "UIGroupItem" and not k.offscreen for k in items(ctx).children())
        if shown == want or time.monotonic() >= deadline:
            return shown == want
        time.sleep(0.5)


def group_by_type(ctx):
    details(ctx)
    # The modes scene before this one turned compact view on and off again.
    if "row" in _m and items_row(ctx) != _m["row"]:
        raise core.Incomplete(f"the details rows are {items_row(ctx)} px, not {_m['row']}: compact view is still on")
    group_by(ctx, "Type")
    if not groups(ctx, True):
        raise core.Incomplete("Group by, Type showed no groups")


# ------------------------------------------------------- measured scenes --

def read_items(ctx):
    """Each item of the view: its box and its name label's box."""
    out = {}
    for k in items(ctx).children():
        if k.control_type != "ListItem":
            continue
        label = next((d for d in k.children() if d.automation_id == "System.ItemNameDisplay"), None)
        out[k.name] = {"box": list(k.bounds), "label": list(label.bounds) if label else None}
    return out


# UI Automation exposes no icon element (an item holds only its text fields,
# 2026-11-20), so the icon is measured from the pixels of the long-named item's
# icon slot: not selected, and with the pointer moved off the view, not hovered.
ICON_DIFF = 24                           # a channel this far from the slot's commonest colour is artwork
POINTER_REST = (10, 10)
LINE_SHARE = 0.9                         # of a row's width: a line, not artwork


def icon_bounds(rows):
    """The artwork in a block of (r, g, b) rows: (x, y, w, h) of what differs from
    the block's commonest colour, or None. A row that differs nearly all the way
    across is a separator line, not artwork (the content view rules its rows,
    2026-11-20)."""
    bg = collections.Counter(p for r in rows for p in r).most_common(1)[0][0]
    xs, ys = [], []
    for y, r in enumerate(rows):
        hit = [x for x, p in enumerate(r) if max(abs(a - b) for a, b in zip(p, bg)) > ICON_DIFF]
        if len(hit) >= LINE_SHARE * len(r):
            continue
        xs.extend(hit)
        ys.extend([y] * len(hit))
    if not xs:
        return None
    return min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def icon_slot(entry, slug):
    """The part of an item that holds its icon: above the label in the icon
    modes, before it elsewhere."""
    x, y, w, h = entry["box"]
    lx, ly = entry["label"][:2]
    return (x, y, w, ly - y) if slug in ICON_MODES else (x, y, lx - x, h)


def read_icon(ctx, entry, slug):
    sx, sy, sw, sh = icon_slot(entry, slug)
    found = icon_bounds(ctx.pixels(sx, sy, sw, sh))
    # Artwork touching the slot's edge is cut off, or the slot holds more than the icon.
    if not found or found[0] == 0 or found[1] == 0 or found[0] + found[2] == sw or found[1] + found[3] == sh:
        raise core.Incomplete(f"{slug}: no whole icon in the slot {[sx, sy, sw, sh]}: {found}")
    return [sx + found[0], sy + found[1], found[2], found[3]]


def read_measured(ctx):
    """read_items with the two measured items on screen: the list is virtualized,
    so an item scrolled out of view is missing until it is brought back
    (2026-11-19: the long-named item, last of ten, in extra large icons)."""
    now = read_items(ctx)
    for name in (ONE_LINE, LONG):
        if name not in now:
            ctx.bring_into_view(items(ctx), name)
            now = read_items(ctx)
    return now


def items_row(ctx):
    return read_items(ctx)[ONE_LINE]["box"][3]


def shown_entry(ctx, name, rightmost=False):
    """The menu entry `name` that is on screen. A closed flyout's entries stay in
    the tree offscreen, so the first match can be the last menu's (2026-11-19:
    turning compact view off clicked where the first menu's entry had been)."""
    shown = [k for k in descendants(ctx, window(ctx), MENU_SEARCH_DEPTH, skip=("UIItemsView", "SysTreeView32"))
             if k.control_type == "MenuItem" and k.name == name and not k.offscreen]
    if not shown:
        raise core.Incomplete(f"no {name} entry on screen")
    return max(shown, key=lambda k: k.bounds[0]) if rightmost else shown[-1]


def compact(ctx, on):
    """Turn compact view on or off through View, Show, Compact view, the way a
    user does, and check the details rows changed height. The menu is opened
    afresh for each try; the end state is what is checked, never assumed."""
    details(ctx)
    before = items_row(ctx)
    seen = []
    for _ in range(COMPACT_TRIES):
        ctx.click(in_bar(ctx, controlType="Button", name="View"))
        m = menu(ctx)
        ctx.expand_only(ctx.find(within=m, controlType="MenuItem", name="Show", depth=3))
        time.sleep(SETTLE)
        # A toggle entry, with no Invoke pattern (2026-11-19): clicked as a user clicks it.
        entry = shown_entry(ctx, "Compact view")
        ctx.click(entry)
        time.sleep(SETTLE * 2)
        # A toggle entry leaves its menus open: Esc closes the Show submenu, a
        # second Esc the View menu.
        esc_twice(ctx)
        after = items_row(ctx)
        seen.append({"entry": list(entry.bounds), "rows": after, "focus": focused(ctx)})
        if after != before and (after < before) == on:
            return
    raise core.Incomplete(f"compact view {'on' if on else 'off'} left the details rows at {before} px: {seen}")


def measure_modes(ctx):
    fresh(ctx)
    details(ctx)
    _m["row"] = items_row(ctx)
    got = {}
    for variant in ("normal", "compact"):
        if variant == "compact":
            compact(ctx, True)
        focus_item(ctx)
        got[variant] = {}
        last = read_measured(ctx)           # the details view the pass starts from
        for slug, _label, chord in MODES:
            set_mode(ctx, chord)
            now = read_measured(ctx)
            # Each mode lays the items out differently: the same boxes as the
            # mode before mean the key did not take.
            if now[ONE_LINE]["box"] == last[ONE_LINE]["box"]:
                raise core.Incomplete(f"{chord} left the view as it was ({variant}): {now[ONE_LINE]['box']}")
            ctx.hover_point(*POINTER_REST)
            time.sleep(0.5)
            now[LONG]["icon"] = read_icon(ctx, now[LONG], slug)
            got[variant][slug] = last = now
    _m["modes"] = got


def compact_off(ctx):
    """Turn compact view off at its setting, which a new File Explorer window
    reads. Clicking the menu entry again did not turn it off (2026-11-19,
    three tries on the entry on screen), and the next scene opens a new window."""
    ctx.powershell(COMPACT_OFF)
    close_all(ctx)


COMPACT_OFF = r"""
Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name UseCompactMode -Value 0 -Type DWord
"""


def measure_new_names(ctx):
    """Make new folders in the empty folder with Ctrl+Shift+N and keep their
    names, then remove them (not to the Recycle Bin, which a scene walks)."""
    fresh(ctx, SAMPLE + "\\Empty folder")
    for _ in range(NEW_NAMES):
        ctx.keys("ctrl+shift+n")
        time.sleep(SETTLE)
        ctx.keys("enter")
        time.sleep(SETTLE)
    _m["new"] = sorted(read_items(ctx))
    ctx.powershell(f"Get-ChildItem -LiteralPath '{SAMPLE}\\Empty folder' -Force | Remove-Item -Recurse -Force")
    fresh(ctx)


def measure_tip(ctx):
    """Rest the pointer on a file and read its info tip, a tooltip window."""
    fresh(ctx)
    set_mode(ctx, "ctrl+shift+2")
    ctx.hover(item(ctx, "Sample document"), settle=0.5)
    deadline = time.monotonic() + TIP_WAIT
    while True:
        # A tooltip is a window File Explorer owns, so UI Automation may list
        # it under the File Explorer window as well as on the desktop.
        tops = list(ctx.live.children(ctx.live.uia.GetRootElement()))
        tip = next((w for w in tops if w.control_type == "ToolTip" and w.name), None)
        if tip is None:
            tip = next((w for w in owned_windows(ctx) if w.control_type == "ToolTip" and w.name), None)
        if tip is not None:
            break
        if time.monotonic() > deadline:
            raise core.Incomplete(f"no info tip within {TIP_WAIT} s of resting on Sample document")
        time.sleep(0.3)
    _m["tip"] = (tip.name or "").splitlines()
    ctx.hover_point(10, 10)


# A column header is a split button whose Expand opens nothing (2026-11-19), so
# the scenes walk without expanding; only the search options menu has submenus.
SCENES = [
    dict(slug="details-view", label="Details view", kind="region", open=at(then=details), root=view_root, close=nothing, expand=False),
    *[dict(slug=f"columns-{kind}", label=f"Details columns, {name}", kind="region",
           open=at(target=f"{SAMPLE}\\{name}", then=details), root=header, close=nothing, expand=False) for kind, name in FOLDER_KINDS],
    dict(slug="header-menu", label="Column header menu", kind="contextMenu", open=at(then=open_header_menu),
         root=opened, close=close_menu, expand=False),
    dict(slug="filter-list", label="Column filter list", kind="menu", open=at(then=open_filter), root=opened,
         close=close_menu, expand=False),
    dict(slug="choose-details", label="Choose Details", kind="dialog", open=at(then=open_choose_details), root=opened,
         close=cancel, environment_lists=[{"className": "SysListView32"}]),
    dict(slug="home", label="Home", kind="page", open=at(target=HOME), root=home_list, close=nothing, expand=False),
    dict(slug="gallery", label="Gallery", kind="page", open=at(target=GALLERY), root=gallery, close=nothing, expand=False),
    dict(slug="this-pc", label="This PC", kind="page", open=at(target="shell:MyComputerFolder"), root=view_root,
         close=nothing, expand=False),
    dict(slug="network", label="Network", kind="page", open=at(target="shell:NetworkPlacesFolder"), root=view_root,
         close=nothing, expand=False),
    dict(slug="recycle-bin", label="Recycle Bin", kind="page", open=at(target=BIN, then=details), root=view_root,
         close=nothing, expand=False),
    dict(slug="library", label="Library", kind="page", open=at(target=LIBRARY), root=view_root, close=nothing, expand=False),
    dict(slug="compressed-folder", label="Compressed folder", kind="page",
         open=at(target=SAMPLE + "\\Sample archive.zip"), root=view_root, close=nothing, expand=False),
    dict(slug="empty-folder", label="Empty folder", kind="page", open=at(target=SAMPLE + "\\Empty folder"),
         root=view_root, close=nothing, expand=False),
    dict(slug="search-results", label="Search results", kind="page", open=at(then=search), root=view_root,
         close=nothing, expand=False),
    dict(slug="search-command-bar", label="Command bar, searching", kind="region", open=at(then=search),
         root=command_bar, close=nothing, expand=False),
    dict(slug="search-options-menu", label="Search options menu", kind="menu", open=at(then=search_options),
         root=opened, close=close_menu),
    dict(slug="new-names", label="New folder names", kind="state", open=measure_new_names, root=window, close=nothing,
         keep=False, expand=False),
    dict(slug="info-tip", label="Info tip", kind="state", open=measure_tip, root=window, close=nothing,
         keep=False, expand=False),
    dict(slug="modes", label="View modes", kind="state", open=measure_modes, root=window, close=compact_off,
         keep=False, expand=False),
    # Last: grouping is kept for the folder, and no scene after it reads the sample folder.
    dict(slug="grouped", label="Grouped by type", kind="state", open=at(then=group_by_type), root=view_root,
         close=nothing, expand=False),
]


# --------------------------------------------------------------- records --

def chord_name(chord):
    return "+".join(k.capitalize() if len(k) > 1 else k.upper() for k in chord.split("+"))


def mode_metrics(found, variant):
    """The sizes of one layout's eight modes. `found` maps a mode slug to its
    items ({name: {"box": [x, y, w, h], "label": [x, y, w, h] or None}})."""
    out = []
    suffix = "" if variant == "normal" else "-compact"
    tail = "" if variant == "normal" else ", compact view"
    for n, (slug, label, chord) in enumerate(MODES):
        its = found.get(slug) or {}
        one, long_ = its.get(ONE_LINE), its.get(LONG)
        if not one or not one["label"] or not long_ or not long_["label"] or not long_.get("icon"):
            raise ValueError(f"{slug} ({variant}): the one-line or the long-named item was not read: "
                             f"{ONE_LINE}={one}, long={long_}, read {sorted(its)}")
        x, y, w, h = one["box"]
        lx, ly, _lw, lh = one["label"]
        # The icon sits above the label in the icon modes and left of it elsewhere.
        slot = ly - y if slug in ICON_MODES else lx - x
        lines = max(1, round(long_["label"][3] / lh))
        boxes = [v["box"] for v in its.values()]
        same_row = sorted(b for b in boxes if b[1] == y)
        col_gaps = [b2[0] - (b1[0] + b1[2]) for b1, b2 in zip(same_row, same_row[1:])]
        below = sorted((b for b in boxes if b[0] == x and b[1] > y), key=lambda b: b[1])
        rows = [("cell-width", "cell width", w), ("cell-height", "cell height", h),
                ("icon-slot", "space above the label" if slug in ICON_MODES else "space before the label", slot),
                ("icon-width", "icon artwork width", long_["icon"][2]),
                ("icon-height", "icon artwork height", long_["icon"][3]),
                ("label-line-height", "label line height", lh), ("label-lines", "label lines at most", lines)]
        if col_gaps:
            rows.append(("column-gap", "gap between cells in a row", min(col_gaps)))
        if below:
            rows.append(("row-gap", "gap between rows", below[0][1] - (y + h)))
        mode_id = f"{ROOT}.{slug}{suffix}"
        out.append({"id": mode_id, "parent": ROOT, "kind": "state", "label": label + tail,
                    "order": 100 + n + (0 if variant == "normal" else 20),
                    "behaviour": f"{label} view{tail}; {chord_name(chord)} switches the items view to it."})
        for i, (m, what, value) in enumerate(rows):
            lines_ = m == "label-lines"
            out.append({"id": f"{mode_id}.{m}", "parent": mode_id, "kind": "metric", "label": f"{label}: {what}{tail}",
                        "order": i, "measure": {"value": value, "unit": "lines" if lines_ else "px",
                                                "scaling": "fixed" if lines_ else "scales"}})
    return out


def mode_key_records():
    return [{"id": f"{ROOT}.keys.{slug}", "parent": ROOT, "kind": "shortcut", "label": label, "order": 140 + n,
             "shortcut": chord_name(chord), "shortcutScope": "window",
             "behaviour": f"Switches the items view to {label.lower()}."} for n, (slug, label, chord) in enumerate(MODES)]


def new_name_records(names):
    if names != NEW_WANT:
        raise ValueError(f"the new folders were named {names}, not {NEW_WANT}")
    return [{"id": f"{ROOT}.new-name", "parent": ROOT, "kind": "shortcut", "label": "New folder", "order": 160,
             "shortcut": "Ctrl+Shift+N", "shortcutScope": "window",
             "behaviour": "Makes a folder named \"New folder\" with its name open for editing; the next in the "
                          "same folder is \"New folder (2)\"."}]


def tip_records(lines):
    keys = [ln.split(":", 1)[0].strip() for ln in lines if ":" in ln]
    if not keys:
        raise ValueError(f"the info tip held no rows: {lines}")
    return [{"id": f"{ROOT}.info-tip", "parent": ROOT, "kind": "state", "label": "Info tip", "order": 161,
             "behaviour": "Resting the pointer on a file shows a tip with one row per property: "
                          + ", ".join(keys) + "."}]


def METRICS(records):
    modes = _m.get("modes")
    if not modes:
        raise ValueError("the view modes were not measured")
    # Under the File Explorer window, the frame file's root record.
    out = [{"id": ROOT, "parent": "frame", "kind": "group", "label": "Views", "order": VIEWS_ORDER}]
    out += mode_metrics(modes["normal"], "normal") + mode_metrics(modes["compact"], "compact")
    out += mode_key_records() + new_name_records(_m.get("new")) + tip_records(_m.get("tip") or [])
    return out


HAND = [
    {"id": f"{ROOT}.select-click", "parent": ROOT, "kind": "gesture", "label": "Click an item", "order": 170,
     "behaviour": "Clicking an item selects it alone and moves the focus to it."},
    {"id": f"{ROOT}.select-ctrl-click", "parent": ROOT, "kind": "gesture", "label": "Ctrl-click an item", "order": 171,
     "behaviour": "Ctrl-clicking an item adds it to the selection or takes it out, and moves the focus to it."},
    {"id": f"{ROOT}.select-shift-click", "parent": ROOT, "kind": "gesture", "label": "Shift-click an item",
     "order": 172, "behaviour": "Shift-clicking an item selects every item from the focused one to it, in view order."},
    {"id": f"{ROOT}.rubber-band", "parent": ROOT, "kind": "gesture", "label": "Drag over empty space", "order": 173,
     "behaviour": "Dragging from empty space draws a translucent rectangle and selects every item it touches; "
                  "with Ctrl held it adds to the selection."},
    {"id": f"{ROOT}.check-boxes", "parent": ROOT, "kind": "state", "label": "Item check boxes", "order": 174,
     "behaviour": "With View, Show, Item check boxes on, each item shows a check box on hover and while selected; "
                  "clicking it adds or removes the item, and the details header gains a select-all box."},
    {"id": f"{ROOT}.select-all", "parent": ROOT, "kind": "shortcut", "label": "Select all", "order": 175,
     "shortcut": "Ctrl+A", "shortcutScope": "window", "behaviour": "Selects every item in the view."},
    {"id": f"{ROOT}.invert-selection", "parent": ROOT, "kind": "state", "label": "Invert selection", "order": 176,
     "behaviour": "See more, Invert selection selects exactly the items that were not selected."},
    {"id": f"{ROOT}.focus-and-selection", "parent": ROOT, "kind": "state", "label": "Focus against selection",
     "order": 177, "behaviour": "Selected items carry the accent fill; the focused item carries a focus rectangle, "
                                "shown once the keyboard is used, whether or not it is selected."},
    {"id": f"{ROOT}.rename", "parent": ROOT, "kind": "shortcut", "label": "Rename in place", "order": 178,
     "shortcut": "F2", "shortcutScope": "control",
     "behaviour": "F2, or a second click on a selected item, opens its name for editing in place with the name "
                  "selected up to the extension; Enter keeps the new name, Esc restores the old one."},
    {"id": f"{ROOT}.sort-by-column", "parent": ROOT, "kind": "gesture", "label": "Click a column header",
     "order": 179, "behaviour": "Clicking a column header sorts by it, ascending, with an arrow over the header; "
                                "a second click sorts descending."},
    {"id": f"{ROOT}.resize-column", "parent": ROOT, "kind": "gesture", "label": "Drag a column edge", "order": 180,
     "behaviour": "Dragging the edge between two headers resizes the column on its left; a double-click on the "
                  "edge sizes it to fit its widest value."},
    {"id": f"{ROOT}.reorder-column", "parent": ROOT, "kind": "gesture", "label": "Drag a column header",
     "order": 181, "behaviour": "Dragging a header along the row moves its column, with a marker where it lands."},
    {"id": f"{ROOT}.drag-image", "parent": ROOT, "kind": "state", "label": "Drag image", "order": 182,
     "behaviour": "Dragging items shows their icons under the pointer with a count badge for several, and a tip "
                  "naming the operation (Move to, Copy to, Create link in) and the target."},
    {"id": f"{ROOT}.drop-highlight", "parent": ROOT, "kind": "state", "label": "Drop highlight", "order": 183,
     "behaviour": "A folder in the list or an entry of the navigation pane under a drag takes the hover fill; "
                  "resting on a collapsed navigation entry expands it."},
    {"id": f"{ROOT}.sort-and-group-menus", "parent": ROOT, "kind": "state", "label": "Sort and Group by menus",
     "order": 184, "behaviour": "The command bar's Sort menu and its Group by submenu are recorded with the frame "
                                "(explorer/frame.json, sort-menu); the grouped scene is this view grouped by type."},
    {"id": f"{ROOT}.group-header", "parent": ROOT, "kind": "state", "label": "Group header row",
     "order": 185, "behaviour": "A group's header row shows its name in the accent text colour with a collapse chevron "
                                "at its left and no item count (the grouped shot); the chevron collapses the group to its header."},
]
