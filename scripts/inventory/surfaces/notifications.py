"""Surface module: the notification center, its cards, the calendar and a toast.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8
(Notification Center, Calendar and Toast). Writes
docs/design/inventory/flyouts/notifications.json, over the section 20 fixture:

    python3 scripts/inventory/capture.py --surface flyouts --file notifications --module notifications \
        --prepare notifications:groups

Measured on 26300.9974 (2026-11-17): Win+N opens one full-height window holding
the notification center (a header with Do not disturb and Clear all, a group
per app, a card per notification) over the calendar (the date row, which
expands to a month grid that zooms to a year and a decade, and the focus
session row). A toast is a banner in its own window above the tray, one at a
time. The fixture's three senders give the groups; this module adds a fourth,
Sample Updates, whose toast fills every slot a card has: hero image, app logo,
title, two body lines, a progress bar with its title, status and value, a
reply box and five buttons. The reference VM starts with Do not disturb on, so
each scene sets the state it shows.
"""
import time

import capture_core as core

SURFACE = "flyouts"
ROOT = "notifications"

# The window is named for its host until it takes focus, then for itself.
CENTER_NAMES = ("Notification Center", "Windows Shell Experience Host")
CENTER_CLASS = "Windows.UI.Core.CoreWindow"
BANNER = {"className": "Windows.UI.Core.CoreWindow", "name": "New notification"}
SETTLE = 2
SENDER = "IosSample.Updates"
SENDER_NAME = "Sample Updates"
RICH_TITLE = "Sample update ready"
# The banner's default life is measured from a plain toast; the captured one is
# long-lived (duration long) so the walk finishes while it is on screen.
TIMING_TITLE = "Sample timing toast"
TIMING_MAX = 40
POST = r"""
$ErrorActionPreference = 'Stop'
[void][Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime]
[void][Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom, ContentType = WindowsRuntime]
Add-Type -AssemblyName System.Drawing
$id = '{sender}'; $k = "HKCU:\Software\Classes\AppUserModelId\$id"
New-Item -Path $k -Force | Out-Null
New-ItemProperty -LiteralPath $k -Name DisplayName -Value '{name}' -PropertyType String -Force | Out-Null
foreach ($img in @(@('C:\ios\hero.png', 364, 180, 40, 110, 170), @('C:\ios\logo.png', 48, 48, 200, 90, 40))) {{
    $b = New-Object System.Drawing.Bitmap $img[1], $img[2]; $g = [System.Drawing.Graphics]::FromImage($b)
    $g.Clear([System.Drawing.Color]::FromArgb(255, $img[3], $img[4], $img[5])); $b.Save($img[0]); $g.Dispose(); $b.Dispose()
}}
$xml = New-Object Windows.Data.Xml.Dom.XmlDocument; $xml.LoadXml(@'
{xml}
'@)
$t = New-Object Windows.UI.Notifications.ToastNotification $xml; $t.Tag = '{tag}'; $t.Group = 'sample'
$t.SuppressPopup = ${suppress}
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($id).Show($t)
"""
RICH = ("<toast displayTimestamp='2026-01-05T09:30:00Z' duration='long'><visual><binding template='ToastGeneric'>"
        f"<text>{RICH_TITLE}</text><text>A body line that says what happened.</text><text>A second body line.</text>"
        "<image placement='hero' src='file:///C:/ios/hero.png' alt='Hero image'/>"
        "<image placement='appLogoOverride' src='file:///C:/ios/logo.png' alt='App logo'/>"
        "<progress title='Sample package' value='0.6' valueStringOverride='6 of 10 MB' status='Downloading'/>"
        "</binding></visual><actions><input id='reply' type='text' placeHolderContent='Type a reply'/>"
        + "".join(f"<action content='{w}' arguments='{i}'/>" for i, w in enumerate(("One", "Two", "Three", "Four",
                                                                                    "Five"), 1))
        + "</actions></toast>")
PLAIN = ("<toast><visual><binding template='ToastGeneric'>"
         f"<text>{TIMING_TITLE}</text><text>Measures how long a banner stays.</text></binding></visual></toast>")
MEASURED = {}
GRID_WAIT = 6
VIEWS = (("MonthViewScrollViewer", "month"), ("YearViewScrollViewer", "year"),
         ("DecadeViewScrollViewer", "decade"))
CALENDAR_TRIES = 3

BUTTONS = ("One", "Two", "Three", "Four", "Five")
IDENTITY = ([({"automationId": "VerbButton", "name": w}, f"button-{i}") for i, w in enumerate(BUTTONS, 1)]
            + [({"automationId": "NotificationCenterGrid"}, "center"), ({"automationId": "MainListView"}, "groups"),
               ({"automationId": "CalendarCenterGrid"}, "calendar"),
               ({"controlType": "Group", "namePrefix": "Notifications from Sample Mail"}, "mail-group"),
               ({"controlType": "Group", "namePrefix": "Notifications from Sample Calendar"}, "calendar-group"),
               ({"controlType": "Group", "namePrefix": "Notifications from Sample Chat"}, "chat-group"),
               ({"controlType": "Group", "namePrefix": "Notifications from Sample Updates"}, "updates-group"),
               ({"controlType": "ListItem", "namePrefix": RICH_TITLE}, "card"),
               # The groups fixture numbers each sender's cards 1 to 3; an
               # opened group shows them side by side.
               *[({"controlType": "ListItem", "namePrefix": f"Sample {who} {i}"}, f"card-{i}")
                 for who in ("Mail", "Calendar", "Chat") for i in (3, 2, 1)],
               ({"controlType": "ListItem", "namePrefix": "Sample "}, "card"),
               ({"automationId": "Title", "className": "TextBlock"}, "title"),
               ({"automationId": "Content"}, "body"), ({"automationId": "TimeStamp"}, "time"),
               ({"automationId": "ExpandButton"}, "expand"), ({"automationId": "SeeMoreLessButton"}, "more"),
               ({"automationId": "StandardHeroImage"}, "hero"), ({"controlType": "Image", "name": "Hero image"}, "hero"),
               ({"controlType": "Image", "name": "App logo"}, "logo"), ({"automationId": "CalendarControl"}, "view"), ({"automationId": "MonthViewScrollViewer"}, "days"),
               ({"automationId": "YearViewScrollViewer"}, "months"),
               ({"automationId": "SenderName"}, "sender"), ({"automationId": "MessageText"}, "body"),
               ({"automationId": "AppLogo"}, "logo"), ({"automationId": "progressBar"}, "progress"),
               ({"controlType": "Text", "name": "Sample package"}, "progress-title"),
               ({"controlType": "Text", "name": "Downloading"}, "progress-status"),
               ({"automationId": "ValueStringText"}, "progress-value"), ({"controlType": "Edit"}, "reply"),
               ({"automationId": "DateTextButtonOld"}, "date"), ({"automationId": "HeaderButton"}, "header"),
               ({"automationId": "DurationTextBlock"}, "duration"),
               ({"controlType": "Text", "name": "Do not disturb is on"}, "dnd-title"),
               ({"controlType": "Text", "namePrefix": "You"}, "dnd-text")])
# Text a sender or the clock supplies, named by its role.
VALUES = [{"automationId": "Title", "className": "TextBlock"}, {"automationId": "Content"},
          {"automationId": "TimeStamp"}, {"automationId": "SenderName"}, {"automationId": "MessageText"},
          {"controlType": "Text", "name": "Sample package"}, {"controlType": "Text", "name": "Downloading"},
          {"automationId": "ValueStringText"}, {"automationId": "DateTextButtonOld"},
          {"automationId": "HeaderButton"}, {"automationId": "DurationTextBlock"},
          {"controlType": "ListItem", "namePrefix": "Sample "}, {"automationId": "ExpandButton"},
          {"automationId": "SeeMoreLessButton"}, {"controlType": "Window", "namePrefix": "New notification from"}]
LABELS = [({"automationId": "Title", "className": "TextBlock"}, "Title"), ({"automationId": "Content"}, "Body"),
          ({"automationId": "TimeStamp"}, "Time"), ({"automationId": "SenderName"}, "Sender"),
          ({"automationId": "MessageText"}, "Body"), ({"controlType": "Text", "name": "Sample package"},
                                                      "Progress title"),
          ({"controlType": "Text", "name": "Downloading"}, "Progress status"),
          ({"automationId": "ValueStringText"}, "Progress value"), ({"automationId": "DateTextButtonOld"}, "Date"),
          ({"automationId": "HeaderButton"}, "Period"), ({"automationId": "DurationTextBlock"}, "Focus duration"),
          ({"controlType": "ListItem", "namePrefix": "Sample "}, "Notification"),
          ({"automationId": "ExpandButton"}, "Expand or collapse"), ({"automationId": "SeeMoreLessButton"},
                                                                     "More or fewer notifications"),
          ({"controlType": "Window", "namePrefix": "New notification from"}, "Toast"),
          ({"automationId": "progressBar"}, "Progress bar"), ({"automationId": "MainListView"}, "Notification groups"),
          ({"automationId": "CalendarControl"}, "Calendar view"), ({"automationId": "MonthViewScrollViewer"}, "Days"),
          ({"automationId": "YearViewScrollViewer"}, "Months")]
HOSTS = [{"className": "ScrollViewer"}]
# The days, months and years shown depend on today's date: measured, not listed.
ENVIRONMENT_LISTS = [{"automationId": "MonthViewScrollViewer"}, {"automationId": "YearViewScrollViewer"},
                     {"automationId": "DecadeViewScrollViewer"}]
# A button's caption repeats the button.
OMIT = [{"automationId": "VerbText"}, {"automationId": "StartButtonText"},
        {"automationId": "NotificationSettingsButtonText"}]
REST = (8, 8)


def center(ctx, timeout=10):
    deadline = time.monotonic() + timeout
    while True:
        for name in CENTER_NAMES:
            try:
                return ctx.find_window(timeout=0, className=CENTER_CLASS, name=name)
            except core.Incomplete:
                pass
        if time.monotonic() >= deadline:
            raise core.Incomplete(f"no notification center window ({' or '.join(CENTER_NAMES)}) within {timeout} s")
        time.sleep(0.5)


def post(ctx, xml, tag, suppress):
    ctx.powershell(POST.format(sender=SENDER, name=SENDER_NAME, xml=xml, tag=tag,
                               suppress="true" if suppress else "false"))


def dnd_on(ctx, c):
    try:
        ctx.find(within=c, controlType="Text", name="Do not disturb is on", depth=3, timeout=1)
        return True
    except core.Incomplete:
        return False


def open_center(ctx, dnd=False):
    ctx.keys("win+n")
    c = center(ctx)
    ctx.find(within=c, automationId="CalendarCenterGrid", depth=3, timeout=10)
    if dnd_on(ctx, c) != dnd:
        ctx.click(ctx.find(within=c, automationId="DoNotDisturbButton", depth=3))
        deadline = time.monotonic() + 10
        while dnd_on(ctx, center(ctx)) != dnd:
            if time.monotonic() > deadline:
                raise core.Incomplete(f"Do not disturb did not turn {'on' if dnd else 'off'}")
            time.sleep(0.5)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def close_center(ctx):
    ctx.keys("esc")
    deadline = time.monotonic() + 15
    while center_present(ctx):
        if time.monotonic() > deadline:
            raise core.Incomplete("the notification center stayed open after Esc")
        time.sleep(0.5)


def open_main(ctx):
    post(ctx, RICH, "rich", True)
    open_center(ctx)


def rich_card(ctx):
    return ctx.find(within=center(ctx), controlType="ListItem", namePrefix=RICH_TITLE, depth=6, timeout=10)


def open_expanded(ctx):
    open_center(ctx)
    ctx.click(ctx.find(within=rich_card(ctx), automationId="ExpandButton", depth=2))
    ctx.find(within=rich_card(ctx), automationId="ExpandButton", namePrefix="Received", depth=2)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def mail_group(ctx):
    return ctx.find(within=center(ctx), controlType="Group", namePrefix="Notifications from Sample Mail", depth=6,
                    timeout=10)


def open_group(ctx):
    # The fixture gives Sample Mail three cards; the group shows one and a
    # "More notifications" button until it is opened.
    open_center(ctx)
    ctx.click(ctx.find(within=mail_group(ctx), automationId="SeeMoreLessButton", depth=3))
    deadline = time.monotonic() + 10
    while sum(1 for k in mail_group(ctx).children() if k.control_type == "ListItem" and not k.offscreen) < 3:
        if time.monotonic() > deadline:
            raise core.Incomplete("Sample Mail's group did not show its three cards after More notifications")
        time.sleep(0.5)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def card_menu(ctx):
    return ctx.find(within=center(ctx), automationId="NotificationMenuFlyout", depth=8, timeout=10)


def open_card_menu(ctx):
    open_center(ctx)
    ctx.click(ctx.find(within=rich_card(ctx), automationId="SettingsButton", depth=2))
    card_menu(ctx)
    time.sleep(1)


def close_card_menu(ctx):
    ctx.keys("esc")
    time.sleep(1)
    close_center(ctx)


def calendar(ctx):
    return ctx.find(within=center(ctx), automationId="CalendarCenterGrid", depth=3, timeout=10)


def calendar_view(ctx):
    return ctx.find(within=center(ctx), automationId="CalendarControl", depth=5, timeout=10)


def showing(ctx):
    """The calendar's header and the views on screen, for a failure report."""
    try:
        cal = calendar_view(ctx)
        parts = [f"{k.automation_id or k.control_type}:{k.name!r}" for k in cal.children() if not k.offscreen]
        return ", ".join(parts) or "nothing"
    except (core.Incomplete, ctx.live.COMError) as exc:
        return f"(unreadable: {exc})"


def decade_header(ctx):
    return ctx.find(within=calendar_view(ctx), automationId="HeaderButton", depth=2, timeout=10)


def measure_grid(ctx, view, key):
    """Columns, rows, item size and pitch of the calendar view on screen."""
    deadline = time.monotonic() + GRID_WAIT
    last = "no read finished"
    while True:
        # A zoom swaps the view's items while it animates; a read that meets a
        # vanished item looks again, as find() does.
        try:
            pane = ctx.find(within=calendar_view(ctx), automationId=view, depth=2, timeout=10)
            kids = pane.children()
            items = [k for k in kids if not k.offscreen]
            if items:
                break
            last = f"{len(kids)} item(s), none on screen"
        except ctx.live.COMError as exc:
            last = f"UI Automation failed: {exc}"
        if time.monotonic() > deadline:
            raise core.Incomplete(f"the calendar's {view} showed no readable items within {GRID_WAIT} s ({last}); "
                                  f"the calendar showed {showing(ctx)}")
        time.sleep(0.5)
    xs, ys = sorted({k.bounds[0] for k in items}), sorted({k.bounds[1] for k in items})
    MEASURED[key] = {"columns": len(xs), "rows": len(ys), "size": items[0].bounds[2],
                     "pitch-x": xs[1] - xs[0], "pitch-y": ys[1] - ys[0]}


def open_calendar_once(ctx, zoom):
    open_center(ctx)
    # The calendar stays expanded between openings; it opens on the month view.
    toggle = ctx.find(within=calendar(ctx), automationId="ExpandCollapseButton", depth=2)
    if toggle.name == "Expand Calendar":
        ctx.click(toggle)
    ctx.find(within=calendar(ctx), automationId="ExpandCollapseButton", name="Collapse Calendar", depth=2)
    # Each zoom waits until its view reads before the next click: a click while
    # the view still animates left the calendar unreadable for the opening.
    time.sleep(SETTLE)
    measure_grid(ctx, *VIEWS[0])
    for level in range(1, zoom + 1):
        ctx.click(ctx.find(within=calendar_view(ctx), automationId="HeaderButton", depth=2))
        time.sleep(SETTLE)
        measure_grid(ctx, *VIEWS[level])
    ctx.hover_point(*REST)


def open_calendar(ctx, zoom=0):
    # WORKAROUND: Windows' CalendarView provider sometimes fails every read of
    # its year or decade items for a whole opening (0x80040201, as an Incomplete
    # from measure_grid or a raw COMError from a find, 2026-11-17), while the
    # next opening reads them; nothing on our side can make it answer.
    last = None
    for _ in range(CALENDAR_TRIES):
        try:
            return open_calendar_once(ctx, zoom)
        except (core.Incomplete, ctx.live.COMError) as exc:
            last = exc
            print(f"notifications: a calendar opening failed, opening again ({exc})", flush=True)
            close_calendar(ctx)
    raise core.Incomplete(f"{CALENDAR_TRIES} openings of the calendar failed; the last: {last}")


def center_present(ctx):
    try:
        center(ctx, timeout=0)
        return True
    except core.Incomplete:
        return False


def close_calendar(ctx):
    """Esc first leaves a zoomed calendar view, then closes the window."""
    for _ in range(3):
        ctx.keys("esc")
        time.sleep(1)
        if not center_present(ctx):
            return
    raise core.Incomplete("three Esc presses left the notification center open")


def banner_window(ctx):
    return ctx.find_window(timeout=15, **BANNER)


def toast(ctx):
    return ctx.find(within=banner_window(ctx), controlType="Window", namePrefix="New notification from", depth=3,
                    timeout=10)


def open_banner(ctx):
    """Times a plain banner from posting until it leaves, then shows the rich one."""
    post(ctx, PLAIN, "timing", False)
    banner_window(ctx)
    shown = time.monotonic()
    while True:
        try:
            ctx.find_window(timeout=0, **BANNER)
        except core.Incomplete:
            break
        if time.monotonic() - shown > TIMING_MAX:
            raise core.Incomplete(f"the plain banner stayed over {TIMING_MAX} s")
        time.sleep(0.25)
    MEASURED["banner-ms"] = round((time.monotonic() - shown) * 1000 / 250) * 250
    post(ctx, RICH, "banner", False)
    toast(ctx)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def close_banner(ctx):
    ctx.click(ctx.find(within=toast(ctx), automationId="DismissButton", depth=2))
    ctx.gone(timeout=15, **BANNER)


def center_grid(ctx):
    return ctx.find(within=center(ctx), automationId="NotificationCenterGrid", depth=2, timeout=10)


def open_dnd(ctx):
    open_center(ctx, dnd=True)


def open_empty(ctx):
    open_center(ctx)
    ctx.click(ctx.find(within=center(ctx), automationId="ClearAllButton", depth=4))
    deadline = time.monotonic() + 10
    while True:
        try:
            ctx.find(within=center(ctx), controlType="ListItem", depth=6, timeout=0)
        except core.Incomplete:
            break
        if time.monotonic() > deadline:
            raise core.Incomplete("Clear all left notifications in the center")
        time.sleep(0.5)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def close_dnd(ctx):
    close_center(ctx)
    open_center(ctx, dnd=False)
    close_center(ctx)


SCENES = [
    dict(slug=ROOT, label="Notification center", kind="surface", open=open_main, root=center, close=close_center,
         expand=False),
    # The matrix's collapsed state is the center as it opens (the first scene).
    dict(slug="notifications-expanded", label="Opened notification group", kind="region", open=open_group,
         root=mail_group, close=close_center, expand=False),
    dict(slug="card-expanded", label="Expanded notification", kind="region", open=open_expanded, root=rich_card,
         close=close_center, expand=False),
    dict(slug="card-menu", label="Notification menu", kind="contextMenu", open=open_card_menu, root=card_menu,
         close=close_card_menu, expand=False),
    dict(slug="calendar-month", label="Calendar, month", kind="region", open=lambda ctx: open_calendar(ctx, 0),
         root=calendar, close=close_calendar, expand=False),
    dict(slug="calendar-year", label="Calendar, year", kind="region", open=lambda ctx: open_calendar(ctx, 1),
         root=calendar_view, close=close_calendar, expand=False),
    # The decade view's items fail UI Automation reads at random (0x80040201,
    # 2026-11-17); its grid is measured with retries as it opens, and the walk
    # takes only what the decade view adds: its header, unavailable at the top.
    dict(slug="calendar-decade", label="Calendar, decade", kind="region", open=lambda ctx: open_calendar(ctx, 2),
         root=decade_header, close=close_calendar, expand=False),
    dict(slug="toast", label="Toast", kind="region", open=open_banner, root=toast, close=close_banner, expand=False),
    dict(slug="dnd-on", label="Do not disturb on", kind="region", open=open_dnd, root=center_grid, close=close_dnd,
         omit=OMIT + [{"automationId": "MainListView"}], expand=False),
    dict(slug="empty", label="No notifications", kind="region", open=open_empty, root=center_grid,
         close=close_center, expand=False),
]


def METRICS(records):
    """The center, card, banner and calendar sizes, the slots' pitch and the banner's life."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}

    def box(rid, what):
        r = by.get(p + rid)
        if r is None or "bounds" not in r:
            raise ValueError(f"{what} was not captured ({rid})")
        return r["bounds"]

    for i in range(1, 6):
        box(f"toast.button-{i}", f"toast button {i}")
    for slot in ("hero", "sender", "title", "body", "progress", "progress-title", "progress-status", "progress-value",
                 "reply"):
        box(f"toast.{slot}", f"the toast's {slot} slot")
    out = []

    def metric(slug, label, value, order, unit="px"):
        out.append({"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": order,
                    "measure": {"value": value, "unit": unit, "scaling": "scales" if unit == "px" else "fixed"}})

    grid = box(f"{ROOT}.center", "the center")
    # A closed group shows one card (the fixture's first: its cards share one timestamp).
    shown = [r["id"][len(p):] for r in records if r.get("parent") == f"{p}{ROOT}.center.groups.mail-group"
             and r["label"] == "Notification"]
    if len(shown) != 1:
        raise ValueError(f"a closed app group showed {len(shown)} cards, not one")
    card = box(shown[0], "a collapsed card")
    group = box(f"{ROOT}.center.groups.mail-group", "an app group")
    t = box("toast", "the toast")
    b1, b2 = box("toast.button-1", "button 1"), box("toast.button-2", "button 2")
    metric("center-width", "Center width", grid["width"], 0)
    metric("group-height", "App group with one card", group["height"], 1)
    metric("card-height", "Collapsed card height", card["height"], 2)
    metric("card-expanded-height", "Expanded card height", box("card-expanded", "the expanded card")["height"], 3)
    metric("toast-width", "Toast width", t["width"], 4)
    metric("toast-hero-height", "Hero image height", box("toast.hero", "the hero image")["height"], 5)
    metric("toast-button-height", "Toast button height", b1["height"], 6)
    metric("toast-button-pitch", "Toast button pitch", b2["x"] - b1["x"], 7)
    metric("toast-reply-height", "Reply box height", box("toast.reply", "the reply box")["height"], 8)
    metric("toast-progress-height", "Progress bar height", box("toast.progress", "the progress bar")["height"], 9)
    for i, key in enumerate(("month", "year", "decade")):
        g = MEASURED.get(key)
        if g is None:
            raise ValueError(f"the calendar's {key} view was not measured")
        metric(f"calendar-{key}-columns", f"Calendar {key} columns", g["columns"], 10 + 4 * i, "count")
        metric(f"calendar-{key}-rows", f"Calendar {key} rows", g["rows"], 11 + 4 * i, "count")
        metric(f"calendar-{key}-item", f"Calendar {key} item size", g["size"], 12 + 4 * i)
        metric(f"calendar-{key}-pitch", f"Calendar {key} pitch across", g["pitch-x"], 13 + 4 * i)
    if (MEASURED["month"]["columns"], MEASURED["month"]["rows"]) != (7, 6):
        raise ValueError(f"the month grid is {MEASURED['month']['columns']} by {MEASURED['month']['rows']}, not 7 by 6")
    if "banner-ms" not in MEASURED:
        raise ValueError("the banner's life was not measured")
    metric("toast-duration", "Time a banner stays", MEASURED["banner-ms"], 22, "ms")
    return out


BEHAVIOURS = [
    ("open", "Opening", "Win+N or a click on the clock opens the center and the calendar together; Esc or a click "
                        "outside closes them."),
    ("dnd-toggle", "Do not disturb", "The bell button turns Do not disturb on and off; while on, the center says so "
                                     "and only priority notifications and alarms show a banner."),
    ("clear", "Clearing", "Clear all empties the center; a group's close button clears that app; a card's close "
                          "button clears one card, and the cards below move up."),
    ("expand", "Expanding a card", "The time button expands a card to its full text and image and collapses it "
                                   "again; a group shows one card and a More notifications "
                                   "button that opens the rest and closes them again."),
    ("calendar-zoom", "Calendar zoom", "The month and year header zooms out to the year and then the decade; picking "
                                       "a year or month zooms back in; Previous, Next and the wheel move a page."),
    ("focus", "Focus session", "The row under the calendar sets a focus length in 5 minute steps and starts a session, "
                               "which turns on Do not disturb until it ends."),
    ("toast-stack", "Banners in turn", "Banners show one at a time above the tray; the next waits until the shown one "
                                       "times out or is dismissed."),
    ("toast-dismiss", "Dismissing a banner", "The close button or a swipe to the right sends a banner to the center; "
                                             "a click on it opens the sending app."),
]
FRAMES = [
    ("high-priority", "No make high priority", "This build's card menu holds only Turn off notifications for the app "
                                               "and Go to notification settings; priority is set in Settings."),
]
KINDS_OF_TOAST = [
    ("plain", "Plain toast", "Title and body lines only; times out to the center."),
    ("buttons", "Toast with buttons", "Up to five buttons in one row under the content; a button acts and dismisses."),
    ("reply", "Toast with a reply", "A text box with a send button; the reply goes to the app."),
    ("selection", "Toast with a selection", "A drop-down of choices above the buttons, read by the button pressed."),
    ("progress", "Toast with progress", "A progress bar with its title, status and value, updated in place."),
    ("reminder", "Reminder or alarm", "Stays on screen until the user acts, with snooze and dismiss."),
    ("call", "Incoming call", "A call layout with accept and decline that stays and rings until answered."),
    ("urgent", "Urgent toast", "Breaks through Do not disturb, marked as important."),
]
HAND = ([{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "gesture", "label": label, "order": 40 + i,
          "behaviour": text} for i, (slug, label, text) in enumerate(BEHAVIOURS)]
        + [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "region", "label": label, "order": 60 + i,
            "behaviour": text} for i, (slug, label, text) in enumerate(FRAMES)]
        + [{"id": f"{ROOT}.toast-{slug}", "parent": ROOT, "kind": "state", "label": label, "order": 70 + i,
            "behaviour": text} for i, (slug, label, text) in enumerate(KINDS_OF_TOAST)])
