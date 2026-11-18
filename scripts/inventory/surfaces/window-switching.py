"""Surface module: snapping, Alt+Tab, Task View and virtual desktops.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9 (snap,
Alt+Tab, Task View and virtual desktops). Writes
docs/design/inventory/window/switching.json at fhd-100, and the observation
files switching.wide-100.json and switching.tall-100.json:

    python3 scripts/inventory/capture.py --surface window --file switching --module window-switching \
        --merge --timeout 1800
    python3 scripts/inventory/capture.py --surface window --file switching --module window-switching \
        --profile wide-100 --observe --timeout 1800     # and tall-100, after the canonical file

A run takes about 16 minutes, past the 900 s default. Every display offers the
same six layouts: a portrait one turns them a quarter, renames the two that
lean to one side and reorders them, so a layout's id is its landscape name and
a zone's its order in the layout. Things the shell does that the scenes allow
for: every task switcher after a session's first shows no list to UI
Automation (Explorer is restarted, WORKAROUND); after a display profile change
the desktop holds the foreground (the window is brought forward, WORKAROUND);
the switcher marks its selection with keyboard focus, not IsSelected; a click
on a Task View menu item leaves the menu open (chosen by keyboard); the drop
preview has no window or element, so it is measured from pixels.

Notepad and File Explorer are the two windows every scene arranges. The snap
zones are measured, not read off the flyout's miniature: Notepad is snapped
into every zone of every layout the flyout offers and its frame (the DWM
extended frame, without the invisible resize border) is the zone. The Win and
arrow key table is measured the same way: Notepad is put in each state, the
arrow pressed, and the state it lands in named from its frame.
"""
import ctypes
import subprocess
import time
from ctypes import wintypes

import capture_core as core

SURFACE = "window"
user32 = ctypes.WinDLL("user32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi")
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

NOTEPAD = (300, 200, 900, 600)           # where Notepad rests: x, y, width, height
EXPLORER = (1000, 260, 800, 560)
CAPTION = (600, 12)                      # an empty point of Notepad's title bar, from its frame's top left
SETTLE = 1.5
WAIT = 20
OPEN_TRIES = 3
GROUP_SETTLE = 3
WM_CLOSE = 0x0010
SW_MINIMIZE, SW_MAXIMIZE, SW_RESTORE = 6, 3, 9
DWMWA_EXTENDED_FRAME_BOUNDS, DWMWA_CLOAKED = 9, 14
CREATE_NO_WINDOW = 0x08000000
DRAG_STEPS, DRAG_STEP_PX = 15, 20        # a drag to the top edge: this many moves this far each

STATES = ("restored", "maximized", "left", "right", "top-left", "top-right", "bottom-left", "bottom-right",
          "minimized")
ARROWS = ("up", "down", "left", "right")
# How each snapped state is reached with the keys, from a restored window.
REACH = {"left": ("win+left",), "right": ("win+right",), "top-left": ("win+left", "win+up"),
         "top-right": ("win+right", "win+up"), "bottom-left": ("win+left", "win+down"),
         "bottom-right": ("win+right", "win+down")}
# The two layouts whose zones name the snapped states.
HALVES, QUARTERS = "2 window split layout", "4 window grid layout"

NOT_WINDOW = [{"controlType": "Button", "name": ""}]          # a snap bar's drop targets: unnamed hit areas
GROUP = {"namePrefix": "Group, "}          # a snap group's entry: "Group, " and its windows' titles, latest first
WINDOWS = [({"name": "Untitled - Notepad"}, "notepad-window"), ({"name": "Windows - File Explorer"}, "explorer-window"),
           (GROUP, "snap-group")]
SUGGESTION = {"automationId": "WindowGroupSuggestionButton"}
VALUES = [spec for spec, _ in WINDOWS]
# The six layouts, by the names they carry on the 1920 by 1080 display, in flyout order.
LAYOUTS = ("2 window split layout", "2 window focus on left layout", "3 window split layout",
           "3 window focus on left layout", "4 window grid layout", "3 window grid layout focus in center")
# A portrait display turns each layout a quarter and renames the two that
# lean to one side ("2 window focus on top layout" is "2 window focus on left
# layout" turned), and it lists the six in another order (2026-11-18). A layout
# is named for its landscape self, so every display names it the same way.
ROTATED = {"2 window focus on top layout": "2 window focus on left layout",
           "3 window focus on top layout": "3 window focus on left layout"}


def landscape(name):
    """A layout's name as the 1920 by 1080 display gives it."""
    return ROTATED.get(name, name)


# Inside a layout a zone is told apart by its order, which a quarter turn
# keeps; its name holds its place and share, which change with the display
# ("Left, 25% width" on a wide one for 28%). Only a button whose name ends in
# its share is a zone.
ZONES_MAX = 4
IDENTITY = WINDOWS + [({"automationId": "LayoutGrid", "name": n}, core.slug(landscape(n)))
                      for n in (*LAYOUTS, *ROTATED)] + [
    ({"className": "Button", "nameSuffix": "% height", "nth": k}, f"zone-{k}") for k in range(1, ZONES_MAX + 1)] + [
    ({**SUGGESTION, "nth": n}, f"suggested-group-{n}") for n in range(1, 5)]
LABELS = [({"name": "Untitled - Notepad"}, "Window"), ({"name": "Windows - File Explorer"}, "Window"),
          (GROUP, "Snap group"),
          (SUGGESTION, "Suggested group"), ({"automationId": "DesktopsListScrollViewer"}, "Desktop strip")]

_m = {}                                  # measured during the scenes, read by METRICS


class RECT(ctypes.Structure):
    _fields_ = [("l", wintypes.LONG), ("t", wintypes.LONG), ("r", wintypes.LONG), ("b", wintypes.LONG)]


def frame(h):
    """The window's visible frame as (x, y, width, height)."""
    r = RECT()
    if dwmapi.DwmGetWindowAttribute(wintypes.HWND(h), DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(r), ctypes.sizeof(r)):
        raise core.Incomplete("DwmGetWindowAttribute failed")
    return (r.l, r.t, r.r - r.l, r.b - r.t)


def work_area():
    r = RECT()
    user32.SystemParametersInfoW(0x30, 0, ctypes.byref(r), 0)       # SPI_GETWORKAREA
    return (r.l, r.t, r.r - r.l, r.b - r.t)


def cloaked(h):
    v = wintypes.DWORD()
    dwmapi.DwmGetWindowAttribute(wintypes.HWND(h), DWMWA_CLOAKED, ctypes.byref(v), ctypes.sizeof(v))
    return bool(v.value)


def handle(ctx, spec):
    """The window the launch just opened. A window of the same class left on
    another desktop by an earlier run is cloaked, and UI Automation can return
    it first (the wide display profile, 2026-11-18), so a cloaked one is passed
    over until an uncloaked one shows."""
    if "className" not in spec:
        return ctx.find_window(timeout=WAIT, foreground=False, **spec).el.CurrentNativeWindowHandle
    deadline = time.monotonic() + WAIT
    while True:
        found = []
        proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(
            lambda h, _: found.append(h) or True)
        user32.EnumWindows(proc, 0)
        buf = ctypes.create_unicode_buffer(256)
        for h in found:
            user32.GetClassNameW(h, buf, 256)
            if buf.value == spec["className"] and user32.IsWindowVisible(h) and not cloaked(h):
                return h
        if time.monotonic() > deadline:
            raise core.Incomplete(f"no uncloaked {spec['className']} window within {WAIT} s")
        time.sleep(0.5)


def ensure(ctx):
    """Both windows open, restored and in place, Notepad in front."""
    if not user32.IsWindow(_m.get("explorer", 0)):              # first use, or Explorer was restarted
        ctx.launch("explorer.exe", "C:\\Windows")
        _m["explorer"] = handle(ctx, {"className": "CabinetWClass"})
        time.sleep(SETTLE)
    if not user32.IsWindow(_m.get("notepad", 0)):
        ctx.launch("notepad.exe")
        _m["notepad"] = handle(ctx, {"className": "Notepad"})
        time.sleep(SETTLE)
    for name, place in (("explorer", EXPLORER), ("notepad", NOTEPAD)):
        h = _m[name]
        user32.ShowWindow(h, SW_RESTORE)
        user32.SetWindowPos(h, 0, *place, 0x0004)                     # SWP_NOZORDER
    time.sleep(SETTLE)
    activate(ctx)


def activate(ctx):
    """Bring Notepad forward by clicking its title bar, and check it came. A
    window just placed can take a moment to accept the click (the wide display
    profile, 2026-11-18), so the click is tried again before giving up."""
    h = _m["notepad"]
    for _ in range(OPEN_TRIES):
        x, y, _, _ = frame(h)
        # WORKAROUND: after a display profile change the desktop (Progman)
        # holds the foreground above every window and takes a click meant for
        # the title bar (the wide profile, 2026-11-18); the shell owns that
        # z-order, so the capture brings the window forward before clicking.
        user32.SetForegroundWindow(h)
        ctx.click_at(x + CAPTION[0], y + CAPTION[1])
        time.sleep(SETTLE)
        if user32.GetForegroundWindow() == h:
            return
    buf, under = ctypes.create_unicode_buffer(256), ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(user32.GetForegroundWindow(), buf, 256)
    point = wintypes.POINT(x + CAPTION[0], y + CAPTION[1])
    user32.GetClassNameW(user32.GetAncestor(user32.WindowFromPoint(point), 2), under, 256)
    stack = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(lambda w, _: stack.append(w) or True)
    user32.EnumWindows(proc, 0)
    rows, name = [], ctypes.create_unicode_buffer(256)
    for w in stack:
        r = wintypes.RECT()
        user32.GetWindowRect(w, ctypes.byref(r))
        if user32.IsWindowVisible(w) and r.left <= point.x < r.right and r.top <= point.y < r.bottom:
            user32.GetClassNameW(w, name, 256)
            rows.append(f"{name.value}:{(r.left, r.top, r.right, r.bottom)}:ex={user32.GetWindowLongW(w, -20):#x}"
                        f":icon={user32.IsIconic(w)}:hung={user32.IsHungAppWindow(w)}:cloak={cloaked(w)}"
                        f":me={w == h}")
    raise core.Incomplete(f"Notepad did not come forward: frame {frame(h)}, cloaked {cloaked(h)}, clicked "
                          f"{(point.x, point.y)} over {under.value!r}, in front {buf.value!r}; at the point "
                          + "; ".join(rows[:8]))


def flyout(ctx):
    """The snap layouts flyout. Win+Z opens it with a number keytip over each
    layout, each in a popup host of its own ahead of the flyout's, so the
    flyout is looked for in every popup host of the shell's island windows."""
    deadline = time.monotonic() + WAIT
    while time.monotonic() < deadline:
        for w in ctx.live.children(ctx.live.uia.GetRootElement()):
            if w.class_name != "XamlExplorerHostIslandWindow":
                continue
            for host in w.children():
                for popup in host.children():
                    for c in popup.children():
                        if c.automation_id == "SnapFlyoutControl":
                            return c
        time.sleep(0.3)
    raise core.Incomplete("no snap layouts flyout opened")


def layouts(ctx, fly):
    """The flyout's layouts: (label, [zone elements]) in its order."""
    return [(g.name, list(g.children())) for g in fly.children() if g.automation_id == "LayoutGrid"]


def dismiss(ctx):
    """Close whatever a snap left open (snap assist), without touching the windows."""
    ctx.keys("esc")
    time.sleep(SETTLE)


def measure_zones(ctx):
    """Snap Notepad into every zone the flyout offers; each frame is a zone."""
    ensure(ctx)
    ctx.keys("win+z")
    offered = [(label, [z.name for z in zones]) for label, zones in layouts(ctx, flyout(ctx))]
    ctx.keys("esc")
    time.sleep(SETTLE)
    zones = {}
    for i, (label, names) in enumerate(offered):
        zones[label] = []
        for j, name in enumerate(names):
            ensure(ctx)
            ctx.keys("win+z")
            target = layouts(ctx, flyout(ctx))[i][1][j]
            if target.name != name:
                raise core.Incomplete(f"the flyout's zone {i}.{j} changed from {name!r} to {target.name!r}")
            ctx.click(target)
            time.sleep(SETTLE)
            zones[label].append((name, frame(_m["notepad"])))
            dismiss(ctx)
    _m["zones"] = zones
    _m["work_area"] = work_area()


def halves_and_quarters():
    """The work area's halves and quarters, by the state a window in each is in."""
    x, y, w, ht = _m["work_area"]
    hw, hh = w // 2, ht // 2
    return {"left": (x, y, hw, ht), "right": (x + hw, y, w - hw, ht), "top": (x, y, w, hh),
            "bottom": (x, y + hh, w, ht - hh), "top-left": (x, y, hw, hh), "top-right": (x + hw, y, w - hw, hh),
            "bottom-left": (x, y + hh, hw, ht - hh), "bottom-right": (x + hw, y + hh, w - hw, ht - hh)}


def state_of(h):
    """The state a window is in, named from its frame."""
    if user32.IsIconic(h):
        return "minimized"
    if user32.IsZoomed(h):
        return "maximized"
    f = frame(h)
    # Named by geometry against the work area, not from the flyout's layouts:
    # those follow the display's shape (a portrait display offers other
    # layouts, 2026-11-18) while the Win and arrow keys snap to the same halves
    # and quarters on every display.
    named = {z: n for n, z in halves_and_quarters().items()}
    if f in named:
        return named[f]
    if f == _m["restored"]:
        return "restored"
    return f"a frame at {f}"


def put(ctx, state):
    """Put Notepad in a state with the keys a person would use."""
    ensure(ctx)
    h = _m["notepad"]
    if state == "maximized":
        ctx.keys("win+up")
    elif state == "minimized":
        ctx.keys("win+down")
    else:
        for k in REACH.get(state, ()):
            ctx.keys(k)
            time.sleep(SETTLE)
            dismiss(ctx)
    time.sleep(SETTLE)
    if state_of(h) != state:
        raise core.Incomplete(f"Notepad did not reach {state} (it is {state_of(h)})")


def measure_keys(ctx):
    ensure(ctx)
    _m["restored"] = frame(_m["notepad"])
    table = {}
    for state in STATES:
        for arrow in ARROWS:
            put(ctx, state)
            ctx.keys(f"win+{arrow}")
            time.sleep(SETTLE)
            table[(state, arrow)] = state_of(_m["notepad"])
            dismiss(ctx)
    _m["keys"] = table
    ensure(ctx)


def measure_desktop_close(ctx):
    """A window left on a desktop that closes: where it goes."""
    ensure(ctx)
    ctx.keys("ctrl+win+d")
    time.sleep(SETTLE * 2)
    script = ("Add-Type -AssemblyName System.Windows.Forms;$f=New-Object Windows.Forms.Form;"
              "$f.Text='Desktop witness';$f.StartPosition='Manual';$f.Location='200,200';$f.Size='300,200';"
              "[void]$f.ShowDialog()")
    subprocess.Popen(["powershell.exe", "-NoProfile", "-Command", script], creationflags=CREATE_NO_WINDOW)
    witness = handle(ctx, {"name": "Desktop witness"})
    time.sleep(SETTLE)
    ctx.keys("ctrl+win+f4")
    time.sleep(SETTLE * 2)
    _m["closed_desktop_keeps_window"] = bool(user32.IsWindow(witness)) and not cloaked(witness)
    user32.PostMessageW(witness, WM_CLOSE, 0, 0)
    time.sleep(SETTLE)


def top_windows():
    """Every visible, uncloaked top-level window: (handle, class, rect)."""
    found, out, buf = [], [], ctypes.create_unicode_buffer(256)
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(lambda h, _: found.append(h) or True)
    user32.EnumWindows(proc, 0)
    for h in found:
        if user32.IsWindowVisible(h) and not cloaked(h):
            r = wintypes.RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            user32.GetClassNameW(h, buf, 256)
            out.append((h, buf.value, (r.left, r.top, r.right - r.left, r.bottom - r.top)))
    return out


def drag_caption(ctx, to):
    """Press on Notepad's title bar and carry it to the point `to`, held."""
    x, y, _, _ = frame(_m["notepad"])
    sx, sy = x + CAPTION[0], y + CAPTION[1]
    ctx.hover_point(sx, sy)
    time.sleep(0.3)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.3)
    for k in range(1, DRAG_STEPS + 1):
        ctx.hover_point(sx + (to[0] - sx) * k // DRAG_STEPS, sy + (to[1] - sy) * k // DRAG_STEPS)
        time.sleep(0.06)
    ctx.hover_point(*to)


def measure_edges(ctx):
    """Drag Notepad's title bar to each side edge and corner of the work area:
    what shows while it is held there (a window that was not on screen before
    the drag) and the frame it lands in when let go."""
    ax, ay, aw, ah = _m["work_area"]
    right, bottom = ax + aw - 1, ay + ah - 1
    targets = {"left": (ax, ay + ah // 2), "right": (right, ay + ah // 2), "top-left": (ax, ay),
               "top-right": (right, ay), "bottom-left": (ax, bottom), "bottom-right": (right, bottom)}
    edges = {}
    for name, point in targets.items():
        ensure(ctx)
        zone = halves_and_quarters()[name]
        start = frame(_m["notepad"])
        quiet = ctx.pixels(*zone)
        before = {h for h, _, _ in top_windows()}
        drag_caption(ctx, point)
        time.sleep(SETTLE * 2)
        shown = [(cls, rect) for h, cls, rect in top_windows() if h not in before]
        held = ctx.pixels(*zone)
        preview = changed_box(zone, quiet, held, [start, frame(_m["notepad"]), (point[0] - 32, point[1] - 32, 64, 64)])
        user32.mouse_event(0x0004, 0, 0, 0, 0)
        time.sleep(SETTLE)
        dismiss(ctx)                       # snap assist offers the other window for the rest
        edges[name] = {"shown": shown, "preview": preview, "zone": zone, "lands": state_of(_m["notepad"]),
                       "frame": frame(_m["notepad"])}
    _m["edges"] = edges


PIXEL_DELTA = 24                 # a channel sum change past this is a pixel the preview changed


def changed_box(zone, quiet, held, masks):
    """The bounding box, in screen pixels, of the pixels of `zone` that differ
    between two reads, outside the rectangles in `masks` (the dragged window
    where it was and where it is, and the pointer). The drop preview has no
    window or UI Automation element of its own, so this is how it is seen."""
    zx, zy, _, _ = zone
    xs, ys = [], []
    for r, (row0, row1) in enumerate(zip(quiet, held)):
        y = zy + r
        for c, (a, b) in enumerate(zip(row0, row1)):
            if abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) <= PIXEL_DELTA:
                continue
            x = zx + c
            if any(mx <= x < mx + mw and my <= y < my + mh for mx, my, mw, mh in masks):
                continue
            xs.append(x)
            ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)


def open_layouts(ctx):
    measure_zones(ctx)
    measure_keys(ctx)
    measure_edges(ctx)
    measure_desktop_close(ctx)
    ensure(ctx)
    ctx.keys("win+z")
    flyout(ctx)
    time.sleep(SETTLE)


def close_esc(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)


def snap_bar(ctx):
    for w in ctx.live.children(ctx.live.uia.GetRootElement()):
        if w.class_name == "XamlExplorerHostIslandWindow" and w.bounds[1] == 0 and 0 < w.bounds[3] < 600:
            return w
    raise core.Incomplete("no snap bar at the top of the screen")


def open_snap_bar(ctx):
    """Drag Notepad's title bar to the top edge and hold it there."""
    ensure(ctx)
    x, y, _, _ = frame(_m["notepad"])
    sx, sy = x + CAPTION[0], y + CAPTION[1]
    ctx.hover_point(sx, sy)
    time.sleep(0.3)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.3)
    for k in range(1, DRAG_STEPS + 1):
        ctx.hover_point(sx, max(3, sy - k * DRAG_STEP_PX))
        time.sleep(0.06)
    ctx.hover_point(sx, 3)
    time.sleep(SETTLE * 2)
    snap_bar(ctx)
    _m["snap_bar"] = snap_bar(ctx).bounds


def close_snap_bar(ctx):
    """Carry the window back down before letting go, so it lands restored."""
    x, y = NOTEPAD[0] + CAPTION[0], NOTEPAD[1] + CAPTION[1]
    for k in range(DRAG_STEPS, 0, -1):
        ctx.hover_point(x, y + k)
        time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(SETTLE)
    ensure(ctx)


def open_assist(ctx):
    """Snap Notepad into the left half from the flyout: snap assist offers
    File Explorer for the right half."""
    ensure(ctx)
    ctx.keys("win+z")
    label, zones = layouts(ctx, flyout(ctx))[0]
    if label != HALVES:
        raise core.Incomplete(f"the flyout's first layout is {label!r}, not {HALVES!r}")
    ctx.click(zones[0])
    ctx.find_window(name="Snap Assist", timeout=10, foreground=False)
    time.sleep(SETTLE)


def assist(ctx):
    return ctx.find(within=ctx.find_window(name="Snap Assist", timeout=10, foreground=False),
                    automationId="SnapAssistRoot", depth=3)


def close_assist(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)
    ensure(ctx)


def switcher(ctx):
    return ctx.find(within=ctx.find_window(name="Task Switching", timeout=10, foreground=False),
                    automationId="TaskSwitchingWindow", depth=3)


def hold_alt_tab(ctx):
    show_switcher(ctx, lambda: ctx.hold("alt+tab"), lambda: ctx.release("alt+tab"))


def show_switcher(ctx, press, let_go):
    """Open the task switcher with `press` until UI Automation shows its list."""
    for attempt in range(1, OPEN_TRIES + 1):
        press()
        try:
            ctx.find(within=ctx.find_window(name="Task Switching", timeout=10, foreground=False),
                     automationId="TaskSwitchingWindow", depth=3, timeout=15)
            time.sleep(SETTLE)
            return
        except core.Incomplete:
            if attempt == OPEN_TRIES:
                lines = []

                def walk(el, depth, pad):
                    for c in el.children():
                        lines.append(f"{pad}{c.control_type}|{c.class_name}|{c.name[:30]!r}|{c.automation_id}|{c.bounds}")
                        if depth:
                            walk(c, depth - 1, pad + " ")
                for w in ctx.live.children(ctx.live.uia.GetRootElement()):
                    lines.append(f"TOP {w.class_name}|{w.name[:30]!r}|{w.bounds}")
                    if w.class_name == "XamlExplorerHostIslandWindow":
                        walk(w, 6, " ")
                let_go()
                raise core.Incomplete("no switcher:\n" + "\n".join(lines)) from None
            let_go()
            ctx.keys("esc")
            # WORKAROUND: every task switcher after a session's first (a held
            # Alt+Tab or Ctrl+Alt+Tab) shows its window with no list in UI
            # Automation however long it stays (measured 2026-11-18, whichever
            # scene comes second); a restarted Explorer shows it whole again.
            # The shell owns the defect; the capture cannot reach it otherwise.
            core.restart_explorer(ctx)
            ensure(ctx)


# The switcher marks its selection with keyboard focus: its entries report no
# selection pattern and IsSelected false throughout (probed 2026-11-18).
UIA_HAS_FOCUS = 30008                    # UIA_HasKeyboardFocusPropertyId


def switch_items(ctx):
    """The switcher's entries in order: (name, bounds, selected)."""
    lst = ctx.find(within=switcher(ctx), automationId="SwitchItemListControl", depth=3)
    return [(c.name, c.bounds, bool(c.el.GetCurrentPropertyValue(UIA_HAS_FOCUS)))
            for c in lst.children() if c.control_type == "ListItem"]


def open_alt_tab(ctx):
    """Hold Alt+Tab, then press Tab once per entry with Alt still held, reading
    the list after each press: which entry it opens on, where each press moves
    the selection, and each entry's box selected and not."""
    ensure(ctx)
    hold_alt_tab(ctx)
    steps = [switch_items(ctx)]
    for _ in range(len(steps[0])):
        ctx.keys("tab")
        time.sleep(SETTLE)
        steps.append(switch_items(ctx))
    _m["alt_tab"] = steps


def close_alt_tab(ctx):
    ctx.release("alt+tab")
    time.sleep(SETTLE)
    ensure(ctx)


def open_group_alt_tab(ctx):
    """Make a snap group (Notepad left, File Explorer picked in snap assist),
    then hold Alt+Tab. Alt+Tab pressed while the picked window is still
    settling draws an empty switcher (2026-11-18), so the group settles first."""
    open_assist(ctx)
    item = ctx.find(within=assist(ctx), name="Windows - File Explorer", controlType="ListItem", depth=4)
    ctx.click(item)
    time.sleep(SETTLE)
    time.sleep(GROUP_SETTLE)
    _m["group"] = {"notepad": frame(_m["notepad"]), "explorer": frame(_m["explorer"])}
    hold_alt_tab(ctx)


def open_sticky(ctx):
    ensure(ctx)
    show_switcher(ctx, lambda: ctx.keys("ctrl+alt+tab"), lambda: None)


def close_sticky(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)
    ensure(ctx)


def timeline(ctx):
    return ctx.find(within=ctx.find_window(name="Task View", timeout=10, foreground=False),
                    automationId="TaskViewTimelineRoot", depth=3)


def open_task_view(ctx):
    ensure(ctx)
    ctx.keys("win+tab")
    timeline(ctx)
    time.sleep(SETTLE)


def close_task_view(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)
    ensure(ctx)


def task_view_menu(ctx):
    return ctx.find(within=ctx.find_window(name="Task View", timeout=10, foreground=False),
                    controlType="Menu", depth=4)


def right_click(ctx, el):
    x, y, w, h = el.bounds
    ctx.click_at(x + w // 2, y + h // 2, "right")
    time.sleep(SETTLE)


def open_window_menu(ctx):
    open_task_view(ctx)
    right_click(ctx, ctx.find(within=timeline(ctx), name="Untitled - Notepad", controlType="ListItem", depth=4))
    task_view_menu(ctx)


def close_menu(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)
    close_task_view(ctx)


def open_two_desktops(ctx):
    ensure(ctx)
    ctx.keys("ctrl+win+d")
    time.sleep(SETTLE * 2)
    ctx.keys("win+tab")
    timeline(ctx)
    time.sleep(SETTLE)


def close_two_desktops(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)
    ctx.keys("ctrl+win+f4")                  # the new desktop is the current one; closing it returns to the first
    time.sleep(SETTLE * 2)
    ensure(ctx)


def open_desktop_menu(ctx):
    open_two_desktops(ctx)
    right_click(ctx, ctx.find(within=timeline(ctx), name="Desktop 1", controlType="ListItem", depth=4))
    task_view_menu(ctx)


def desktop_names(ctx):
    strip = ctx.find(within=timeline(ctx), automationId="DesktopsList", depth=4)
    return [c.name for c in strip.children() if c.control_type == "ListItem"]


def menu_choose(ctx, label):
    """Choose a Task View menu item by keyboard. The menu opens with its first
    item focused; a click on an item leaves the menu open, and hover plus Enter
    works only for the first (both probed 2026-11-18)."""
    menu = task_view_menu(ctx)
    names = [c.name for c in menu.children() if c.control_type == "MenuItem"]
    for _ in range(names.index(label)):
        ctx.keys("down")
    ctx.keys("enter")
    time.sleep(SETTLE)


RENAME_TO = "work"


def close_desktop_menu(ctx):
    """Use the menu's Rename and Move right before closing the second desktop:
    what Rename lets the keyboard do to the name, and the desktop order that
    Move right leaves."""
    before = desktop_names(ctx)
    menu_choose(ctx, "Rename")
    try:
        box = ctx.find(within=timeline(ctx), controlType="Edit", depth=8, timeout=3)
        edit = box.bounds
    except core.Incomplete:
        edit = None
    for ch in RENAME_TO:
        ctx.keys(ch)
    ctx.keys("enter")
    time.sleep(SETTLE)
    renamed = desktop_names(ctx)
    _m["rename"] = {"before": before, "after": renamed, "box": edit}
    right_click(ctx, ctx.find(within=timeline(ctx), name=renamed[0], controlType="ListItem", depth=4))
    menu_choose(ctx, "Move right")
    _m["move_right"] = {"before": renamed, "after": desktop_names(ctx)}
    close_two_desktops(ctx)


SCENES = [
    dict(slug="snap-layouts", label="Snap layouts", kind="region", open=open_layouts, root=flyout, close=close_esc,
         omit=NOT_WINDOW, expand=False),
    dict(slug="snap-bar", label="Snap bar", kind="region", open=open_snap_bar, root=snap_bar, close=close_snap_bar,
         omit=NOT_WINDOW, expand=False),
    dict(slug="snap-assist", label="Snap assist", kind="region", open=open_assist, root=assist, close=close_assist,
         expand=False),
    dict(slug="alt-tab-snap-group", label="Alt+Tab with a snap group", kind="state", open=open_group_alt_tab,
         root=switcher, close=close_alt_tab, expand=False),
    dict(slug="alt-tab", label="Alt+Tab", kind="region", open=open_alt_tab, root=switcher, close=close_alt_tab,
         expand=False),
    dict(slug="alt-tab-sticky", label="Ctrl+Alt+Tab", kind="state", open=open_sticky, root=switcher,
         close=close_sticky, expand=False),
    dict(slug="task-view", label="Task View", kind="region", open=open_task_view, root=timeline,
         close=close_task_view, expand=False),
    dict(slug="task-view-window-menu", label="Task View window menu", kind="contextMenu", open=open_window_menu,
         root=task_view_menu, close=close_menu, expand=False),
    dict(slug="task-view-desktops", label="Task View with two desktops", kind="state", open=open_two_desktops,
         root=timeline, close=close_two_desktops, expand=False),
    dict(slug="task-view-desktop-menu", label="Task View desktop menu", kind="contextMenu", open=open_desktop_menu,
         root=task_view_menu, close=close_desktop_menu, expand=False),
]


def _box(f):
    return {"x": f[0], "y": f[1], "width": f[2], "height": f[3]}


def _need(key):
    if key not in _m:
        raise ValueError(f"{key} was not measured")
    return _m[key]


def zone_records():
    """The work area and each layout's zones and gutters, as integer bounds."""
    area = _need("work_area")
    out = [{"id": "snap-zones", "kind": "region", "label": "Snap zones", "order": 50, "bounds": _box(area),
            "behaviour": "The work area; each layout below tiles it with the frames a snapped window takes. On a "
                         "portrait display each layout turns a quarter, its columns becoming rows (the tall "
                         "profile's observation holds those frames)"}]
    taken = _need("zones")
    if sorted(map(landscape, taken)) != sorted(LAYOUTS):
        raise core.Incomplete(f"the flyout offers {list(taken)}, not the six layouts {list(LAYOUTS)}")
    for layout, zones in taken.items():
        i = LAYOUTS.index(landscape(layout))
        lid = f"snap-zones.{core.slug(landscape(layout))}"
        out.append({"id": lid, "parent": "snap-zones", "kind": "group", "label": layout, "order": i,
                    "bounds": _box(area)})
        for j, (name, f) in enumerate(zones):
            out.append({"id": f"{lid}.zone-{j + 1}", "parent": lid, "kind": "region", "label": name,
                        "order": j, "bounds": _box(f)})
        # Columns between zones that no zone covers: a gutter, recorded so the
        # layout still tiles the work area exactly.
        n = 0
        for _, (x, y, w, h) in zones:
            right = x + w
            if right < area[0] + area[2] and not any(z[0] == right and z[1] < y + h and y < z[1] + z[3]
                                                     for _, z in zones):
                nxt = min((z[0] for _, z in zones if z[0] > right and z[1] < y + h and y < z[1] + z[3]), default=None)
                if nxt is not None:
                    out.append({"id": f"{lid}.gutter-{n + 1}", "parent": lid, "kind": "region", "label": "Gutter",
                                "order": 100 + n, "bounds": _box((right, y, nxt - right, h))})
                    n += 1
    return out


def key_records():
    table = _need("keys")
    out = [{"id": "snap-keys", "parent": "snap-zones", "kind": "group", "label": "Win and arrow keys", "order": 100,
            "behaviour": "Measured on Notepad: put in each state, the arrow pressed with Win held, the state named "
            "from the frame it lands in"}]
    for i, ((state, arrow), to) in enumerate(table.items()):
        text = "Stays " + state if to == state else f"Goes to {to}"
        if state == "minimized" and to == state:
            text += ": a minimized window is not in front, so the keys act on the window that is"
        out.append({"id": f"snap-keys.{state}-{arrow}", "parent": "snap-keys", "kind": "gesture",
                    "label": f"Win+{arrow.capitalize()} from {state}", "order": i, "shortcut": f"Win+{arrow.capitalize()}",
                    "behaviour": text})
    return out


def alt_tab_records():
    """Alt+Tab measured through its Tab presses: the entry it opens on, the
    wrap past the last entry, and each entry's box selected and not, from
    which the outline, the thumbnail height and the gap follow."""
    steps = _need("alt_tab")
    names = [n for n, _, _ in steps[0]]

    def chosen(step):
        return next((n for n, _, sel in step if sel), None)

    order = [chosen(st) for st in steps]
    boxes = {}                                          # name -> {selected: bounds}
    for st in steps:
        for n, b, sel in st:
            boxes.setdefault(n, {})[sel] = b
    out = []
    plain = {n: b[False] for n, b in boxes.items() if False in b}
    if len(plain) != len(names) or not all(True in b for b in boxes.values()):
        raise ValueError(f"alt-tab: every entry was not seen both selected and not: {boxes}")
    heights = sorted({b[3] for b in plain.values()})
    _metric(out, "alt-tab", "thumbnail-height", "Entry height, not selected", 100, heights[-1])
    a, b = sorted(plain.values())[:2] if len(plain) > 1 else (None, None)
    if a:
        _metric(out, "alt-tab", "thumbnail-gap", "Gap between entries, not selected", 103, b[0] - (a[0] + a[2]))
    n0 = names[0]
    sel, un = boxes[n0][True], boxes[n0][False]
    _metric(out, "alt-tab", "selection-outline", "Selection: how far a selected entry grows past each side", 104,
            un[0] - sel[0])
    shapes = "; ".join(f"{n} {plain[n][2]} by {plain[n][3]}" for n in names)
    out.append({"id": "alt-tab.size-rule", "parent": "alt-tab", "kind": "gesture", "label": "Entry size", "order": 105,
                "behaviour": ("Every entry is the same height and its width follows the window's shape: "
                              if len(heights) == 1 else "Entries differ in height: ") + shapes +
                f"; a selected entry grows {un[0] - sel[0]} px past each side"})
    out.append({"id": "alt-tab.opens-on", "parent": "alt-tab", "kind": "gesture", "label": "Opening selection",
                "order": 106, "behaviour": f"Opens with {order[0]} selected, the entry after the active window"
                if order[0] != names[0] else f"Opens with {order[0]}, the first entry, selected"})
    out.append({"id": "alt-tab.wrap", "parent": "alt-tab", "kind": "gesture", "label": "Tab with Alt held",
                "order": 107, "shortcut": "Alt+Tab",
                "behaviour": "Each Tab moves the selection one entry on and the last wraps to the first: "
                + " then ".join(order) if order[-1] == order[0] else "Tab moved the selection " + " then ".join(map(str, order))})
    out.append({"id": "alt-tab.title-row", "parent": "alt-tab", "kind": "gesture", "label": "Title row",
                "order": 108, "behaviour": "Each entry carries its window's title as its name; UI Automation "
                "exposes no separate title element, so the row's look is in the shot"})
    return out


def edge_records():
    """A drag to each side edge and corner, from measure_edges."""
    edges = _need("edges")
    out = [{"id": "snap-edges", "parent": "snap-zones", "kind": "group", "label": "Dragging to an edge or corner",
            "order": 110,
            "behaviour": "Notepad's title bar carried to each side edge and corner of the work area and let go"}]
    for i, (name, e) in enumerate(edges.items()):
        windows = ", ".join(f"{cls} at {rect}" for cls, rect in e["shown"]) or "none"
        p, z = e["preview"], e["zone"]
        if p == z:
            look = f"the drop preview fills the {name} zone {z} exactly"
        elif p:
            look = (f"only {p} of the {name} zone {z} changed; the pointer is on the top edge, where the drag "
                    "also opens the snap layouts bar over it"
                    if z[1] == _m["work_area"][1] and "top" in name else f"only {p} of the {name} zone {z} changed")
        else:
            look = f"no pixel of the {name} zone {z} changed outside the window"
        out.append({"id": f"snap-edges.{name}", "parent": "snap-edges", "kind": "gesture",
                    "label": f"Drag to the {name.replace('-', ' ')} " + ("corner" if "-" in name else "edge"),
                    "order": i, "behaviour": f"While held, {look}; new top-level windows: {windows}. "
                    f"Let go: lands {e['lands']} at {e['frame']}"})
    return out


def desktop_menu_records():
    out = []
    r = _need("rename")
    where = f"an edit box at {r['box']}" if r["box"] else "no edit box that UI Automation exposes"
    out.append({"id": "task-view-desktop-menu.rename-edits", "parent": "task-view-desktop-menu", "kind": "gesture",
                "label": "Rename", "order": 100,
                "behaviour": f"Puts the first desktop's name into editing ({where}); typing replaces it and Enter "
                f"commits: {r['before']} became {r['after']}"})
    m = _need("move_right")
    out.append({"id": "task-view-desktop-menu.move-right-reorders", "parent": "task-view-desktop-menu",
                "kind": "gesture", "label": "Move right", "order": 101,
                "behaviour": f"Desktop order {m['before']} became {m['after']}"})
    return out


def _metric(out, parent, slug, label, order, value):
    out.append({"id": f"{parent}.{slug}", "parent": parent, "kind": "metric", "label": label, "order": order,
                "measure": {"value": value, "unit": "px", "scaling": "scales"}})


def METRICS(records):
    p = f"win11.{SURFACE}."
    out = zone_records() + key_records()

    def under(scene, pred):
        return sorted((r for r in records if r["id"].startswith(p + scene + ".") and pred(r)),
                      key=lambda r: (r["bounds"]["y"], r["bounds"]["x"]))

    out += alt_tab_records() + edge_records() + desktop_menu_records()
    for scene, base in (("alt-tab", 100), ("task-view", 100), ("snap-assist", 100)):
        items = under(scene, lambda r: r.get("controlType") == "listitem" and r["label"] == "Window")
        if not items:
            raise ValueError(f"{scene}: no window thumbnails captured")
        close = under(scene, lambda r: r.get("controlType") == "button" and r["label"] == "Close")
        if close:
            _metric(out, scene, "close-button-width", "Close button width", base + 1, close[0]["bounds"]["width"])
            _metric(out, scene, "close-button-height", "Close button height", base + 2, close[0]["bounds"]["height"])
        if scene == "alt-tab":
            continue                                    # its sizes come from alt_tab_records, selected and not
        _metric(out, scene, "thumbnail-height", "Window thumbnail height", base, max(i["bounds"]["height"] for i in items))
        if len(items) > 1:
            a, b = sorted((i["bounds"] for i in items), key=lambda b: b["x"])[:2]
            _metric(out, scene, "thumbnail-gap", "Gap between thumbnails", base + 3, b["x"] - (a["x"] + a["width"]))
    desk = under("task-view", lambda r: r.get("controlType") == "listitem" and r["label"].startswith("Desktop"))
    new = under("task-view", lambda r: r.get("controlType") == "button" and r["label"] == "New desktop")
    if not desk or not new:
        raise ValueError("task-view: no desktop strip captured")
    _metric(out, "task-view", "desktop-width", "Desktop thumbnail width", 110, desk[0]["bounds"]["width"])
    _metric(out, "task-view", "desktop-height", "Desktop thumbnail height", 111, desk[0]["bounds"]["height"])
    _metric(out, "task-view", "new-desktop-width", "New desktop button width", 112, new[0]["bounds"]["width"])
    _metric(out, "task-view", "new-desktop-height", "New desktop button height", 113, new[0]["bounds"]["height"])
    bar = _need("snap_bar")
    _metric(out, "snap-bar", "height", "Snap bar reach from the top edge", 100, bar[3])
    g = _need("group")
    area = _need("work_area")
    out.append({"id": "alt-tab-snap-group.forms", "parent": "alt-tab-snap-group", "kind": "gesture",
                "label": "Picking a window in snap assist", "order": 100,
                "behaviour": f"Fills the empty zone: Notepad at {g['notepad']}, File Explorer at {g['explorer']} on "
                f"a {area[2]} by {area[3]} work area, and the two become a snap group"})
    out.append({"id": "task-view-desktops.close-desktop", "parent": "task-view-desktops", "kind": "gesture",
                "label": "Closing a desktop", "order": 100,
                "behaviour": "Its windows move to the desktop on its left and stay open" if _need("closed_desktop_keeps_window")
                else "Its windows did not move to another desktop in the measured run"})
    return out


SHORTCUTS = ("Microsoft Support, Keyboard shortcuts in Windows (support.microsoft.com, article "
             "dcc61a57-8ff0-cffe-9796-cb9706c75eec), read 2026-11-18")
HAND = [
    {"id": "task-view.keys", "parent": "task-view", "kind": "group", "label": "Desktop keys", "order": 120},
    {"id": "task-view.keys.new", "parent": "task-view.keys", "kind": "shortcut", "label": "New desktop", "order": 0,
     "shortcut": "Win+Ctrl+D", "behaviour": f"Creates a desktop and switches to it. {SHORTCUTS}"},
    {"id": "task-view.keys.left", "parent": "task-view.keys", "kind": "shortcut", "label": "Desktop to the left",
     "order": 1, "shortcut": "Win+Ctrl+Left", "behaviour": SHORTCUTS},
    {"id": "task-view.keys.right", "parent": "task-view.keys", "kind": "shortcut", "label": "Desktop to the right",
     "order": 2, "shortcut": "Win+Ctrl+Right", "behaviour": SHORTCUTS},
    {"id": "task-view.keys.close", "parent": "task-view.keys", "kind": "shortcut", "label": "Close this desktop",
     "order": 3, "shortcut": "Win+Ctrl+F4", "behaviour": SHORTCUTS},
    {"id": "task-view.keys.swipe", "parent": "task-view.keys", "kind": "gesture", "label": "Four-finger swipe",
     "order": 4, "behaviour": "Four fingers swiped left or right on a precision touchpad switch desktops. Microsoft "
     "Support, Touchpad gestures for Windows, read 2026-11-18"},
    {"id": "snap-keys.between-displays", "parent": "snap-keys", "kind": "shortcut", "label": "Move to another display",
     "order": 100, "shortcut": "Win+Shift+Left", "behaviour": f"Moves the active window to the display on the left; "
     f"Win+Shift+Right to the right. {SHORTCUTS}"},
    {"id": "snap-keys.stretch", "parent": "snap-keys", "kind": "shortcut", "label": "Stretch to full height",
     "order": 101, "shortcut": "Win+Shift+Up", "behaviour": SHORTCUTS},
]
