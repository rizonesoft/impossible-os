"""Surface module: the system tray, its hidden icons flyout and its menus.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (System
Tray and Overflow). Writes docs/design/inventory/taskbar/tray.json:

    python3 scripts/inventory/capture.py --surface taskbar --module tray --file tray

Measured on 26300.9974 (2026-11-17): the tray's buttons sit beside the
TaskbarFrame under the bar's input site, all with automation id SystemTrayIcon.
Their names are a fixed word and a live status ("Volume No audio device is
installed"), so they are matched by that word. The clock and the bell are one
group and share one menu. The touch keyboard and input indicator entries show
only when turned on (a second input language, the touch keyboard setting); the
pen entry needs pen hardware and never shows in the VM. The hidden icons flyout
is its own top-level window. The clock's seconds apply only at Explorer's start.
"""
import re
import time

import capture_core as core

SURFACE = "taskbar"
ROOT = "tray"
ADVANCED = r"HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"
TABLET_TIP = r"HKCU:\Software\Microsoft\TabletTip\1.7"
SETTLE = 2.5

# A fixed first word, the slug and the label of each tray entry.
ENTRIES = [("Show Hidden Icons", "hidden-icons", "Show hidden icons"), ("Touch Keyboard", "touch-keyboard", "Touch keyboard"),
           ("Tray Input Indicator", "input-indicator", "Input indicator"), ("Network", "network", "Network"),
           ("Volume", "volume", "Volume"), ("Clock", "clock", "Clock"), ("Notifications", "notifications", "Notifications"),
           ("Show Desktop", "show-desktop", "Show desktop")]
# Pinned to the tray's automation id: a menu item such as "Network and Internet
# settings" shares the first word (seen 2026-11-17).
IDENTITY = [({"automationId": "SystemTrayIcon", "namePrefix": word}, slug) for word, slug, _ in ENTRIES]
LABELS = ([({"automationId": "SystemTrayIcon", "namePrefix": word}, label) for word, _, label in ENTRIES]
          + [({"controlType": "Image", "name": ""}, "Icon"), ({"className": "TextBlock"}, "Date and time")])
# Every entry but the two fixed-name buttons carries a live status in its name.
VALUES = [{"automationId": "SystemTrayIcon", "namePrefix": word} for word, _, _ in ENTRIES
          if word not in ("Show Hidden Icons", "Touch Keyboard", "Show Desktop")]
# The input switcher's rows are this machine's languages.
ENVIRONMENT_LISTS = [{"automationId": "Windows.Shell.InputApp.InputSwitcherList"}]
HOSTS = []
# The bar's buttons (taskbar.json), the legacy panes behind the XAML bar, and
# the empty host the menus open in (captured with each menu).
OMIT = [{"automationId": "TaskbarFrame"}, {"className": "TrayNotifyWnd"}, {"className": "MSTaskSwWClass"},
        {"className": "Xaml_WindowedPopupClass"}]

# Turns on the optional entries so the fixed order is captured whole: a second
# input language (the input indicator) and the touch keyboard button.
EXTRAS = (f"New-Item -Path '{TABLET_TIP}' -Force | Out-Null; "
          f"Set-ItemProperty -LiteralPath '{TABLET_TIP}' -Name TipbandDesiredVisibility -Value 1 -Type DWord; "
          "$l = Get-WinUserLanguageList; $l.Add('fr-FR'); Set-WinUserLanguageList $l -Force -WarningAction SilentlyContinue")


def tray(ctx):
    return ctx.find_window(className="Shell_TrayWnd")


def entry(ctx, word):
    return ctx.find(within=tray(ctx), automationId="SystemTrayIcon", namePrefix=word, depth=3, timeout=30)


def nothing(ctx):
    pass


SECONDS_OFF = f"Set-ItemProperty -LiteralPath '{ADVANCED}' -Name ShowSecondsInSystemClock -Value 0 -Type DWord"


def open_tray(ctx):
    # The clock starts without seconds whatever an earlier run left (2026-11-17:
    # a run that never restored it made the plain clock as wide as with seconds).
    ctx.powershell(EXTRAS + "; " + SECONDS_OFF)
    core.restart_explorer(ctx)
    entry(ctx, "Tray Input Indicator")
    entry(ctx, "Touch Keyboard")
    time.sleep(SETTLE)


def flyout(ctx):
    return ctx.find_window(className="TopLevelWindowForOverflowXamlIsland", timeout=10)


def open_flyout(ctx):
    ctx.click(entry(ctx, "Show Hidden Icons"))
    flyout(ctx)
    time.sleep(1)


def close_flyout(ctx):
    ctx.keys("esc")
    try:
        ctx.gone(className="TopLevelWindowForOverflowXamlIsland", timeout=3)
    except core.Incomplete:
        # After an app icon's menu closes the flyout no longer has the keyboard
        # (2026-11-17); the chevron toggles it shut.
        ctx.click(entry(ctx, "Show Hidden Icons"))
        ctx.gone(className="TopLevelWindowForOverflowXamlIsland")


def menu(ctx):
    return ctx.find(within=tray(ctx), className="MenuFlyout", depth=4, timeout=10)


def open_menu(word):
    def opener(ctx):
        ctx.click(entry(ctx, word), button="right")
        menu(ctx)
        time.sleep(1)
    return opener


def close_menu(ctx):
    ctx.keys("esc")
    deadline = time.monotonic() + 10
    while True:
        try:
            ctx.find(within=tray(ctx), className="MenuFlyout", depth=4, timeout=0)
        except core.Incomplete:
            return
        if time.monotonic() > deadline:
            raise core.Incomplete("a tray menu stayed open")
        time.sleep(0.3)


def app_menu(ctx):
    return ctx.find_window(className="#32768", timeout=10)


def open_app_menu(ctx):
    """An app icon's own menu, from the hidden icons flyout (Windows Security's
    on this machine): a classic Win32 menu the app fills."""
    open_flyout(ctx)
    ctx.click(ctx.find(within=flyout(ctx), automationId="NotifyItemIcon", depth=4), button="right")
    app_menu(ctx)
    time.sleep(1)


def close_app_menu(ctx):
    ctx.keys("esc")
    ctx.gone(className="#32768")
    close_flyout(ctx)


def switcher(ctx):
    return ctx.find_window(className="Shell_InputSwitchTopLevelWindow", timeout=10)


SWITCHER_TRIES = 3


def open_switcher(ctx):
    for attempt in range(1, SWITCHER_TRIES + 1):
        ctx.click(entry(ctx, "Tray Input Indicator"))
        try:
            switcher(ctx)
            break
        except core.Incomplete:
            if attempt == SWITCHER_TRIES:
                raise
    print(f"tray: the input switcher opened on click {attempt}", flush=True)
    time.sleep(1)


def close_switcher(ctx):
    ctx.keys("esc")
    ctx.gone(className="Shell_InputSwitchTopLevelWindow")


def tooltip(ctx):
    return ctx.find(controlType="ToolTip", depth=6, timeout=8)


def open_tooltip(ctx):
    ctx.hover(entry(ctx, "Clock"), settle=0)
    tooltip(ctx)
    time.sleep(1)


def close_tooltip(ctx):
    ctx.hover_point(900, 400)
    time.sleep(1)


SECONDS_WAIT = 30


# The time in the clock's name with seconds (the name is "Clock", the time and the date).
WITH_SECONDS = re.compile(r"\d:\d\d:\d\d")


def open_seconds(ctx, clock=time.monotonic):
    """The clock with seconds, checked by what the setting does: the time gains
    its seconds. Its width is no check (2026-11-17): the clock is as wide as its
    widest line, and at 1:11:52 AM that was still the date, 70 px either way."""
    ctx.powershell(f"Set-ItemProperty -LiteralPath '{ADVANCED}' -Name ShowSecondsInSystemClock -Value 1 -Type DWord")
    core.restart_explorer(ctx)
    deadline = clock() + SECONDS_WAIT
    while not WITH_SECONDS.search(entry(ctx, "Clock").name):
        if clock() >= deadline:
            raise core.Incomplete(f"the clock showed no seconds within {SECONDS_WAIT} s of turning them on")
        time.sleep(0.5)
    time.sleep(SETTLE)


def close_seconds(ctx):
    ctx.powershell(SECONDS_OFF)
    core.restart_explorer(ctx)


SCENES = [
    dict(slug=ROOT, label="System tray", kind="region", open=open_tray, root=tray, close=nothing),
    dict(slug="input-switcher", label="Input indicator flyout", kind="region", open=open_switcher, root=switcher,
         close=close_switcher),
    dict(slug="hidden-icons-flyout", label="Hidden icons flyout", kind="region", open=open_flyout, root=flyout,
         close=close_flyout),
    dict(slug="clock-menu", label="Clock menu", kind="contextMenu", open=open_menu("Clock"), root=menu,
         close=close_menu),
    dict(slug="network-menu", label="Network menu", kind="contextMenu", open=open_menu("Network"), root=menu,
         close=close_menu),
    dict(slug="volume-menu", label="Volume menu", kind="contextMenu", open=open_menu("Volume"), root=menu,
         close=close_menu),
    dict(slug="app-icon-menu", label="App icon menu", kind="contextMenu", open=open_app_menu, root=app_menu,
         close=close_app_menu, expand=False),
    # The tooltip's text is the long date and the time, a live value.
    dict(slug="clock-tooltip", label="Clock tooltip", kind="region", open=open_tooltip, root=tooltip,
         close=close_tooltip, values=VALUES + [{"className": "TextBlock"}]),
    # Last: it restarts the shell with the clock's seconds on.
    dict(slug="clock-seconds", label="Clock with seconds", kind="state", open=open_seconds, root=tray,
         close=close_seconds,
         keep=False),
]


def METRICS(records):
    """Each entry's width in the tray, the hidden icons flyout's cell and edge,
    and the clock's width with seconds."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}

    def width(rid):
        r = by.get(p + rid)
        if r is None:
            raise ValueError(f"{rid} was not captured")
        return r["bounds"]["width"]

    out = [{"id": f"{ROOT}.{slug}-width", "parent": ROOT, "kind": "metric", "label": f"{label} width", "order": i,
            "measure": {"value": width(f"{ROOT}.{slug}"), "unit": "px", "scaling": "scales"}}
           for i, (_, slug, label) in enumerate(ENTRIES)]
    icons = [r for r in records if r["id"].startswith(p + "hidden-icons-flyout.") and r.get("controlType") == "button"]
    if not icons:
        raise ValueError("the hidden icons flyout held no icon")
    cell = icons[0]["bounds"]
    box = by[p + "hidden-icons-flyout"]["bounds"]
    out += [{"id": f"{ROOT}.flyout-cell", "parent": ROOT, "kind": "metric", "label": "Hidden icons cell",
             "order": 20, "measure": {"value": cell["width"], "unit": "px", "scaling": "scales"}},
            {"id": f"{ROOT}.flyout-padding", "parent": ROOT, "kind": "metric", "label": "Hidden icons flyout padding",
             "order": 21, "measure": {"value": (box["width"] - cell["width"]) // 2, "unit": "px", "scaling": "scales"}}]
    if not any(r["id"] == p + "clock-seconds.clock" for r in records):
        raise ValueError("the clock with seconds was not measured")
    out.append({"id": f"{ROOT}.clock-seconds", "parent": ROOT, "kind": "state", "label": "Clock with seconds",
                "order": 22, "behaviour": "With seconds on, the time reads h:mm:ss; the clock is as wide as its "
                                          "wider line, so it grows only when the time is wider than the date."})
    return out


STATES = [
    ("network-wired", "Network, wired", "A monitor glyph when the connection is wired."),
    ("network-wireless", "Network, wireless", "A fan of arcs filled by signal strength, in four steps."),
    ("network-no-internet", "Network, no internet", "The connection's glyph with a small no-internet badge."),
    ("network-disconnected", "Network, disconnected", "A globe with a no-entry badge when nothing is connected."),
    ("network-airplane", "Network, airplane mode", "An aeroplane glyph replaces the network glyph."),
    ("volume-levels", "Volume levels", "A speaker with zero to three waves for four volume bands."),
    ("volume-muted", "Volume, muted", "The speaker with a cross in place of its waves."),
    ("volume-no-device", "Volume, no device", "The speaker with a no-device badge, as on this machine."),
    ("battery-levels", "Battery levels", "A battery outline filled to the charge, beside network and volume in one group."),
    ("battery-charging", "Battery, charging", "A plug or bolt over the battery while on AC."),
    ("battery-saver", "Battery, saver", "A leaf badge over the battery while Energy saver is on."),
    ("clock-lines", "Clock lines", "Two lines, the time over the short date, right-aligned; one line on a small bar."),
    ("clock-date-forms", "Clock date forms", "The date follows the region's short date format; the tooltip shows the "
                                             "long date and the time."),
    ("bell-states", "Notification bell states", "A bell; a count badge with unread notifications; a moon with Do not "
                                                "disturb on, as on this machine."),
    ("microphone-in-use", "Microphone in use", "A microphone glyph joins the tray while an app records; hover names "
                                                "the app."),
    ("location-in-use", "Location in use", "A location glyph joins the tray while an app reads the location."),
    ("screen-capture", "Screen capture", "A capture glyph joins the tray while the screen is shared or recorded."),
]
BEHAVIOURS = [
    ("flyout-drag", "Drag between flyout and bar", "An icon dragged from the hidden icons flyout onto the tray stays "
                                                   "there; one dragged into the flyout hides."),
    ("show-desktop-click", "Show desktop", "A click on the far right edge minimizes every window; a second click "
                                           "restores them; peek on hover is a setting, off by default."),
    ("group-click", "Network, volume and battery", "One click on the group opens Quick Settings; the clock and bell "
                                                   "open the notification centre and calendar."),
]


# Entries no stand-in and no host can show, each designed as ours from the
# sibling named (expected.json's absent rows give the reason).
ABSENT = [("pen", "Pen menu", "Needs pen hardware, which no stand-in has: designed as ours like the Touch keyboard "
                              "entry, a fixed tray button that opens a menu of pen shortcuts.")]


def _hand():
    out = [{"id": f"{ROOT}.absent-{slug}", "parent": ROOT, "kind": "control", "label": label, "order": 80 + i,
            "behaviour": f"Absent on the reference machine: {text}"} for i, (slug, label, text) in enumerate(ABSENT)]
    out += [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "state", "label": label, "order": 40 + i, "behaviour": text}
           for i, (slug, label, text) in enumerate(STATES)]
    out += [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "gesture", "label": label, "order": 60 + i,
             "behaviour": text} for i, (slug, label, text) in enumerate(BEHAVIOURS)]
    return out


HAND = _hand()                     # orders after the metrics, which number from 0
