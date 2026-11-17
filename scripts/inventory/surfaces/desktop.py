"""Surface module: the desktop, its icon grid and its context menus.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 8 (the
Desktop Surface item). Writes docs/design/inventory/desktop/desktop.json:

    python3 scripts/inventory/capture.py --surface desktop --file desktop [--merge]

The fixture icons (a folder, a text file and a shortcut) are made by the first
scene; the reference VM restores its clean checkpoint after the run, so they
never outlive it. Measured on 26300.9974 (2026-11-16): the desktop is a
SysListView32 "FolderView" under Progman, its menus are WinUI menus in their
own PopupWindowSiteBridge windows, and "Show more options" opens a classic
#32768 menu.
"""
SURFACE = "desktop"
ENVIRONMENT_LISTS = []
VALUES = []
IDENTITY = []
# A modern menu is a Flyout pane holding a command bar (the icon row: Cut, Copy,
# Paste, Rename, Share, Delete) and an overflow popup (the list); both report
# offscreen while their entries are on screen, so the walk reads through them.
HOSTS = [{"className": "ApplicationBar"}, {"automationId": "OverflowPopup"}]
# Each entry's icon is an unnamed Image: its slot is recorded, its artwork never.
LABELS = [({"controlType": "Image", "name": ""}, "Icon")]

BRIDGE = "Microsoft.UI.Content.PopupWindowSiteBridge"
# Icon names the fixture gives (file extensions are hidden by default).
FOLDER, FILE, SHORTCUT, BIN = "Folder", "Text", "Shortcut", "Recycle Bin"
FIXTURE = r"""
$d = [Environment]::GetFolderPath('Desktop')
New-Item -ItemType Directory -Force (Join-Path $d 'Folder') | Out-Null
Set-Content -LiteralPath (Join-Path $d 'Text.txt') -Value 'x'
$s = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $d 'Shortcut.lnk'))
$s.TargetPath = "$env:WINDIR\notepad.exe"; $s.Save()
(New-Object -ComObject Shell.Application).ToggleDesktop()
Start-Sleep -Seconds 3
"""


def desktop(ctx):
    return ctx.find_window(className="Progman")


def icon(ctx, name):
    return ctx.find(within=desktop(ctx), controlType="ListItem", name=name, depth=4)


def empty_point(ctx):
    """A point on the desktop no icon covers: icons fill columns from the left."""
    x, y, w, h = desktop(ctx).bounds
    return x + w * 3 // 4, y + h * 3 // 5


def modern(ctx):
    return ctx.find(className="Flyout", controlType="Pane", depth=6)


def classic(ctx):
    return ctx.find_window(className="#32768")


def right_click(target):
    def opener(ctx):
        if target is None:
            ctx.click_at(*empty_point(ctx), button="right")
        else:
            ctx.click(icon(ctx, target), button="right")
        modern(ctx)                   # waits until the menu is on screen
    return opener


def close_modern(ctx):
    ctx.keys("esc")
    ctx.gone(className=BRIDGE)


def show_more(target):
    def opener(ctx):
        right_click(target)(ctx)
        # A synthesized click on this entry is ignored (2026-11-16); Invoke reaches it.
        ctx.invoke(ctx.find(within=modern(ctx), automationId="expandtoclassic", depth=4))
        classic(ctx)
    return opener


def close_classic(ctx):
    ctx.keys("esc")
    ctx.gone(className="#32768")


def open_desktop(ctx):
    ctx.powershell(FIXTURE)
    # The desktop lists a new file late (Text and Folder were still missing 15 s
    # after the fixture, 2026-11-16); a refresh and a wait for each makes the walk
    # see all three.
    ctx.click_at(*empty_point(ctx))
    ctx.keys("f5")
    for name in (FOLDER, FILE, SHORTCUT):
        ctx.find(within=desktop(ctx), controlType="ListItem", name=name, depth=4, timeout=60)


def open_empty_menu(ctx):
    # Something to paste and something to undo, so the menu shows Paste and
    # Undo: Explorer's own copy and paste of the fixture file (a copy made
    # outside Explorer leaves nothing to undo, 2026-11-16).
    ctx.click(icon(ctx, FILE))
    ctx.keys("ctrl+c")
    ctx.click_at(*empty_point(ctx))
    ctx.keys("ctrl+v")
    ctx.find(within=desktop(ctx), controlType="ListItem", name=f"{FILE} - Copy", depth=4, timeout=30)
    right_click(None)(ctx)


def icon_size(chord):
    def opener(ctx):
        ctx.click_at(*empty_point(ctx))       # the desktop takes the keys
        ctx.keys(chord)
        icon(ctx, BIN)
    return opener


def medium_again(ctx):
    ctx.click_at(*empty_point(ctx))
    ctx.keys("ctrl+shift+3")


def nothing(ctx):
    pass


# The background page's fit list. Settings' own fit list is disabled while the
# reference VM's Windows is not activated (seen 2026-11-16); the classic Desktop
# Background page lists the same fits and stays enabled (2026-11-17).
BACKGROUND = r"Start-Process explorer.exe -ArgumentList 'shell:::{ED834ED6-4B5A-4bfe-8F11-A626DCB6A921} -Microsoft.Personalization\pageWallpaper'"
BACKGROUND_WINDOW = {"className": "CabinetWClass", "name": "Desktop Background - File Explorer"}


def background(ctx):
    return ctx.find_window(timeout=30, **BACKGROUND_WINDOW)


def fit_list(ctx):
    return ctx.find(within=background(ctx), className="ComboLBox", depth=14, timeout=10)


def open_fit_list(ctx):
    ctx.powershell(BACKGROUND)
    position = ctx.find(within=background(ctx), automationId="ComboBox_Position", depth=12, timeout=30)
    ctx.click(ctx.find(within=position, automationId="DropDown", depth=3, timeout=10))
    fit_list(ctx)


def close_fit_list(ctx):
    ctx.keys("esc")
    ctx.keys("alt+f4")
    ctx.gone(timeout=15, **BACKGROUND_WINDOW)


TARGETS = [("file", FILE, "File"), ("folder", FOLDER, "Folder"), ("shortcut", SHORTCUT, "Shortcut"),
           ("recycle-bin", BIN, "Recycle Bin")]

SCENES = [dict(slug="desktop", label="Desktop", kind="surface", open=open_desktop, root=desktop, close=nothing),
          dict(slug="menu", label="Desktop context menu", kind="contextMenu", open=open_empty_menu, root=modern,
               close=close_modern)]
SCENES += [dict(slug=f"{slug}-menu", label=f"{label} context menu", kind="contextMenu", open=right_click(name),
                root=modern, close=close_modern) for slug, name, label in TARGETS]
SCENES += [dict(slug="classic-menu", label="Desktop classic context menu", kind="contextMenu", open=show_more(None),
                root=classic, close=close_classic, expand=False)]
SCENES += [dict(slug=f"classic-{slug}-menu", label=f"{label} classic context menu", kind="contextMenu",
                open=show_more(name), root=classic, close=close_classic, expand=False) for slug, name, label in TARGETS]
SCENES += [dict(slug="fit-modes", label="Wallpaper fit", kind="menu", open=open_fit_list, root=fit_list,
                close=close_fit_list, expand=False)]
# Measured for METRICS and not published: the Recycle Bin's cell at each icon size.
SCENES += [dict(slug=f"icons-{size}", label=f"{size.title()} icons", kind="state", open=icon_size(chord),
                root=lambda ctx: icon(ctx, BIN), close=medium_again, keep=False)
           for size, chord in (("large", "ctrl+shift+2"), ("small", "ctrl+shift+4"), ("medium", "ctrl+shift+3"))]

ROOT = "desktop"                       # the surface record every metric and note hangs under


def METRICS(records):
    """The icon grid, measured from the captured icon cells (fhd-100: physical
    pixels at 100 %, so every value scales with the display)."""
    by = {r["id"]: r for r in records}
    cells = [r for r in records if r.get("parent") == f"win11.{SURFACE}.desktop.desktop" and r.get("controlType") == "listitem"]
    if not cells:
        raise ValueError("no desktop icon was captured; the grid cannot be measured")
    b = [c["bounds"] for c in cells]
    heights = sorted({x["height"] for x in b})
    column = sorted(x["y"] for x in b if x["x"] == min(y["x"] for y in b))
    pitches = sorted({q - p for p, q in zip(column, column[1:])})
    out = [("cell-width", "Icon cell width", b[0]["width"]),
           ("cell-height-one-line", "Icon cell height, one label line", heights[0]),
           ("label-line-height", "Icon label line height", heights[-1] - heights[0] if len(heights) > 1 else None),
           ("row-pitch", "Icon row pitch", pitches[0] if pitches else None),
           ("margin-top", "Grid top margin", min(x["y"] for x in b)),
           ("margin-left", "Grid left margin", min(x["x"] for x in b))]
    for size in ("large", "medium", "small"):
        cell = by.get(f"win11.{SURFACE}.icons-{size}")
        if cell is None:
            raise ValueError(f"the {size} icon scene was not measured")
        out += [(f"cell-width-{size}", f"Icon cell width, {size} icons", cell["bounds"]["width"]),
                (f"cell-height-{size}", f"Icon cell height, {size} icons", cell["bounds"]["height"])]
    missing = [slug for slug, _, v in out if v is None]
    if missing:
        raise ValueError(f"the grid could not be measured: {', '.join(missing)}")
    return [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": i,
             "measure": {"value": v, "unit": "px", "scaling": "scales"}} for i, (slug, label, v) in enumerate(out)]


def _hand(start):
    notes = [
        ("rubber-band", "gesture", "Rubber-band selection",
         "Dragging from an empty spot draws a selection rectangle; every icon it touches is selected."),
        ("drag-move", "gesture", "Drag to move",
         "Dragging an icon moves it to the drop point; onto a folder icon, the item moves into that folder."),
        ("ctrl-drag-copy", "gesture", "Ctrl-drag copy", "Holding Ctrl while dropping copies the item instead of moving it."),
        ("alt-drag-shortcut", "gesture", "Alt-drag shortcut", "Holding Alt while dropping creates a shortcut to the item."),
        ("ctrl-wheel-size", "gesture", "Ctrl and wheel icon size",
         "Ctrl with the mouse wheel steps the icon size through small, medium and large."),
        ("double-click-open", "gesture", "Double-click to open", "Double-clicking an icon opens it, as Enter does."),
        ("drop-tip-move", "cursor", "Move to",
         "Drag tip beside the pointer, followed by the target folder's name, when the drop will move the item."),
        ("drop-tip-copy", "cursor", "Copy to",
         "Drag tip with the copy cursor (a plus badge) when Ctrl is held or the target is on another drive."),
        ("drop-tip-link", "cursor", "Create link in", "Drag tip with the link cursor (an arrow badge) when Alt is held."),
        ("fit-span", "state", "Span fit",
         "With two or more displays the fit list adds Span, one picture stretched across every display; the reference "
         "VM has one display, so its list ends at Center."),
        ("fit-apply", "gesture", "Choosing a fit",
         "Choosing a fit redraws the wallpaper at once; Fill crops to cover, Fit letterboxes, Stretch distorts, Tile "
         "repeats from the top left and Center leaves the picture at its own size over the background colour."),
    ]
    out = [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": kind, "label": label, "order": start + i, "behaviour": text}
           for i, (slug, kind, label, text) in enumerate(notes)]
    out.append({"id": f"{ROOT}.rename", "parent": ROOT, "kind": "shortcut", "label": "Rename", "order": start + len(notes),
                "shortcut": "F2", "shortcutScope": "control",
                "behaviour": "Turns the selected icon's label into an edit box; Enter keeps the new name, Esc keeps the old."})
    return out


HAND = _hand(start=20)                 # orders after the metrics, which number from 0
