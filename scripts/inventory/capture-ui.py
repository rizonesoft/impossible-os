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

PROBE = r"C:\ios\probe\probe-profile.ps1"
SETTLE_S = 10.0         # a flyout gets this long to open and stop changing
MIN_POLLS = 3           # and is always looked at this often: a provider busy animating answers slowly
POLL_S = 0.2
EXPAND_COLLAPSE = 10005
PROP_EC_AVAILABLE = 30028
PROP_EC_STATE = 30070
EXPANDED, COLLAPSED = 1, 0
TREE_ELEMENT, TREE_CHILDREN, TREE_DESCENDANTS = 1, 2, 4
# Top-level window classes that are popups themselves: a Win32 menu, a combo box
# list, a windowed XAML popup. A popup hosted elsewhere fails the capture (exit 3)
# rather than being missed, and is added here when it is met.
POPUP_WINDOWS = {"#32768", "ComboLBox", "Xaml_WindowedPopupClass", "Popup"}
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


def read_profile():
    r = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PROBE],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise SystemExit(f"capture-ui: the profile probe failed: {r.stderr.strip()}")
    return json.loads(r.stdout.strip().splitlines()[-1])


# ---------------------------------------------------------------- input --
VK = {"alt": 0x12, "ctrl": 0x11, "shift": 0x10, "win": 0x5B, "esc": 0x1B, "enter": 0x0D, "tab": 0x09,
      "f4": 0x73, "f10": 0x79, "space": 0x20, "down": 0x28, "up": 0x26}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
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
        visible = self.uia.CreatePropertyCondition(PROP_IS_OFFSCREEN, False)
        popup = self.uia.CreateOrCondition(self.uia.CreatePropertyCondition(PROP_CONTROL_TYPE, 50009),
                                           self.uia.CreatePropertyCondition(PROP_CONTROL_TYPE, 50008))
        self.visible_popup = self.uia.CreateAndCondition(popup, visible)

    def wrap(self, el):
        """A LiveElement for a raw element, its properties fetched in one call."""
        return LiveElement(self, el.BuildUpdatedCache(self.cache))

    def children(self, el, condition=None):
        found = el.FindAllBuildCache(TREE_CHILDREN, condition or self.control_view, self.cache)
        return [LiveElement(self, found.GetElement(i)) for i in range(found.Length)] if found else []

    def hosts(self, pid):
        """Where process `pid` shows popups: its top-level popup windows (a Win32 menu,
        a combo list, a windowed XAML popup) and the popup hosts its own windows
        carry as direct children (a WinUI PopupHost bridge, seen 2026-11-14 on
        Notepad). Only these are searched, never a whole window's content, so the
        cost of a poll does not grow with the surface."""
        out = []
        tops = self.children(self.uia.GetRootElement(), self.uia.CreatePropertyCondition(PROP_PROCESS_ID, pid))
        for top in tops:
            if top.offscreen:
                continue
            if top.class_name in POPUP_WINDOWS or top.control_type in ("Menu", "List"):
                out.append(top.el)
            else:
                out.extend(k.el for k in self.children(top.el) if "Popup" in k.class_name and not k.offscreen)
        return out

    def popups(self, pid):
        """Menu and List containers now on screen in process `pid`, by runtime id."""
        out = {}
        for host in self.hosts(pid):
            found = host.FindAll(TREE_ELEMENT | TREE_DESCENDANTS, self.visible_popup)
            for j in range(found.Length if found else 0):
                e = found.GetElement(j)
                out[tuple(e.GetRuntimeId())] = e
        return out

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

    def children(self):
        return self.live.children(self.el)

    def _pattern(self):
        return self.el.GetCurrentPattern(EXPAND_COLLAPSE).QueryInterface(self.live.UIA.IUIAutomationExpandCollapsePattern)

    def expand(self):
        before = self.live.popups(self.pid)
        self._pattern().Expand()
        last, stable, counts = None, 0, []
        deadline = time.monotonic() + SETTLE_S
        while len(counts) < MIN_POLLS or time.monotonic() < deadline:
            time.sleep(POLL_S)
            items = self._entries(before)
            counts.append(len(items))
            sig = tuple((i.control_type, i.name, i.automation_id, i.bounds) for i in items)
            if items and sig == last:
                stable += 1
                if stable >= 2:                # three equal polls: open and done animating
                    return items
            else:
                stable = 0
            last = sig
        raise core.Incomplete(f"{self.name or self.automation_id}: its entries did not appear and settle within "
                              f"{SETTLE_S:.0f} s (entries per poll: {counts}; new popups: {len(self._opened)})")

    def _entries(self, before):
        # A Win32 menu or a WinUI flyout opens as a new Menu or List container in
        # the same process; a combo box may also show its list as its own children.
        # Once the new container is found it is read directly, not searched for again.
        if not self._opened:
            self._opened = {k: e for k, e in self.live.popups(self.pid).items() if k not in before}
        if self._opened:
            return [c for e in self._opened.values() for c in self.live.children(e)]
        return [c for c in self.children() if c.control_type in ("ListItem", "MenuItem") and not c.offscreen]

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

    def find_window(self, timeout=15, **spec):
        """The first top-level window matching spec (className, name, automationId)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for w in self.live.children(self.live.uia.GetRootElement()):
                if core.matches(w, spec) and not w.offscreen:
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
    # Checked first, before anything opens: the probe runs in its own process
    # in ref's desktop, so it reads the session's scaling, not this process's.
    seen = read_profile()
    wrong = core.compare_profile(seen, profiles[args.profile], args.profile)
    if wrong:
        for w in wrong:
            print(w)
        return 2
    win32()
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    build = seen["build"]
    mod = load_surface(args.surface)
    live = Live()
    ctx = Context(live)
    meta = {"surface": args.surface, "build": build, "profile": args.profile, "complete": False}
    shots = pathlib.Path(args.shots) / args.surface if args.shots else None
    if shots:
        shots.mkdir(parents=True, exist_ok=True)

    def shot(state, el, items):
        boxes = [el.bounds] + [i.bounds for i in items if i.bounds[2] and i.bounds[3]]
        x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
        x1, y1 = max(b[0] + b[2] for b in boxes), max(b[1] + b[3] for b in boxes)
        name = f"{state}.{args.profile}.png"
        grab(str(shots / name), x0, y0, x1 - x0, y1 - y0)
        return {"state": state, "file": name, "width": x1 - x0, "height": y1 - y0}

    walk_args = dict(surface=mod.SURFACE, build=build, profile=args.profile, root_slug=mod.ROOT_SLUG,
                     root_label=mod.ROOT_LABEL, root_kind=getattr(mod, "ROOT_KIND", "surface"),
                     environment_lists=getattr(mod, "ENVIRONMENT_LISTS", ()), values=getattr(mod, "VALUES", ()),
                     identity=getattr(mod, "IDENTITY", ()))

    def walk_once():
        if shots:                     # each pass replaces the last pass's screenshots
            for old in shots.glob("*.png"):
                old.unlink()
        return core.walk(mod.root(ctx), shot=shot if shots else None, **walk_args)

    code = 3
    mod.open(ctx)
    try:
        cap, passes = core.settle(walk_once)
        if args.dump:
            tree = core.dump(mod.root(ctx), environment_lists=walk_args["environment_lists"])
            core.confirm_dump(tree, cap, **walk_args)
            pathlib.Path(args.dump).write_text(json.dumps(tree, indent=1), encoding="utf-8")
        core.write_records(args.out, cap.records)
        meta.update(complete=True, passes=passes, records=len(cap.records), unnamed=cap.unnamed,
                    ambiguous=cap.ambiguous, skipped=cap.skipped, states=cap.states)
        code = 0
    except core.Incomplete as exc:
        meta["incomplete"] = str(exc)
        print(f"capture-ui: incomplete: {exc}")
    finally:
        try:
            mod.close(ctx)
        except core.Incomplete as exc:
            meta["complete"] = False
            meta["incomplete"] = f"close: {exc}"
            code = 3
        if args.meta:
            pathlib.Path(args.meta).write_text(json.dumps(meta, indent=1), encoding="utf-8")
    if code == 0:
        print(f"capture-ui: {args.surface}: {meta['records']} records in {meta['passes']} walks, {len(meta['unnamed'])} unnamed, "
              f"{len(meta['ambiguous'])} ambiguous, {len(meta['states'])} screenshot(s)")
    return code


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    sys.exit(main())
