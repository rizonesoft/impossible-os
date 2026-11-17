"""Surface module: the system tray on a laptop, from the stand-in machine.

Owner: the System Tray and Overflow item of the parity inventory's shell
surfaces section (todo/08-graphics-ui/TODO-17-win11-parity-inventory.md). Writes docs/design/inventory/taskbar/tray-laptop.json:

    python3 scripts/inventory/capture.py --surface taskbar --file tray-laptop \
        --module tray-laptop --standin discharging

What the reference VM cannot show (taskbar/tray.json has the rest): the battery
in the network and volume group, on the stand-in laptop on battery. Its name
is "Power" and a live status ("Battery status: 60% remaining" and the time
left), so it is matched by that word and its status withheld. Its
charging and saver glyphs are states of the same button (tray.json).
"""
import time

import capture_core as core

SURFACE = "taskbar"
ROOT = "tray-laptop"
SETTLE = 2.5

ENTRIES = [("Show Hidden Icons", "hidden-icons", "Show hidden icons"), ("Network", "network", "Network"),
           ("Volume", "volume", "Volume"), ("Power", "battery", "Battery"), ("Clock", "clock", "Clock"),
           ("Show Desktop", "show-desktop", "Show desktop")]
IDENTITY = [({"automationId": "SystemTrayIcon", "namePrefix": word}, slug) for word, slug, _ in ENTRIES]
LABELS = ([({"automationId": "SystemTrayIcon", "namePrefix": word}, label) for word, _, label in ENTRIES]
          + [({"controlType": "Image", "name": ""}, "Icon"), ({"className": "TextBlock"}, "Date and time")])
VALUES = [{"automationId": "SystemTrayIcon", "namePrefix": word} for word, _, _ in ENTRIES
          if word not in ("Show Hidden Icons", "Show Desktop")]
HOSTS = []
OMIT = [{"automationId": "TaskbarFrame"}, {"className": "TrayNotifyWnd"}, {"className": "MSTaskSwWClass"},
        {"className": "Xaml_WindowedPopupClass"}]


def tray(ctx):
    return ctx.find_window(className="Shell_TrayWnd")


def open_tray(ctx):
    ctx.find(within=tray(ctx), automationId="SystemTrayIcon", namePrefix="Power", depth=3, timeout=30)
    time.sleep(SETTLE)


def nothing(ctx):
    pass


SCENES = [dict(slug=ROOT, label="System tray on a laptop", kind="region", open=open_tray, root=tray, close=nothing)]


def METRICS(records):
    """The battery button's size beside the network and volume buttons."""
    p = f"win11.{SURFACE}."
    by = {r["id"].rsplit(".", 1)[-1]: r for r in records if r.get("id", "").startswith(p + ROOT + ".")}
    for slug in ("battery", "network", "volume"):
        if slug not in by or "bounds" not in by[slug]:
            raise ValueError(f"the tray's {slug} button was not captured on the stand-in laptop")
    b = by["battery"]["bounds"]
    if by["volume"]["bounds"]["x"] >= b["x"]:
        raise ValueError("the battery does not follow the volume button in the group")
    return [{"id": f"{ROOT}.battery-width", "parent": ROOT, "kind": "metric", "label": "Battery button width",
             "order": 0, "measure": {"value": b["width"], "unit": "px", "scaling": "scales"}}]


HAND = [{"id": f"{ROOT}.battery-click", "parent": ROOT, "kind": "gesture", "label": "Battery in the group",
         "order": 10, "behaviour": "The battery joins the network and volume group: a click on it opens Quick "
                                   "Settings like the rest of the group, with the battery read-out in its footer."}]
