"""Capture one Windows 11 surface through UI Automation, inside the reference VM.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 3.

Runs on the VM's capture runtime (embeddable Python and comtypes, runtime.json)
in ref's desktop, started by capture.py through ref-run.ps1:

    python capture-ui.py --surface notepad-smoke --out C:\\ios\\out\\records.json
                         [--shots DIR] [--meta FILE] [--profile fhd-100] [--dump FILE]

It refuses (exit 2) when any value of the capture profile does not hold, naming
it, as "profile fhd-100: scaling is 125, expected 100". It opens the surface
through scripts/inventory/surfaces/<surface>.py, walks it (capture_core.walk),
closes it the way a user would and writes raw records, the unnamed and ambiguous
lists, and a PNG per state. A capture that did not see everything (a flyout that
never settled, a walk error) writes complete: false and exits 3; capture.py then
publishes nothing. --dump writes the tree itself, as a test fixture.
"""
import argparse
import ctypes
import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import time
from ctypes import wintypes

HERE = pathlib.Path(__file__).resolve().parent


def _load(name, path):
    # The embeddable Python's ._pth mode leaves the script's folder off sys.path,
    # so the shared core is loaded by its path.
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


core = _load("capture_core", HERE / "capture_core.py")

# Staged beside this tool by capture.py, so a capture reads the session with the
# probe of this checkout, not the copy baked into the clean checkpoint.
PROBE = HERE / "probe-profile.ps1"
SET_PROFILE = HERE / "set-profile.ps1"
# set-profile.ps1's own waits add up to at most about 230 s (scaling 3 x 3 s,
# a theme 60 s, the text size slider 60 s plus 90 s plus its settling); this is
# the same allowance ref-vm.ps1 -Verify gives it.
APPLY_TIMEOUT_S = 300
SETTLE_S = 10.0         # a flyout gets this long to open and stop changing
MIN_POLLS = 3           # and is always looked at this often: a provider busy animating answers slowly
POLL_S = 0.2
EXPAND_COLLAPSE = 10005
INVOKE = 10000
PROP_EC_AVAILABLE = 30028
PROP_EC_STATE = 30070
EXPANDED, COLLAPSED = 1, 0
TREE_ELEMENT, TREE_CHILDREN, TREE_DESCENDANTS = 1, 2, 4
# Top-level window classes that are popups themselves: a Win32 menu, a combo box
# list, a windowed XAML popup. A popup hosted elsewhere fails the capture (exit 3)
# rather than being missed, and is added here when it is met.
# A WinUI 3 menu opens in its own top-level PopupWindowSiteBridge (the desktop's
# context menu and its submenus, seen 2026-11-16 on 26300.9974).
POPUP_WINDOWS = {"#32768", "ComboLBox", "Xaml_WindowedPopupClass", "Popup", "Microsoft.UI.Content.PopupWindowSiteBridge"}
PROP_CONTROL_TYPE = 30003
PROP_PROCESS_ID = 30002
PROP_IS_OFFSCREEN = 30022
# Name, AutomationId, ClassName, ControlType, BoundingRectangle, IsOffscreen,
# AcceleratorKey, IsEnabled, IsExpandCollapsePatternAvailable, ProcessId
CACHED = (30005, 30011, 30012, PROP_CONTROL_TYPE, 30001, PROP_IS_OFFSCREEN, 30006, 30010, PROP_EC_AVAILABLE,
          PROP_PROCESS_ID)

user32 = gdi32 = None    # opened by win32() on Windows, so the module also imports on Linux for its tests


def win32():
    """Open user32 and gdi32 with the handle types declared, so no handle is cut to 32 bits."""
    global user32, gdi32
    if user32 is not None:
        return
    u = ctypes.WinDLL("user32", use_last_error=True)
    g = ctypes.WinDLL("gdi32", use_last_error=True)
    H = ctypes.c_void_p
    u.GetDC.restype, u.GetDC.argtypes = H, [H]
    u.ReleaseDC.argtypes = [H, H]
    u.SetForegroundWindow.argtypes = [H]
    u.SetProcessDpiAwarenessContext.argtypes = [H]
    g.CreateCompatibleDC.restype, g.CreateCompatibleDC.argtypes = H, [H]
    g.CreateCompatibleBitmap.restype, g.CreateCompatibleBitmap.argtypes = H, [H, ctypes.c_int, ctypes.c_int]
    g.SelectObject.restype, g.SelectObject.argtypes = H, [H, H]
    g.BitBlt.argtypes = [H, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, H, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    g.GetDIBits.argtypes = [H, H, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
    g.DeleteObject.argtypes = [H]
    g.DeleteDC.argtypes = [H]
    user32, gdi32 = u, g


def apply_profile(name, profiles):
    """Apply the live settings of profile `name` (set-profile.ps1). Returns the
    lines to print and exit 2 with when the profile cannot be applied here."""
    try:
        r = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SET_PROFILE),
                            "-Profiles", str(profiles), "-Name", name], capture_output=True, text=True,
                            timeout=APPLY_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return [f"profile {name}: set-profile did not finish within {APPLY_TIMEOUT_S} s"]
    if r.returncode != 0:
        return [line for line in (r.stdout + r.stderr).strip().splitlines() if line] or [f"set-profile exited {r.returncode}"]
    return []


def read_profile():
    r = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(PROBE)],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise SystemExit(f"capture-ui: the profile probe failed: {r.stderr.strip()}")
    return json.loads(r.stdout.strip().splitlines()[-1])


# ---------------------------------------------------------------- input --
VK = {"alt": 0x12, "ctrl": 0x11, "shift": 0x10, "win": 0x5B, "esc": 0x1B, "enter": 0x0D, "tab": 0x09,
      "space": 0x20, "down": 0x28, "up": 0x26, "left": 0x25, "right": 0x27, "home": 0x24, "end": 0x23,
      **{f"f{n}": 0x6F + n for n in range(1, 13)}}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def keys(chord):
    """Press a chord such as "alt+f4" or "esc" the way a keyboard does."""
    codes = [VK[k] if k in VK else ord(k.upper()) for k in chord.lower().split("+")]
    seq = [(c, 0) for c in codes] + [(c, 2) for c in reversed(codes)]
    arr = (INPUT * len(seq))(*[INPUT(type=1, ki=KEYBDINPUT(wVk=c, dwFlags=f)) for c, f in seq])
    if user32.SendInput(len(seq), arr, ctypes.sizeof(INPUT)) != len(seq):
        raise core.Incomplete(f"SendInput refused {chord}")
    time.sleep(0.3)


# Down and up flags of MOUSEINPUT per button.
BUTTONS = {"left": (0x0002, 0x0004), "right": (0x0008, 0x0010), "middle": (0x0020, 0x0040)}


def click(x, y, button="left"):
    """Press and release a mouse button at (x, y) in physical pixels (the tool is
    per-monitor DPI aware, so UI Automation bounds are in the same pixels)."""
    down, up = BUTTONS[button]
    if not user32.SetCursorPos(int(x), int(y)):
        raise core.Incomplete(f"SetCursorPos refused ({x}, {y})")
    arr = (INPUT * 2)(INPUT(type=0, mi=MOUSEINPUT(dwFlags=down)), INPUT(type=0, mi=MOUSEINPUT(dwFlags=up)))
    if user32.SendInput(2, arr, ctypes.sizeof(INPUT)) != 2:
        raise core.Incomplete(f"SendInput refused a {button} click")
    time.sleep(0.3)


# ----------------------------------------------------------- screenshot --
class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG), ("biYPelsPerMeter", wintypes.LONG),
                ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


def grab(path, x, y, w, h):
    """Copy a screen rectangle to a PNG."""
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    bmp = gdi32.CreateCompatibleBitmap(screen, w, h)
    try:
        gdi32.SelectObject(mem, bmp)
        if not gdi32.BitBlt(mem, 0, 0, w, h, screen, x, y, 0x00CC0020 | 0x40000000):  # SRCCOPY | CAPTUREBLT
            raise core.Incomplete("BitBlt failed")
        info = BITMAPINFOHEADER(biSize=ctypes.sizeof(BITMAPINFOHEADER), biWidth=w, biHeight=-h, biPlanes=1, biBitCount=32)
        buf = ctypes.create_string_buffer(w * h * 4)
        if gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(info), 0) != h:
            raise core.Incomplete("GetDIBits failed")
        bgra = buf.raw
        rgb = bytearray(w * h * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = bgra[2::4], bgra[1::4], bgra[0::4]
        core.write_png(path, w, h, bytes(rgb))
    finally:
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(None, screen)


# ------------------------------------------------------------- the tree --
class Live:
    """The UIA client: one per capture.

    Every read goes through one CacheRequest: a FindAllBuildCache call returns an
    element's children with all their properties in one cross-process round trip,
    where reading each property on its own costs one each (about 9 per element,
    which made a menu poll take seconds in the VM, seen 2026-11-14).
    """

    def __init__(self):
        from _ctypes import COMError       # what a call on a vanished element raises (not an OSError)
        import comtypes.client
        from comtypes.gen import UIAutomationClient as UIA
        self.COMError = COMError
        self.UIA = UIA
        self.uia = comtypes.client.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
        self.cache = self.uia.CreateCacheRequest()
        for prop in CACHED:
            self.cache.AddProperty(prop)
        self.control_view = self.uia.ControlViewCondition
        self.walker = self.uia.ControlViewWalker
        visible = self.uia.CreatePropertyCondition(PROP_IS_OFFSCREEN, False)
        popup = self.uia.CreateOrCondition(self.uia.CreatePropertyCondition(PROP_CONTROL_TYPE, 50009),
                                           self.uia.CreatePropertyCondition(PROP_CONTROL_TYPE, 50008))
        self.visible_popup = self.uia.CreateAndCondition(popup, visible)

    def wrap(self, el):
        """A LiveElement for a raw element, its properties fetched in one call."""
        return LiveElement(self, el.BuildUpdatedCache(self.cache))

    def children(self, el, condition=None, limit=core.MAX_RECORDS):
        """el's children, refused past `limit` before one wrapper is made.

        The children come in one FindAllBuildCache call, never one round trip
        per element: an element-by-element walk is what made a poll take seconds
        before the CacheRequest (2026-11-14), and it would slow every surface to
        spare only one that fails the capture anyway.
        """
        found = el.FindAllBuildCache(TREE_CHILDREN, condition or self.control_view, self.cache)
        n = found.Length if found else 0
        if n > limit:
            raise core.Incomplete(f"{n} children under one element, more than the {limit} left to read")
        return [LiveElement(self, found.GetElement(i)) for i in range(n)]

    def hosts(self, pid):
        """Where process `pid` shows popups: its top-level popup windows (a Win32 menu,
        a combo list, a windowed XAML popup) and the popup hosts its own windows
        carry as direct children (a WinUI PopupHost bridge, seen 2026-11-14 on
        Notepad). Only these are searched, never a whole window's content, so the
        cost of a poll does not grow with the surface."""
        self.discovery_complete = True
        out = []
        tops = self.children(self.uia.GetRootElement(), self.uia.CreatePropertyCondition(PROP_PROCESS_ID, pid))
        for top in tops:
            if top.offscreen:
                continue
            if top.class_name in POPUP_WINDOWS or top.control_type in ("Menu", "List"):
                out.append(top.el)
            else:
                try:
                    kids = self.children(top.el)
                except self.COMError:
                    # A top-level window that closed between the listing and this
                    # read (a tooltip, a closing flyout; seen 2026-11-14). This
                    # discovery is incomplete: expand() does not count the poll
                    # toward settling, and the next poll looks again.
                    self.discovery_complete = False
                    continue
                out.extend(k.el for k in kids if "Popup" in k.class_name and not k.offscreen)
        return out

    def popups(self, pid):
        """The outermost Menu and List containers now on screen in process `pid`,
        by runtime id: a container inside another is read as one of its entries,
        never on its own (core.outermost)."""
        out = {}
        for host in self.hosts(pid):
            found = host.FindAll(TREE_ELEMENT | TREE_DESCENDANTS, self.visible_popup)
            n = found.Length if found else 0
            if n > core.MAX_RECORDS:
                raise core.Incomplete(f"{n} popup containers in one host")
            for j in range(n):
                e = found.GetElement(j)
                out[tuple(e.GetRuntimeId())] = e
        return {k: out[k] for k in core.outermost(out, self._parent)}

    def _parent(self, el):
        """(runtime id, element) of el's parent, None at the desktop root. A read
        that fails is not a root: it fails the capture (core.outermost)."""
        try:
            up = self.walker.GetParentElement(el)
            return (tuple(up.GetRuntimeId()), up) if up else None
        except self.COMError as exc:
            raise core.Incomplete(f"a popup's parent could not be read ({exc})") from None

    def gone(self, el):
        """True once a popup element is closed: hidden, empty or no longer there."""
        try:
            r = el.CurrentBoundingRectangle
            return bool(el.CurrentIsOffscreen) or r.right <= r.left or r.bottom <= r.top
        except self.COMError:
            return True


class LiveElement:
    def __init__(self, live, el):
        self.live, self.el = live, el          # el carries the CACHED properties
        self.name = el.CachedName or ""
        self.automation_id = el.CachedAutomationId or ""
        self.class_name = el.CachedClassName or ""
        self.control_type = core.CONTROL_TYPES.get(el.CachedControlType, "Custom")
        r = el.CachedBoundingRectangle
        self.bounds = (r.left, r.top, max(0, r.right - r.left), max(0, r.bottom - r.top))
        self.offscreen = bool(el.CachedIsOffscreen)
        self.accelerator = el.CachedAcceleratorKey or ""
        self.enabled = bool(el.CachedIsEnabled)
        self.can_expand = bool(el.GetCachedPropertyValue(PROP_EC_AVAILABLE))
        self.pid = el.CachedProcessId
        self._opened = {}

    def children(self, limit=core.MAX_RECORDS):
        return self.live.children(self.el, limit=limit)

    def _pattern(self):
        return self.el.GetCurrentPattern(EXPAND_COLLAPSE).QueryInterface(self.live.UIA.IUIAutomationExpandCollapsePattern)

    def expand(self, skip=None):
        """Open, wait for the entries to settle, return them. A popup container
        that `skip` accepts (an environment list) is returned itself, rows unread."""
        for _ in range(MIN_POLLS):            # what was open before, read whole
            before = self.live.popups(self.pid)
            if getattr(self.live, "discovery_complete", True):
                break
            time.sleep(POLL_S)
        else:
            raise core.Incomplete(f"{self.name or self.automation_id}: the popups open before it could not be read")
        self._pattern().Expand()
        last, stable, counts = None, 0, []
        deadline = time.monotonic() + SETTLE_S
        while len(counts) < MIN_POLLS or time.monotonic() < deadline:
            time.sleep(POLL_S)
            keys, items = self._entries(before, skip)
            counts.append(len(items))
            # The popup set is part of what must hold still: a host found late, or
            # one whose read failed this poll (keys None), resets the count.
            sig = None if keys is None else (keys, tuple((i.control_type, i.name, i.automation_id, i.bounds)
                                                         for i in items))
            if items and sig is not None and sig == last:
                stable += 1
                if stable >= 2:                # three equal polls: open and done animating
                    return items
            else:
                stable = 0
            last = sig
        raise core.Incomplete(f"{self.name or self.automation_id}: its entries did not appear and settle within "
                              f"{SETTLE_S:.0f} s (entries per poll: {counts}; new popups: {len(self._opened)})")

    def _entries(self, before, skip=None):
        """(popup keys, entries) for one poll; keys is None when discovery was
        incomplete. A Win32 menu or a WinUI flyout opens as a new Menu or List
        container in the same process, found again on every poll (the search is
        scoped to popup hosts, so it stays cheap); a combo box may instead show
        its list as its own children. Every popup ever opened is kept in
        _opened, so collapse() waits for all of them to go."""
        now = {k: e for k, e in self.live.popups(self.pid).items() if k not in before}
        complete = getattr(self.live, "discovery_complete", True)
        self._opened.update(now)
        if now:
            out = []
            for e in now.values():
                box = self.live.wrap(e)
                out.extend([box] if skip and skip(box) else self.live.children(e))
        else:
            out = [c for c in self.children() if c.control_type in ("ListItem", "MenuItem") and not c.offscreen]
        return (tuple(sorted(now)) if complete else None), out

    def _closed(self):
        try:
            expanded = self.el.GetCurrentPropertyValue(PROP_EC_STATE) == EXPANDED
        except self.live.COMError:
            expanded = False
        return not expanded and all(self.live.gone(e) for e in self._opened.values())

    def collapse(self):
        """Close what expand() opened, the way a keyboard does, and wait until it is gone."""
        keys("esc")
        deadline = time.monotonic() + SETTLE_S
        while time.monotonic() < deadline:
            if self._closed():
                self._opened = {}
                return
            time.sleep(POLL_S)
        self._pattern().Collapse()
        time.sleep(POLL_S)
        if not self._closed():
            raise core.Incomplete(f"{self.name or self.automation_id}: its popup would not close")
        self._opened = {}


class Context:
    """What a surface module may use to open, find and close its surface."""

    def __init__(self, live):
        self.live = live
        self.keys = keys

    def launch(self, *argv):
        return subprocess.Popen(list(argv))

    def click(self, el, button="left"):
        """Click the centre of an element (a LiveElement or anything with bounds)."""
        x, y, w, h = el.bounds
        if w <= 0 or h <= 0:
            raise core.Incomplete(f"{getattr(el, 'name', '') or 'element'} has no area to click")
        click(x + w // 2, y + h // 2, button)

    def invoke(self, el):
        """Run an element's default action through its Invoke pattern (a menu
        entry that a synthesized click does not reach)."""
        try:
            el.el.GetCurrentPattern(INVOKE).QueryInterface(self.live.UIA.IUIAutomationInvokePattern).Invoke()
        except self.live.COMError as exc:
            raise core.Incomplete(f"{el.name or el.automation_id}: Invoke failed ({exc})") from None
        time.sleep(0.3)

    def click_at(self, x, y, button="left"):
        click(x, y, button)

    def powershell(self, script, timeout=120):
        """Run a PowerShell script in the capture's session (a fixture: desktop
        icons, a setting); a failure or a timeout fails the capture."""
        try:
            r = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                               capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise core.Incomplete(f"a fixture script ran past {timeout} s") from None
        if r.returncode:
            raise core.Incomplete(f"a fixture script failed ({r.returncode}): {(r.stderr or r.stdout).strip()[:300]}")
        return r.stdout

    def gone(self, timeout=10, **spec):
        """Wait until no top-level window matching spec is on screen (a menu closing)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if not any(core.matches(w, spec) and not w.offscreen
                           for w in self.live.children(self.live.uia.GetRootElement())):
                    return
            except self.live.COMError:
                pass
            time.sleep(0.3)
        raise core.Incomplete(f"a window matching {spec} stayed open past {timeout} s")

    def find(self, within=None, timeout=15, depth=8, **spec):
        """The first element matching spec, searched breadth first below `within`
        (the desktop when None) to `depth` levels, waiting up to `timeout` s.
        Offscreen containers are searched too: a WinUI menu's popup window
        reports offscreen while its entries are on screen."""
        deadline = time.monotonic() + timeout
        while True:
            level = [within] if within is not None else None
            try:
                level = level or self.live.children(self.live.uia.GetRootElement())
                for _ in range(depth):
                    nxt = []
                    for el in level:
                        if el is not within and core.matches(el, spec):
                            return el
                        nxt.extend(el.children())
                    level = nxt
                    if not level:
                        break
            except self.live.COMError:
                pass                 # a window closed mid-search; look again
            if time.monotonic() >= deadline:
                raise core.Incomplete(f"no element matching {spec} within {timeout} s")
            time.sleep(0.5)

    def find_window(self, timeout=15, **spec):
        """The first top-level window matching spec (className, name, automationId)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            # The raw view: a classic menu's #32768 window is not a control
            # element, so the control view leaves it out.
            for w in self.live.children(self.live.uia.GetRootElement(), self.live.uia.CreateTrueCondition()):
                if core.matches(w, spec) and not w.offscreen:
                    # A popup is never brought forward: a menu that loses the
                    # foreground to itself closes (2026-11-16, the classic menu).
                    if w.class_name not in POPUP_WINDOWS:
                        user32.SetForegroundWindow(w.el.CurrentNativeWindowHandle)
                    return w
            time.sleep(0.5)
        raise core.Incomplete(f"no window matching {spec} within {timeout} s")


def load_surface(name):
    path = HERE / "surfaces" / f"{name}.py"
    if not path.is_file():
        raise SystemExit(f"capture-ui: no surface module {path}")
    return _load(f"surface_{core.slug(name).replace('-', '_')}", path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--surface", required=True)
    ap.add_argument("--module", help="the surface module to walk (default: the one named after --surface)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--shots")
    ap.add_argument("--meta")
    ap.add_argument("--profile", default="fhd-100")
    ap.add_argument("--profiles", default=str(HERE / "profiles.json"))
    ap.add_argument("--dump")
    args = ap.parse_args(argv)

    profiles = json.loads(pathlib.Path(args.profiles).read_text(encoding="utf-8"))
    if args.profile not in profiles:
        print(f"capture-ui: no profile {args.profile} in {args.profiles}")
        return 2
    # Applied and then checked, before anything opens: the probe runs in its own
    # process in ref's desktop, so it reads the session's scaling, not this process's.
    refused = apply_profile(args.profile, args.profiles)
    if refused:
        for line in refused:
            print(line)
        return 2
    seen = read_profile()
    wrong = core.compare_profile(seen, profiles[args.profile], args.profile)
    if wrong:
        for w in wrong:
            print(w)
        return 2
    session = core.session_type(seen)
    if session is None:
        print("capture-ui: the capture runs in a remote session; captures run in the machine's console session")
        return 2
    win32()
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    build = seen["build"]
    mod = load_surface(args.module or args.surface)
    if mod.SURFACE != args.surface:
        print(f"capture-ui: module {args.module} walks surface {mod.SURFACE}, not {args.surface}")
        return 2
    live = Live()
    ctx = Context(live)
    meta = {"surface": args.surface, "build": build, "profile": args.profile, "complete": False}
    shots = pathlib.Path(args.shots) / args.surface if args.shots else None
    if shots:
        shots.mkdir(parents=True, exist_ok=True)

    try:
        scenes = core.scenes_of(mod)
    except ValueError as exc:
        print(f"capture-ui: {args.surface}: {exc}")
        return 2
    written = []                      # this scene's screenshots from its last walk

    def shot(state, el, items, box=None):
        # box: the root's extent when it has no area of its own (core.extent).
        boxes = [box or el.bounds] + [i.bounds for i in items if i.bounds[2] and i.bounds[3]]
        x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
        x1, y1 = max(b[0] + b[2] for b in boxes), max(b[1] + b[3] for b in boxes)
        name = f"{state}.{args.profile}.png"
        grab(str(shots / name), x0, y0, x1 - x0, y1 - y0)
        written.append(shots / name)
        return {"state": state, "file": name, "width": x1 - x0, "height": y1 - y0}

    def walk_args(sc):
        return dict(surface=mod.SURFACE, build=build, profile=args.profile, session=session, root_slug=sc.slug,
                    root_label=sc.label, root_kind=sc.kind, environment_lists=sc.environment_lists,
                    values=sc.values, identity=sc.identity, expand=sc.expand, hosts=sc.hosts, labels=sc.labels)

    def walk_scene(sc):
        # Each pass replaces this scene's screenshots from the pass before; an
        # earlier scene's are kept.
        for old in written:
            old.unlink(missing_ok=True)
        written.clear()
        return core.walk(sc.root(ctx), shot=shot if shots else None, **walk_args(sc))

    trees = {}

    def dump_scene(sc, cap):
        written.clear()               # settled: these screenshots stay
        if args.dump:
            tree = core.dump(sc.root(ctx), environment_lists=sc.environment_lists, expand=sc.expand, hosts=sc.hosts)
            core.confirm_dump(tree, cap, **walk_args(sc))
            trees[sc.slug] = tree

    code = 3
    try:
        cap, passes = core.run_scenes(scenes, ctx, walk_scene, on_open=dump_scene)
        records = cap.records
        records += core.authored(getattr(mod, "HAND", ()), surface=mod.SURFACE, build=build, source="hand")
        metrics = mod.METRICS(cap.records + cap.measured) if hasattr(mod, "METRICS") else ()
        records += core.authored(metrics, surface=mod.SURFACE, build=build, source="uia",
                                 profile=args.profile, session=session)
        if args.dump:
            # One scene keeps the bare tree the fixtures were written from.
            body = trees[scenes[0].slug] if len(scenes) == 1 else {"scenes": trees}
            pathlib.Path(args.dump).write_text(json.dumps(body, indent=1), encoding="utf-8")
        core.write_records(args.out, records)
        meta.update(complete=True, passes=max(passes.values()), scenePasses=passes, records=len(records),
                    unnamed=cap.unnamed, ambiguous=cap.ambiguous, skipped=cap.skipped, states=cap.states)
        code = 0
    except (core.Incomplete, live.COMError, ValueError) as exc:
        # A UI Automation call that fails mid-walk (an element gone, a provider
        # that stopped answering) leaves the capture unproven, never half-taken.
        # A ValueError is a module whose authored records are malformed.
        meta["incomplete"] = str(exc) if not isinstance(exc, live.COMError) else f"UI Automation failed: {exc}"
        print(f"capture-ui: incomplete: {meta['incomplete']}")
    finally:
        if args.meta:
            pathlib.Path(args.meta).write_text(json.dumps(meta, indent=1), encoding="utf-8")
    if code == 0:
        print(f"capture-ui: {args.surface}: {meta['records']} records in {meta['passes']} walks, {len(meta['unnamed'])} unnamed, "
              f"{len(meta['ambiguous'])} ambiguous, {len(meta['states'])} screenshot(s)")
    return code


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    sys.exit(main())
