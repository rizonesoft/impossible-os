"""Shared by the dialog surface modules: find the window an action opened, wait
for it to settle, press its buttons, and close every window a scene opened.

Used by explorer-dialogs.py (File Explorer's dialogs) and dialogs-common.py
(the common dialogs, message boxes and task dialogs). Each list of windows is
taken from both Win32 and UI Automation, because each leaves out a window the
other has (2026-11-20, see top_windows).
"""
import ctypes
import time

import capture_core as core
from explorer_window import SETTLE, descendants, user32

DIALOG_WAIT = 20                         # s for a dialog to show
ENABLE_WAIT = 15                         # s for a button to be enabled
WM_CLOSE = 0x0010
CLOSE_PASSES = 5                         # a closed window can open another (a "cancel this copy?" prompt)
GW_OWNER = 4
CLASS_CHARS = 256
OWNER_DEPTH = 8                          # owner chains followed when ordering windows to close
DISMISS = ("Cancel", "Close", "No", "Ignore", "OK")  # the buttons that leave a dialog without acting, the safest first
QUIET_GAP = 1.5                          # s between two looks that must agree
QUIET_WAIT = 30                          # s for a window to stop changing
QUIET_DEPTH = 6                          # levels a settling look reads
SM_CXSCREEN = 0
PARK_INSET = 8                           # px in from the primary screen's top-right corner: no surface sits there
if user32:
    user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
    user32.GetWindow.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    user32.GetWindow.restype = ctypes.c_void_p
    user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
    user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user32.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user32.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_ssize_t]
    user32.GetSystemMetrics.argtypes = [ctypes.c_int]
    user32.GetForegroundWindow.restype = ctypes.c_void_p

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PATH_CHARS = 1024


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True) if user32 else None
if user32:
    user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    user32.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(RECT)]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_bool, ctypes.c_ulong]
    kernel32.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p,
                                                    ctypes.POINTER(ctypes.c_ulong)]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

_base = {}                               # the top-level windows before the first scene opened anything


def win32_windows():
    """(handle, class, title) of every visible top-level window, from Win32."""
    out = set()
    proc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_ssize_t)

    def one(h, _):
        if user32.IsWindowVisible(h):
            cls, title = ctypes.create_unicode_buffer(CLASS_CHARS), ctypes.create_unicode_buffer(CLASS_CHARS)
            user32.GetClassNameW(h, cls, CLASS_CHARS)
            user32.GetWindowTextW(h, title, CLASS_CHARS)
            out.add((h, cls.value, title.value))
        return True
    callback = proc(one)
    user32.EnumWindows(ctypes.cast(callback, ctypes.c_void_p), 0)
    return out


def top_windows(ctx, known=()):
    """(handle, class, title) of each top-level window on screen, from both lists:
    UI Automation's root leaves out a dialog its owner holds (Customize's Browse),
    and Win32's visible windows leave out the Open With picker, which UI
    Automation lists at the root (both 2026-11-20). A window that only holds a
    tooltip is left out: help text, which the walk never records either. One
    entry per handle: the two lists can name a window differently, and a handle,
    not its title, is what makes it a window. A handle in `known` is returned
    without reading whether it is a tooltip: a caller looking for new windows
    drops it anyway (review, 2026-11-21)."""
    out = {w[0]: w for w in win32_windows()}
    for w in ctx.live.children(ctx.live.uia.GetRootElement(), ctx.live.uia.CreateTrueCondition()):
        if not w.offscreen:
            out.setdefault(w.el.CurrentNativeWindowHandle, (w.el.CurrentNativeWindowHandle, w.class_name, w.name))
    return {w for w in out.values() if w[0] in known or not tooltip_only(ctx, w)}


# The window classes that host a tooltip: the classic control, and the XAML popup
# the taskbar's Start button showed once a dialog closed and the taskbar took the
# keyboard focus (2026-11-20).
TOOLTIP_HOSTS = ("tooltips_class32", "Xaml_WindowedPopupClass")


TOOLTIP_DEPTH = 8                        # levels read under a tooltip host before it counts as a real window
TOOLTIP_BUDGET = 64                      # elements read under one before it counts as a real window
CONTAINERS = ("Window", "Pane", "Text")


def tooltip_only(ctx, window):
    """Whether a top-level window holds nothing but a tooltip (core.NEVER's rule):
    every branch is read to its end, through containers, and any other control,
    a branch deeper than TOOLTIP_DEPTH, or more than TOOLTIP_BUDGET elements makes
    it a real window. A poll that ends mid-read raises core.Incomplete."""
    if window[1] not in TOOLTIP_HOSTS:
        return False
    try:
        el = ctx.live.wrap(ctx.live.uia.ElementFromHandle(window[0]))
        if el.control_type in core.NEVER:
            return True
        tips, level, read = 0, [el], 0
        for _ in range(TOOLTIP_DEPTH):
            below = []
            fetched = []
            for e in level:
                if ctx.overdue():
                    raise TimeoutError
                kids = ctx.live.children(e.el, ctx.live.uia.CreateTrueCondition(), TOOLTIP_BUDGET - read)
                read += len(kids)
                fetched.extend(kids)
            for k in fetched:
                if k.control_type in core.NEVER:
                    tips += 1                 # a tooltip's own text is the tooltip
                elif k.control_type in CONTAINERS:
                    below.append(k)
                else:
                    return False
            if not below:
                return tips > 0
            level = below
        return False                          # not read to its end: treat it as real
    except TimeoutError:
        raise core.Incomplete("the look at a tooltip host ran past its poll's deadline") from None
    except (ctx.live.COMError, core.Incomplete):
        return False                          # gone meanwhile, or past the budget: the next look decides


def baseline(ctx):
    """Before a scene opens anything: the pointer rests where nothing is, and the
    windows open before the first scene are remembered (closing leaves them alone).
    A pointer left over the taskbar showed the tooltip of the button a dialog's
    process added there, an XAML PopupHost no scene opened (2026-11-20)."""
    ctx.hover_point(user32.GetSystemMetrics(SM_CXSCREEN) - PARK_INSET, PARK_INSET)
    if "windows" not in _base:
        _base["windows"] = {w[0] for w in top_windows(ctx)}


def appeared(ctx, before, wait=DIALOG_WAIT, skip=("CabinetWClass",)):
    """The top-level window that was not there before an action (the one whose
    title is not known ahead: Open with, a wizard). Refused when none or several appear."""
    known = {w[0] for w in before}            # by handle: a renamed window is not a new one
    new = []
    with ctx.within(wait) as deadline:
        while True:
            try:
                new = [w for w in top_windows(ctx, known) if w[0] not in known and w[1] not in skip]
            except core.Incomplete:
                pass                              # the look ran out of time; the deadline below decides
            else:
                if len(new) == 1:
                    return ctx.live.wrap(ctx.live.uia.ElementFromHandle(new[0][0]))
            if time.monotonic() > deadline:
                raise core.Incomplete(f"after the action {len(new)} new window(s) showed: {sorted(w[1:] for w in new)}")
            time.sleep(0.5)


def innermost_first(windows):
    """Owned windows before their owners: a sheet ignores WM_CLOSE while a dialog it
    opened is still up (2026-11-20)."""
    handles = {w[0] for w in windows}
    depth = {}
    for w in windows:
        d, h = 0, w[0]
        for _ in range(OWNER_DEPTH):
            h = user32.GetWindow(h, GW_OWNER)
            if not h or h not in handles:
                break
            d += 1
        depth[w[0]] = d
    return sorted(windows, key=lambda w: -depth[w[0]])


def close_new_windows(ctx, keep=()):
    """Close every top-level window opened since the baseline, except the classes in
    `keep`: WM_CLOSE (a dialog's Cancel) twice, then Esc with the window in front
    (a sheet back from a modal dialog ignored WM_CLOSE), then the window's own
    leave-without-acting button."""
    def opened():
        return [w for w in top_windows(ctx) if w[0] not in _base["windows"] and w[1] not in keep]
    missed = {}
    for attempt in range(CLOSE_PASSES):
        new = opened()
        if not new:
            return
        for h, _, title in innermost_first(new):
            if attempt < 2:
                user32.PostMessageW(h, WM_CLOSE, 0, 0)
            elif attempt == 2:
                # Esc only to a window that really is in front: Windows can refuse the
                # foreground, and the key then went to the taskbar, whose button
                # showed a tooltip no scene opened (2026-11-20).
                user32.SetForegroundWindow(h)
                time.sleep(0.3)
                if user32.GetForegroundWindow() == h:
                    ctx.keys("esc")
                else:
                    seen = dismiss(ctx, h)
                    if seen is not None:
                        missed[title] = seen
            else:
                seen = dismiss(ctx, h)
                if seen is not None:
                    missed[title] = seen
        time.sleep(1)
    # Look once more: the last pass acted, and what it closed is not a failure.
    new = opened()
    if new:
        why = f"; no leave button among {missed}" if missed else ""
        raise core.Incomplete(f"windows the scene opened stayed open: {sorted(describe(w) for w in new)}{why}")


def describe(window):
    """(class, title, the owning program, the rectangle) of a window, for a failure
    to name whose window it is."""
    h, cls, title = window
    pid, rect = ctypes.c_ulong(), RECT()
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    user32.GetWindowRect(h, ctypes.byref(rect))
    program = "?"
    proc = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if proc:
        buf, size = ctypes.create_unicode_buffer(PATH_CHARS), ctypes.c_ulong(PATH_CHARS)
        if kernel32.QueryFullProcessImageNameW(proc, 0, buf, ctypes.byref(size)):
            program = buf.value.rsplit("\\", 1)[-1]
        kernel32.CloseHandle(proc)
    return cls, title, program, (rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top)


def dismiss(ctx, handle):
    """Press the window's own leave-without-acting button: a message box whose
    only button is OK can refuse WM_CLOSE and Esc from another process. Returns
    None when it pressed one (or the window went meanwhile), else the button
    names it found, for the failure to name."""
    try:
        el = ctx.live.wrap(ctx.live.uia.ElementFromHandle(handle))
        # Enabled ones only: a Yes/No message box's title bar Close is disabled (the box
        # has no close command), and pressing it did nothing (2026-11-20).
        buttons = {k.name: k for k in descendants(ctx, el, 4) if k.control_type == "Button" and k.enabled}
    except (ctx.live.COMError, core.Incomplete):
        return None                           # gone meanwhile: the next look sees it closed
    for name in DISMISS:
        if name in buttons:
            ctx.invoke(buttons[name])
            return None
    return sorted(buttons)


def press(ctx, within, click=False, **spec):
    """Invoke the element the spec names under `within` (click it, for a link with no
    Invoke pattern: Details' Remove Properties, 2026-11-20); a miss lists the buttons
    and links there. A button a page enables only once loaded is waited for."""
    try:
        el = ctx.find(within=within, depth=6, **spec)
    except core.Incomplete:
        seen = [(k.control_type, k.name) for k in descendants(ctx, within, 4)
                if k.control_type in ("Button", "Hyperlink", "SplitButton", "MenuItem")]
        raise core.Incomplete(f"no element matching {spec}; the buttons and links there are {seen[:40]}") from None
    with ctx.within(ENABLE_WAIT) as deadline:         # the refreshing find ends by it too
        while not el.enabled:
            time.sleep(min(0.5, max(0.0, deadline - time.monotonic())))
            if time.monotonic() >= deadline:
                raise core.Incomplete(f"{el.name!r} stayed disabled for {ENABLE_WAIT} s")
            el = ctx.find(within=within, depth=6, **spec)
    if click:
        ctx.click(el)
    else:
        ctx.invoke(el)
    time.sleep(SETTLE)


def quiet(ctx, el):
    """Wait until a window's layout stops changing: the Open With picker fills its
    app groups in for several seconds after it shows (2026-11-20)."""
    deadline = time.monotonic() + QUIET_WAIT

    def look():
        # Bounded like the walk: no more than MAX_RECORDS elements, and the deadline
        # is checked between levels, so one look cannot outlast the wait.
        # Each parent's children are charged as they come, with what is left of
        # the budget passed down, and the deadline is checked before every fetch.
        out, level = [], [el]
        for _ in range(QUIET_DEPTH):
            below = []
            for e in level:
                if time.monotonic() > deadline:
                    return sorted(out)
                kids = ctx.live.children(e.el, ctx.live.uia.CreateTrueCondition(), core.MAX_RECORDS - len(out))
                out.extend((k.control_type, k.name, tuple(k.bounds)) for k in kids)
                below.extend(kids)
            if not below:
                break
            level = below
        return sorted(out)
    prev = look()
    while time.monotonic() < deadline:
        time.sleep(QUIET_GAP)
        now = look()
        if now == prev and time.monotonic() <= deadline:
            return
        prev = now
    raise core.Incomplete(f"{el.name or el.class_name!r} kept changing for {QUIET_WAIT} s")


def ps_quote(text):
    """A PowerShell single-quoted string literal."""
    return "'" + text.replace("'", "''") + "'"
