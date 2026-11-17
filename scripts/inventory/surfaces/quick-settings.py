"""Surface module: Quick Settings, its pages, its tile menu.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (Quick
Settings). Writes docs/design/inventory/flyouts/quick-settings.json:

    python3 scripts/inventory/capture.py --surface flyouts --file quick-settings --module quick-settings

Measured on 26300.9974 (2026-11-17): Win+A opens a full-height
ControlCenterWindow whose content is one region at its foot: a grid of three
tiles by two rows, a pager down its right edge (two pages on the reference VM),
the volume row and a footer with All settings. A tile is a button with its
caption under it; Cast's caption is its status. Accessibility, Cast and Project
open a page in place of the grid, each with a Back button and a footer link.
This build has no edit mode: a tile's context menu holds only Go to Settings.
The reference VM has no wireless adapter, no Bluetooth radio, no battery, no
backlight and no audio device, so the Wi-Fi, Bluetooth, Airplane mode, hotspot
and battery tiles are absent and the volume row is disabled; what a laptop
shows comes from the stand-in machine (its own file).
"""
import time

import capture_core as core

SURFACE = "flyouts"
ROOT = "quick-settings"

WINDOW = {"className": "ControlCenterWindow"}
SETTLE = 2
# The automation id after "Microsoft.QuickAction.", its slug and its caption.
TILES = [("Accessibility", "accessibility", "Accessibility"), ("EnergySaverAcOnly", "energy-saver", "Energy saver"),
         ("LiveCaptions", "live-captions", "Live captions"), ("BlueLightReduction", "night-light", "Night light"),
         ("NearShare", "nearby-sharing", "Nearby sharing"), ("Cast", "cast", "Cast"), ("ProjectL2", "project", "Project"),
         ("VolumeNoTimer", "mute", "Mute volume"), ("AllSettings", "all-settings", "All settings")]
SWITCHES = [("Magnifier", "magnifier"), ("Narrator", "narrator"), ("Color filters", "color-filters"),
            ("Live captions", "live-captions"), ("Mono audio", "mono-audio")]
IDENTITY = ([({"automationId": f"Microsoft.QuickAction.{aid}"}, slug) for aid, slug, _ in TILES]
            + [({"automationId": "TitleText", "name": cap}, f"{slug}-caption") for _, slug, cap in TILES]
            + [({"automationId": "StatusText"}, "cast-status"), ({"controlType": "Slider"}, "volume"),
               ({"automationId": "VolumeL2Button"}, "sound-output"), ({"automationId": "FooterGrid"}, "footer"),
               ({"automationId": "QuickActionsPager"}, "pager")]
            + [({"className": "ToggleSwitch", "name": name}, f"{seg}-switch") for name, seg in SWITCHES])
# Cast shows its connection status where the other tiles show their caption.
VALUES = [{"automationId": "StatusText"}]
LABELS = [({"automationId": "StatusText"}, "Cast status"), ({"automationId": "FooterGrid"}, "Footer"),
          ({"automationId": "Footer"}, "Footer"), ({"automationId": "ListViewDevices"}, "Devices")]
# The window's input site and the pages' scroll viewers are read through.
HOSTS = [{"className": "Windows.UI.Input.InputSite.WindowClass"}, {"className": "ScrollViewer"}]
# The cast page's device list is the machine's own.
ENVIRONMENT_LISTS = [{"automationId": "ListViewDevices"}]
# The popup host a tile's menu opens in is captured with that menu.
OMIT = [{"className": "Xaml_WindowedPopupClass"}]
# The pager is a strip of page dots, not a menu of commands.
KINDS = [({"automationId": "QuickActionsPager"}, "group")]
REST = (8, 8)


def window(ctx):
    return ctx.find_window(timeout=10, **WINDOW)


def region(ctx):
    return ctx.find(within=window(ctx), automationId="ControlCenterRegion", depth=4, timeout=10)


def tile(ctx, aid):
    return ctx.find(within=region(ctx), automationId=f"Microsoft.QuickAction.{aid}", depth=3, timeout=10)


def open_qs(ctx):
    ctx.keys("win+a")
    region(ctx)
    tile(ctx, "AllSettings")
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def close_qs(ctx):
    ctx.keys("esc")
    ctx.gone(timeout=15, **WINDOW)


def open_page_2(ctx):
    open_qs(ctx)
    ctx.click(ctx.find(within=region(ctx), automationId="NextPageButton", depth=3))
    ctx.find(within=region(ctx), automationId="Microsoft.QuickAction.ProjectL2", depth=2, timeout=10)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def page_opener(aid, title, on_page_2=False):
    def opener(ctx):
        (open_page_2 if on_page_2 else open_qs)(ctx)
        ctx.click(tile(ctx, aid))
        page(title)(ctx)
        time.sleep(SETTLE)
        ctx.hover_point(*REST)
    return opener


def page(title):
    return lambda ctx: ctx.find(within=region(ctx), automationId="PageWindow", name=title, depth=2, timeout=10)


def open_tile_menu(ctx):
    open_qs(ctx)
    ctx.click(tile(ctx, "NearShare"), button="right")
    tile_menu(ctx)
    time.sleep(1)


def tile_menu(ctx):
    return ctx.find(within=window(ctx), className="MenuFlyout", depth=4, timeout=10)


def close_tile_menu(ctx):
    ctx.keys("esc")
    time.sleep(1)
    close_qs(ctx)


# A page shows in place of the grid: the volume row and the footer it keeps are
# recorded once, with the first page.
GRID_ONLY = [{"automationId": "Microsoft.QuickAction.VolumeNoTimer"}, {"controlType": "Slider"},
             {"automationId": "VolumeL2Button"}, {"automationId": "FooterGrid"}]

SCENES = [
    dict(slug=ROOT, label="Quick Settings", kind="surface", open=open_qs, root=window, close=close_qs, expand=False),
    dict(slug="page-2", label="Second page of tiles", kind="region", open=open_page_2, root=region, close=close_qs,
         omit=OMIT + GRID_ONLY, expand=False),
    dict(slug="accessibility-page", label="Accessibility page", kind="region",
         open=page_opener("Accessibility", "Accessibility"), root=page("Accessibility"), close=close_qs, expand=False),
    dict(slug="cast-page", label="Cast page", kind="region", open=page_opener("Cast", "Cast"), root=page("Cast"),
         close=close_qs, expand=False),
    dict(slug="project-page", label="Project page", kind="region", open=page_opener("ProjectL2", "Project", True),
         root=page("Project"), close=close_qs, expand=False),
    dict(slug="tile-menu", label="Tile menu", kind="contextMenu", open=open_tile_menu, root=tile_menu,
         close=close_tile_menu, omit=(), expand=False),
]


def METRICS(records):
    """The grid's sizes and pitch, the volume row, the footer and a page's rows."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}
    grid = f"{ROOT}.controlcenterregion"

    def box(rid, what):
        r = by.get(p + rid)
        if r is None or "bounds" not in r:
            raise ValueError(f"{what} was not captured ({rid})")
        return r["bounds"]

    for _, slug, _ in TILES[:6]:
        box(f"{grid}.{slug}", f"the {slug} tile")
    if box(f"{grid}.mute", "the mute button") and by[f"{p}{grid}.mute"].get("enabled", True):
        raise ValueError("the volume row is enabled: the reference VM was expected to have no audio device")
    out = []

    def metric(slug, label, value, order):
        out.append({"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": order,
                    "measure": {"value": value, "unit": "px", "scaling": "scales"}})

    reg = box(grid, "the tile region")
    acc, energy, night = (box(f"{grid}.{s}", s) for s in ("accessibility", "energy-saver", "night-light"))
    cap = box(f"{grid}.accessibility-caption", "a tile caption")
    metric("region-width", "Content width", reg["width"], 0)
    metric("tile-width", "Tile width", acc["width"], 1)
    metric("tile-height", "Tile height", acc["height"], 2)
    metric("tile-pitch-x", "Tile pitch across", energy["x"] - acc["x"], 3)
    metric("tile-pitch-y", "Tile pitch down", night["y"] - acc["y"], 4)
    metric("caption-gap", "Caption gap below a tile", cap["y"] - acc["y"] - acc["height"], 5)
    metric("caption-height", "Caption height", cap["height"], 6)
    metric("volume-row-height", "Volume row height", box(f"{grid}.volume", "the volume slider")["height"], 7)
    metric("footer-height", "Footer height", box(f"{grid}.footer", "the footer")["height"], 8)
    pg = box("accessibility-page", "the accessibility page")
    mag, nar = (box(f"accessibility-page.vision.{s}-switch", s) for s in ("magnifier", "narrator"))
    metric("page-width", "Page width", pg["width"], 9)
    metric("page-back-size", "Page back button", box("accessibility-page.backbutton", "the back button")["width"], 10)
    metric("switch-width", "Page switch width", mag["width"], 11)
    metric("page-row-pitch", "Page row pitch", nar["y"] - mag["y"], 12)
    return out


BEHAVIOURS = [
    ("open", "Opening", "Win+A, or a click on the tray's network, volume and battery group, opens Quick Settings; Esc "
                        "or a click outside closes it."),
    ("keyboard", "Keyboard path", "Tab moves between the grid, the pager, the volume row and the footer; arrows move "
                                  "between tiles; Space or Enter toggles a tile or opens its page; Back or Esc leaves "
                                  "a page."),
    ("tile-states", "Tile states", "A toggle tile is off or on (Energy saver, Night light, Nearby sharing, Live "
                                   "captions); a tile with a page opens it (Accessibility, Cast, Project); a tile "
                                   "whose hardware is missing is absent, and the mute button is unavailable with no "
                                   "audio device."),
    ("live-captions", "Live captions on", "Turning Live captions on closes Quick Settings and opens the captions bar."),
    ("pager", "Pages of tiles", "Tiles beyond six go to further pages; the pager's dots and the wheel move between "
                                "them, the pager disables Previous on the first page and Next on the last."),
]
FRAMES = [
    ("edit-mode", "No edit mode", "This build has no edit control: the footer holds only All settings and a tile's "
                                  "menu holds only Go to Settings, so tiles cannot be pinned, unpinned or moved."),
    ("media-card", "Media card", "The media card above the tiles shows only while an app plays media with a "
                                 "transport session; the reference machine plays none, so it is absent."),
    ("brightness", "Brightness slider", "Absent on the reference VM, whose display has no brightness control, as on "
                                        "a desktop monitor; the laptop file shows it."),
    ("focus", "Focus", "Focus is not a tile on this build: it starts from the notification center's clock area."),
    ("project-modes", "Projection modes", "The Project page lists PC screen only, Duplicate, Extend and Second screen "
                                          "only when the display driver can project; the reference VM's cannot, so "
                                          "the page shows its error line instead."),
]
# Tiles no stand-in and no host can show (expected.json's absent rows give each
# one's reason): each is designed as ours from the sibling named. Airplane
# mode, VPN, Energy saver on battery and the battery read-out are in the laptop
# file, captured on the stand-in.
ABSENT_TILES = [
    ("wifi", "Wi-Fi", "A split tile like VPN (quick-settings-laptop): the switch half turns the radio on or off, "
                      "the arrow half opens the network list page."),
    ("bluetooth", "Bluetooth", "A split tile like VPN (quick-settings-laptop): the switch half turns the radio on "
                               "or off, the arrow half opens the device list page."),
    ("mobile-hotspot", "Mobile hotspot", "A toggle tile like Night light: on shares the connection."),
    ("rotation-lock", "Rotation lock", "A toggle tile like Night light: on keeps the screen's orientation; shown "
                                       "only on a device with an orientation sensor."),
]
HAND = ([{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "gesture", "label": label, "order": 40 + i,
          "behaviour": text} for i, (slug, label, text) in enumerate(BEHAVIOURS)]
        + [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "region", "label": label, "order": 60 + i,
            "behaviour": text} for i, (slug, label, text) in enumerate(FRAMES)]
        + [{"id": f"{ROOT}.absent-{slug}", "parent": ROOT, "kind": "control", "label": label, "order": 80 + i,
            "behaviour": f"Absent on the reference machine (no hardware): {text}"}
           for i, (slug, label, text) in enumerate(ABSENT_TILES)])
