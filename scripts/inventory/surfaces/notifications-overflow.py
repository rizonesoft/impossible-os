"""Surface module: the notification center holding more than it shows.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8
(Notification Center, Calendar and Toast). Writes
docs/design/inventory/flyouts/notifications-overflow.json:

    python3 scripts/inventory/capture.py --surface flyouts --file notifications-overflow \
        --module notifications-overflow --prepare notifications:overflow

The overflow fixture gives three senders twelve cards each. Measured on
26300.9974 (2026-11-17): the center still fits them, because each group shows
one card and a More notifications button (552 px for three groups); it
overflows only once a group is opened, so the scene opens Sample Mail's twelve
and records the list as it then shows. The center's own parts are recorded by
the notifications module on the groups fixture.
"""
import time

import capture_core as core

SURFACE = "flyouts"
ROOT = "notifications-overflowing"

# The center's window is named either way on this build (2026-11-17).
CENTER_NAMES = ("Notification Center", "Windows Shell Experience Host")
CENTER_CLASS = "Windows.UI.Core.CoreWindow"
SETTLE = 2
REST = (8, 8)
SENDERS = (("Mail", "mail"), ("Calendar", "calendar"), ("Chat", "chat"))
IDENTITY = ([({"controlType": "Group", "namePrefix": f"Notifications from Sample {name}"}, f"{slug}-group")
             for name, slug in SENDERS]
            # The fixture numbers each sender's cards 1 to 12; the higher numbers
            # come first, so "Sample Mail 1" does not take "Sample Mail 12".
            + [({"controlType": "ListItem", "namePrefix": f"Sample {name} {i}"}, f"card-{i}")
               for name, _ in SENDERS for i in range(12, 0, -1)]
            + [({"controlType": "ListItem", "namePrefix": "Sample "}, "card"),
               ({"automationId": "Title", "className": "TextBlock"}, "title"), ({"automationId": "Content"}, "body"),
               ({"automationId": "TimeStamp"}, "time"), ({"automationId": "ExpandButton"}, "expand"),
               ({"automationId": "SeeMoreLessButton"}, "more")])
# Text a sender or the clock supplies, named by its role.
VALUES = [{"automationId": "Title", "className": "TextBlock"}, {"automationId": "Content"},
          {"automationId": "TimeStamp"}, {"controlType": "ListItem", "namePrefix": "Sample "},
          {"automationId": "ExpandButton"}, {"automationId": "SeeMoreLessButton"}]
LABELS = [({"automationId": "Title", "className": "TextBlock"}, "Title"), ({"automationId": "Content"}, "Body"),
          ({"automationId": "TimeStamp"}, "Time"), ({"controlType": "ListItem", "namePrefix": "Sample "}, "Notification"),
          ({"automationId": "ExpandButton"}, "Expand or collapse"),
          ({"automationId": "SeeMoreLessButton"}, "More or fewer notifications")]
HOSTS = [{"className": "ScrollViewer"}]


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


def groups(ctx):
    return ctx.find(within=center(ctx), automationId="MainListView", depth=5, timeout=10)


def dnd_on(ctx):
    try:
        ctx.find(within=center(ctx), controlType="Text", name="Do not disturb is on", depth=3, timeout=1)
        return True
    except core.Incomplete:
        return False


def open_center(ctx):
    ctx.keys("win+n")
    groups(ctx)
    # The reference VM starts with Do not disturb on; its banner would take the
    # list's place at the top.
    if dnd_on(ctx):
        ctx.click(ctx.find(within=center(ctx), automationId="DoNotDisturbButton", depth=3))
        deadline = time.monotonic() + 10
        while dnd_on(ctx):
            if time.monotonic() > deadline:
                raise core.Incomplete("Do not disturb did not turn off")
            time.sleep(0.5)
    mail = ctx.find(within=groups(ctx), controlType="Group", namePrefix="Notifications from Sample Mail", depth=2)
    ctx.click(ctx.find(within=mail, automationId="SeeMoreLessButton", depth=3))
    deadline = time.monotonic() + 10
    while not overflowing(ctx):
        if time.monotonic() > deadline:
            raise core.Incomplete("the list did not grow past the center after Sample Mail's group was opened")
        time.sleep(0.5)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def overflowing(ctx):
    """Part of the list is scrolled out of view: a card or a whole group reports offscreen."""
    for group in groups(ctx).children():
        if group.offscreen or any(k.offscreen for k in group.children() if k.control_type == "ListItem"):
            return True
    return False


def close_center(ctx):
    ctx.keys("esc")
    deadline = time.monotonic() + 15
    while True:
        try:
            center(ctx, timeout=0)
        except core.Incomplete:
            return
        if time.monotonic() > deadline:
            raise core.Incomplete("the notification center stayed open after Esc")
        time.sleep(0.5)


SCENES = [dict(slug=ROOT, label="Overflowing notification list", kind="surface", open=open_center, root=groups,
               close=close_center, expand=False)]


def METRICS(records):
    """The list's height and how many of the opened group's cards it shows."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}
    root = by.get(p + ROOT)
    if root is None or "bounds" not in root:
        raise ValueError("the overflowing list was not captured")
    cards = [r for r in records if r.get("parent") == f"{p}{ROOT}.mail-group" and r["label"] == "Notification"]
    if not 1 < len(cards) < 12:
        raise ValueError(f"the opened group showed {len(cards)} of its twelve cards; an overflowing list shows some")
    return [{"id": f"{ROOT}.list-height", "parent": ROOT, "kind": "metric", "label": "List height", "order": 0,
             "measure": {"value": root["bounds"]["height"], "unit": "px", "scaling": "scales"}},
            {"id": f"{ROOT}.cards-shown", "parent": ROOT, "kind": "metric", "label": "Cards of an opened group shown",
             "order": 1, "measure": {"value": len(cards), "unit": "count", "scaling": "scales"}}]


HAND = [{"id": f"{ROOT}.scroll", "parent": ROOT, "kind": "gesture", "label": "Scrolling the list", "order": 10,
         "behaviour": "A list taller than the center scrolls under the header with the wheel or a drag of its "
                      "scroll bar, which shows while the pointer is over it; each group keeps one card and its "
                      "More notifications button until it is opened, so only an opened group makes the list "
                      "scroll."}]
