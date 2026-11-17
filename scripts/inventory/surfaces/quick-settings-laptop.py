"""Surface module: Quick Settings on a laptop, from the stand-in machine.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (Quick
Settings). Writes docs/design/inventory/flyouts/quick-settings-laptop.json:

    python3 scripts/inventory/capture.py --surface flyouts --file quick-settings-laptop \
        --module quick-settings-laptop --standin discharging

What the reference VM cannot show (flyouts/quick-settings.json has the rest),
measured on 26300.9974 (2026-11-17) on the stand-in laptop on battery: the grid
with Airplane mode (unavailable: no radio) and Energy saver in its battery form,
the brightness slider over a live volume row, the battery read-out in the
footer, the output chooser page the volume row opens, and the media card that
shows above the tiles while an app plays. The stand-in's sound output is QEMU's
HD Audio speakers (stand-ins.json `sound-output`).
"""
import time

import capture_core as core

SURFACE = "flyouts"
ROOT = "quick-settings-laptop"

WINDOW = {"className": "ControlCenterWindow"}
SETTLE = 2
# A sound any build ships; Media Player opens it and gives the card a session.
MEDIA = r"C:\Windows\Media\Alarm01.wav"
MEDIA_WAIT = 8
MEDIA_TRIES = 4
# A VPN profile is software, so the stand-in shows the VPN tile, the one split
# tile it can: Wi-Fi and Bluetooth need radios QEMU cannot give Windows.
VPN = "Zephyrine VPN"
TILES = [("AirplaneMode", "airplane-mode", "Airplane mode"), ("Accessibility", "accessibility", "Accessibility"),
         ("Vpn", "vpn", "VPN"), ("BatterySaver", "energy-saver", "Energy saver"),
         ("LiveCaptions", "live-captions", "Live captions"),
         ("BlueLightReduction", "night-light", "Night light"), ("NearShare", "nearby-sharing", "Nearby sharing"),
         ("VolumeNoTimer", "mute", "Mute volume"), ("Battery", "battery", "Battery"),
         ("AllSettings", "all-settings", "All settings")]
IDENTITY = ([({"automationId": f"Microsoft.QuickAction.{aid}"}, slug) for aid, slug, _ in TILES]
            + [({"automationId": "TitleText", "name": cap}, f"{slug}-caption") for _, slug, cap in TILES]
            + [({"controlType": "Slider", "name": "Brightness"}, "brightness"),
               ({"controlType": "Slider", "name": "Sound output"}, "volume"),
               ({"automationId": "SplitL2Button", "name": "Manage VPN connections"}, "vpn-arrow"),
               ({"automationId": "VolumeL2Button"}, "sound-output"), ({"automationId": "FooterGrid"}, "footer"),
               ({"automationId": "QuickActionsPager"}, "pager"),
               ({"automationId": "ListWithOutputGroupTitle"}, "devices"),
               ({"controlType": "List", "name": "Spatial sound"}, "spatial-sound"),
               ({"controlType": "List", "name": "Volume mixer"}, "mixer"),
               ({"automationId": "MediaTransportControlsButton"}, "card"), ({"automationId": "AppNameText"}, "app"),
               ({"automationId": "TitleText", "className": "TextBlock"}, "title"),
               ({"automationId": "SubtitleText"}, "subtitle"),
               ({"automationId": "SystemSettings_Vpn_Connections_Flyout_ListView"}, "connections"),
               ({"controlType": "ListItem", "namePrefix": VPN}, "connection"),
               ({"automationId": "SystemSettings_Connections_VPN_Collection_TitleTextBlock"}, "name"),
               ({"automationId": "SystemSettings_Connections_VPN_Collection_DescriptionTextBlock"}, "status")])
# The playing app and its track are the session's own.
VALUES = [{"automationId": "AppNameText"}, {"automationId": "SubtitleText"},
          {"automationId": "TitleText", "name": "Alarm01"},
          {"controlType": "ListItem", "namePrefix": VPN},
          {"automationId": "SystemSettings_Connections_VPN_Collection_TitleTextBlock"},
          {"automationId": "SystemSettings_Connections_VPN_Collection_DescriptionTextBlock"}]
LABELS = [({"automationId": "FooterGrid"}, "Footer"), ({"automationId": "Footer"}, "Footer"),
          ({"automationId": "AppNameText"}, "App name"), ({"automationId": "SubtitleText"}, "Artist"),
          ({"automationId": "TitleText", "name": "Alarm01"}, "Track title"),
          ({"automationId": "SystemSettings_Vpn_Connections_Flyout_ListView"}, "VPN connections"),
          ({"controlType": "ListItem", "namePrefix": VPN}, "VPN connection"),
          ({"automationId": "SystemSettings_Connections_VPN_Collection_TitleTextBlock"}, "Connection name"),
          ({"automationId": "SystemSettings_Connections_VPN_Collection_DescriptionTextBlock"}, "Connection status")]
HOSTS = [{"className": "Windows.UI.Input.InputSite.WindowClass"}, {"className": "ScrollViewer"}]
# The output devices and the apps in the mixer are the machine's own.
ENVIRONMENT_LISTS = [{"automationId": "ListWithOutputGroupTitle"}, {"controlType": "List", "name": "Volume mixer"}]
KINDS = [({"automationId": "QuickActionsPager"}, "group")]
REST = (8, 8)
ADD_VPN = (f"if (-not (Get-VpnConnection -Name '{VPN}' -ErrorAction Ignore)) {{ "
           f"Add-VpnConnection -Name '{VPN}' -ServerAddress 'vpn.zephyrine.invalid' -TunnelType Ikev2 -Force }}")


def window(ctx):
    return ctx.find_window(timeout=10, **WINDOW)


def region(ctx):
    return ctx.find(within=window(ctx), automationId="ControlCenterRegion", depth=4, timeout=10)


def tile(ctx, aid):
    return ctx.find(within=region(ctx), automationId=f"Microsoft.QuickAction.{aid}", depth=3, timeout=10)


def open_qs(ctx):
    ctx.powershell(ADD_VPN)
    ctx.keys("win+a")
    region(ctx)
    tile(ctx, "Battery")
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def close_qs(ctx):
    ctx.keys("esc")
    ctx.gone(timeout=15, **WINDOW)


def chooser(ctx):
    return ctx.find(within=region(ctx), automationId="PageWindow", name="Sound output", depth=2, timeout=10)


def open_chooser(ctx):
    open_qs(ctx)
    ctx.click(ctx.find(within=region(ctx), automationId="VolumeL2Button", depth=2, timeout=10))
    chooser(ctx)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def vpn_page(ctx):
    return ctx.find(within=region(ctx), automationId="PageWindow", depth=2, timeout=10)


def open_vpn_page(ctx):
    open_qs(ctx)
    ctx.click(ctx.find(within=region(ctx), automationId="SplitL2Button", name="Manage VPN connections", depth=2))
    vpn_page(ctx)
    time.sleep(SETTLE)
    ctx.hover_point(*REST)


def card(ctx):
    return ctx.find(within=window(ctx), automationId="MediaTransportControls", depth=3, timeout=10)


def open_card(ctx):
    """The card element is always in the tree, offscreen until a session reaches
    the shell, which can be after the flyout opened: reopen until it shows."""
    ctx.powershell(f"Start-Process -FilePath '{MEDIA}'")
    for attempt in range(MEDIA_TRIES):
        time.sleep(MEDIA_WAIT)
        open_qs(ctx)
        if not card(ctx).offscreen:
            time.sleep(SETTLE)
            ctx.hover_point(*REST)
            return
        close_qs(ctx)
    raise core.Incomplete(f"the media card stayed offscreen through {MEDIA_TRIES} openings with {MEDIA} playing")


def close_card(ctx):
    close_qs(ctx)
    # The player may already have closed after the short clip; only a running one is stopped.
    ctx.powershell("$p = @(Get-Process -Name Microsoft.Media.Player -ErrorAction Ignore); "
                   "if ($p.Count) { $p | Stop-Process -Force }")


SCENES = [
    dict(slug=ROOT, label="Quick Settings on a laptop", kind="surface", open=open_qs, root=window, close=close_qs,
         expand=False),
    dict(slug="sound-output-page", label="Sound output page", kind="region", open=open_chooser, root=chooser,
         close=close_qs, expand=False),
    dict(slug="vpn-page", label="VPN page", kind="region", open=open_vpn_page, root=vpn_page, close=close_qs,
         expand=False),
    dict(slug="media-card", label="Media card", kind="region", open=open_card, root=card, close=close_card,
         expand=False),
]


def METRICS(records):
    """The laptop rows: brightness and volume, the battery read-out, the chooser's rows and the media card."""
    p = f"win11.{SURFACE}."
    by = {r["id"]: r for r in records}
    grid = f"{ROOT}.controlcenterregion"

    def rec(rid, what):
        r = by.get(p + rid)
        if r is None or "bounds" not in r:
            raise ValueError(f"{what} was not captured ({rid})")
        return r

    for slug in ("mute", "volume", "sound-output"):
        if not rec(f"{grid}.{slug}", slug).get("enabled", True):
            raise ValueError(f"{slug} is unavailable: the stand-in's sound output did not reach Quick Settings")
    out = []

    def metric(slug, label, value, order):
        out.append({"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": order,
                    "measure": {"value": value, "unit": "px", "scaling": "scales"}})

    bright, vol = (rec(f"{grid}.{s}", s)["bounds"] for s in ("brightness", "volume"))
    battery = rec(f"{grid}.footer.battery", "the battery read-out")["bounds"]
    region_box = rec(grid, "the tile region")["bounds"]
    metric("region-height", "Content height on a laptop", region_box["height"], 0)
    metric("slider-pitch", "Brightness to volume pitch", vol["y"] - bright["y"], 1)
    metric("slider-width", "Slider width", bright["width"], 2)
    metric("battery-width", "Battery read-out width", battery["width"], 3)
    metric("battery-height", "Battery read-out height", battery["height"], 4)
    dev = rec("sound-output-page.devices", "the output device list")["bounds"]
    spatial = rec("sound-output-page.spatial-sound", "the spatial sound list")["bounds"]
    mixer = rec("sound-output-page.mixer", "the volume mixer")["bounds"]
    metric("device-row-height", "Output device row height", dev["height"], 5)
    metric("spatial-row-height", "Spatial sound row height", spatial["height"] // 2, 6)
    metric("mixer-row-height", "Mixer row height", mixer["height"], 7)
    cardbox = rec("media-card", "the media card")["bounds"]
    play = rec("media-card.playpausebutton", "the play button")["bounds"]
    metric("card-width", "Media card width", cardbox["width"], 8)
    metric("card-height", "Media card height", cardbox["height"], 9)
    metric("card-button-size", "Transport button size", play["width"], 10)
    vpn, arrow = (rec(f"{grid}.{s}", s)["bounds"] for s in ("vpn", "vpn-arrow"))
    if vpn["x"] + vpn["width"] != arrow["x"]:
        raise ValueError("the VPN tile's two parts do not meet")
    metric("split-part-width", "Split tile part width", vpn["width"], 11)
    metric("vpn-row-height", "VPN connection row height",
           rec("vpn-page.connections.connection", "the VPN connection row")["bounds"]["height"], 12)
    return out


BEHAVIOURS = [
    ("battery-click", "Battery read-out", "The battery percentage in the footer opens the Power & battery page in "
                                          "Settings; its glyph shows the charge and whether the charger is in."),
    ("sliders", "Sliders", "Dragging, the arrow keys or the wheel set brightness and volume; the mute button toggles "
                           "the output, and the volume glyph follows the level."),
    ("chooser", "Output chooser", "The arrow beside the volume slider opens the sound output page: picking a device "
                                  "makes it the default output at once; Volume mixer opens the per-app levels in "
                                  "Settings."),
    ("card-session", "Media card session", "The card appears while an app holds a media session and follows the "
                                           "most recent one; clicking it brings the app forward, and its buttons "
                                           "act on that session."),
]
FRAMES = [
    ("airplane-unavailable", "Airplane mode without radios", "Airplane mode shows but is unavailable on a machine "
                                                            "with no wireless radio; with one it toggles every radio "
                                                            "off and back."),
]
HAND = ([{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "gesture", "label": label, "order": 40 + i,
          "behaviour": text} for i, (slug, label, text) in enumerate(BEHAVIOURS)]
        + [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "region", "label": label, "order": 60 + i,
            "behaviour": text} for i, (slug, label, text) in enumerate(FRAMES)])
