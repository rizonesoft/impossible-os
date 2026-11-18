"""Surface module: window chrome, the frame every program draws.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9 (window
chrome and system menu). Writes docs/design/inventory/window/chrome.json:

    python3 scripts/inventory/capture.py --surface window --file chrome --module window-chrome

Four programs stand for the two generations: System Information (a classic
Win32 dialog frame), Settings (a UWP frame), File Explorer and Notepad (WinUI
programs that draw their own bar). Each is placed at the same rectangle over a
solid magenta desktop, where its border and shadow can be told from the
background, and measured three ways: UI Automation for the caption buttons'
rectangles, the screen for colours and shadows, and the real pointer for the
resize areas (the cursor shape it gets at each pixel).

Window corners are not measured here: Windows does not round a window in a
virtual machine (Microsoft Learn, "Apply rounded corners in desktop apps for
Windows 11", 2026-11-18), and the read gives 0 on every window. The radii are
hand records citing Microsoft's geometry guidance; the measured read is the
bare-metal check section 9 parks.
"""
import ctypes
import subprocess
import time
import winreg
from ctypes import wintypes

import capture_core as core

SURFACE = "window"
user32 = ctypes.WinDLL("user32", use_last_error=True)
dwmapi = ctypes.WinDLL("dwmapi")
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPCWSTR, wintypes.UINT,
                                       wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
user32.LoadCursorW.restype = wintypes.HANDLE
user32.LoadCursorW.argtypes = [wintypes.HINSTANCE, ctypes.c_void_p]

PLACE = (240, 140, 1200, 720)            # where every window is put: x, y, width, height
DESKTOP_POINT = (120, 980)               # a desktop point no placed window covers
CAPTION_POINT = 520                      # pixels in from the frame's left: an empty part of each title bar
MAGENTA = 0xFF00FF                       # COLORREF of the measuring desktop
SETTLE = 1.5
WAIT = 20
PROBE = 0.12                             # a pointer move settles its cursor in this long (0.06 s left
                                         # the band 1 px short on some runs, 2026-11-18)
WM_CLOSE, WM_SETTINGCHANGE = 0x0010, 0x001A
SW_MAXIMIZE, SW_RESTORE = 3, 9
SCAN = 14                                # pixels probed outside and inside an edge
SHADOW_MAX = 120                         # the farthest a shadow is followed
CORNER_MAX = 40                          # the farthest a corner grip is followed
INK = 60                                 # a pixel this far from the bar's fill is ink (icon or title)
GREEN_EDGE = 20                          # half the green the 1 px border adds over magenta
CREATE_NO_WINDOW = 0x08000000
LEAVE_STEPS, LEAVE_STEP_PX = 10, 20       # a pressed button is left over 200 px, downwards
SHAKES, SHAKE_PX = 12, 80                # a shake: this many swings this far each way

CURSORS = {32512: "arrow", 32644: "size-we", 32645: "size-ns", 32642: "size-nwse", 32643: "size-nesw",
           32649: "hand", 32513: "ibeam"}

PROGRAMS = {
    "classic": dict(label="System Information", argv=["msinfo32.exe"], spec={"className": "#32770"},
                    bar={"controlType": "Button", "name": "Close"}),
    "settings": dict(label="Settings", argv=["explorer.exe", "ms-settings:"], spec={"className": "ApplicationFrameWindow"},
                     bar={"automationId": "TitleBar"}, title={"automationId": "SettingsLabel"}),
    "explorer": dict(label="File Explorer", argv=["explorer.exe", "C:\\Windows"], spec={"className": "CabinetWClass"},
                     bar={"automationId": "TabView"}),
    "notepad": dict(label="Notepad", argv=["notepad.exe"], spec={"className": "Notepad"},
                    bar={"automationId": "Tabs"}),
}
# The window styles a classic program can take, as Windows Forms border styles.
VARIANTS = {
    "dialog": dict(label="Dialog", style="FixedDialog", extra="$f.MinimizeBox=$false;$f.MaximizeBox=$false;"),
    "tool-window": dict(label="Tool window", style="SizableToolWindow", extra=""),
    "borderless": dict(label="Borderless window", style="None", extra=""),
}

_m = {}                                  # measured during the scenes, read by METRICS


class RECT(ctypes.Structure):
    _fields_ = [("left", wintypes.LONG), ("top", wintypes.LONG), ("right", wintypes.LONG), ("bottom", wintypes.LONG)]


class CURSORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD), ("hCursor", wintypes.HANDLE),
                ("ptScreenPos", wintypes.POINT)]


def frame(hwnd):
    """The visible frame (DWMWA_EXTENDED_FRAME_BOUNDS) as left, top, right, bottom."""
    r = RECT()
    if dwmapi.DwmGetWindowAttribute(wintypes.HWND(hwnd), 9, ctypes.byref(r), ctypes.sizeof(r)):
        raise core.Incomplete("DwmGetWindowAttribute refused the frame bounds")
    return r.left, r.top, r.right, r.bottom


def work_area():
    r = RECT()
    user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(r), 0)       # SPI_GETWORKAREA
    return r.left, r.top, r.right, r.bottom


_shapes = {}


def cursor_at(ctx, x, y):
    """The cursor shape the real pointer gets at (x, y)."""
    if not _shapes:
        _shapes.update({user32.LoadCursorW(None, ctypes.c_void_p(i)): name for i, name in CURSORS.items()})
    ctx.hover_point(x, y)
    time.sleep(PROBE)
    ci = CURSORINFO(cbSize=ctypes.sizeof(CURSORINFO))
    if not user32.GetCursorInfo(ctypes.byref(ci)):
        raise core.Incomplete("GetCursorInfo failed")
    return _shapes.get(ci.hCursor, "other")


def desktop(px):
    """Whether a pixel is the magenta desktop, darkened by a shadow or not: its
    green stays 0 under a shadow, and anything the frame draws over it (the
    border line) adds green (2026-11-18: #ad2fad under the border, #d500d5 in
    the shadow beside it)."""
    r, g, b = px
    return g < GREEN_EDGE and r > 2 * g and b > 2 * g


def hexof(px):
    return "#%02x%02x%02x" % tuple(px)


class Solid:
    """The desktop as solid magenta while a frame is read, then its wallpaper again."""

    def __enter__(self):
        buf = ctypes.create_unicode_buffer(520)
        user32.SystemParametersInfoW(0x0073, 520, buf, 0)               # SPI_GETDESKWALLPAPER
        self.wallpaper = buf.value
        self.colour = user32.GetSysColor(1)
        user32.SetSysColors(1, (ctypes.c_int * 1)(1), (wintypes.DWORD * 1)(MAGENTA))
        user32.SystemParametersInfoW(0x0014, 0, "", 2)                  # SPI_SETDESKWALLPAPER, none
        time.sleep(SETTLE)
        return self

    def __exit__(self, *exc):
        user32.SetSysColors(1, (ctypes.c_int * 1)(1), (wintypes.DWORD * 1)(self.colour))
        user32.SystemParametersInfoW(0x0014, 0, self.wallpaper, 2)
        time.sleep(SETTLE)


def top_windows(ctx):
    return ctx.live.children(ctx.live.uia.GetRootElement(), ctx.live.uia.CreateTrueCondition())


def close_others(ctx, keep):
    """Close every program and variant window but `keep`: one window on the
    desktop, so no other shadow falls where a frame is read."""
    specs = [p["spec"] for n, p in PROGRAMS.items() if n != keep]
    specs += [{"name": v["label"]} for n, v in VARIANTS.items() if n != keep]
    gone = False
    for w in top_windows(ctx):
        if any(core.matches(w, s) for s in specs):
            user32.PostMessageW(w.el.CurrentNativeWindowHandle, WM_CLOSE, 0, 0)
            gone = True
    if gone:
        time.sleep(SETTLE)


def activate(ctx, h):
    """Make h the foreground window with a click on an empty part of its title bar."""
    left, top, _, _ = frame(h)
    ctx.click_at(left + CAPTION_POINT, top + 8)
    time.sleep(SETTLE)
    if user32.GetForegroundWindow() != h:
        raise core.Incomplete("the window did not come to the foreground")


def deactivate(ctx, h):
    ctx.click_at(*DESKTOP_POINT)
    time.sleep(SETTLE)
    if user32.GetForegroundWindow() == h:
        raise core.Incomplete("the window stayed in the foreground")


def place(h):
    user32.ShowWindow(h, SW_RESTORE)
    user32.SetWindowPos(h, 0, *PLACE, 0x0004)                            # SWP_NOZORDER
    time.sleep(SETTLE)


def ensure(ctx, name, active=True):
    """The program's window: open, alone on the desktop, placed, and active or not."""
    close_others(ctx, name)
    p = PROGRAMS[name]
    try:
        w = ctx.find_window(timeout=1, foreground=False, **p["spec"])
    except core.Incomplete:
        ctx.launch(*p["argv"])
        w = ctx.find_window(timeout=WAIT, foreground=False, **p["spec"])
        time.sleep(3)
    h = w.el.CurrentNativeWindowHandle
    if user32.IsZoomed(h) or not _placed(h):
        place(h)
    (activate if active else deactivate)(ctx, h)
    return w, h


def _placed(h):
    r = RECT()
    user32.GetWindowRect(wintypes.HWND(h), ctypes.byref(r))
    return (r.left, r.top, r.right - r.left, r.bottom - r.top) == PLACE


def find_in(ctx, w, spec):
    return ctx.find(within=w, timeout=WAIT, depth=6, **spec)


def border(ctx, left, y):
    """The frame line's width at the left edge: pixels from the edge in that are
    the border drawn over the desktop."""
    row = ctx.pixels(left, y, 8, 1)[0]
    return next((i for i, px in enumerate(row) if not (px[0] > px[1] + 40 and px[2] > px[1] + 40)), 8)


def shadow(ctx, x, y, dx, dy):
    """How far a shadow reaches from (x, y) in direction (dx, dy): the distance
    until the desktop is as bright as it is far away."""
    far = max(ctx.pixels(x + dx * (SHADOW_MAX + 20), y + dy * (SHADOW_MAX + 20), 1, 1)[0][0])
    if dx:
        line = ctx.pixels(x - SHADOW_MAX if dx < 0 else x + 1, y, SHADOW_MAX, 1)[0]
        line = line[::-1] if dx < 0 else line
    else:
        line = [r[0] for r in ctx.pixels(x, y + 1, 1, SHADOW_MAX)]
    return next((i for i, px in enumerate(line) if max(px) >= far - 2), SHADOW_MAX)


def band(ctx, along, lo, hi):
    """The cursor shape at each offset lo..hi from an edge, as {offset: shape};
    `along` maps an offset to the screen point probed."""
    return {d: cursor_at(ctx, *along(d)) for d in range(lo, hi)}


def extent(shapes, shape):
    """The offsets carrying `shape`: (first, last), or None."""
    hits = sorted(d for d, s in shapes.items() if s == shape)
    return (hits[0], hits[-1]) if hits else None


def title_scan(ctx, left, top, bar):
    """Where a classic title bar puts its icon and its title: the columns of
    ink on the bar, read left to right (icon, a gap, then the title)."""
    width = 400
    rows = ctx.pixels(left + 1, top + 1, width, bar - 2)
    fill = rows[len(rows) // 2][width - 1]
    ink = [[sum(abs(a - b) for a, b in zip(px, fill)) > INK for px in row] for row in rows]
    cols = [c for c in range(width) if any(r[c] for r in ink)]
    if not cols:
        raise core.Incomplete("no icon or title ink on the classic title bar")
    runs, start = [], cols[0]
    for a, b in zip(cols, cols[1:] + [None]):
        if b is None or b - a > 3:
            runs.append((start, a))
            start = b
    icon = runs[0]
    icon_rows = [r for r, row in enumerate(ink) if any(row[icon[0]:icon[1] + 1])]
    title = runs[1] if len(runs) > 1 else None
    out = {"icon_left": icon[0] + 1, "icon_size": icon[1] - icon[0] + 1, "icon_top": icon_rows[0] + 1}
    if title:
        out["title_left"] = title[0] + 1
    return out


def colour_at(ctx, x, y):
    return hexof(ctx.pixels(x, y, 1, 1)[0][0])


def press(down, settle=SETTLE):
    """The left button down or up where the pointer is."""
    user32.mouse_event(0x0002 if down else 0x0004, 0, 0, 0, 0)
    time.sleep(settle)


def measure(ctx, name, w, h):
    """Everything about one program's frame the screen and the pointer show."""
    if name in _m:
        return
    p = PROGRAMS[name]
    left, top, right, bottom = frame(h)
    m = {"frame": (left, top, right, bottom)}
    bar = find_in(ctx, w, p["bar"]).bounds
    m["bar_height"] = bar[1] + bar[3] - top
    if "title" in p:
        t = find_in(ctx, w, p["title"]).bounds
        m["title_left"] = t[0] - left
    mid_y, mid_x = (top + bottom) // 2, (left + right) // 2
    with Solid():
        activate(ctx, h)
        m["border"] = border(ctx, left, mid_y)
        m["shadow_left"] = shadow(ctx, left - 1, mid_y, -1, 0)
        m["shadow_bottom"] = shadow(ctx, mid_x, bottom - 1, 0, 1)
        deactivate(ctx, h)
        m["shadow_left_inactive"] = shadow(ctx, left - 1, mid_y, -1, 0)
        m["shadow_bottom_inactive"] = shadow(ctx, mid_x, bottom - 1, 0, 1)
        activate(ctx, h)
    # The resize areas, as the real pointer finds them: the cursor at each pixel
    # across the left, bottom and top edges and along the top-left diagonal.
    m["left"] = band(ctx, lambda d: (left + d, mid_y), -SCAN, SCAN)
    m["bottom"] = band(ctx, lambda d: (mid_x - 200, bottom - 1 + d), -SCAN, SCAN)
    m["top"] = band(ctx, lambda d: (left + CAPTION_POINT, top + d), 0, SCAN)
    # The top-left corner grip: how far down the left edge, just outside it, the
    # diagonal cursor reaches.
    m["corner_down"] = band(ctx, lambda d: (left - 3, top + d), 0, CORNER_MAX)
    ctx.hover_point(*DESKTOP_POINT)
    if name == "classic":
        m.update(title_scan(ctx, left, top, m["bar_height"]))
    _m[name] = m


def caption_buttons(ctx, w):
    """The title bar's buttons by role, from UI Automation (Settings names them
    "Close Settings")."""
    out = {}
    for b in ctx.live.children(title_of(ctx, w).el, ctx.live.uia.CreateTrueCondition()):
        if b.control_type == "Button" and b.bounds[2] > 0 and not b.offscreen:
            for role in ("Minimize", "Maximize", "Restore", "Close"):
                if b.name.startswith(role):
                    out[role.lower()] = b
    return out


def title_of(ctx, w):
    for c in ctx.live.children(w.el, ctx.live.uia.CreateTrueCondition()):
        if c.control_type == "TitleBar" or c.automation_id == "TitleBar":
            return c
    raise core.Incomplete(f"{w.name!r}: no title bar")


def leave(ctx, x, y):
    """Drag the pointer down off a pressed button in steps, so the button sees
    it leave before the release (a jump straight to the desktop let the release
    close the window once, 2026-11-18)."""
    for step in range(1, LEAVE_STEPS + 1):
        ctx.hover_point(x, y + step * LEAVE_STEP_PX)
        time.sleep(0.05)
    time.sleep(0.5)


def measure_states(ctx, w, h):
    """The classic caption buttons' fills at rest, under the pointer and pressed,
    read a few pixels in from each button's corner (clear of its glyph)."""
    if "states" in _m:
        return
    s = {}
    for role, b in caption_buttons(ctx, w).items():
        x, y, bw, bh = b.bounds
        at = (x + 4, y + bh // 2)
        s[(role, "rest")] = colour_at(ctx, *at)
        ctx.hover_point(x + bw // 2, y + bh // 2)
        time.sleep(SETTLE)
        s[(role, "hover")] = colour_at(ctx, *at)
        if role != "close":
            press(True)
            s[(role, "pressed")] = colour_at(ctx, *at)
            leave(ctx, x + bw // 2, y + bh // 2)     # off the button, so the release does nothing
            press(False)
        ctx.hover_point(*DESKTOP_POINT)
        time.sleep(SETTLE)
    _m["states"] = s
    # The close button pressed: the release happens off it, so the window stays.
    b = caption_buttons(ctx, w)["close"]
    x, y, bw, bh = b.bounds
    ctx.hover_point(x + bw // 2, y + bh // 2)
    time.sleep(SETTLE)
    press(True)
    s[("close", "pressed")] = colour_at(ctx, x + 4, y + bh // 2)
    leave(ctx, x + bw // 2, y + bh // 2)
    press(False)
    if not user32.IsWindow(h):
        raise core.Incomplete("the pressed close button closed the window")


def measure_behaviour(ctx, w, h):
    """What the title bar does, tried on the classic window: a double-click on
    the caption maximizes and restores, the top-right screen pixel of a
    maximized window is its close button, a double-click on the top edge
    fills the work area's height."""
    if "behaviour" in _m:
        return
    b = {}
    left, top, right, _ = frame(h)
    for _ in range(2):
        ctx.click_at(left + CAPTION_POINT, top + 8)
    time.sleep(SETTLE)
    b["caption_double_click_maximizes"] = bool(user32.IsZoomed(h))
    sw = user32.GetSystemMetrics(0)
    ctx.hover_point(sw - 1, 0)
    time.sleep(SETTLE)
    close = caption_buttons(ctx, w)["close"].bounds
    b["corner_is_close"] = colour_at(ctx, close[0] + 4, close[1] + close[3] // 2)
    ctx.hover_point(*DESKTOP_POINT)
    time.sleep(SETTLE)
    left, top, _, _ = frame(h)
    for _ in range(2):
        ctx.click_at(left + CAPTION_POINT, top + 8)
    time.sleep(SETTLE)
    b["second_double_click_restores"] = not user32.IsZoomed(h)
    place(h)
    activate(ctx, h)
    left, top, _, _ = frame(h)
    for _ in range(2):
        ctx.click_at(left + CAPTION_POINT, top)
    time.sleep(SETTLE)
    _, ftop, _, fbottom = frame(h)
    _, wtop, _, wbottom = work_area()
    b["top_edge_fills_height"] = (ftop, fbottom) == (wtop, wbottom) or (abs(ftop - wtop) <= 1 and abs(fbottom - wbottom) <= 8)
    b["min_track"] = (user32.GetSystemMetrics(34), user32.GetSystemMetrics(35))
    # A drag on a maximized window's title bar restores it under the pointer.
    user32.ShowWindow(h, SW_MAXIMIZE)
    time.sleep(SETTLE)
    _, mtop, _, _ = frame(h)
    ctx.hover_point(PLACE[0] + CAPTION_POINT, max(0, mtop) + 8)
    press(True, 0.2)
    for step in range(1, 11):
        ctx.hover_point(PLACE[0] + CAPTION_POINT, max(0, mtop) + 8 + step * 20)
        time.sleep(0.05)
    press(False)
    b["drag_restores"] = not user32.IsZoomed(h)
    place(h)
    activate(ctx, h)
    # A right-click on the title bar opens the system menu.
    left, top, _, _ = frame(h)
    ctx.click_at(left + CAPTION_POINT, top + 8, "right")
    try:
        ctx.find(controlType="Menu", name="System", depth=3, timeout=5)
        b["right_click_menu"] = True
    except core.Incomplete:
        b["right_click_menu"] = False
    ctx.keys("esc")
    time.sleep(SETTLE)
    b["shake_minimizes_others"] = shake(ctx, h)
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced") as k:
            b["disallow_shaking"] = winreg.QueryValueEx(k, "DisallowShaking")[0]
    except OSError:
        b["disallow_shaking"] = None
    place(h)
    _m["behaviour"] = b


def shake(ctx, h):
    """Shake the window by its title bar with a second window open: whether the
    second one is minimized."""
    script = ("Add-Type -AssemblyName System.Windows.Forms;$f=New-Object Windows.Forms.Form;"
              "$f.Text='Shake witness';$f.StartPosition='Manual';$f.Location='1500,200';$f.Size='300,200';"
              "[void]$f.ShowDialog()")
    subprocess.Popen(["powershell.exe", "-NoProfile", "-Command", script], creationflags=CREATE_NO_WINDOW)
    witness = ctx.find_window(timeout=WAIT, name="Shake witness").el.CurrentNativeWindowHandle
    activate(ctx, h)
    left, top, _, _ = frame(h)
    x, y = left + CAPTION_POINT, top + 8
    ctx.hover_point(x, y)
    press(True, 0.2)
    for i in range(SHAKES):
        ctx.hover_point(x + (SHAKE_PX if i % 2 else -SHAKE_PX), y)
        time.sleep(0.04)
    ctx.hover_point(x, y)
    press(False)
    time.sleep(SETTLE)
    minimized = bool(user32.IsIconic(witness))
    user32.PostMessageW(witness, WM_CLOSE, 0, 0)
    time.sleep(SETTLE)
    place(h)
    return minimized


def opener(name, active=True, then=None):
    def open_(ctx):
        w, h = ensure(ctx, name, active)
        measure(ctx, name, w, h)
        if name == "classic":
            measure_states(ctx, w, h)
            measure_behaviour(ctx, w, h)
        w, h = ensure(ctx, name, active)
        if then:
            then(ctx, w, h)
    return open_


def title_root(name):
    def root(ctx):
        return title_of(ctx, ctx.find_window(timeout=WAIT, foreground=False, **PROGRAMS[name]["spec"]))
    return root


def maximize(ctx, w, h):
    user32.ShowWindow(h, SW_MAXIMIZE)
    time.sleep(SETTLE)


def restore(ctx):
    w = ctx.find_window(timeout=WAIT, foreground=False, **PROGRAMS["classic"]["spec"])
    place(w.el.CurrentNativeWindowHandle)


def hover_close(ctx, w, h):
    x, y, bw, bh = caption_buttons(ctx, w)["close"].bounds
    ctx.hover_point(x + bw // 2, y + bh // 2)
    time.sleep(SETTLE)


def pointer_away(ctx):
    ctx.hover_point(*DESKTOP_POINT)
    time.sleep(SETTLE)


def system_menu(ctx, w, h):
    ctx.keys("alt+space")
    time.sleep(SETTLE)


def menu_root(ctx):
    # UI Automation puts the open system menu under its window, not on the desktop.
    return ctx.find(controlType="Menu", name="System", depth=3, timeout=WAIT)


def esc(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)


def accent(on):
    """Show the accent colour on title bars and borders (the Colors page's
    switch writes this value; DWM reads it on the theme-change broadcast)."""
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\DWM") as k:
        winreg.SetValueEx(k, "ColorPrevalence", 0, winreg.REG_DWORD, 1 if on else 0)
    res = ctypes.c_size_t()
    user32.SendMessageTimeoutW(wintypes.HWND(0xFFFF), WM_SETTINGCHANGE, 0, "ImmersiveColorSet", 2, 5000,
                               ctypes.byref(res))
    time.sleep(SETTLE * 2)


def accent_on(ctx, w, h):
    left, top, _, _ = frame(h)
    off = colour_at(ctx, left + CAPTION_POINT, top + 8)
    accent(True)
    activate(ctx, h)
    on = colour_at(ctx, left + CAPTION_POINT, top + 8)
    edge = colour_at(ctx, left, (top + frame(h)[3]) // 2)
    deactivate(ctx, h)
    on_inactive = colour_at(ctx, left + CAPTION_POINT, top + 8)
    activate(ctx, h)
    if on == off:
        raise core.Incomplete(f"the title bar stayed {off} with the accent colour on")
    _m["accent"] = {"off": off, "on": on, "border": edge, "inactive": on_inactive}


def accent_off(ctx):
    accent(False)


def variant_open(name):
    def open_(ctx):
        close_others(ctx, name)
        v = VARIANTS[name]
        try:
            ctx.find_window(timeout=1, foreground=False, name=v["label"])
        except core.Incomplete:
            script = ("Add-Type -AssemblyName System.Windows.Forms;$f=New-Object Windows.Forms.Form;"
                      f"$f.Text='{v['label']}';$f.FormBorderStyle='{v['style']}';{v['extra']}"
                      f"$f.StartPosition='Manual';$f.Location='{PLACE[0]},{PLACE[1]}';$f.Size='640,400';"
                      "[void]$f.ShowDialog()")
            subprocess.Popen(["powershell.exe", "-NoProfile", "-Command", script], creationflags=CREATE_NO_WINDOW)
        w = ctx.find_window(timeout=WAIT, name=v["label"])
        h = w.el.CurrentNativeWindowHandle
        if name != "borderless":
            activate(ctx, h)
            left, top, _, _ = frame(h)
            b = caption_buttons(ctx, w)["close"].bounds
            _m.setdefault("variants", {})[name] = {"bar_height": b[1] + b[3] - top,
                                                   "buttons": sorted(caption_buttons(ctx, w))}
    return open_


def variant_root(name):
    def root(ctx):
        w = ctx.find_window(timeout=WAIT, foreground=False, name=VARIANTS[name]["label"])
        return w if name == "borderless" else title_of(ctx, w)
    return root


def variant_close(name):
    def close(ctx):
        w = ctx.find_window(timeout=WAIT, foreground=False, name=VARIANTS[name]["label"])
        user32.PostMessageW(w.el.CurrentNativeWindowHandle, WM_CLOSE, 0, 0)
        ctx.gone(name=VARIANTS[name]["label"])
    return close


def nothing(ctx):
    pass


SCENES = []
for _name, _p in PROGRAMS.items():
    SCENES += [dict(slug=f"{_name}-title-bar", label=f"{_p['label']} title bar", kind="region", open=opener(_name),
                    root=title_root(_name), close=nothing, expand=False),
               dict(slug=f"{_name}-title-bar-inactive", label=f"{_p['label']} title bar, inactive", kind="state",
                    open=opener(_name, False), root=title_root(_name), close=nothing, expand=False)]
SCENES += [
    dict(slug="classic-title-bar-maximized", label="System Information title bar, maximized", kind="state",
         open=opener("classic", then=maximize), root=title_root("classic"), close=restore, expand=False),
    dict(slug="classic-close-hover", label="Close button, hover", kind="state",
         open=opener("classic", then=hover_close), root=title_root("classic"), close=pointer_away, expand=False),
    dict(slug="system-menu", label="System menu", kind="contextMenu", open=opener("classic", then=system_menu),
         root=menu_root, close=esc),
]
for _name, _v in VARIANTS.items():
    SCENES.append(dict(slug=_name, label=_v["label"], kind="surface", open=variant_open(_name),
                       root=variant_root(_name), close=variant_close(_name), expand=False))
SCENES.append(dict(slug="classic-title-bar-accent", label="System Information title bar, accent colour on",
                   kind="state", open=opener("classic", then=accent_on), root=title_root("classic"),
                   close=accent_off, expand=False))


def _metric(out, parent, slug, label, order, value, read_by=None, behaviour=None):
    m = {"value": value, "unit": "px", "scaling": "scales"}
    if read_by:
        m["readBy"] = read_by
    r = {"id": f"{parent}.{slug}", "parent": parent, "kind": "metric", "label": label, "order": order, "measure": m}
    if behaviour:
        r["behaviour"] = behaviour
    out.append(r)


def _band(shapes, shape, what):
    found = extent(shapes, shape)
    if found is None:
        raise ValueError(f"no {shape} cursor along {what}")
    return found


def _need(key):
    if key not in _m:
        raise ValueError(f"{key} was not measured")
    return _m[key]


def METRICS(records):
    """The frame of each program as numbers, from the captured buttons and what
    the scenes measured; the caption buttons' colours as states; and what the
    title bar does, as tried."""
    p = f"win11.{SURFACE}."
    out = []
    for order, (name, prog) in enumerate(PROGRAMS.items()):
        m, root = _need(name), f"{name}-title-bar"
        close = next((r for r in records if r.get("parent") == p + root and r["label"].startswith("Close")), None)
        if close is None:
            raise ValueError(f"{root}: no close button was captured")
        _metric(out, root, "title-bar-height", "Title bar height", 100, m["bar_height"],
                behaviour="From the frame's top edge, its 1 px border included, to the bottom of the bar")
        _metric(out, root, "caption-button-width", "Caption button width", 101, close["bounds"]["width"])
        _metric(out, root, "caption-button-height", "Caption button height", 102, close["bounds"]["height"])
        _metric(out, root, "border-width", "Border width", 103, m["border"])
        _metric(out, root, "shadow-side", "Shadow reach beside the window", 104, m["shadow_left"])
        _metric(out, root, "shadow-below", "Shadow reach below the window", 105, m["shadow_bottom"])
        _metric(out, f"{root}-inactive", "shadow-side", "Shadow reach beside the window", 104, m["shadow_left_inactive"])
        _metric(out, f"{root}-inactive", "shadow-below", "Shadow reach below the window", 105,
                m["shadow_bottom_inactive"])
        lo, hi = _band(m["left"], "size-we", f"{name}'s left edge")
        _metric(out, root, "resize-outside", "Resize band outside the border", 106, -lo,
                behaviour="Columns beside the left (and right) border that give the horizontal resize cursor")
        _metric(out, root, "resize-inside", "Resize band on and inside the border", 107, hi + 1)
        lo, hi = _band(m["bottom"], "size-ns", f"{name}'s bottom edge")
        _metric(out, root, "resize-below", "Resize band below the bottom border", 108, hi)
        lo, hi = _band(m["top"], "size-ns", f"{name}'s top edge")
        _metric(out, root, "resize-top", "Resize band at the top edge", 109, hi - lo + 1,
                behaviour=f"Rows {lo} to {hi} below the frame's top edge; a window has no resize band above its top")
        _, hi = _band(m["corner_down"], "size-nwse", f"{name}'s top-left corner, down")
        _metric(out, root, "corner-grip", "Corner grip along the side edge", 110, hi + 1)
        if "title_left" in m:
            _metric(out, root, "title-left", "Title start", 112, m["title_left"])
    c = _m["classic"]
    _metric(out, "classic-title-bar", "icon-left", "Icon start", 113, c["icon_left"])
    _metric(out, "classic-title-bar", "icon-top", "Icon top", 114, c["icon_top"])
    _metric(out, "classic-title-bar", "icon-width", "Icon ink width", 115, c["icon_size"])
    for name, v in _need("variants").items():
        _metric(out, name, "title-bar-height", "Title bar height", 100, v["bar_height"],
                behaviour=f"Caption buttons: {', '.join(v['buttons'])}")
    b = _need("behaviour")
    _metric(out, "classic-title-bar", "minimum-width", "Minimum window width", 116, b["min_track"][0],
            read_by="SM_CXMINTRACK")
    _metric(out, "classic-title-bar", "minimum-height", "Minimum window height", 117, b["min_track"][1],
            read_by="SM_CYMINTRACK")
    st = _need("states")
    for order, (role, label) in enumerate((("minimize", "Minimize"), ("maximize", "Maximize"), ("close", "Close"))):
        for state in ("rest", "hover", "pressed"):
            out.append({"id": f"classic-title-bar.{role}-{state}", "parent": "classic-title-bar", "kind": "state",
                        "label": f"{label}, {state}", "order": 120 + order * 3 + ("rest", "hover", "pressed").index(state),
                        "state": state, "colours": {"fill": {"light": st[(role, state)]}}})
    a = _need("accent")
    out.append({"id": "classic-title-bar-accent.fill", "parent": "classic-title-bar-accent", "kind": "state",
                "label": "Accent colour on title bars and borders", "order": 100,
                "colours": {"title-bar": {"light": a["on"]}, "border": {"light": a["border"]},
                            "title-bar-off": {"light": a["off"]}, "title-bar-inactive": {"light": a["inactive"]}},
                "behaviour": "The Colors page's switch for title bars and borders paints the active window's title "
                f"bar and border with the accent colour ({a['on']}); an inactive window keeps {a['inactive']}."})
    cursors = (("edges-side", "Left and right edges", _need("classic")["left"], "size-we"),
               ("edges-top-bottom", "Top and bottom edges", _need("classic")["bottom"], "size-ns"),
               ("corners", "Corners", _need("classic")["corner_down"], "size-nwse"))
    for order, (slug, label, shapes, shape) in enumerate(cursors):
        _band(shapes, shape, label)
        out.append({"id": f"classic-title-bar.cursor-{slug}", "parent": "classic-title-bar", "kind": "cursor",
                    "label": label, "order": 140 + order,
                    "behaviour": f"The {shape} resize cursor shows over the band, the arrow everywhere else"})

    def said(ok, yes, no):
        return yes if ok else no
    gestures = [
        ("double-click", "Double-click the title bar", said(
            b["caption_double_click_maximizes"] and b["second_double_click_restores"],
            "Maximizes a restored window and restores a maximized one",
            "Did not toggle maximize in the measured run")),
        ("maximized-corner", "Maximized window's corner", said(
            b["corner_is_close"] == st[("close", "hover")],
            "With the window maximized, the screen's top-right pixel is the close button's hover area",
            f"With the window maximized, the screen's top-right pixel left the close button at {b['corner_is_close']}")),
        ("top-edge-double-click", "Double-click the top edge", said(
            b["top_edge_fills_height"], "Stretches the window to the work area's full height and keeps its width",
            "Did not stretch the window to the work area's height in the measured run")),
        ("drag-maximized", "Drag a maximized window", said(
            b["drag_restores"], "Dragging the title bar of a maximized window restores it under the pointer",
            "Dragging the title bar of a maximized window left it maximized in the measured run")),
        ("right-click", "Right-click the title bar", said(
            b["right_click_menu"], "Opens the system menu at the pointer", "Opened no system menu in the measured run")),
        ("shake", "Shake the title bar", said(
            b["shake_minimizes_others"], "Minimizes every other window",
            "Does nothing at the reference's defaults: the other windows stay open")),
    ]
    for order, (slug, label, text) in enumerate(gestures):
        out.append({"id": f"classic-title-bar.{slug}", "parent": "classic-title-bar", "kind": "gesture",
                    "label": label, "order": 150 + order, "behaviour": text})
    return out


# Not measurable in the reference VM (see the module's docstring): Microsoft's
# published radii, until the bare-metal read section 9 parks.
GEOMETRY = ("Microsoft Learn, Geometry in Windows (windows/apps/design/signature-experiences/geometry), "
            "read 2026-11-18")
HAND = [
    {"id": "classic-title-bar.corners", "parent": "classic-title-bar", "kind": "group", "label": "Corner radii",
     "order": 170},
    {"id": "classic-title-bar.corners.window", "parent": "classic-title-bar.corners", "kind": "metric", "label": "Window corner radius", "order": 1,
     "measure": {"value": 8, "unit": "px", "scaling": "scales"},
     "behaviour": f"Top-level windows; square when maximized or snapped. {GEOMETRY}"},
    {"id": "classic-title-bar.corners.flyout", "parent": "classic-title-bar.corners", "kind": "metric", "label": "Flyout and dialog corner radius",
     "order": 2, "measure": {"value": 8, "unit": "px", "scaling": "scales"}, "behaviour": GEOMETRY},
    {"id": "classic-title-bar.corners.tooltip", "parent": "classic-title-bar.corners", "kind": "metric", "label": "Tooltip corner radius", "order": 3,
     "measure": {"value": 4, "unit": "px", "scaling": "scales"}, "behaviour": GEOMETRY},
    {"id": "classic-title-bar.drag-to-edge", "parent": "classic-title-bar", "kind": "gesture",
     "label": "Drag to a screen edge", "order": 160,
     "behaviour": "Dragging the title bar to the top edge maximizes the window and to a side edge or corner "
     "snaps it; window/switching.json records the zones and their preview"},
]
