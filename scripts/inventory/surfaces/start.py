"""Surface module: the Start menu, its regions, its views and its menus.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (Start
Menu). Writes docs/design/inventory/start/start.json:

    python3 scripts/inventory/capture.py --surface start --file start

Measured on 26300.9974 (2026-11-17): Start is a full-screen CoreWindow named
"Start" whose panel is a child window (named Search, 858 x 890, standing on the
taskbar). It holds the search box, one scrolling list (AppsList) with the
Pinned, Recommended and All regions in order, and a bottom bar with the account
and power buttons. The search box hosts a web view, the search flyout's
(start/search.json). Pinned tiles, recommended entries and all-apps rows are this
machine's data. Every menu is a MenuFlyout in a PopupHost inside Start; the
account card is a Flyout pane there.
"""
import time

import capture_core as core

SURFACE = "start"
ROOT = "start"

# Lists whose rows are this machine's data: recorded once, never entered. The
# letter grid's letters depend on the language and the installed apps.
ENVIRONMENT_LISTS = [{"automationId": "PinnedList"}, {"automationId": "RecommendedList"},
                     {"automationId": "ZoomedOutListView"}]
# The user tile is named for the account; the view button names the view chosen.
VALUES = [{"automationId": "UserTileButton"}, {"automationId": "ViewSelectionButton"}]
# The account menu's three texts are told apart by what the reference machine's
# one local account shows (REFERENCE.md: the local administrator ref); their
# names stay withheld as this machine's data.
ACCOUNT_TEXTS = [("Microsoft", "provider", "Account provider"), ("ref", "account-name", "Account name"),
                 ("Local account", "account-type", "Account type")]
IDENTITY = ([({"automationId": "UserTileButton"}, "account"), ({"automationId": "ViewSelectionButton"}, "view")]
            + [({"className": "TextBlock", "name": name}, seg) for name, seg, _ in ACCOUNT_TEXTS])
HOSTS = []
LABELS = [({"controlType": "Image", "name": ""}, "Icon"),
          ({"automationId": "UserTileButton"}, "Account"),
          ({"automationId": "ViewSelectionButton"}, "View"),
          ({"automationId": "TopLevelRoot"}, "Start content"),
          ({"automationId": "AppsList"}, "Start list"),
          ({"automationId": "StartMenuPinnedList"}, "Pinned grid"),
          ({"automationId": "ZoomedOutListView"}, "Letters"),
          ({"automationId": "LevelOneGridView"}, "Folder grid"),
          *[({"className": "TextBlock", "name": name}, label) for name, _, label in ACCOUNT_TEXTS],
          ({"className": "TextBlock"}, "Account text")]
# The search box's web view is the search flyout's; an all-apps category tile is
# named by the apps in it (this machine's data).
OMIT = [{"className": "Chrome_WidgetWin_1"}, {"className": "GridViewItem", "automationId": ""}]

ADVANCED = r"HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"
# The folder shortcuts Settings can put beside the power button, in its order.
PLACES = ["Settings", "File Explorer", "Documents", "Downloads", "Music", "Pictures", "Videos", "Network",
          "Personal folder"]
TOGGLE = 10015                     # UIA_TogglePatternId
START_TRIES = 3
SETTLE = 2


def start(ctx):
    return ctx.find_window(className="Windows.UI.Core.CoreWindow", name="Start", timeout=8)


def within_start(ctx, **spec):
    return ctx.find(within=start(ctx), depth=6, timeout=15, **spec)


def open_start(ctx):
    """Click the taskbar's Start button: the Win key alone did not open Start on
    a fresh sign-in (2026-11-17), and Start's window exists hidden while closed."""
    bar = ctx.find_window(className="Shell_TrayWnd")
    for attempt in range(1, START_TRIES + 1):
        ctx.click(ctx.find(within=bar, automationId="StartButton", depth=4))
        try:
            start(ctx)
            break
        except core.Incomplete:
            if attempt == START_TRIES:
                raise
    within_start(ctx, automationId="AppsList")
    time.sleep(SETTLE)


def close_start(ctx):
    ctx.keys("esc")
    ctx.gone(className="Windows.UI.Core.CoreWindow", name="Start")


def flyout_menu(ctx):
    """A WinUI menu opened from Start: a MenuFlyout under Start's own popup."""
    return ctx.find(within=start(ctx), className="MenuFlyout", depth=8, timeout=10)


def account_flyout(ctx):
    """The account card: a Flyout pane, not a menu, under Start's popup host."""
    return ctx.find(within=start(ctx), className="Flyout", depth=8, timeout=10)


def menu_from(button_spec, button="left", root=flyout_menu):
    def opener(ctx):
        open_start(ctx)
        ctx.click(within_start(ctx, **button_spec), button=button)
        root(ctx)
        time.sleep(1)
    return opener


def open_entry_menu(ctx):
    """The menu of an all-apps entry: the list view's first app, right-clicked."""
    open_start(ctx)
    set_view(ctx, "List")
    for g in _kids(ctx, apps_list(ctx)):
        if g.class_name == "GridViewHeaderItem":
            for it in _kids(ctx, g):
                if it.automation_id.startswith(APP_PREFIXES) and not it.offscreen:
                    ctx.click(it, button="right")
                    flyout_menu(ctx)
                    time.sleep(1)
                    return
    raise core.Incomplete("the list view showed no app to open a menu on")


def open_folder(ctx):
    """A folder made from the Settings tile (the menu's Create a new app folder;
    a synthesized click on the entry is ignored, Invoke reaches it, 2026-11-17),
    then opened: its own grid and its name."""
    open_start(ctx)
    ctx.click(within_start(ctx, className="GridViewItem", name="Settings"), button="right")
    ctx.invoke(ctx.find(within=flyout_menu(ctx), name="Create a new app folder", depth=3))
    folder = within_start(ctx, className="GridViewItem", namePrefix="Folder with")
    time.sleep(1)
    ctx.click(folder)
    folder_modal(ctx)
    time.sleep(SETTLE)


def folder_modal(ctx):
    return ctx.find(within=start(ctx), className="StartMenu.FolderModal", depth=3, timeout=10)


def open_folder_menu(ctx):
    """The folder tile's own menu: only the Move entries (26300.9974)."""
    open_start(ctx)
    ctx.click(within_start(ctx, className="GridViewItem", namePrefix="Folder with"), button="right")
    flyout_menu(ctx)
    time.sleep(SETTLE)


def close_folder(ctx):
    ctx.keys("esc")
    _modal_gone(ctx)
    close_start(ctx)


def close_entry_menu(ctx):
    ctx.keys("esc")
    time.sleep(1)
    close_view(ctx)


def close_menu(ctx):
    ctx.keys("esc")
    time.sleep(1)
    close_start(ctx)


def _toggle_pattern(el, ctx):
    return el.el.GetCurrentPattern(TOGGLE).QueryInterface(ctx.live.UIA.IUIAutomationTogglePattern)


def toggle(el, ctx):
    """Flip a toggle menu entry or switch: Invoke fails on both (NULL COM pointer, 2026-11-17)."""
    _toggle_pattern(el, ctx).Toggle()


def is_on(el, ctx):
    return _toggle_pattern(el, ctx).CurrentToggleState == 1


def set_view(ctx, name):
    """Choose an all-apps view from the view menu, Start left open."""
    ctx.click(within_start(ctx, automationId="ViewSelectionButton"))
    menu = ctx.find(within=start(ctx), automationId="ViewSelectionMenuFlyout", depth=8, timeout=10)
    toggle(ctx.find(within=menu, name=name, depth=2), ctx)
    time.sleep(SETTLE)
    shown = within_start(ctx, automationId="ViewSelectionButton").name
    if not shown.endswith(name):
        raise core.Incomplete(f"the {name} view was chosen and Start shows {shown!r}")


def apps_list(ctx):
    return within_start(ctx, automationId="AppsList")


def open_view(name):
    def opener(ctx):
        open_start(ctx)
        set_view(ctx, name)
    return opener


def close_view(ctx):
    set_view(ctx, "Category")          # the default, so later scenes see what a new user does
    close_start(ctx)


def open_letters(ctx):
    """The letter grid: selecting a letter heading in the list view zooms out."""
    open_start(ctx)
    set_view(ctx, "List")
    ctx.click(ctx.find(within=apps_list(ctx), className="GridViewHeaderItem", depth=2))
    grid = within_start(ctx, automationId="ZoomedOutListView")
    time.sleep(1)
    # The letter cells are omitted from the records (an unnamed GridViewItem is
    # a category tile's shape); their size and columns are read here.
    _letter_cells[:] = [k.bounds for k in _kids(ctx, grid) if k.control_type == "ListItem" and not k.offscreen]
    if not _letter_cells:
        raise core.Incomplete("the letter grid showed no letters")


# Read by open_letters, published by METRICS: each letter cell's bounds (x, y, w, h).
_letter_cells = []


def letters(ctx):
    return within_start(ctx, automationId="TopLevelRoot")


def close_letters(ctx):
    ctx.click(within_start(ctx, automationId="ZoomInButton"))
    time.sleep(1)
    close_view(ctx)


def panel(ctx):
    return ctx.find(within=start(ctx), className="Windows.UI.Core.CoreWindow", name="Search", depth=2)


def open_left(ctx):
    """Start with the taskbar aligned left (TaskbarAl applies live, 2026-11-16)."""
    ctx.powershell(f"Set-ItemProperty -LiteralPath '{ADVANCED}' -Name TaskbarAl -Value 0 -Type DWord")
    time.sleep(4)
    open_start(ctx)


def close_left(ctx):
    close_start(ctx)
    ctx.powershell(f"Set-ItemProperty -LiteralPath '{ADVANCED}' -Name TaskbarAl -Value 1 -Type DWord")
    time.sleep(4)


_places_before = {}


def set_places(ctx, want):
    """Set each folder shortcut in Settings (want: name -> on), close Settings,
    and return how they were."""
    ctx.launch("explorer.exe", "ms-settings:personalization-start-places")
    win = ctx.find_window(name="Settings", timeout=30)
    was = {}
    for name in PLACES:
        switch = ctx.find(within=win, className="ToggleSwitch", name=name, depth=12, timeout=20)
        was[name] = is_on(switch, ctx)
        if was[name] != want[name]:
            toggle(switch, ctx)
    time.sleep(1)
    ctx.keys("alt+f4")
    ctx.gone(name="Settings", timeout=20)
    return was


def open_places(ctx):
    _places_before.update(set_places(ctx, dict.fromkeys(PLACES, True)))
    open_start(ctx)


def bottom_bar(ctx):
    return within_start(ctx, automationId="UserControl")


def close_places(ctx):
    close_start(ctx)
    set_places(ctx, dict(_places_before) or dict.fromkeys(PLACES, False))   # as the machine had them


def open_all_pinned(ctx):
    open_start(ctx)
    ctx.click(within_start(ctx, name="Show all pinned apps", className="Button"))
    time.sleep(SETTLE)


def pinned_grid(ctx):
    return within_start(ctx, automationId="StartMenuPinnedList")


SCROLL = 10004                     # UIA_ScrollPatternId
SCAN_STEPS = 80
APP_PREFIXES = ("P~", "W~")        # packaged and desktop entries; the rest is the AppID
# Read by open_apps, published by METRICS: every app the All list shows, in
# order, as (automation id, name, folder name or None).
_apps = []


def _kids(ctx, el):
    return ctx.live.children(el.el, ctx.live.uia.CreateTrueCondition())


def _scroll(ctx, el):
    return el.el.GetCurrentPattern(SCROLL).QueryInterface(ctx.live.UIA.IUIAutomationScrollPattern)


def _read_rows(ctx, rows, seen, folders, folder=None):
    for it in rows:
        if it.offscreen or it.control_type != "ListItem":
            continue
        if it.automation_id.startswith(APP_PREFIXES):
            if it.automation_id not in seen:
                seen.add(it.automation_id)
                _apps.append((it.automation_id, it.name, folder))
        elif folder is None and it.name not in folders:
            folders[it.name] = None          # a folder: opened after the scan


def open_apps(ctx):
    """Every app in the All list: the list view scrolled a page at a time (its
    rows below the fold are not on screen), then each folder opened (a folder
    opens a FolderModal window inside Start)."""
    _apps.clear()
    open_start(ctx)
    set_view(ctx, "List")
    seen, folders = set(), {}
    for _ in range(SCAN_STEPS):
        try:
            for g in _kids(ctx, apps_list(ctx)):
                if g.class_name == "GridViewHeaderItem":
                    _read_rows(ctx, _kids(ctx, g), seen, folders)
            sc = _scroll(ctx, apps_list(ctx))
            if sc.CurrentVerticalScrollPercent >= 99.9:
                break
            sc.Scroll(2, 3)          # ScrollAmount NoAmount across, LargeIncrement down
            time.sleep(0.8)
        except ctx.live.COMError:    # a row recycled under the read: read the page again
            time.sleep(1)
    else:
        raise core.Incomplete(f"the All list did not reach its end in {SCAN_STEPS} pages")
    for name in folders:
        _scroll(ctx, apps_list(ctx)).SetScrollPercent(-1, 0)
        time.sleep(1)
        row = _find_row(ctx, name)
        ctx.click(row)
        modal = ctx.find(within=start(ctx), className="StartMenu.FolderModal", depth=2, timeout=10)
        time.sleep(1)
        _read_rows(ctx, [k for k in _walk_rows(ctx, modal)], seen, folders, folder=name.split(" folder")[0])
        ctx.keys("esc")
        _modal_gone(ctx)
    if not _apps:
        raise core.Incomplete("the All list showed no apps")


def _modal_gone(ctx, wait=10):
    """A folder closes inside Start, where ctx.gone (top-level windows) cannot see it."""
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        try:
            ctx.find(within=start(ctx), className="StartMenu.FolderModal", depth=2, timeout=0)
        except core.Incomplete:
            return
        time.sleep(0.3)
    raise core.Incomplete(f"a Start folder stayed open past {wait} s")


def _find_row(ctx, name):
    for _ in range(SCAN_STEPS):
        for g in _kids(ctx, apps_list(ctx)):
            if g.class_name == "GridViewHeaderItem":
                for it in _kids(ctx, g):
                    if it.name == name and not it.offscreen:
                        return it
        sc = _scroll(ctx, apps_list(ctx))
        if sc.CurrentVerticalScrollPercent >= 99.9:
            break
        sc.Scroll(2, 3)
        time.sleep(0.8)
    raise core.Incomplete(f"the folder {name!r} was not found again in the All list")


def _walk_rows(ctx, el, depth=0):
    """The list items under a folder window, however deep its grid sits."""
    for k in _kids(ctx, el):
        if k.control_type == "ListItem":
            yield k
        elif depth < 6:
            yield from _walk_rows(ctx, k, depth + 1)


SCENES = [
    # Not expanded: the view dropdown opens its own menu (the view-menu scene), and
    # expanding it during the walk closed Start (2026-11-17).
    dict(slug=ROOT, label="Start", kind="surface", open=open_start, root=start, close=close_start, expand=False),
    dict(slug="power-menu", label="Power menu", kind="menu", open=menu_from({"automationId": "PowerButton"}),
         root=flyout_menu, close=close_menu),
    # The account's name and type are this machine's data.
    dict(slug="account-menu", label="Account menu", kind="region",
         open=menu_from({"automationId": "UserTileButton"}, root=account_flyout), root=account_flyout,
         close=close_menu, values=[{"className": "TextBlock"}], count=True),
    dict(slug="view-menu", label="All apps view menu", kind="menu",
         open=menu_from({"automationId": "ViewSelectionButton"}), root=flyout_menu, close=close_menu),
    dict(slug="pinned-menu", label="Pinned app menu", kind="contextMenu",
         open=menu_from({"className": "GridViewItem", "name": "Settings"}, "right"), root=flyout_menu,
         close=close_menu),
    dict(slug="recommended-menu", label="Recommended entry menu", kind="contextMenu",
         open=menu_from({"automationId": "RecommendedItemRoot0"}, "right"), root=flyout_menu, close=close_menu),
    dict(slug="entry-menu", label="All apps entry menu", kind="contextMenu", open=open_entry_menu, root=flyout_menu,
         close=close_entry_menu),
    dict(slug="letter-jump", label="Letter jump grid", kind="region", open=open_letters, root=letters,
         close=close_letters, expand=False),
    dict(slug="bottom-bar", label="Bottom bar with every folder", kind="region", open=open_places, root=bottom_bar,
         close=close_places),
    # Measured and not published: the views' row sizes, the panel aligned left,
    # and the pinned grid shown whole.
    dict(slug="grid-view", label="Grid view", kind="state", open=open_view("Grid"), root=apps_list, close=close_view,
         keep=False, expand=False),
    dict(slug="list-view", label="List view", kind="state", open=open_view("List"), root=apps_list, close=close_view,
         keep=False, expand=False),
    dict(slug="left", label="Start, taskbar aligned left", kind="state", open=open_left, root=panel, close=close_left,
         keep=False, expand=False),
    dict(slug="all-pinned", label="All pinned apps", kind="state", open=open_all_pinned, root=pinned_grid,
         close=close_start, keep=False, expand=False),
    dict(slug="all-apps", label="All apps", kind="state", open=open_apps, root=apps_list, close=close_view,
         keep=False, expand=False),
    # Last: the folder stays in the pinned grid for the rest of the run.
    dict(slug="pinned-folder", label="Pinned folder", kind="region", open=open_folder, root=folder_modal,
         close=close_folder),
    dict(slug="folder-menu", label="Pinned folder menu", kind="contextMenu", open=open_folder_menu, root=flyout_menu,
         close=close_menu),
]


def METRICS(records):
    """The panel's size and its place at centre and left alignment, the pinned
    grid's columns and rows, and the tile and row sizes of each view."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}

    def box(rid, what):
        r = by.get(p + rid)
        if r is None or "bounds" not in r:
            raise ValueError(f"{what} was not captured ({rid})")
        return r["bounds"]

    def kids(rid):
        return sorted((r for r in records if r.get("parent") == rid and "bounds" in r), key=lambda r: r["order"])

    out = []

    def metric(slug, label, value, order, unit="px"):
        # A length scales with the display; a count of columns or rows does not.
        out.append({"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": order,
                    "measure": {"value": value, "unit": unit, "scaling": "scales" if unit == "px" else "fixed"}})

    screen = box(ROOT, "Start")
    centre = box(f"{ROOT}.search", "the Start panel")
    left = box("left", "the Start panel aligned left")
    if (left["width"], left["height"]) != (centre["width"], centre["height"]):
        raise ValueError("the Start panel changed size with the taskbar's alignment")
    metric("panel-width", "Panel width", centre["width"], 0)
    metric("panel-height", "Panel height", centre["height"], 1)
    metric("panel-gap", "Gap between the panel and the taskbar", screen["height"] - centre["y"] - centre["height"], 2)
    metric("panel-x-centre", "Panel left edge, taskbar centred", centre["x"], 3)
    metric("panel-x-left", "Panel left edge, taskbar aligned left", left["x"], 4)

    # A letter group is its heading and its apps: one row of tiles in the grid
    # view, one row per app in the list view.
    def groups(view):
        out = [(g, kids(g["id"])) for g in kids(p + view) if g.get("className") == "GridViewHeaderItem"]
        out = [(g, apps) for g, apps in out if apps and all(a["bounds"]["height"] for a in apps)]
        if not out:
            raise ValueError(f"the {view} showed no letter group with its apps on screen")
        return out

    g, apps = groups("grid-view")[0]
    tile = apps[0]["bounds"]
    heading = tile["y"] - g["bounds"]["y"]
    g, apps = groups("list-view")[0]
    row = apps[0]["bounds"]["height"]
    if apps[0]["bounds"]["y"] - g["bounds"]["y"] != heading:
        raise ValueError("the letter heading is a different height in the grid and list views")
    grid = box(f"{ROOT}.toplevelroot.appslist.startmenupinnedlist.pinnedlist", "the pinned grid")
    metric("tile-width", "App tile width (pinned and grid view)", tile["width"], 10)
    metric("tile-height", "App tile height", tile["height"], 11)
    metric("pinned-columns", "Pinned grid columns", grid["width"] // tile["width"], 12, unit="count")
    metric("pinned-rows", "Pinned grid rows before Show all", grid["height"] // tile["height"], 13, unit="count")
    metric("list-row-height", "List view row height", row, 14)
    metric("group-heading-height", "Letter group heading height", heading, 15)
    rec = box(f"{ROOT}.toplevelroot.appslist.recommendedlist", "the recommended list")
    metric("recommended-row-height", "Recommended row height", rec["height"], 16)

    if not _letter_cells:
        raise ValueError("the letter grid's cells were not read")
    x0, y0, cw, ch = _letter_cells[0]
    metric("letter-cell-width", "Letter cell width", cw, 17)
    metric("letter-cell-height", "Letter cell height", ch, 18)
    metric("letter-columns", "Letter grid columns", sum(1 for b in _letter_cells if b[1] == y0), 19, unit="count")

    fgrid = box("pinned-folder.levelonegridview", "the folder's grid")
    metric("folder-columns", "Folder grid columns", fgrid["width"] // tile["width"], 26, unit="count")
    metric("folder-rows", "Folder grid rows", fgrid["height"] // tile["height"], 27, unit="count")

    whole = box("all-pinned", "the pinned grid shown whole")
    if whole["height"] <= grid["height"]:
        raise ValueError("Show all pinned apps did not show more of the grid")
    out.append({"id": f"{ROOT}.all-pinned", "parent": ROOT, "kind": "state", "label": "All pinned apps", "order": 20,
                "behaviour": "Show all pinned apps grows the pinned grid in place to every pinned app; the "
                             "columns stay the same."})
    for i, (slug, label, how) in enumerate([
            ("view-category", "Category view", "the default: apps in category tiles of up to four icons"),
            ("view-grid", "Grid view", "apps as tiles under letter headings"),
            ("view-list", "List view", "apps as one row each under letter headings")]):
        out.append({"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "state", "label": label, "order": 21 + i,
                    "behaviour": f"The All region in {label.lower()}: {how}; a letter heading opens the letter "
                                 "grid." if slug != "view-category" else f"The All region in {label.lower()}: {how}."})
    out.extend(app_records())
    return out


def app_records():
    """One record per app the All list showed, matched to its start-apps row
    (the row id is the AppID the entry's automation id carries)."""
    out = [{"id": f"{ROOT}.all-apps", "parent": ROOT, "kind": "region", "label": "All apps", "order": 30,
            "behaviour": "Every app Start lists, in the list view's order; a folder's apps follow it."}]
    labels = [name.split(", ")[0] for _, name, _ in _apps]    # the name and a status ("Calculator, New")
    for i, ((aid, _, folder), label) in enumerate(zip(_apps, labels)):
        # The id is the AppID, never the name or the list position: a second
        # app taking the name, a reordered list or an app replaced by another
        # of the same name moves no note onto the wrong app.
        slug = core.slug(aid[2:]) or core.slug(label) or "app"
        out.append({"id": f"{ROOT}.all-apps.{slug}", "parent": f"{ROOT}.all-apps", "kind": "control", "label": label,
                    "order": i, "uiaId": aid, "baselineId": f"start-apps:{aid[2:]}",
                    **({"behaviour": f"In the {folder} folder."} if folder else {})})
    return out


BEHAVIOURS = [
    ("type-to-search", "Type to search", "Typing with Start open moves the text into the search box and opens search."),
    ("drag-reorder", "Drag to reorder", "Dragging a pinned tile moves it; the others close up around it."),
    ("drag-folder", "Drag onto a tile", "Dropping a pinned tile on another makes a folder of the two, which opens "
                                         "as Making a folder describes."),
    ("drag-to-taskbar", "Drag to the taskbar", "Dragging a pinned tile onto the taskbar pins it there as well."),
    ("keyboard-regions", "Keyboard between regions", "Tab moves between the search box, the pinned grid, "
                                                      "recommended, all apps and the bottom bar; arrows move within one."),
    ("drag-out", "Drag out to unpin", "No drag unpins a tile: a tile dragged out of the grid returns to its "
                                      "place, and Unpin from Start is the tile menu's entry."),
    ("recommended-empty", "Recommended with nothing to show", "With every Recommended switch off and Start "
                                                               "restarted, the region still holds Get Started: this "
                                                               "build has no empty Recommended state."),
    ("folder-made", "Making a folder", "Create a new app folder on a tile's menu turns that tile into a folder "
                                       "UI Automation names after it (\"Folder with Settings\"); a click opens it over Start as a "
                                       "panel of about 448 by 377 titled Folder above its own grid of 4 by 3 tiles (the "
                                       "pinned-folder screenshot)."),
    ("folder-rename", "Renaming a folder", "Clicking the open folder's title turns it into an edit box of about 320 by 40 with "
                                           "an accent underline (F2 does nothing); UI Automation exposes neither the "
                                           "title nor the box on this build, so both are read from the screen."),
    ("close-keys", "Closing", "Esc, the Windows key, or a click outside closes Start."),
    ("letter-back", "Back from the letter grid", "Back, or choosing a letter, returns from the letter grid to the "
                                                  "list at that letter; letters with no apps are dimmed."),
]
HAND = [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "gesture", "label": label, "order": 40 + i,
         "behaviour": text} for i, (slug, label, text) in enumerate(BEHAVIOURS)]
