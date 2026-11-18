"""Surface module: the volume indicator, from the stand-in laptop.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9 (the
shell overlays). Writes docs/design/inventory/flyouts/overlays-laptop.json:

    python3 scripts/inventory/capture.py --surface flyouts --file overlays-laptop \
        --module overlays-laptop --standin discharging

The reference VM has no sound device, so the volume keys show nothing there;
the stand-in has QEMU's HD Audio speakers (stand-ins.json `sound-output`).
Measured on 26300.9974 (2026-11-18): a volume key shows the indicator about
0.2 s later, centred on the screen and resting on the taskbar, and it leaves
about 2 s after the last press. The stand-in also has brightness keys
(standin.asl WMI0, an ACPI-WMI method that raises Notify (LCD0, 0x86)): they
move the level as a laptop's keys do, but on this display path Windows shows
no brightness indicator, so that one check waits for a real laptop panel.
"""
import time

import capture_core as core

SURFACE = "flyouts"
WINDOW = "XamlExplorerHostIslandWindow"
SHOW_WAIT = 3       # s for the indicator to appear after a key
GONE_WAIT = 10      # s for it to leave
POLL = 0.05

IDENTITY = [({"controlType": "Custom", "automationId": ""}, "indicator"),
            ({"className": "Button", "name": ""}, "speaker"),
            ({"automationId": "volumeSlider"}, "volume")]

LABELS = [({"controlType": "Custom", "automationId": ""}, "Indicator"), ({"className": "Button", "name": ""}, "Speaker")]

_measured = {}


def _indicator(ctx):
    live = ctx.live
    for w in live.children(live.uia.GetRootElement(), live.uia.CreateTrueCondition()):
        if w.class_name == WINDOW and not w.offscreen and w.bounds[2] > 0:
            return w
    return None


def _taskbar_top(ctx):
    return ctx.find_window(className="Shell_TrayWnd", foreground=False).bounds[1]


def _wait(ctx, shown, limit):
    end = time.monotonic() + limit
    while (_indicator(ctx) is not None) != shown:
        if time.monotonic() > end:
            raise core.Incomplete(f"the volume indicator did not {'appear' if shown else 'leave'} within {limit} s")
        time.sleep(POLL)


def press(key):
    def opener(ctx):
        _wait(ctx, False, GONE_WAIT)
        ctx.keys(key)
        _wait(ctx, True, SHOW_WAIT)
        x, y, w, h = _indicator(ctx).bounds
        _measured.update(gap=_taskbar_top(ctx) - (y + h), centre=x + w // 2)
    return opener


def root(ctx):
    w = _indicator(ctx)
    if w is None:
        raise core.Incomplete("the volume indicator left before it was walked")
    return w


def leave(key=None):
    """Time the indicator out from a fresh press, then undo the press."""
    def closer(ctx):
        _wait(ctx, False, GONE_WAIT)
        ctx.keys("volume_down" if key is None else key)
        _wait(ctx, True, SHOW_WAIT)
        t = time.monotonic()
        _wait(ctx, False, GONE_WAIT)
        _measured.setdefault("timeout", []).append((time.monotonic() - t) * 1000)
    return closer


SCENES = [
    dict(slug="volume-indicator", label="Volume indicator", kind="surface", open=press("volume_up"), root=root,
         close=leave(), expand=False),
    dict(slug="volume-indicator-muted", label="Volume indicator, muted", kind="state", open=press("volume_mute"),
         root=root, close=leave("volume_mute"), expand=False),
]


def METRICS(records):
    """The indicator's size and its gap above the taskbar from captured bounds,
    and its timeout as the shortest of the measured runs (a poll can only add)."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}
    r = by.get(p + "volume-indicator")
    if r is None or "gap" not in _measured or not _measured.get("timeout"):
        raise ValueError("the volume indicator was not captured")

    def metric(slug, label, order, value, unit="px", scaling="scales"):
        return {"id": f"volume-indicator.{slug}", "parent": "volume-indicator", "kind": "metric", "label": label,
                "order": order, "measure": {"value": value, "unit": unit, "scaling": scaling}}

    return [metric("width", "Volume indicator width", 100, r["bounds"]["width"]),
            metric("height", "Volume indicator height", 101, r["bounds"]["height"]),
            metric("taskbar-gap", "Gap above the taskbar", 102, _measured["gap"]),
            metric("timeout", "Time shown after the last key", 103, round(min(_measured["timeout"]), -2),
                   unit="ms", scaling="fixed")]


HAND = [
    {"id": "volume-indicator.volume-keys", "parent": "volume-indicator", "kind": "gesture", "label": "Volume keys",
     "order": 140, "behaviour": "Volume up, down and mute show the indicator, centred on the screen and resting on "
     "the taskbar; each press moves the level and restarts its timeout, and it leaves without a click."},
    {"id": "volume-indicator.speaker-mutes", "parent": "volume-indicator", "kind": "gesture",
     "label": "Speaker mutes", "order": 141,
     "behaviour": "The speaker button mutes and unmutes; muted, it shows the crossed speaker and the bar keeps the level."},
    {"id": "volume-indicator.level-number", "parent": "volume-indicator", "kind": "gesture", "label": "Level number",
     "order": 142, "behaviour": "Right of the bar the level shows as a number from 0 to 100; it is drawn text, not "
     "an element, so it carries no record of its own."},
]
