"""Surface module: the taskbar on a second display and its three button modes.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (the
Taskbar item, second display). Writes docs/design/inventory/taskbar/secondary.json
on the two-display stand-in (profile dual-100-150: the second display at 150 %,
right of the first):

    python3 scripts/inventory/capture.py --surface taskbar --module taskbar-secondary --file secondary --profile dual-100-150

Measured on 26300.9974 (2026-11-16): the second bar is Shell_SecondaryTrayWnd,
holding its own TaskbarFrame (Widgets, Start, Search, Task View, task buttons)
and a clock; it has no tray. The button mode (MMTaskbarMode) and the combine
setting are read only when Explorer starts, so each mode restarts the shell.
"""
import time

import capture_core as core

SURFACE = "taskbar"
ENVIRONMENT_LISTS = []
VALUES = [{"className": "SystemTray.OmniButton"}]      # the clock's name is the time and date
IDENTITY = [({"automationId": "StartButton"}, "start"), ({"automationId": "SearchButton"}, "search"),
            ({"automationId": "TaskViewButton"}, "task-view"), ({"automationId": "WidgetsButton"}, "widgets"),
            ({"className": "SystemTray.OmniButton"}, "clock")]
HOSTS = []
LABELS = [({"className": "SystemTray.OmniButton"}, "Clock"),
          ({"automationId": "TaskbarFrame"}, "Bar"),
          ({"controlType": "Image", "name": ""}, "Icon")]
# The legacy task list window behind the XAML bar (an accessibility shim), and
# the empty host window the bar's menus open in (captured with the menus).
OMIT = [{"className": "MSTaskListWClass"}, {"className": "Xaml_WindowedPopupClass"}]

ROOT = "secondary"
APPS = "Appid: "
NOTEPAD = "Appid: Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"
ADVANCED = r"HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"
# MMTaskbarMode: where task buttons show when the bar is on every display.
MODES = [("all-taskbars", 0, "All taskbars"), ("main-and-where-open", 1, "Main taskbar and taskbar where window is open"),
         ("where-open", 2, "Taskbar where window is open")]
SETTLE = 2.5
NOTEPAD_WAIT = 60                 # a first start on the stand-in


def bar(ctx):
    return ctx.find_window(className="Shell_SecondaryTrayWnd", timeout=60)


def frame(ctx):
    """The second bar's XAML frame: the task buttons are its direct children,
    which a scene that does not expand records (the window holds them three
    levels down, so a mode rooted at the window saw none)."""
    return ctx.find(within=bar(ctx), automationId="TaskbarFrame", depth=3, timeout=30)


def restart_shell(ctx):
    """Restart Explorer and let the bar settle: the settings these scenes change
    apply only at Explorer's start (TaskbarGlomLevel set live changed nothing, 2026-11-16)."""
    core.restart_explorer(ctx, "Shell_SecondaryTrayWnd")
    time.sleep(SETTLE)


def open_secondary(ctx):
    ctx.launch("notepad.exe")             # a window on the first display, to see where its button goes
    t0 = time.monotonic()
    ctx.find_window(className="Notepad", timeout=NOTEPAD_WAIT)
    print(f"secondary: Notepad opened in {time.monotonic() - t0:.0f} s", flush=True)
    ctx.find(within=ctx.find_window(className="Shell_TrayWnd"), automationId=NOTEPAD, depth=4, timeout=30)
    time.sleep(SETTLE)


def nothing(ctx):
    pass


def open_mode(value):
    def opener(ctx):
        ctx.powershell(f"Set-ItemProperty -LiteralPath '{ADVANCED}' -Name MMTaskbarMode -Value {value} -Type DWord")
        restart_shell(ctx)
    return opener


SCENES = [dict(slug=ROOT, label="Taskbar on a second display", kind="surface", open=open_secondary, root=bar,
               close=nothing)]
# Measured and not published: the second bar's task buttons in each mode (the
# default, all taskbars, last, so the shell ends as it began).
SCENES += [dict(slug=f"mode-{slug}", label=label, kind="state", open=open_mode(value), root=frame, close=nothing,
                keep=False, expand=False) for slug, value, label in MODES[1:] + MODES[:1]]


def METRICS(records):
    """The second bar's height, and one state record per button mode naming the
    task buttons that mode shows on the second display (the first display holds
    the only window, Notepad)."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}
    root = by.get(p + ROOT)
    if root is None:
        raise ValueError("the second taskbar was not captured")
    out = [{"id": f"{ROOT}.bar-height", "parent": ROOT, "kind": "metric", "label": "Bar height, second display at 150 %",
            "order": 0, "measure": {"value": root["bounds"]["height"], "unit": "px", "scaling": "scales"}}]
    seen = []
    for i, (slug, _, label) in enumerate(MODES):
        kids = sorted((r for r in records if r.get("parent") == p + f"mode-{slug}" and r.get("uiaId", "").startswith(APPS)),
                      key=lambda r: r["order"])
        if f"{p}mode-{slug}" not in by:
            raise ValueError(f"the {label} mode was not measured")
        names = [r["label"] for r in kids]
        seen.append(tuple(names))
        shown = ", ".join(names) if names else "no task buttons"
        out.append({"id": f"{ROOT}.mode-{slug}", "parent": ROOT, "kind": "state", "label": label, "order": 10 + i,
                    "behaviour": f"With this button mode the second display's bar shows: {shown}; the only open "
                                 "window is on the first display."})
    if len(set(seen)) < 2:
        raise ValueError("every button mode showed the same buttons; a mode change was not applied")
    return out
