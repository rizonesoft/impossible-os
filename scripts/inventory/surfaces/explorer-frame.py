"""Surface module: the File Explorer frame.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 12 (File
Explorer frame). Writes docs/design/inventory/explorer/frame.json:

    python3 scripts/inventory/capture.py --surface explorer --file frame --module explorer-frame --prepare explorer:sample

One window, placed where window-chrome.py places every window, on the
fixture's sample folder (fixtures/explorer.ps1). The scenes walk its rows (the
tab row, the navigation row, the command bar, the body, the status bar), the
address bar's two forms and their lists, the command bar's menus and how the
bar changes with the selection and the folder, the navigation pane's options,
the details and preview panes and the tab row. The items view's rows are the
views capture's (explorer-views.py); here the list is one record.

Two scenes are measured and not published: the keyboard map (each key pressed,
and what took the focus or the place after it), and the namespace (the window
sent to shell:<name> for every known folder, and where it landed), which
METRICS turns into records matching the known-folders and shell-commands
baselines.
"""
import collections
import ctypes
import json
import time
from ctypes import wintypes

import capture_core as core

SURFACE = "explorer"
# The guest is Windows; the tests load this module on Linux for its pure parts.
user32 = getattr(ctypes, "WinDLL", None) and ctypes.WinDLL("user32", use_last_error=True)

PLACE = (240, 140, 1200, 720)            # where the window is put: x, y, width, height
TAB_ROW_POINT = (340, 18)                # from the window's corner: the tab row right of the new-tab button
NARROW = (240, 140, 640, 720)            # narrow enough that the breadcrumb hides its first segments
SETTLE = 1.5
WAIT = 20
SAMPLE = "C:\\Users\\ref\\Documents\\Explorer sample"
ARCHIVE = SAMPLE + "\\Sample archive.zip"
BIN = "shell:RecycleBinFolder"
HOME = "shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}"   # Home, the folder a new tab opens on
SPEC = {"className": "CabinetWClass"}
SW_RESTORE = 9
TABS_TO_OVERFLOW = 14                    # tabs that no longer fit the placed window's tab row
MENU_DEPTH = 6                           # levels from a focused entry up to its menu
SUGGEST_KEYS = ("d", "o")                # typed into the edit form: Documents, Downloads and more start so
SPLIT_TRIES, SPLIT_WAIT = 3, 5                # a segment list is asked to open this often, waited for this long
KEY_TRIES = 3                            # clicks that may be needed before Windows hands the window the foreground
CLOUD_ENTRY, CLOUD_WAIT = "OneDrive", 300   # the navigation pane's cloud entry, and how long sign-in may take to add it
# The class each side pane always has; the preview pane's reading module is
# there only with a file selected (2026-11-19).
PANES = {"details": "DetailsPaneHwndHostClass", "preview": "PreviewThumbnailInner"}
PANE_KEYS = {"details": "alt+shift+p", "preview": "alt+p"}
LIST_EMPTY_OFFSET = 20                   # px above the item list's bottom edge: below the sample folder's rows
LIST_DEPTH, REGION_DEPTH = 12, 16        # levels searched for the address list, and up from the focus for its row
# The frame's rows, by the automation id or class name of the element that holds each.
REGIONS = {"TabView": "tab row", "NavigationCommands": "navigation buttons", "PART_AutoSuggestBox": "address bar",
           "PART_BreadcrumbBar": "address bar", "FirstCrumbStackPanel": "address bar",
           "FileExplorerSearchBox": "search box", "FileExplorerCommandBar": "command bar",
           "FileExplorerSecondaryCommandBar": "command bar", "ProperTreeHost": "navigation pane",
           "DUIListView": "item list", "StatusBarModuleInner": "status bar",
           "DetailsPaneHwndHostClass": "details pane", "ReadingPaneModuleInner": "preview pane",
           "PreviewThumbnailInner": "preview pane"}
DRAG_STEPS, TEAR_OUT = 24, 240           # a tab drag's pointer steps, and how far below the row a tab is dropped
DRAG_HOVER = 0.8                         # seconds over the drop point before the button is let go
F6_STOPS = 8                             # F6 presses: more than the frame has panes, so the cycle shows

# The items view is the views capture's; here it is one record.
ENVIRONMENT_LISTS = [{"className": "UIItemsView"}]
# The command bar's buttons are an image and a label; the image needs no name.
LABELS = [({"controlType": "Image", "className": "Image"}, "Icon"),
          # The frame's rows carry an automation id and no name.
          ({"automationId": "TabView"}, "Tab row"), ({"automationId": "TabListView"}, "Tabs"),
          ({"automationId": "NavigationCommands"}, "Navigation buttons"),
          ({"automationId": "PART_AutoSuggestBox"}, "Address bar"), ({"automationId": "PART_BreadcrumbBar"}, "Breadcrumb"),
          ({"automationId": "FileExplorerSearchBox"}, "Search box"),
          ({"automationId": "FileExplorerCommandBar"}, "Command bar"),
          ({"automationId": "FileExplorerSecondaryCommandBar"}, "Pane button"),
          ({"automationId": "SuggestionsList"}, "Address list")]

_m = {}                                  # measured by the unpublished scenes, read by METRICS
_root = {}                               # the element the open scene's opener found, which its walks read


# ------------------------------------------------------------- the window --

def window(ctx):
    return ctx.find_window(timeout=WAIT, foreground=False, **SPEC)


def hwnd(ctx):
    return window(ctx).el.CurrentNativeWindowHandle


def place(ctx, rect=PLACE):
    h = hwnd(ctx)
    user32.ShowWindow(h, SW_RESTORE)
    user32.SetWindowPos(h, 0, *rect, 0x0004)                              # SWP_NOZORDER
    time.sleep(SETTLE)
    # Windows refuses SetForegroundWindow from a process in the background, and
    # then the keys a scene presses reach the desktop (2026-11-19: Alt-D focused
    # the desktop's Recycle Bin); a click on an empty part of the tab row is a
    # user's activation.
    x, y = rect[0] + TAB_ROW_POINT[0], rect[1] + TAB_ROW_POINT[1]
    for _ in range(KEY_TRIES):
        ctx.click_at(x, y)
        time.sleep(SETTLE)
        if user32.GetForegroundWindow() == h:
            return
    raise core.Incomplete("the File Explorer window did not come to the foreground")


def navigate(ctx, target):
    """Send the window to `target` (a path or a shell: name) the way its address
    bar does, through the window's own browser object, and wait until it is idle."""
    ctx.powershell(NAVIGATE % (hwnd(ctx), target.replace("'", "''")))
    time.sleep(SETTLE)


NAVIGATE = r"""
$w = @((New-Object -ComObject Shell.Application).Windows() | Where-Object { $_.HWND -eq %d })[0]
if (-not $w) { throw 'the File Explorer window is not a shell window' }
$w.Navigate2('%s')
$t0 = Get-Date
do { Start-Sleep -Milliseconds 200 } while ($w.Busy -and ((Get-Date) - $t0).TotalSeconds -lt 15)
"""


def close_all(ctx):
    """Close every File Explorer window. Each folder tab is a shell window
    object of its own and quits on its own: a window holding several tabs
    ignores WM_CLOSE (2026-11-19, the tab scenes)."""
    ctx.powershell("@((New-Object -ComObject Shell.Application).Windows()) | ForEach-Object { $_.Quit() }")
    time.sleep(SETTLE)
    # A Home tab is no shell window and stays (2026-11-19, the tab scenes):
    # Ctrl-W closes the window's tabs one by one, the way a user would.
    for _ in range(TABS_TO_OVERFLOW + 2):
        if not windows(ctx):
            return
        h = hwnd(ctx)
        place(ctx)
        ctx.keys("ctrl+w")
        time.sleep(0.5)
        if user32.IsWindow(h) and not windows(ctx):
            return
    ctx.gone(**SPEC)


def fresh(ctx, target=SAMPLE, rect=PLACE):
    """One File Explorer window, placed, on `target`, with nothing selected
    and neither side pane open. The panes are a setting File Explorer keeps
    across windows, so a scene that opened one left it open for the next
    (2026-11-19: the empty details pane scene toggled it shut)."""
    if windows(ctx):
        close_all(ctx)
    ctx.launch("explorer.exe", SAMPLE)
    window(ctx)
    time.sleep(3)
    place(ctx, rect)
    cloud_entry(ctx)
    set_pane(ctx, None)
    if target != SAMPLE:
        navigate(ctx, target)


def cloud_entry(ctx):
    """Wait for the navigation pane's cloud entry. OneDrive adds it some
    minutes after sign-in, so the first scenes of a run lacked it and the
    later ones had it (2026-11-19); a capture without it is not the reference."""
    if _root.get("cloud"):
        return
    tree = in_bar(ctx, automationId="100", controlType="Tree")
    ctx.find(within=tree, controlType="TreeItem", name=CLOUD_ENTRY, depth=3, timeout=CLOUD_WAIT)
    _root["cloud"] = True


def descendants(ctx, el, depth, skip=()):
    """Every element under `el` to `depth` levels, from the raw view, without
    going into an element whose class is in `skip`."""
    out, level = [], [el]
    for _ in range(depth):
        level = [k for e in level for k in ctx.live.children(e.el, ctx.live.uia.CreateTrueCondition())
                 if k.class_name not in skip]
        out.extend(level)
    return out


def shown_pane(ctx):
    """The side pane the window shows: "details", "preview" or None."""
    tab = ctx.find(within=window(ctx), className="ShellTabWindowClass", depth=2)
    names = {k.class_name for k in descendants(ctx, tab, 2)}
    found = [pane for pane, cls in PANES.items() if cls in names]
    return found[0] if found else None


def set_pane(ctx, want):
    """Open the details or the preview pane, or close both, with the pane's own
    key; a key that shows one pane replaces the other."""
    have = shown_pane(ctx)
    if have == want:
        return
    ctx.keys(PANE_KEYS[want or have])
    time.sleep(SETTLE * 2)
    if shown_pane(ctx) != want:
        raise core.Incomplete(f"the {want or have} pane key left the window showing {shown_pane(ctx) or 'no pane'}")


def items(ctx):
    return ctx.find(within=window(ctx), className="UIItemsView", depth=6)


def item(ctx, name):
    return ctx.find(within=items(ctx), controlType="ListItem", name=name, depth=2)


def in_bar(ctx, **spec):
    return ctx.find(within=window(ctx), depth=8, **spec)


def nothing(ctx):
    pass


# -------------------------------------------------------------- openers --

def at(target=SAMPLE, select=None, then=None, rect=PLACE):
    def open_(ctx):
        fresh(ctx, target, rect)
        if select:
            ctx.click(item(ctx, select))
            time.sleep(SETTLE)
        if then:
            then(ctx)
    return open_


def command_bar(ctx):
    return in_bar(ctx, automationId="FileExplorerCommandBar")


def menu(ctx, kinds=("Menu",), wait=WAIT):
    """The menu a command bar button or a breadcrumb opened: Explorer hosts it
    inside its own window, so it is found from the entry that took the focus
    (2026-11-19: no popup window opens, and the focus is on the first entry)."""
    deadline = time.monotonic() + wait
    while True:
        el = ctx.live.uia.GetFocusedElement()
        for _ in range(MENU_DEPTH):
            w = ctx.live.wrap(el)
            if w.control_type in kinds:
                return w
            up = ctx.live._parent(el)
            if up is None:
                break
            el = up[1]
        # The command bar's overflow opens with the focus left on its button
        # (2026-11-19, "Less app bar"): the menu is then the one open in the window.
        try:
            return ctx.find(within=window(ctx), controlType="Menu", depth=12, timeout=0)
        except core.Incomplete:
            pass
        if time.monotonic() > deadline:
            raise core.Incomplete(f"no {' or '.join(kinds)} holds the focus within {wait} s (focus: {focused(ctx)})")
        time.sleep(0.5)


def press_more(ctx):
    ctx.click(in_bar(ctx, automationId="MoreButton"))
    _root["el"] = overflow(ctx)


def press_button(**spec):
    def then(ctx):
        ctx.click(in_bar(ctx, **spec))
        _root["el"] = menu(ctx)
    return then


def overflow(ctx):
    """The command bar's See more list: a popup inside the window's island
    (2026-11-19: a Window named Popup, OverflowPopup, holding the entries)."""
    return ctx.find(within=window(ctx), automationId="OverflowPopup", depth=6)


def opened(ctx):
    """The root the scene's opener found: a list that keeps the focus only
    until it is first read (2026-11-19, the address history) is not looked for again."""
    return _root["el"]


def close_menu(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)


def open_split(ctx, el):
    """Open a breadcrumb segment's list through its ExpandCollapse pattern. The
    first Expand after a layout change can open nothing (2026-11-19, the
    narrow window's first segment), so it is asked again, a bounded number of times."""
    for attempt in range(1, SPLIT_TRIES + 1):
        ctx.expand_only(el)
        try:
            _root["el"] = menu(ctx, wait=SPLIT_WAIT)
            return
        except core.Incomplete:
            if attempt == SPLIT_TRIES:
                raise


def open_crumb(name):
    def then(ctx):
        open_split(ctx, ctx.find(within=in_bar(ctx, automationId="PART_BreadcrumbBar"), controlType="SplitButton",
                                 name=name, depth=2))
    return then


def open_first_crumb(ctx):
    open_split(ctx, in_bar(ctx, automationId="FirstCrumbStackPanel"))


def address(ctx):
    return in_bar(ctx, automationId="PART_AutoSuggestBox")


def edit_form(ctx):
    """Alt-D: the address bar's edit form, with the focus in it."""
    ctx.keys("alt+d")
    time.sleep(SETTLE)
    f = focused(ctx)
    if (f["name"], f["controlType"]) != ("Address Bar", "Edit"):
        raise core.Incomplete(f"Alt-D did not move the focus to the address bar (focus: {f})")


def history(ctx):
    edit_form(ctx)
    ctx.keys("f4")
    time.sleep(SETTLE)
    _root["el"] = address_list(ctx)


def suggest(ctx):
    """Type the start of a folder's name into the edit form: the list of
    places it could complete to opens under it (setting the text through UI
    Automation opens none, 2026-11-19; typed keys do)."""
    edit_form(ctx)
    for key in SUGGEST_KEYS:
        ctx.keys(key)
        time.sleep(0.4)
    time.sleep(SETTLE)
    _root["el"] = address_list(ctx)


def address_list(ctx):
    """The address bar's list, history or suggestions: the AutoSuggestBox's
    own list, in a popup inside the window (2026-11-19); the focus stays in the box."""
    return ctx.find(within=window(ctx), automationId="SuggestionsList", controlType="List", depth=12)


def esc_twice(ctx):
    ctx.keys("esc")
    ctx.keys("esc")
    time.sleep(SETTLE)


def pane_menu(ctx):
    """Right-click the navigation pane below its last entry: the pane's own menu."""
    tree = in_bar(ctx, automationId="100", controlType="Tree")
    x, y, w, h = tree.bounds
    ctx.click_at(x + w // 2, y + h - 12, button="right")
    time.sleep(SETTLE)
    # A command bar flyout whose entries are all in its overflow list
    # (2026-11-19: an empty bar of 48 px and the five options under OverflowPopup).
    _root["el"] = overflow(ctx)


def details_pane(ctx):
    set_pane(ctx, "details")


def preview_pane(ctx):
    set_pane(ctx, "preview")


def tab_row(ctx):
    return in_bar(ctx, automationId="TabView")


def new_tab(ctx):
    ctx.keys("ctrl+t")
    time.sleep(SETTLE)


def many_tabs(ctx):
    """More tabs than the row holds; the tabs on screen are measured here, since
    the scene keeps the tab list as one record (every new tab is a Home tab, and
    fourteen tabs of one name give ids no note can be kept on)."""
    for _ in range(TABS_TO_OVERFLOW):
        ctx.keys("ctrl+t")
        time.sleep(0.5)
    time.sleep(SETTLE)
    _m["overflow"] = [t.bounds[2] for t in descendants(ctx, tab_row(ctx), 2)
                      if t.control_type == "TabItem" and not t.offscreen]


# ------------------------------------------------------- measured scenes --

def focused(ctx):
    el = ctx.live.wrap(ctx.live.uia.GetFocusedElement())
    return {"name": el.name, "automationId": el.automation_id, "className": el.class_name,
            "controlType": el.control_type}


def title(ctx):
    return window(ctx).name


def windows(ctx):
    return sum(1 for w in ctx.live.children(ctx.live.uia.GetRootElement()) if core.matches(w, SPEC))


def tabs(ctx):
    return sum(1 for t in tab_row(ctx).children() if t.control_type == "List"
               for _ in t.children())


def address_list_open(ctx):
    # The item list and the folder tree are hundreds of rows and hold no popup.
    return any(k.automation_id == "SuggestionsList"
               for k in descendants(ctx, window(ctx), LIST_DEPTH, skip=("UIItemsView", "SysTreeView32")))


def focus_region(ctx):
    """The row of the frame the focus is in: the automation id or class of the
    nearest ancestor that names one of the frame's rows."""
    el = ctx.live.uia.GetFocusedElement()
    for _ in range(REGION_DEPTH):
        w = ctx.live.wrap(el)
        for key in (w.automation_id, w.class_name):
            if key in REGIONS:
                return REGIONS[key]
        up = ctx.live._parent(el)
        if up is None:
            break
        el = up[1]
    return None


def focus_list(ctx):
    """Click the item list below its last row: the focus in the list, nothing
    selected. Clicking an item that is already selected starts renaming it
    (2026-11-19: Alt+P went into the rename box), so no key starts from that."""
    x, y, w, h = items(ctx).bounds
    ctx.click_at(x + w // 2, y + h - LIST_EMPTY_OFFSET)
    time.sleep(0.5)


def measure_keys(ctx):
    """Press each key of the frame's keyboard map from the item list and keep
    what it did: where the focus went, what opened, how the tabs, the windows
    or the folder changed."""
    fresh(ctx)
    k = {}
    for chord in ("alt+d", "ctrl+l", "ctrl+e", "f4"):
        focus_list(ctx)
        ctx.keys(chord)
        time.sleep(SETTLE)
        k[chord] = {"region": focus_region(ctx), "focus": focused(ctx), "list": address_list_open(ctx)}
        esc_twice(ctx)
    for chord, pane in (("alt+p", "preview"), ("alt+shift+p", "details")):
        focus_list(ctx)
        ctx.keys(chord)
        time.sleep(SETTLE * 2)
        opened_ = shown_pane(ctx)
        ctx.keys(chord)
        time.sleep(SETTLE * 2)
        k[chord] = {"opens": opened_, "then": shown_pane(ctx)}
    # One pane's key while the other pane shows.
    set_pane(ctx, "details")
    ctx.keys("alt+p")
    time.sleep(SETTLE * 2)
    k["swap"] = {"details-then-alt+p": shown_pane(ctx)}
    set_pane(ctx, None)
    focus_list(ctx)
    before = tabs(ctx)
    ctx.keys("ctrl+t")
    time.sleep(SETTLE)
    k["ctrl+t"] = {"tabs": [before, tabs(ctx)], "new": tab_names(ctx)[-1]}
    before = tabs(ctx)
    ctx.keys("ctrl+w")
    time.sleep(SETTLE)
    k["ctrl+w"] = {"tabs": [before, tabs(ctx)]}
    before = windows(ctx)
    ctx.keys("ctrl+n")
    time.sleep(3)
    k["ctrl+n"] = {"windows": [before, windows(ctx)]}
    close_all(ctx)
    fresh(ctx)
    start = title(ctx)
    ctx.click(item(ctx, "General items"))
    ctx.keys("enter")
    time.sleep(SETTLE)
    inside = title(ctx)
    ctx.keys("alt+left")
    time.sleep(SETTLE)
    back = title(ctx)
    ctx.keys("alt+right")
    time.sleep(SETTLE)
    forward = title(ctx)
    ctx.keys("alt+up")
    time.sleep(SETTLE)
    k["alt+left"] = {"from": inside, "to": back, "start": start}
    k["alt+right"] = {"from": back, "to": forward, "inside": inside}
    k["alt+up"] = {"from": forward, "to": title(ctx), "start": start}
    focus_list(ctx)
    stops = []
    for _ in range(F6_STOPS):
        ctx.keys("f6")
        time.sleep(0.6)
        stops.append(focus_region(ctx))
    k["f6"] = {"stops": stops}
    _m["keys"] = k
    measure_tab_drag(ctx)


def tab_names(ctx):
    return [t.name for t in descendants(ctx, tab_row(ctx), 2) if t.control_type == "TabItem"]


def drag(ctx, start, end):
    """Hold the left button at `start` and move to `end` in steps a drag listener sees."""
    (x0, y0), (x1, y1) = start, end
    ctx.hover_point(x0, y0)
    user32.mouse_event(0x0002, 0, 0, 0, 0)                               # left button down
    time.sleep(0.3)
    # Relative moves only, as a mouse sends them: a warp of the pointer is no drag.
    px, py = x0, y0
    for step in range(1, DRAG_STEPS + 1):
        nx, ny = x0 + (x1 - x0) * step // DRAG_STEPS, y0 + (y1 - y0) * step // DRAG_STEPS
        user32.mouse_event(0x0001, nx - px, ny - py, 0, 0)               # MOUSEEVENTF_MOVE, relative
        px, py = nx, ny
        time.sleep(0.05)
    time.sleep(DRAG_HOVER)                                               # over the target before the drop
    user32.mouse_event(0x0004, 0, 0, 0, 0)                               # left button up
    time.sleep(SETTLE)


def measure_tab_drag(ctx):
    """Drag the second tab down out of the row and count the windows. (A drag
    along the row tore the tab out too under synthesized input, 2026-11-19, so
    the reorder is a hand record.)"""
    fresh(ctx)
    ctx.keys("ctrl+t")
    time.sleep(SETTLE)
    row = [t for t in descendants(ctx, tab_row(ctx), 2) if t.control_type == "TabItem"]
    if len(row) != 2:
        raise core.Incomplete(f"Ctrl+T left {len(row)} tabs, not two")
    x, y, w, h = row[1].bounds
    before = windows(ctx)
    drag(ctx, (x + w // 2, y + h // 2), (x + w // 2, y + TEAR_OUT))
    _m["tabs"] = {"windows": [before, windows(ctx)], "left": tab_names(ctx)}
    fresh(ctx)                           # one window again: the scene's walk reads it


NAMESPACE = r"""
$names = '%s' | ConvertFrom-Json
$sh = New-Object -ComObject Shell.Application
$out = foreach ($n in $names) {
    $f = $null
    try { $f = $sh.Namespace("shell:$n") } catch { }
    if ($f) { [pscustomobject]@{ name = $n; opens = $true; title = $f.Title; path = $f.Self.Path } }
    else { [pscustomobject]@{ name = $n; opens = $false } }
}
ConvertTo-Json -InputObject @($out) -Compress
"""


def measure_namespace(ctx):
    """Resolve shell:<name> for every known folder through the shell namespace,
    the parser the address bar hands a typed shell: name to, and keep the
    folder it names (its title and path) or that it names none. One window
    navigation per name took past 900 s for the 134 names (2026-11-19)."""
    fresh(ctx)
    names = [r["name"] for r in _known_folders()]
    out = ctx.powershell(NAMESPACE % json.dumps(names).replace("'", "''"), timeout=300)
    _m["namespace"] = json.loads(out.strip().splitlines()[-1])


KNOWN_FOLDERS = r"""
Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FolderDescriptions' |
  ForEach-Object { [pscustomobject]@{ id = $_.PSChildName; name = (Get-ItemProperty $_.PSPath).Name } } |
  ConvertTo-Json -Compress
"""
_kf = []


def _known_folders():
    """The known folders, read from the same registry key the baseline was read from."""
    return _kf


def read_known_folders(ctx):
    _kf[:] = [r for r in json.loads(ctx.powershell(KNOWN_FOLDERS).strip()) if r.get("name")]


def measuring(fn):
    def open_(ctx):
        if not _kf:
            read_known_folders(ctx)
        fn(ctx)
    return open_


# --------------------------------------------------------------- scenes --

SCENES = [
    dict(slug="frame", label="File Explorer", kind="surface", open=at(), root=window, close=nothing, expand=False),
    dict(slug="file-selected", label="File Explorer, a file selected", kind="state", open=at(select="Sample document"),
         root=window, close=nothing, expand=False),
    dict(slug="folder-selected", label="File Explorer, a folder selected", kind="state",
         open=at(select="General items"), root=window, close=nothing, expand=False),
    dict(slug="archive-command-bar", label="Command bar in a compressed folder", kind="state", open=at(ARCHIVE),
         root=command_bar, close=nothing, expand=False),
    dict(slug="recycle-bin-command-bar", label="Command bar in the Recycle Bin", kind="state", open=at(BIN),
         root=command_bar, close=nothing, expand=False),
    # Home's bar carries Filter, which a folder's bar does not (2026-11-19).
    dict(slug="home-command-bar", label="Command bar on Home", kind="state", open=at(HOME), root=command_bar,
         close=nothing, expand=False),
    dict(slug="new-menu", label="New menu", kind="menu", open=at(then=press_button(controlType="Button", name="New")),
         root=opened, close=close_menu),
    dict(slug="sort-menu", label="Sort menu", kind="menu",
         open=at(then=press_button(automationId="SortAndGroupButton")), root=opened, close=close_menu),
    dict(slug="view-menu", label="View menu", kind="menu",
         open=at(then=press_button(controlType="Button", name="View")), root=opened, close=close_menu),
    dict(slug="see-more-menu", label="See more menu", kind="menu", open=at(then=press_more),
         root=opened, close=close_menu),
    dict(slug="crumb-menu", label="Breadcrumb segment list", kind="menu", open=at(then=open_crumb("Documents")),
         root=opened, close=close_menu),
    dict(slug="crumb-overflow", label="Breadcrumb overflow list", kind="menu",
         open=at(then=open_first_crumb, rect=NARROW), root=opened, close=close_menu),
    dict(slug="address-edit", label="Address bar, edit form", kind="state", open=at(then=edit_form), root=address,
         close=esc_twice, expand=False),
    dict(slug="address-history", label="Address bar history list", kind="menu", open=at(then=history), root=opened,
         close=esc_twice, expand=False),
    dict(slug="address-suggestions", label="Address bar suggestions", kind="menu", open=at(then=suggest), root=opened,
         close=esc_twice, expand=False),
    dict(slug="navigation-pane-menu", label="Navigation pane options", kind="contextMenu", open=at(then=pane_menu),
         root=opened, close=close_menu),
    dict(slug="details-pane", label="Details pane", kind="region", open=at(select="Sample document", then=details_pane),
         root=window, close=nothing, expand=False),
    dict(slug="details-pane-empty", label="Details pane, nothing selected", kind="state", open=at(then=details_pane),
         root=window, close=nothing, expand=False),
    dict(slug="preview-pane-empty", label="Preview pane, nothing selected", kind="state", open=at(then=preview_pane),
         root=window, close=nothing, expand=False),
    dict(slug="preview-pane", label="Preview pane", kind="region", open=at(select="Sample document", then=preview_pane),
         root=window, close=nothing, expand=False),
    dict(slug="two-tabs", label="Tab row, two tabs", kind="state", open=at(then=new_tab), root=tab_row, close=nothing,
         expand=False),
    dict(slug="tab-overflow", label="Tab row, overflowing", kind="state", open=at(then=many_tabs), root=tab_row,
         close=nothing, expand=False, environment_lists=[{"automationId": "TabListView"}] + ENVIRONMENT_LISTS),
    dict(slug="keys", label="Keyboard map", kind="state", open=measuring(measure_keys), root=window, close=close_all,
         keep=False, expand=False),
    dict(slug="namespace", label="Namespace", kind="state", open=measuring(measure_namespace), root=command_bar,
         close=nothing, keep=False, expand=False),
]


P = f"win11.{SURFACE}."
ROOT = "frame"
PROFILE_DIR = "C:\\Users\\ref"           # the reference account's profile, written as the variable a path holds


def METRICS(records):
    """The frame's sizes read off the captured rows, the keyboard map from the
    keys scene, and the known folders from the namespace scene."""
    by = {r["id"]: r for r in records}

    def box(rid):
        r = by.get(P + rid)
        if r is None or "bounds" not in r:
            raise ValueError(f"{rid} was not captured")
        return r["bounds"]

    def kids(rid):
        return sorted((r for r in records if r.get("parent") == P + rid and "bounds" in r), key=lambda r: r["order"])

    out = []

    def metric(slug, label, value, order):
        out.append({"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": 200 + order,
                    "measure": {"value": value, "unit": "px", "scaling": "scales"}})

    tab = box("frame.tabview.tablistview.explorer-sample")
    close = box("frame.tabview.tablistview.explorer-sample.closebutton")
    add = box("frame.tabview.addbutton")
    # Overflowing tabs shrink to one width; the first and last can be cut by the scroll.
    narrow = collections.Counter(_m.get("overflow") or ()).most_common(1)
    if not narrow:
        raise ValueError("the overflowing tab row held no tabs")
    rows = [
        ("tab-row-height", "Tab row height", box("frame.tabview")["height"]),
        ("tab-width", "Tab width", tab["width"]),
        ("tab-height", "Tab height", tab["height"]),
        ("tab-min-width", "Tab width when the row overflows", narrow[0][0]),
        ("tab-close-width", "Tab close button width", close["width"]),
        ("tab-close-height", "Tab close button height", close["height"]),
        ("new-tab-width", "New tab button width", add["width"]),
        ("navigation-button-size", "Back, forward, up and refresh button size", box("frame.navigationcommands.backbutton")["width"]),
        ("address-bar-height", "Address bar height", box("frame.part-autosuggestbox")["height"]),
        ("search-box-width", "Search box width", box("frame.fileexplorersearchbox")["width"]),
        ("command-bar-height", "Command bar height", box("frame.fileexplorercommandbar")["height"]),
        ("command-icon-width", "Command bar icon button width", box("frame.fileexplorercommandbar.cut")["width"]),
        ("navigation-pane-width", "Navigation pane width", box("frame.explorer-sample.propertreehost")["width"]),
        ("navigation-entry-height", "Navigation pane entry height",
         box("frame.explorer-sample.propertreehost.navigation-pane.desktop.home")["height"]),
        ("status-bar-height", "Status bar height", box("frame.explorer-sample.statusbarmoduleinner")["height"]),
        ("view-switch-size", "Status bar view button size",
         box("frame.explorer-sample.statusbarmoduleinner.viewbuttonsgroup.viewmode-details")["width"]),
        ("details-pane-width", "Details pane width", box("details-pane.explorer-sample.details-pane")["width"]),
        ("preview-pane-width", "Preview pane width", box("preview-pane.explorer-sample.readingpanemoduleinner")["width"]),
    ]
    history = kids("address-history")
    options = kids("navigation-pane-menu")
    if not history or not options:
        raise ValueError("the address history or the navigation pane options were not captured")
    rows += [("address-list-row-height", "Address bar list row height", history[0]["bounds"]["height"]),
             ("pane-option-height", "Navigation pane option height", options[0]["bounds"]["height"])]
    for i, (slug, label, value) in enumerate(rows):
        metric(slug, label, value, i)
    out.extend(key_records(_m.get("keys")))
    out.extend(tab_drag_records(_m.get("tabs")))
    out.extend(folder_records(_m.get("namespace"), _known_folders()))
    return out


def chord_name(chord):
    return "+".join(k.capitalize() if len(k) > 1 else k.upper() for k in chord.split("+"))


def key_records(k):
    """One shortcut record per key of the frame's map, its behaviour written
    from what the keys scene saw; a key that did not do what the map says
    stops the capture, since that is the walk failing, not Windows."""
    if not k:
        raise ValueError("the keys scene measured nothing")

    def need(cond, chord):
        if not cond:
            raise ValueError(f"{chord}: the measured effect is not the frame's ({k.get(chord)})")

    for chord in ("alt+d", "ctrl+l"):
        need(k[chord]["region"] == "address bar" and k[chord]["focus"]["controlType"] == "Edit", chord)
    need(k["ctrl+e"]["region"] == "search box", "ctrl+e")
    need(k["f4"]["region"] == "address bar" and k["f4"]["list"], "f4")
    for chord, pane in (("alt+p", "preview"), ("alt+shift+p", "details")):
        need(k[chord]["opens"] == pane and k[chord]["then"] is None, chord)
    need(k["swap"]["details-then-alt+p"] == "preview", "swap")
    a, b = k["ctrl+t"]["tabs"]
    need(b == a + 1, "ctrl+t")
    a, b = k["ctrl+w"]["tabs"]
    need(b == a - 1, "ctrl+w")
    a, b = k["ctrl+n"]["windows"]
    need(b == a + 1, "ctrl+n")
    need(k["alt+left"]["to"] == k["alt+left"]["start"], "alt+left")
    need(k["alt+right"]["to"] == k["alt+right"]["inside"], "alt+right")
    need(k["alt+up"]["to"] == k["alt+up"]["start"], "alt+up")
    stops = k["f6"]["stops"]
    need(None not in stops and stops[0] in stops[1:], "f6")
    cycle = stops[:stops.index(stops[0], 1)]
    new_tab = k["ctrl+t"]["new"]
    keys = [
        ("alt+d", "Address bar", "Moves the focus into the address bar's edit form with the path selected."),
        ("ctrl+l", "Address bar", "Moves the focus into the address bar's edit form with the path selected, as Alt+D does."),
        ("ctrl+e", "Search box", "Moves the focus into the search box."),
        ("f4", "Address bar history", "Opens the address bar's edit form with its history list."),
        ("alt+p", "Preview pane", "Shows the preview pane, or hides it when it shows; it replaces the details pane."),
        ("alt+shift+p", "Details pane", "Shows the details pane, or hides it when it shows; it replaces the preview pane."),
        ("ctrl+t", "New tab", f"Opens a new tab on {new_tab} and switches to it."),
        ("ctrl+w", "Close tab", "Closes the current tab; with one tab left it closes the window."),
        ("ctrl+n", "New window", "Opens a new File Explorer window."),
        ("alt+left", "Back", "Goes back to the previous folder."),
        ("alt+right", "Forward", "Goes forward again after Back."),
        ("alt+up", "Up", "Goes to the folder that holds this one."),
        ("f6", "Next pane", f"Moves the focus to the next part of the window, in a cycle: {', '.join(cycle)}."),
    ]
    return [{"id": f"{ROOT}.keys.{core.slug(chord)}", "parent": ROOT, "kind": "shortcut", "label": label,
             "order": 100 + i, "shortcut": chord_name(chord), "shortcutScope": "window", "behaviour": how}
            for i, (chord, label, how) in enumerate(keys)]


def folder_records(namespace, known):
    """Each known folder the namespace opened: a page record matched to its
    known-folders row, and under it its shell: name matched to its
    shell-commands row. A name that opens nothing on a new profile has no
    record; REFERENCE.md documents its rows as absent."""
    if not namespace or not known:
        raise ValueError("the namespace scene or the known folders were not read")
    # The baseline's row id is the folder id lowercased (baseline_core.known_folder_rows);
    # the registry keeps each key in its own case (2026-11-19: 83 matches named no row).
    ids = {r["name"]: r["id"].lower() for r in known}
    if len(ids) != len(known):
        raise ValueError("two known folders share a name")
    out = [{"id": f"{ROOT}.known-folders", "parent": ROOT, "kind": "group", "label": "Known folders", "order": 300,
            "behaviour": "The folders Windows names by id; each opens by its shell: name in the address bar."}]
    seen = set()
    for i, f in enumerate(sorted((f for f in namespace if f["opens"]), key=lambda f: f["name"].lower())):
        slug = core.slug(f["name"])
        if slug in seen:
            raise ValueError(f"two known folder names make the id {slug}")
        seen.add(slug)
        where = f["path"].replace(PROFILE_DIR, "%USERPROFILE%")
        rid = f"{ROOT}.known-folders.{slug}"
        out.append({"id": rid, "parent": f"{ROOT}.known-folders", "kind": "page", "label": f["title"], "order": i,
                    "baselineId": f"known-folders:{ids[f['name']]}", "behaviour": f"Opens {where}."})
        out.append({"id": f"{rid}.shell-name", "parent": rid, "kind": "shortcut", "label": f"shell:{f['name']}",
                    "order": 0, "shortcut": f"shell:{f['name']}", "shortcutScope": "control",
                    "baselineId": f"shell-commands:folder:{f['name']}",
                    "behaviour": "Typed into the address bar, opens the folder."})
    return out


def tab_drag_records(t):
    if not t:
        raise ValueError("the tab drag was not measured")
    a, b = t["windows"]
    if b != a + 1 or len(t["left"]) != 1:
        raise ValueError(f"dropping a tab below the row left {b} windows (from {a}) and the tabs {t['left']}")
    return [{"id": f"{ROOT}.tab-tear-out", "parent": ROOT, "kind": "gesture", "label": "Drag a tab out", "order": 150,
             "behaviour": "Dropping a dragged tab outside the tab row opens it in a window of its own."}]


HAND = [
    {"id": f"{ROOT}.tab-reorder", "parent": ROOT, "kind": "gesture", "label": "Drag a tab along the row", "order": 153,
     "behaviour": "Dragging a tab along the tab row and dropping it there moves it to that place."},
    {"id": f"{ROOT}.pane-resize", "parent": ROOT, "kind": "gesture", "label": "Resize a side pane", "order": 151,
     "behaviour": "Dragging the edge between the item list and the navigation, details or preview pane resizes that pane."},
    {"id": f"{ROOT}.tab-scroll", "parent": ROOT, "kind": "state", "label": "Tabs past the row's end", "order": 154,
     "behaviour": "When the tabs no longer fit, each shrinks to its narrow width and the tab list scrolls, with a "
                  "scroll button at each end of it."},
    {"id": f"{ROOT}.linux-entry", "parent": ROOT, "kind": "state", "label": "Linux entry", "order": 152,
     "behaviour": "With the Windows Subsystem for Linux installed, a Linux entry follows Network in the navigation "
                  "pane; the reference machine has none installed."},
]


