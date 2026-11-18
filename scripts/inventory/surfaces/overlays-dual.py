"""Surface module: the Project page's display modes, from the two-display stand-in.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 9 (the
shell overlays). Writes docs/design/inventory/flyouts/overlays-dual.json:

    python3 scripts/inventory/capture.py --surface flyouts --file overlays-dual \
        --module overlays-dual --profile dual-100-150

Win+P opens Quick Settings on its Project page (quick-settings.json holds the
page as the reference VM shows it: one display, so no modes). With a second
display the page lists the four modes, which this file records.
"""
import time

import capture_core as core

SURFACE = "flyouts"
OPEN_TRIES = 4
OPEN_PAUSE = 3
SETTLE = 2

LABELS = [({"automationId": "ListViewDisplaySwitch"}, "Display modes"), ({"automationId": "Footer"}, "Footer")]


def page(ctx):
    return ctx.find(within=ctx.find_window(className="ControlCenterWindow", timeout=10, foreground=False),
                    automationId="PageWindow", depth=5, timeout=10)


def open_project(ctx):
    # Right after sign-in the input host can swallow the first press (2026-11-18).
    for attempt in range(1, OPEN_TRIES + 1):
        ctx.keys("win+p")
        try:
            page(ctx)
            time.sleep(SETTLE)
            return
        except core.Incomplete:
            if attempt == OPEN_TRIES:
                raise
            time.sleep(OPEN_PAUSE)


def close_project(ctx):
    ctx.keys("esc")
    ctx.gone(className="ControlCenterWindow", timeout=10)


SCENES = [dict(slug="project-modes", label="Project, with a second display", kind="state", open=open_project,
               root=page, close=close_project, expand=False)]


def METRICS(records):
    """The mode rows' size and pitch, from captured bounds."""
    p = f"win11.{SURFACE}.project-modes."
    tiles = sorted((r for r in records if r["id"].startswith(p) and r.get("controlType") in ("listitem", "button",
                    "radiobutton") and r["bounds"]["height"] > 40), key=lambda r: (r["bounds"]["y"], r["bounds"]["x"]))
    if len(tiles) < 4:
        raise ValueError(f"the Project page listed {len(tiles)} modes, want 4")
    a, b = tiles[0]["bounds"], tiles[1]["bounds"]
    pitch = (b["x"] - a["x"]) if b["y"] == a["y"] else (b["y"] - a["y"])

    def metric(slug, label, order, value):
        return {"id": f"project-modes.{slug}", "parent": "project-modes", "kind": "metric", "label": label,
                "order": order, "measure": {"value": value, "unit": "px", "scaling": "scales"}}
    return [metric("mode-row-width", "Mode row width", 100, a["width"]),
            metric("mode-row-height", "Mode row height", 101, a["height"]),
            metric("mode-row-pitch", "Mode row pitch", 102, pitch)]


HAND = [{"id": "project-modes.current-mode", "parent": "project-modes", "kind": "gesture", "label": "Current mode",
         "order": 140, "behaviour": "Each mode is a row with its two-screen glyph and name; the current one (Extend on "
         "a fresh two-display machine) carries the accent bar at its left edge and the focus rectangle."}]
