"""Helpers the File Explorer surface modules share: one File Explorer window,
placed and on a folder of the fixture's sample tree (fixtures/explorer.ps1), its
side panes, its menus and the facts a scene reads from it.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 12. Staged
into the guest with every surface module (capture.py run_guest); a module
imports it by name, since capture-ui puts this folder on the path.
"""
import ctypes
import time

import capture_core as core

# The guest is Windows; the tests load this module on Linux for its pure parts.
user32 = getattr(ctypes, "WinDLL", None) and ctypes.WinDLL("user32", use_last_error=True)
PLACE = (240, 140, 1200, 720)            # where the window is put: x, y, width, height
TAB_ROW_POINT = (340, 18)                # from the window's corner: the tab row right of the new-tab button
SETTLE = 1.5
WAIT = 20
LAUNCH_WAIT = 60                         # s for a new File Explorer window to show
SAMPLE = "C:\\Users\\ref\\Documents\\Explorer sample"
HOME = "shell:::{f874310e-b6b7-47dc-bc84-b9e6b38f5903}"   # Home, the folder a new tab opens on
SPEC = {"className": "CabinetWClass"}
SW_RESTORE = 9
TABS_TO_OVERFLOW = 14                    # tabs that no longer fit the placed window's tab row
MENU_SEARCH_DEPTH = 12                   # levels from the window to an open flyout's menu
MENU_SEARCH_SKIP = ("UIItemsView", "SysTreeView32")   # hundreds of rows, never a menu
MENU_DEPTH = 6                           # levels from a focused entry up to its menu
DRAG_STEPS = 24                          # pointer steps of a drag
DRAG_HOVER = 0.8                         # seconds over the drop point before the button is let go
# Down and up flags of mouse_event per button.
DRAG_BUTTONS = {"left": (0x0002, 0x0004), "right": (0x0008, 0x0010)}
KEY_TRIES = 3                            # clicks that may be needed before Windows hands the window the foreground
CLOUD_ENTRY, CLOUD_WAIT = "OneDrive", 300   # the navigation pane's cloud entry, and how long sign-in may take to add it
# The class each side pane always has; the preview pane's reading module is
# there only with a file selected (2026-11-19).
PANES = {"details": "DetailsPaneHwndHostClass", "preview": "PreviewThumbnailInner"}
PANE_KEYS = {"details": "alt+shift+p", "preview": "alt+p"}
LIST_EMPTY_OFFSET = 20                   # px above the item list's bottom edge: below the sample folder's rows
# A date a pane or a column shows: Windows writes a short date with left-to-right
# marks (U+200E) around its parts, and no fixed label starts with one. It is a
# live value (the sample files' times), so it names no id (2026-11-19: the
# details pane's "Date modified" value gave an id that moved every run).
DATE_VALUE = {"className": "TextBlock", "namePrefix": "\u200e"}
_root = {}                               # the element the open scene's opener found, which its walks read


def window(ctx, wait=WAIT):
    """The File Explorer window; wait=0 is one look, for a poll with its own deadline."""
    return ctx.find_window(timeout=wait, foreground=False, **SPEC)


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
    # A new File Explorer process can take past WAIT to show its window on a
    # loaded host (2026-11-19: 20 s passed with the window just appearing).
    ctx.find_window(timeout=LAUNCH_WAIT, foreground=False, **SPEC)
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
    # One budget for the whole look, charged as each parent's children come and
    # before the skip filter, as the walk's is (review, 2026-11-21): a wide tree
    # is refused rather than read whole.
    out, level, fetched = [], [el], 0
    cond = ctx.live.uia.CreateTrueCondition()
    for _ in range(depth):
        below = []
        for e in level:
            left = core.MAX_RECORDS - fetched
            if left <= 0:
                raise core.Incomplete(f"{el.name or el.class_name!r} holds more than {core.MAX_RECORDS} elements")
            kids = ctx.live.children(e.el, cond, left)
            fetched += len(kids)
            below.extend(k for k in kids if k.class_name not in skip)
        out.extend(below)
        if not below:
            break
        level = below
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
            if w.control_type in kinds and not w.offscreen:
                return w
            up = ctx.live._parent(el)
            if up is None:
                break
            el = up[1]
        # The command bar's overflow opens with the focus left on its button
        # (2026-11-19, "Less app bar"): the menu is then the one open in the
        # window. Only one on screen: a closed flyout's menu stays in the tree,
        # offscreen, and was taken for the next menu before that one opened
        # (2026-11-19: the second Group by click went to the first menu's place).
        shown = [k for k in descendants(ctx, window(ctx), MENU_SEARCH_DEPTH, skip=MENU_SEARCH_SKIP)
                 if k.control_type == "Menu" and not k.offscreen]
        if shown:
            return shown[0]
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


def esc_twice(ctx):
    ctx.keys("esc")
    ctx.keys("esc")
    time.sleep(SETTLE)


def details_pane(ctx):
    set_pane(ctx, "details")


def preview_pane(ctx):
    set_pane(ctx, "preview")


def tab_row(ctx):
    return in_bar(ctx, automationId="TabView")


def new_tab(ctx):
    ctx.keys("ctrl+t")
    time.sleep(SETTLE)


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


def focus_list(ctx):
    """Click the item list below its last row: the focus in the list, nothing
    selected. Clicking an item that is already selected starts renaming it
    (2026-11-19: Alt+P went into the rename box), so no key starts from that."""
    x, y, w, h = items(ctx).bounds
    ctx.click_at(x + w // 2, y + h - LIST_EMPTY_OFFSET)
    time.sleep(0.5)


def tab_names(ctx):
    return [t.name for t in descendants(ctx, tab_row(ctx), 2) if t.control_type == "TabItem"]


def drag(ctx, start, end, button="left"):
    """Hold a mouse button at `start` and move to `end` in steps a drag listener
    sees, then let it go. A right-button drag ends in the drop menu."""
    down, up = DRAG_BUTTONS[button]
    (x0, y0), (x1, y1) = start, end
    ctx.hover_point(x0, y0)
    user32.mouse_event(down, 0, 0, 0, 0)
    time.sleep(0.3)
    # Relative moves only, as a mouse sends them: a warp of the pointer is no drag.
    px, py = x0, y0
    for step in range(1, DRAG_STEPS + 1):
        nx, ny = x0 + (x1 - x0) * step // DRAG_STEPS, y0 + (y1 - y0) * step // DRAG_STEPS
        user32.mouse_event(0x0001, nx - px, ny - py, 0, 0)               # MOUSEEVENTF_MOVE, relative
        px, py = nx, ny
        time.sleep(0.05)
    time.sleep(DRAG_HOVER)                                               # over the target before the drop
    user32.mouse_event(up, 0, 0, 0, 0)
    time.sleep(SETTLE)
