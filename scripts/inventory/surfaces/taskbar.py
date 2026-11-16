"""Surface module: the taskbar bar, its menus, jump lists and thumbnails.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (the
Taskbar item). Writes docs/design/inventory/taskbar/taskbar.json:

    python3 scripts/inventory/capture.py --surface taskbar --file taskbar [--merge]

The tray (the buttons right of the bar, inside the same Shell_TrayWnd) is
tray.py's file. Measured on 26300.9974 (2026-11-16): the bar is the XAML
TaskbarFrame inside Shell_TrayWnd; its empty-area menu is a MenuFlyout in a
PopupHost pane of that window; a jump list is a top-level CoreWindow named
"Jump List for <app>"; a hover thumbnail is a Group in a full-screen
XamlExplorerHostIslandWindow, shown only for a pointer that enters the button;
the row of previews for several windows is drawn but not exposed to UI
Automation (a desktop-wide search for a preview's title finds only the windows).
The reference VM restores its clean checkpoint after the run, so the fixture
windows and the settings the measuring scenes change never outlive it.
"""
import ctypes
import time

import capture_core as core

SURFACE = "taskbar"
# A jump list's pinned and recent entries are the machine's own folders and
# files: each is recorded once and never entered. Its commands (Open, Pin,
# Close window) carry automation ids and are walked as usual.
ENVIRONMENT_LISTS = [{"className": "ListViewItem", "automationId": ""},
                     # The overflow flyout's buttons are windows, each named by its live handle.
                     {"automationId": "OverflowScrollView"}]
VALUES = []
IDENTITY = [({"automationId": "StartButton"}, "start"), ({"automationId": "SearchButton"}, "search"),
            ({"automationId": "TaskViewButton"}, "task-view"), ({"automationId": "WidgetsButton"}, "widgets")]
HOSTS = [{"automationId": "JumpListScroller"}]       # the scroller around a jump list's entries
LABELS = [({"automationId": "SystemItemList"}, "App commands"),
          ({"automationId": "OverflowScrollView"}, "Overflow buttons"),
          ({"controlType": "Image", "automationId": "BadgeAnchorSmallTicker"}, "Badge"),
          ({"controlType": "Image", "name": ""}, "Icon")]

ROOT = "bar"                       # the record every metric and note hangs under
APPS = "Appid: "                   # a task button's automation id starts with its app id
EXPLORER, NOTEPAD = "Appid: Microsoft.Windows.Explorer", "Appid: Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"
ISLAND = "XamlExplorerHostIslandWindow"
JUMP = "Windows.UI.Core.CoreWindow"
ADVANCED = r"HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"
SEARCH_KEY = r"HKCU:\Software\Microsoft\Windows\CurrentVersion\Search"
# SearchboxTaskbarMode values, in the order the scenes set them.
SEARCH_MODES = [("hidden", 0), ("icon", 1), ("icon-and-label", 3), ("box", 2)]

MEASURED = {}                      # pixel and timing measurements the openers take, for METRICS


def tray(ctx):
    return ctx.find_window(className="Shell_TrayWnd")


def frame(ctx):
    return ctx.find(within=tray(ctx), automationId="TaskbarFrame", depth=3)


def button(ctx, app, name_has=""):
    """The task button of an app, optionally once its name says something
    (a window count)."""
    deadline = time.monotonic() + 30
    while True:
        for k in frame(ctx).children():
            if k.automation_id.startswith(app) and name_has in k.name:
                return k
        if time.monotonic() > deadline:
            raise core.Incomplete(f"no task button for {app!r} naming {name_has!r} within 30 s")
        time.sleep(0.5)


def away(ctx):
    """The pointer off the bar, on an empty part of the desktop."""
    x, y, w, h = tray(ctx).bounds
    ctx.hover_point(x + w * 3 // 4, y - 400)


def set_value(ctx, key, name, value):
    ctx.powershell(f"Set-ItemProperty -LiteralPath '{key}' -Name {name} -Value {value} -Type DWord")


# ------------------------------------------------------- pixel indicator --
gdi32 = getattr(ctypes, "windll", None) and ctypes.windll.gdi32
user32 = getattr(ctypes, "windll", None) and ctypes.windll.user32
INDICATOR_ROWS = 10                # the indicator sits in the button's bottom rows
COLOUR_STEP = 60                   # a channel sum this far from the background is the indicator


def drawn(el, top, bottom, what):
    """The bounding box (width, height) of what is drawn in rows top..bottom of
    a task button, against the background read at its left edge on each row:
    the icon and the running or active indicator, neither of which UI
    Automation exposes on 26300.9974."""
    x, y, w, h = (int(v) for v in el.bounds)
    dc = user32.GetDC(None)
    try:
        def px(cx, cy):
            c = gdi32.GetPixel(dc, cx, cy)
            return c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF
        hits = []
        for cy in range(y + top, y + bottom):
            bg = px(x + 4, cy)
            for cx in range(x + 4, x + w - 4):
                if sum(abs(a - b) for a, b in zip(px(cx, cy), bg)) > COLOUR_STEP:
                    hits.append((cx, cy))
    finally:
        user32.ReleaseDC(None, dc)
    if not hits:
        raise core.Incomplete(f"nothing drawn for the {what} on the task button")
    xs, ys = [p[0] for p in hits], [p[1] for p in hits]
    return max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def indicator(el):
    h = int(el.bounds[3])
    return drawn(el, h - INDICATOR_ROWS, h, "indicator")


def icon_size(el):
    """The icon of a pinned button that is not running (no tile behind it)."""
    h = int(el.bounds[3])
    return drawn(el, 2, h - INDICATOR_ROWS, "icon")


# ---------------------------------------------------------------- scenes --
SW_MINIMIZE, SW_RESTORE = 6, 9
SETTLE = 2.5                       # the indicator grows into place after a window changes state


def open_bar(ctx):
    ctx.launch("notepad.exe")
    button(ctx, NOTEPAD, "running")
    away(ctx)
    time.sleep(SETTLE)
    MEASURED["icon-centre"] = icon_size(button(ctx, EXPLORER))
    # Running, not active: the window minimized (a click on the desktop leaves
    # Notepad active, 2026-11-16). Then active again, for the walk too.
    hwnd = ctx.find_window(className="Notepad").el.CurrentNativeWindowHandle
    user32.ShowWindow(hwnd, SW_MINIMIZE)
    time.sleep(SETTLE)
    MEASURED["indicator-running"] = indicator(button(ctx, NOTEPAD))
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetForegroundWindow(hwnd)
    time.sleep(SETTLE)
    MEASURED["indicator-active"] = indicator(button(ctx, NOTEPAD))
    # A real click on the desktop last: after only SetForegroundWindow the bar
    # shows no thumbnail for the next hover (2026-11-16, two runs each way).
    desk = ctx.find_window(className="Progman")
    x, y, w, h = desk.bounds
    ctx.click_at(x + w * 3 // 4, y + h * 3 // 5)


def nothing(ctx):
    pass


def open_menu(ctx):
    x, y, w, h = tray(ctx).bounds
    ctx.click_at(x + w // 6, y + h // 2, button="right")  # empty bar: the centred buttons start at the middle
    menu(ctx)


def menu(ctx):
    return ctx.find(within=tray(ctx), className="MenuFlyout", controlType="Menu", depth=5)


def close_menu(ctx):
    ctx.keys("esc")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            ctx.find(within=tray(ctx), className="MenuFlyout", controlType="Menu", depth=5, timeout=0)
        except core.Incomplete:
            return
        time.sleep(0.3)
    raise core.Incomplete("the taskbar menu stayed open")


def jump_list(app_name):
    def root(ctx):
        return ctx.find_window(className=JUMP, name=f"Jump List for {app_name}")
    return root


def open_jump(app, app_name):
    def opener(ctx):
        ctx.click(button(ctx, app), button="right")
        jump_list(app_name)(ctx)
    return opener


def close_jump(app_name):
    def closer(ctx):
        ctx.keys("esc")
        ctx.gone(className=JUMP, name=f"Jump List for {app_name}")
    return closer


def islands(ctx):
    live = ctx.live
    try:
        tops = live.children(live.uia.GetRootElement(), live.uia.CreateTrueCondition())
    except live.COMError:
        return []
    return [w for w in tops if w.class_name == ISLAND and not w.offscreen]


def showing(ctx):
    """The island window showing a preview and its content, or (None, None).
    An island stays on screen, empty, after its preview goes, and a later
    preview can open in another island (2026-11-16), so every one is read."""
    for island in islands(ctx):
        try:
            for site in island.children():
                kids = site.children()
                if kids:
                    return island, kids
        except ctx.live.COMError:
            continue
    return None, None


def preview(ctx):
    return showing(ctx)[1]


def thumbnails(ctx):
    island, _ = showing(ctx)
    if island is None:
        raise core.Incomplete("no island window shows a preview")
    return island


HOVER_TRIES = 3                    # the preview showed on 3 of 5 single approaches in the VM (2026-11-16)
HOVER_WAIT = 4                     # seconds an approach waits for the preview (it shows in about 0.55 s)


def open_thumbnail(app, name_has, measure=False):
    def opener(ctx):
        target = button(ctx, app, name_has)
        for _ in range(HOVER_TRIES):
            leave(ctx)
            time.sleep(1)
            arrived = ctx.hover(target, settle=0)
            while not preview(ctx) and time.monotonic() < arrived + HOVER_WAIT:
                time.sleep(0.05)
            if preview(ctx):
                break
        else:
            raise core.Incomplete(f"no thumbnail after {HOVER_TRIES} approaches of {HOVER_WAIT} s")
        if measure:
            MEASURED["thumbnail-delay"] = round((time.monotonic() - arrived) * 1000)
        time.sleep(1)
    return opener


def leave(ctx):
    """Walk the pointer up off the bar in steps: a preview closes for a pointer
    that leaves the button, not for one that jumps away (2026-11-16)."""
    x, y, w, h = tray(ctx).bounds
    for step in range(1, 25):
        ctx.hover_point(x + w // 6, y + h // 2 - step * 20)
        time.sleep(0.05)


def close_thumbnail(ctx):
    # The island window stays on screen, empty, after the preview goes (2026-11-16).
    leave(ctx)
    deadline = time.monotonic() + 10
    while preview(ctx):
        if time.monotonic() > deadline:
            raise core.Incomplete("the thumbnail stayed on screen after the pointer left")
        time.sleep(0.3)


def wait_start(ctx, left):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        x = ctx.find(within=frame(ctx), automationId="StartButton", depth=2).bounds[0]
        if (x < 100) == left:
            return
        time.sleep(0.5)
    raise core.Incomplete(f"the Start button did not move {'left' if left else 'back to the centre'} within 30 s")


def open_left(ctx):
    set_value(ctx, ADVANCED, "TaskbarAl", 0)
    wait_start(ctx, left=True)
    away(ctx)
    time.sleep(1.5)
    MEASURED["icon-left"] = icon_size(button(ctx, EXPLORER))


def close_left(ctx):
    set_value(ctx, ADVANCED, "TaskbarAl", 1)
    wait_start(ctx, left=False)


def search_shape(ctx):
    try:
        b = ctx.find(within=frame(ctx), automationId="SearchButton", depth=2, timeout=0).bounds
        return int(b[2]), int(b[3])
    except core.Incomplete:
        return None


def open_search(value):
    def opener(ctx):
        before = search_shape(ctx)
        set_value(ctx, SEARCH_KEY, "SearchboxTaskbarMode", value)
        deadline = time.monotonic() + 30
        while search_shape(ctx) == before:
            if time.monotonic() > deadline:
                raise core.Incomplete(f"SearchboxTaskbarMode {value} did not change the search button within 30 s")
            time.sleep(0.5)
        time.sleep(1)
    return opener


# A recent file for the jump list: Explorer did not list one opened this way
# (2026-11-16), so its Recent group is a hand record (see _hand).
RECENT = r"""
$f = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Report.txt'
Set-Content -LiteralPath $f -Value 'x'
Start-Process -FilePath $f
Start-Sleep -Seconds 4
"""


def open_grouped_jump(ctx):
    """A jump list with every part: a recent file (opened through the shell, so
    it is in the recent list) and two windows (so it offers Close all windows)."""
    ctx.powershell(RECENT)
    for _ in range(2):
        ctx.launch("explorer.exe", "C:\\")
    target = button(ctx, EXPLORER, "2 running windows")
    ctx.click(target, button="right")
    jump_list("File Explorer")(ctx)


OVERFLOW_WINDOWS = 30              # enough separate buttons to overflow a 1920 px bar (2026-11-16: 30 did, 16 did not)


def restart_shell(ctx):
    """Restart Explorer, which reads the combine setting only at start
    (TaskbarGlomLevel set live changed nothing, 2026-11-16), and wait for the bar."""
    ctx.powershell("Stop-Process -Name explorer -Force; Start-Sleep -Seconds 3; "
                   "if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }")
    deadline = time.monotonic() + 90
    while True:
        try:
            ctx.find(within=ctx.find_window(className="Shell_TrayWnd", timeout=5), automationId="StartButton",
                     depth=4, timeout=5)
            break
        except core.Incomplete:
            if time.monotonic() > deadline:
                raise
    time.sleep(SETTLE)


def overflow_button(ctx):
    return ctx.find(within=frame(ctx), automationId="OverflowButton", depth=2, timeout=60)


def open_overflow_bar(ctx):
    set_value(ctx, ADVANCED, "TaskbarGlomLevel", 2)        # never combine: one button per window
    restart_shell(ctx)
    for i in range(OVERFLOW_WINDOWS):
        ctx.launch("explorer.exe", "C:\\Windows" if i % 2 else "C:\\")
        time.sleep(0.5)
    overflow_button(ctx)
    time.sleep(SETTLE)


def open_overflow(ctx):
    ctx.click(overflow_button(ctx))
    deadline = time.monotonic() + 15
    while not preview(ctx):
        if time.monotonic() > deadline:
            raise core.Incomplete("the overflow flyout did not open")
        time.sleep(0.2)
    time.sleep(1)


def close_overflow(ctx):
    ctx.keys("esc")
    deadline = time.monotonic() + 10
    while preview(ctx):
        if time.monotonic() > deadline:
            raise core.Incomplete("the overflow flyout stayed open")
        time.sleep(0.3)


SCENES = [
    dict(slug="bar", label="Taskbar", kind="surface", open=open_bar, root=frame, close=nothing),
    # The thumbnails come before the menus: after a menu or jump list closes with
    # Esc the bar keeps the keyboard focus and shows no thumbnail (2026-11-16).
    dict(slug="thumbnail", label="Window thumbnail", kind="region", open=open_thumbnail(NOTEPAD, "", measure=True),
         root=thumbnails, close=close_thumbnail),
    dict(slug="menu", label="Taskbar context menu", kind="contextMenu", open=open_menu, root=menu, close=close_menu),
    dict(slug="pinned-jump-list", label="Pinned app jump list", kind="contextMenu",
         open=open_jump(EXPLORER, "File Explorer"), root=jump_list("File Explorer"), close=close_jump("File Explorer")),
    dict(slug="running-jump-list", label="Running app jump list", kind="contextMenu",
         open=open_jump(NOTEPAD, "Notepad"), root=jump_list("Notepad"), close=close_jump("Notepad")),
    # Measured for METRICS and not published: the bar at left alignment and the
    # search button in each display mode (the last restores the default, box).
    dict(slug="left", label="Taskbar, left aligned", kind="state", open=open_left, root=frame, close=close_left,
         keep=False, expand=False),
]
SCENES += [dict(slug=f"search-{mode}", label=f"Search, {mode}", kind="state", open=open_search(value), root=frame,
                close=nothing, keep=False, expand=False) for mode, value in SEARCH_MODES]
SCENES += [dict(slug="grouped-jump-list", label="Jump list, several windows", kind="contextMenu",
                open=open_grouped_jump, root=jump_list("File Explorer"), close=close_jump("File Explorer"))]
# Last, because they restart the shell with one button per window: the
# overflow button, the full bar measured in its compact form, and the flyout.
SCENES += [
    dict(slug="overflow-button", label="Taskbar overflow button", kind="control", open=open_overflow_bar,
         root=overflow_button, close=nothing),
    dict(slug="compact", label="Taskbar, full", kind="state", open=nothing, root=frame, close=nothing, keep=False,
         expand=False, environment_lists=[{"className": "Taskbar.TaskListButtonAutomationPeer"}]),
    dict(slug="overflow", label="Taskbar overflow flyout", kind="region", open=open_overflow, root=thumbnails,
         close=close_overflow),
]


# --------------------------------------------------------------- metrics --
def METRICS(records):
    """The bar measured from the captured records and the openers' pixel and
    timing reads (fhd-100: physical pixels at 100 %, so sizes scale)."""
    by = {r["id"]: r for r in records}
    p = f"win11.{SURFACE}."

    def rec(rid):
        r = by.get(p + rid)
        if r is None:
            raise ValueError(f"{p + rid} was not captured; the bar cannot be measured")
        return r["bounds"]

    def kids(parent):
        return sorted((r for r in records if r.get("parent") == p + parent), key=lambda r: r["order"])

    def apps(scene, need=2):
        out = [r for r in kids(scene) if r.get("uiaId", "").startswith(APPS)]
        if len(out) < need:
            raise ValueError(f"fewer than {need} pinned task button(s) in {scene}")
        return out

    def icon(tag):
        if f"icon-{tag}" not in MEASURED:
            raise ValueError(f"the icon was not measured at {tag} alignment")
        # The slot is square and the artwork need not fill it (the File
        # Explorer folder draws 24 by 22, 2026-11-16): the slot is the larger side.
        return max(MEASURED[f"icon-{tag}"])

    def scales(slug, label, value, unit="px", scaling="scales", **extra):
        return (slug, label, {"value": value, "unit": unit, "scaling": scaling, **extra})

    out = []
    for scene, tag in (("bar", "centre"), ("left", "left")):
        a = apps(scene)
        out += [scales(f"bar-height-{tag}", f"Bar height, {tag} aligned", rec(scene)["height"]),
                scales(f"button-width-{tag}", f"Task button width, {tag} aligned", a[0]["bounds"]["width"]),
                scales(f"button-pitch-{tag}", f"Task button pitch, {tag} aligned", a[1]["bounds"]["x"] - a[0]["bounds"]["x"]),
                scales(f"icon-size-{tag}", f"Task button icon size, {tag} aligned", icon(tag),
                       readBy="screen pixels of a pinned button's icon"),
                scales(f"start-x-{tag}", f"Start button offset, {tag} aligned", rec(f"{scene}.start")["x"])]
    for mode, _ in SEARCH_MODES:
        if mode == "hidden":
            if p + "search-hidden.search" in by:
                raise ValueError("the search button is still on the bar with search hidden")
            continue
        b = rec(f"search-{mode}.search")
        out += [scales(f"search-width-{mode}", f"Search button width, {mode.replace('-', ' ')}", b["width"]),
                scales(f"search-height-{mode}", f"Search button height, {mode.replace('-', ' ')}", b["height"])]
    if len({v["value"] for k, _, v in out if k.startswith("search-width-")}) < 3:
        raise ValueError("the search modes did not give three different widths; a mode change was not applied")
    if "indicator-active" in MEASURED and "indicator-running" in MEASURED \
            and MEASURED["indicator-running"][0] >= MEASURED["indicator-active"][0]:
        raise ValueError(f"the running indicator ({MEASURED['indicator-running']}) is not narrower than the active one "
                         f"({MEASURED['indicator-active']}); the two states were not told apart")
    for state in ("active", "running"):
        if f"indicator-{state}" not in MEASURED:
            raise ValueError(f"the {state} indicator was not measured")
        w, h = MEASURED[f"indicator-{state}"]
        out += [scales(f"indicator-{state}-width", f"Indicator width, {state}", w,
                       readBy="screen pixels under the task button"),
                scales(f"indicator-{state}-height", f"Indicator height, {state}", h,
                       readBy="screen pixels under the task button")]
    thumb = [r for r in kids("thumbnail") if r.get("controlType") == "group"]
    if not thumb:
        raise ValueError("no thumbnail preview was captured")
    title = [r for r in records if r.get("parent") == thumb[0]["id"] and r.get("controlType") == "text"]
    out += [scales("thumbnail-width", "Thumbnail width, one window", thumb[0]["bounds"]["width"]),
            scales("thumbnail-height", "Thumbnail height, one window", thumb[0]["bounds"]["height"])]
    if title:
        out.append(scales("thumbnail-title-height", "Thumbnail title row height", title[0]["bounds"]["height"]))
    if "thumbnail-delay" not in MEASURED:
        raise ValueError("the thumbnail delay was not measured")
    out.append(scales("thumbnail-delay", "Thumbnail delay after hover", MEASURED["thumbnail-delay"], unit="ms",
                      scaling="fixed", readBy="pointer arrival to the preview on screen, polled every 50 ms"))
    out.append(scales("jump-list-width", "Jump list width", rec("pinned-jump-list")["width"]))
    compact = apps("compact", need=1)       # a full bar moves pinned buttons into the overflow
    out += [scales("compact-button-width", "Task button width, bar full", compact[0]["bounds"]["width"]),
            scales("compact-start-width", "Start button width, bar full", rec("compact.start")["width"]),
            scales("overflow-button-width", "Overflow button width", rec("overflow-button")["width"])]
    flyout = [r for r in records if r.get("uiaId") == "OverflowScrollView" and r["id"].startswith(p + "overflow.")]
    if not flyout:
        raise ValueError("the overflow flyout's list was not captured")
    # Its width grows with the number of buttons in it (480 and 544 px in two runs), so only the height is fixed.
    out.append(scales("overflow-flyout-height", "Overflow flyout height", flyout[0]["bounds"]["height"]))
    return [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": i, "measure": m}
            for i, (slug, label, m) in enumerate(out)]


# ------------------------------------------------------------------ hand --
STATES = [
    ("not-running", "Not running", "A pinned app with no window: the icon alone, no indicator."),
    ("running", "Running", "An app with a window that is not in front: a short indicator under the icon."),
    ("active", "Active", "The app whose window is in front: a longer accent indicator and a lighter tile behind the icon."),
    ("attention-flashing", "Attention, flashing", "A window asking for attention: the tile pulses in the attention colour."),
    ("attention-steady", "Attention, steady", "After the pulses the tile stays in the attention colour until the window is opened."),
    ("progress-normal", "Progress, normal", "A progress bar drawn across the bottom of the button in the accent colour."),
    ("progress-paused", "Progress, paused", "The same bar in the paused (yellow) colour."),
    ("progress-error", "Progress, error", "The same bar in the error (red) colour."),
    ("progress-indeterminate", "Progress, indeterminate", "A short segment that runs along the bottom of the button."),
    ("badge", "Badge", "A small count or glyph over the icon's bottom-right corner, set by the app."),
    ("grouped-windows", "Grouped windows", "Several windows of one app on one button; the indicator is unchanged and hover shows a row of previews."),
    ("preview-row", "Row of previews",
     "Hovering a button with several windows shows one preview per window side by side, each with its own title row; "
     "the row is not exposed to UI Automation on 26300.9974, so it is described here and seen in the bar screenshots."),
    ("full", "Bar full",
     "When the buttons no longer fit, the bar draws smaller buttons, folds the search box to its icon and shortens "
     "window labels; past that, the last buttons move into the overflow flyout, a one-row strip above the overflow button that "
     "grows by one button per window it holds."),
    ("thumbnail-close", "Thumbnail close button", "A close button appears at the top right of a preview while the pointer is over it."),
]
BEHAVIOURS = [
    ("drag-reorder", "gesture", "Drag to reorder", "Dragging a task button along the bar moves it; the others slide aside."),
    ("drag-file-switch", "gesture", "Drag a file over a button",
     "Holding a dragged file over a task button brings that app's window forward so the file can be dropped in it."),
    ("middle-click-new", "gesture", "Middle-click for a new window", "Middle-clicking a task button opens a new window of that app."),
    ("shift-click-new", "gesture", "Shift-click for a new window", "Shift with a click opens a new window of that app."),
    ("ctrl-click-cycle", "gesture", "Ctrl-click to cycle", "Ctrl with a click on a grouped button brings its windows forward one after another."),
    ("auto-hide", "gesture", "Auto-hide reveal",
     "With auto-hide on, the bar slides away and comes back when the pointer reaches the screen edge it sits on."),
]


def _hand(start):
    out = [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "state", "label": label, "order": start + i,
            "behaviour": text} for i, (slug, label, text) in enumerate(STATES)]
    n = start + len(out)
    out += [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": kind, "label": label, "order": n + i, "behaviour": text}
            for i, (slug, kind, label, text) in enumerate(BEHAVIOURS)]
    out.append({"id": "grouped-jump-list.recent", "parent": "grouped-jump-list", "kind": "state", "label": "Recent group",
                "order": 100, "behaviour": "Below Pinned, a Recent group lists the files the app opened last, under the "
                "same heading and row shape; each row offers a pin button on hover."})
    out.append({"id": "grouped-jump-list.tasks", "parent": "grouped-jump-list", "kind": "state", "label": "Tasks group",
                "order": 101, "behaviour": "An app that registers tasks (Explorer and Notepad register none) gets a "
                "Tasks group below Recent, with the same row shape and no pin button."})
    out.append({"id": f"{ROOT}.win-digit", "parent": ROOT, "kind": "shortcut", "label": "Open a task button by position",
                "order": n + len(BEHAVIOURS), "shortcut": "Win+1", "shortcutScope": "global",
                "behaviour": "Win with a digit from 1 to 9 opens or switches to the app at that position on the bar."})
    return out


HAND = _hand(start=40)              # orders after the metrics, which number from 0
