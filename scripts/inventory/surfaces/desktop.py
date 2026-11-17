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
import ctypes
import time

import capture_core as core

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
# A name long enough to wrap past the two lines an unselected label shows.
LONG = "Quarterly planning notes for the whole design team"
FIXTURE = r"""
$d = [Environment]::GetFolderPath('Desktop')
New-Item -ItemType Directory -Force (Join-Path $d 'Folder') | Out-Null
Set-Content -LiteralPath (Join-Path $d 'Text.txt') -Value 'x'
Set-Content -LiteralPath (Join-Path $d 'Quarterly planning notes for the whole design team.txt') -Value 'x'
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


# ------------------------------------------------------------- pixel reads --
# What UI Automation does not expose about an icon (its artwork's size, the
# label's width and lines, the selection and hover rectangles) is read from
# the screen: the cell's pixels compared with the same cell in another state.
user32 = getattr(ctypes, "windll", None) and ctypes.windll.user32
WM_COMMAND = 0x0111
TOGGLE_ICONS = 0x7402              # the desktop view's Show desktop icons command
COLOUR_STEP = 60                   # a channel sum this far from the other state is a change
PAD = 8                            # read this far outside the cell: a highlight may overhang it
FOCUS_EDGE = 2                     # the focus rectangle's band inside the cell's edge
MEASURED = {}                      # read by the icon-size openers, published by METRICS


def changed(a, b, what):
    """The rows and columns where two reads of one box differ, as
    [(row, first column, last column)]."""
    rows = []
    for r, (ra, rb) in enumerate(zip(a, b)):
        cols = [c for c, (pa, pb) in enumerate(zip(ra, rb)) if sum(abs(i - j) for i, j in zip(pa, pb)) > COLOUR_STEP]
        if cols:
            rows.append((r, cols[0], cols[-1]))
    if not rows:
        raise core.Incomplete(f"nothing changed on the screen for {what}")
    return rows


def extent(rows):
    """(width, height) of the changed rows' bounding box."""
    return max(r[2] for r in rows) - min(r[1] for r in rows) + 1, rows[-1][0] - rows[0][0] + 1


def blocks(rows):
    """Runs of consecutive changed rows: the icon's artwork, then its label."""
    out = [[rows[0]]]
    for row in rows[1:]:
        if row[0] == out[-1][-1][0] + 1:
            out[-1].append(row)
        else:
            out.append([row])
    return out


def toggle_icons(ctx):
    view = user32.FindWindowExW(desktop(ctx).el.CurrentNativeWindowHandle, 0, "SHELLDLL_DefView", None)
    if not view:
        raise core.Incomplete("the desktop's view window was not found")
    user32.SendMessageW(view, WM_COMMAND, TOGGLE_ICONS, 0)
    time.sleep(1.5)


def read_cell(ctx, size):
    """The Recycle Bin's artwork and label at this icon size, and at medium size
    its selection and hover rectangles."""
    x, y, w, h = (int(v) for v in icon(ctx, BIN).bounds)
    box = (x - PAD, y - PAD, w + 2 * PAD, h + 2 * PAD)
    # The artwork and label are read inside the cell's edge: the dotted focus
    # rectangle runs along it and goes with the icons (2026-11-17).
    inner = (x + FOCUS_EDGE, y + FOCUS_EDGE, w - 2 * FOCUS_EDGE, h - 2 * FOCUS_EDGE)
    ctx.hover_point(*empty_point(ctx))
    time.sleep(1)
    plain, plain_inner = ctx.pixels(*box), ctx.pixels(*inner)
    toggle_icons(ctx)
    try:
        bare = ctx.pixels(*inner)
    finally:
        toggle_icons(ctx)
        icon(ctx, BIN)
    parts = blocks(changed(plain_inner, bare, f"the {size} Recycle Bin icon"))
    if len(parts) < 2:
        raise core.Incomplete(f"the {size} Recycle Bin showed no gap between its artwork and its label")
    art, label = parts[0], [r for part in parts[1:] for r in part]
    MEASURED[size] = {"art": extent(art), "label": extent(label)}
    if size != "medium":
        return
    ctx.hover_point(x + w // 2, y + h // 2)
    time.sleep(1)
    MEASURED["hover"] = extent(changed(plain, ctx.pixels(*box), "the hovered icon"))
    ctx.click(icon(ctx, BIN))
    ctx.hover_point(*empty_point(ctx))
    time.sleep(1)
    MEASURED["selected"] = extent(changed(plain, ctx.pixels(*box), "the selected icon"))
    ctx.click_at(*empty_point(ctx))         # nothing selected for later scenes
    read_long(ctx)


def read_long(ctx):
    """A long name's label at medium size: how wide it wraps, and how tall its
    cell and its selection rectangle grow (the lines shown unselected and
    selected); the Recycle Bin's caption is too short to show either."""
    x, y, w, h = (int(v) for v in icon(ctx, LONG).bounds)
    # Selected, the whole name shows below the cell: read well past its foot.
    box = (x - PAD, y - PAD, w + 2 * PAD, h + 2 * PAD + 12 * 15)
    inner = (x + FOCUS_EDGE, y + FOCUS_EDGE, w - 2 * FOCUS_EDGE, h - 2 * FOCUS_EDGE)
    ctx.hover_point(*empty_point(ctx))
    time.sleep(1)
    plain, plain_inner = ctx.pixels(*box), ctx.pixels(*inner)
    toggle_icons(ctx)
    try:
        bare = ctx.pixels(*inner)
    finally:
        toggle_icons(ctx)
        icon(ctx, LONG)
    parts = blocks(changed(plain_inner, bare, "the long-named icon"))
    if len(parts) < 2:
        raise core.Incomplete("the long-named icon showed no gap between its artwork and its label")
    ctx.click(icon(ctx, LONG))
    ctx.hover_point(*empty_point(ctx))
    time.sleep(1)
    selected = extent(changed(plain, ctx.pixels(*box), "the selected long-named icon"))
    ctx.click_at(*empty_point(ctx))
    MEASURED["long"] = {"wrap": extent([r for part in parts[1:] for r in part])[0], "height": h,
                        "selected": selected[1]}


def icon_size(chord, size):
    def opener(ctx):
        ctx.click_at(*empty_point(ctx))       # the desktop takes the keys
        ctx.keys(chord)
        icon(ctx, BIN)
        time.sleep(1)
        read_cell(ctx, size)
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
SCENES += [dict(slug=f"icons-{size}", label=f"{size.title()} icons", kind="state", open=icon_size(chord, size),
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
    for size in ("large", "medium", "small"):
        if size not in MEASURED:
            raise ValueError(f"the {size} icon's pixels were not read")
        (aw, ah), (lw, lh) = MEASURED[size]["art"], MEASURED[size]["label"]
        out += [(f"art-width-{size}", f"Icon artwork width, {size} icons", aw),
                (f"art-height-{size}", f"Icon artwork height, {size} icons", ah),
                (f"label-width-{size}", f"Recycle Bin caption ink width, {size} icons", lw)]
    for state, label in (("hover", "Hover rectangle"), ("selected", "Selection rectangle")):
        if state not in MEASURED:
            raise ValueError(f"the icon's {state} rectangle was not read")
        out += [(f"{state}-width", f"{label} width", MEASURED[state][0]),
                (f"{state}-height", f"{label} height", MEASURED[state][1])]
    if "long" not in MEASURED:
        raise ValueError("the long-named icon's label was not read")
    lines = {slug: v for slug, _, v in out}
    one, step = lines["cell-height-one-line"], lines["label-line-height"]
    lng = MEASURED["long"]
    if not step or (lng["height"] - one) % step or (lng["selected"] - one) % step:
        raise ValueError(f"the long label's heights {lng['height']} and {lng['selected']} are not whole lines of "
                         f"{step} over {one}")
    out += [("label-wrap-width", "Label wrap width (a long name's widest line)", lng["wrap"]),
            ("label-lines", "Label lines shown unselected", (lng["height"] - one) // step + 1),
            ("label-lines-selected", "Label lines shown selected (the long name)", (lng["selected"] - one) // step + 1)]
    missing = [slug for slug, _, v in out if v is None]
    if missing:
        raise ValueError(f"the grid could not be measured: {', '.join(missing)}")
    counts = {"label-lines", "label-lines-selected"}
    return [{"id": f"{ROOT}.{slug}", "parent": ROOT, "kind": "metric", "label": label, "order": i,
             "measure": {"value": v, "unit": "count", "scaling": "fixed"} if slug in counts
             else {"value": v, "unit": "px", "scaling": "scales"}} for i, (slug, label, v) in enumerate(out)]


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


HAND = _hand(start=40)                 # orders after the metrics, which number from 0 (25 on 26300.9974)
