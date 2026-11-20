"""Surface module: File Explorer's dialogs.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 12 (the
File Explorer Dialogs item). Writes docs/design/inventory/explorer/dialogs.json:

    python3 scripts/inventory/capture.py --surface explorer --file dialogs --module explorer-dialogs --prepare explorer:sample

The window is the frame module's (shared/explorer_window.py), on the fixture's
sample tree (fixtures/explorer.ps1). A Properties sheet is a Win32 dialog with
a tab control: its frame and button row are one `dialog` scene (the tab pages
left out), and each tab is a `dialogTab` scene rooted at that tab's page, so
its itemCount is the page's own controls.
"""
import re
import time

import capture_core as core
from dialog_window import (CLOSE_PASSES, DIALOG_WAIT, appeared, baseline, close_new_windows, ps_quote, press, quiet,
                           top_windows)
from explorer_window import SAMPLE, SETTLE, _root, close_all, descendants, fresh, items, opened

SURFACE = "explorer"
# The classic address band's toolbar, named "Address: " and the folder's path,
# its buttons the path's folders.
ADDRESS_PATH = {"className": "ToolbarWindow32", "namePrefix": "Address: "}
# Lists whose rows are this machine's data, kept as a frame with no rows: the
# users and groups an access list names (they carry the machine's name), the disks,
# the icons a file holds, a file's signers and their dates, the address band's
# folders, the folder chooser's tree (Quick access pins), the Open With groups
# (the installed apps; its Store and browse links stay), and the file lists (as
# the frame module keeps them).
ENVIRONMENT_LISTS = [ADDRESS_PATH,
                     {"className": "SysTreeView32", "name": "Navigation Pane"},
                     {"className": "ListViewHeaderItem"},
                     {"className": "SysListView32", "name": "Embedded Signatures"},
                     {"className": "SysListView32", "name": "Catalog Signatures"},
                     {"className": "SysListView32", "name": "Group or user names:"},
                     {"className": "CCListViewEx", "automationId": "AceListView"},
                     {"className": "SysListView32", "name": "All disk drives:"},
                     {"className": "ListBox", "name": "Select an icon from the list below:"},
                     {"className": "UIItemsView"}]
# The Details tab's value cells (a list row's only child, a classless Text): this
# machine's dates, paths, owner and computer name, so they name no id and no label.
PROPERTY_VALUE = {"controlType": "Text", "className": ""}
# The drive's amounts, the delete prompt's item line ("Type: ... Size: ... Date
# modified: ..."), and the folder chooser's address band and Refresh button,
# which name the open folder.
BYTES, SIZE = {"className": "Static", "nameSuffix": " bytes"}, {"nameSuffix": " GB"}
ITEM_LINE = {"className": "Static", "namePrefix": "Type: "}
REFRESH = {"controlType": "Button", "namePrefix": "Refresh \""}
UP = {"controlType": "Button", "namePrefix": "Up to \""}       # names the parent folder
# A wizard's page host is named, and its automation id set, from a GUID made at run
# time (the extract wizard's changed between two captures, 2026-11-20).
WIZARD_PAGE = {"className": "Win32PropSheetPageHost"}
# Confirm Folder Replace names the folder's creation time twice.
MERGE_DATE = {"namePrefix": "Date created: "}
MERGE_QUESTION = {"namePrefix": "Do you still want to merge the folder created on "}
# A conflict tile's icon is named with the item's full path; the copy dialogs'
# other fields hold the item's date, size and properties, the space left, and the
# operation's progress, which differ on every run.
ITEM_ICON = {"automationId": "eltItemIcon"}
OPERATION_VALUES = {"eltItemDate": "Date", "eltItemSize": "Size", "eltItemProps": "Item details",
                    "eltItemsRemaining": "Items remaining", "eltRateChart_New": "Progress chart",
                    "eltSummary": "Progress", "eltTimeRemaining": "Time remaining"}
# An operation's tile is named for the operation and its progress so far.
OPERATION_TILE = {"automationIdPrefix": "eltTile", "nameSuffix": "% complete"}
VALUES = [PROPERTY_VALUE, BYTES, SIZE, ITEM_LINE, ADDRESS_PATH, REFRESH, UP, WIZARD_PAGE, MERGE_DATE, MERGE_QUESTION,
          ITEM_ICON, OPERATION_TILE,
          *({"automationId": a} for a in OPERATION_VALUES)]
# Look-alike siblings named for what they are, from their dialog control ids
# (resource ids, fixed for the dialog): the shortcut's two unnamed fields, the
# two signature lists' Details buttons, and the delete prompt's warning icon.
IDENTITY = [({"controlType": "Edit", "automationId": "102", "name": ""}, "shortcut-name"),
            ({"controlType": "Edit", "automationId": "13065", "name": ""}, "target-location-value"),
            ({"controlType": "Button", "name": "Details", "automationId": "100"}, "embedded-signature-details"),
            ({"controlType": "Button", "name": "Details", "automationId": "102"}, "catalog-signature-details"),
            ({"controlType": "Image", "className": "Static", "automationId": "12297"}, "warning-icon"),
            (ITEM_LINE, "item-details"),
            (WIZARD_PAGE, "page"),
            (MERGE_DATE, "date-created"),
            (MERGE_QUESTION, "merge-question"),
            # The drive's General tab: each amount by the row it sits on.
            ({"className": "Static", "automationId": "14406"}, "used-space-bytes"),
            ({"className": "Static", "automationId": "14405"}, "used-space-size"),
            ({"className": "Button", "automationId": "14404"}, "used-space-button"),
            ({"className": "Static", "automationId": "14407"}, "free-space-bytes"),
            ({"className": "Static", "automationId": "14408"}, "free-space-size"),
            ({"className": "Static", "automationId": "14409"}, "capacity-bytes"),
            ({"className": "Static", "automationId": "14410"}, "capacity-size"),
            ({"className": "Button", "automationId": "14411"}, "capacity-button")]
LABELS = [(PROPERTY_VALUE, "Property value"), (BYTES, "Bytes"), (SIZE, "Size"), (ITEM_LINE, "Item details"),
          (ADDRESS_PATH, "Address"), (REFRESH, "Refresh"), (UP, "Up"), (WIZARD_PAGE, "Page"), (MERGE_DATE, "Date created"),
          (MERGE_QUESTION, "Merge question"), (ITEM_ICON, "Item icon"),
          (OPERATION_TILE, "Operation"),
          *(({"automationId": a}, label) for a, label in OPERATION_VALUES.items())]

DIALOG = "#32770"
THIS_PC = "shell:MyComputerFolder"
TAB_CONTROL = "SysTabControl32"
# A tab's page belongs under its own tab in the sheet's strip: the record the
# frame scene writes for it (the strip's segment is its class, the tab's its name).
TAB_STRIP = core.slug(TAB_CONTROL)
# The sheets: (slug, label, folder, the item or items selected, the tabs in order,
# as the reference build shows them: 2026-11-20, Digital Signatures even on a text file).
SHEETS = [
    ("file", "A document", SAMPLE, ["Sample document"],
     ["General", "Digital Signatures", "Security", "Details", "Previous Versions"]),
    ("folder", "A folder", SAMPLE, ["General items"],
     ["General", "Sharing", "Security", "Previous Versions", "Customize"]),
    ("shortcut", "A shortcut", SAMPLE, ["Sample shortcut"],
     ["General", "Shortcut", "Digital Signatures", "Security", "Details", "Previous Versions"]),
    ("program", "A program", SAMPLE, ["Sample program"],
     ["General", "Compatibility", "Digital Signatures", "Security", "Details", "Previous Versions"]),
    ("drive", "A drive", THIS_PC, ["Windows (C:)"],
     ["General", "Tools", "Hardware", "Sharing", "Security", "Previous Versions", "Quota"]),
    ("several", "Several items", SAMPLE, ["Sample document", "Notes"], ["General", "Details"]),
]
FOLDER_OPTIONS_TABS = ["General", "View", "Search"]


# ------------------------------------------------------------- finding --

def start(ctx, folder=SAMPLE):
    """Every scene begins here: the window baseline (first time), then a fresh window on `folder`."""
    baseline(ctx)
    fresh(ctx, folder)


def target(ctx, name):
    """A row of the item list by name, or by a spec (a drive whose name carries its volume label)."""
    spec = name if isinstance(name, dict) else {"name": name}
    return ctx.find(within=items(ctx), controlType="ListItem", depth=4, **spec)


def dialog(ctx, wait=DIALOG_WAIT, owner=None, **spec):
    """A dialog window: top level, or owned by `owner` (UI Automation lists an
    owned window under its owner, in the raw view only; 2026-11-19, Choose Details)."""
    spec = {"className": DIALOG, **spec}
    deadline = time.monotonic() + wait
    while True:
        try:
            return ctx.find_window(timeout=0, **spec)
        except core.Incomplete:
            pass
        if owner is not None:
            for k in descendants(ctx, owner, 1):
                if core.matches(k, spec) and not k.offscreen:
                    return k
        if time.monotonic() > deadline:
            raise core.Incomplete(f"no dialog matching {spec} within {wait} s")
        time.sleep(0.5)


def tab_names(ctx, dlg):
    return [k.name for k in descendants(ctx, dlg, 3) if k.control_type == "TabItem"]


def select_tab(ctx, dlg, name):
    """Click a sheet's tab and return its page (a child dialog named after it)."""
    tabs = [k for k in descendants(ctx, dlg, 3) if k.control_type == "TabItem"]
    hit = [k for k in tabs if k.name == name]
    if not hit:
        raise core.Incomplete(f"the sheet has no tab {name!r}; its tabs are {[k.name for k in tabs]}")
    ctx.click(hit[0])
    time.sleep(SETTLE)
    return View(dlg, page=True)


class View:
    """A property sheet seen as its page or as its frame. UI Automation lists a
    page's controls as children of the sheet itself, beside the tab control
    (2026-11-20), so the page is what lies inside the tab control's rectangle
    and the frame (title bar, tab strip, button row) is the rest. Everything
    else is the sheet's own element."""

    def __init__(self, el, page):
        self._el, self._page = el, page
        tab = [k for k in el.children(limit=core.MAX_RECORDS) if k.class_name == TAB_CONTROL]
        if len(tab) != 1:
            raise core.Incomplete(f"the sheet {el.name!r} has {len(tab)} tab controls")
        self._tab = tab[0].bounds
        if page:
            self.bounds = self._tab

    def __getattr__(self, name):
        return getattr(self._el, name)

    def _inside(self, k):
        """On the page: inside the tab control and not its strip (a TabItem lies in
        the control's rectangle too, and belongs to the frame with the control)."""
        x, y, w, h = k.bounds
        tx, ty, tw, th = self._tab
        return k.class_name != TAB_CONTROL and k.control_type != "TabItem" and w > 0 and h > 0 and \
            tx <= x and ty <= y and x + w <= tx + tw and y + h <= ty + th

    def children(self, limit=core.MAX_RECORDS):
        return [k for k in self._el.children(limit=limit) if self._inside(k) == self._page]


# ------------------------------------------------------------- openers --

def open_sheet(folder, names):
    """Select the items and press Alt+Enter: the Properties sheet."""
    def opener(ctx):
        start(ctx, folder)
        ctx.click(target(ctx, names[0]))
        if len(names) > 1:
            ctx.hold("ctrl")
            try:
                for n in names[1:]:
                    ctx.click(target(ctx, n))
            finally:
                ctx.release("ctrl")
        time.sleep(0.5)
        ctx.keys("alt+enter")
        _root["dlg"] = dialog(ctx, nameSuffix="Properties")
        _root["el"] = View(_root["dlg"], page=False)
    return opener


def open_sheet_tab(folder, names, tab):
    def opener(ctx):
        open_sheet(folder, names)(ctx)
        _root["el"] = select_tab(ctx, _root["dlg"], tab)
    return opener


def open_folder_options(tab=None):
    """See more > Options: the Folder Options sheet."""
    def opener(ctx):
        start(ctx, SAMPLE)
        before = top_windows(ctx)
        ctx.launch("control.exe", "folders")
        _root["dlg"] = appeared(ctx, before)
        _root["el"] = select_tab(ctx, _root["dlg"], tab) if tab else View(_root["dlg"], page=False)
        if tab == "View":
            read_view_tree(ctx)
    return opener


# The View tab's advanced settings, every entry in tree order as (names from the
# top, control type): the walk keeps only the rows on screen, and the tree shows
# about a third of them (2026-11-20). A Win32 tree lists its scrolled-away rows to
# UI Automation as offscreen elements, so they are read here and METRICS writes them.
_view_tree = []
TREE_DEPTH = 4


def read_view_tree(ctx):
    _view_tree.clear()
    tree = ctx.find(within=_root["dlg"], controlType="Tree", depth=6)

    def visit(el, path, depth):
        for k in ctx.live.children(el.el, ctx.live.uia.CreateTrueCondition()):
            if k.control_type in ("TreeItem", "RadioButton", "CheckBox"):
                _view_tree.append((path + (k.name,), k.control_type))
                if depth < TREE_DEPTH:
                    visit(k, path + (k.name,), depth + 1)
    visit(tree, (), 1)
    if not _view_tree:
        raise core.Incomplete("the View tab's advanced settings tree listed no entries")


# File Explorer windows (close_all closes them after), and the helper window
# Explorer itself makes during a copy and keeps (2026-11-20).
KEEP_OPEN = ("CabinetWClass", "ThumbnailDeviceHelperWnd")


def close_dialogs(ctx):
    """Close every window the scene brought up, then the File Explorer windows.
    Esc on "the dialog" was not enough: the advanced security form is no #32770,
    so it and its sheet stayed open and the next scene found the old sheet (2026-11-20)."""
    close_new_windows(ctx, keep=KEEP_OPEN)
    close_all(ctx)


# ---------------------------------------------------------------- scenes --

SCENES = []
for slug, label, folder, names, tabs in SHEETS:
    SCENES.append(dict(slug=f"{slug}-properties", label=f"{label}: Properties", kind="dialog",
                       open=open_sheet(folder, names), root=opened, close=close_dialogs, expand=False))
    for tab in tabs:
        SCENES.append(dict(slug=f"{slug}-properties-{core.slug(tab)}", label=f"{label}: Properties, {tab}",
                           kind="dialogTab", open=open_sheet_tab(folder, names, tab), root=opened,
                           close=close_dialogs, count=True, expand=False,
                           parent=f"{slug}-properties.{TAB_STRIP}.{core.slug(tab)}"))
SCENES.append(dict(slug="folder-options", label="Folder Options", kind="dialog", open=open_folder_options(),
                   root=opened, close=close_dialogs, expand=False))
for tab in FOLDER_OPTIONS_TABS:
    SCENES.append(dict(slug=f"folder-options-{core.slug(tab)}", label=f"Folder Options, {tab}", kind="dialogTab",
                       open=open_folder_options(tab), root=opened, close=close_dialogs, count=True, expand=False,
                       parent=f"folder-options.{TAB_STRIP}.{core.slug(tab)}",
                       # The View tab's advanced settings: each entry of the tree is one fixed setting.
                       kinds=[({"controlType": "TreeItem"}, "setting")] if tab == "View" else []))


# ------------------------------------------------- dialogs the sheet opens --


def open_from_sheet(folder, names, tab, button, title, then=None):
    """A sheet's tab, then one of its buttons: the dialog it opens, by title."""
    def opener(ctx):
        open_sheet_tab(folder, names, tab)(ctx)
        press(ctx, _root["el"], **button)
        sub = dialog(ctx, owner=_root["dlg"], **title)
        if then:
            sub = then(ctx, sub)
        _root["el"] = sub
    return opener


SUBDIALOGS = [
    ("advanced-attributes", "Advanced Attributes", SAMPLE, ["Sample document"], "General",
     {"controlType": "Button", "name": "Advanced..."}, {"name": "Advanced Attributes"}, None),
    ("change-icon", "Change Icon", SAMPLE, ["Sample shortcut"], "Shortcut",
     {"controlType": "Button", "name": "Change Icon..."}, {"name": "Change Icon"}, None),
    ("permissions", "Permissions editor", SAMPLE, ["Sample document"], "Security",
     {"controlType": "Button", "namePrefix": "Edit"}, {"namePrefix": "Permissions for"}, None),
    ("advanced-security", "Advanced security settings", SAMPLE, ["Sample document"], "Security",
     {"controlType": "Button", "name": "Advanced"}, {"namePrefix": "Advanced Security Settings for"}, None),
    ("remove-properties", "Remove Properties", SAMPLE, ["Sample document"], "Details",
     {"namePrefix": "Remove Properties", "click": True}, {"name": "Remove Properties"}, None),
    ("folder-picture", "Folder picture chooser", SAMPLE, ["General items"], "Customize",
     {"controlType": "Button", "name": "Choose File..."}, {"name": "Browse"}, None),
]
for slug, label, folder, names, tab, button, title, then in SUBDIALOGS:
    SCENES.append(dict(slug=slug, label=label, kind="dialog", open=open_from_sheet(folder, names, tab, button, title, then),
                       root=opened, close=close_dialogs, expand=False))


# ------------------------------------------- windows an action brings up --


def close_window(ctx):
    """Esc, then Alt+F4 on whatever the scene opened, then the File Explorer windows."""
    el = _root.get("el")
    for chord in ("esc", "alt+f4"):
        if el is None or ctx.live.gone(el.el):
            break
        ctx.keys(chord)
        time.sleep(0.7)
    close_dialogs(ctx)


def launched(*argv):
    """A tool started on its own, its window the one that appears."""
    def opener(ctx):
        start(ctx, SAMPLE)
        before = top_windows(ctx)
        ctx.launch(*argv)
        _root["el"] = appeared(ctx, before)
    return opener


def by_menu_entry(folder, name, *entries):
    """Select an item, open its modern menu with Shift+F10 and invoke each entry by
    name in turn (a submenu's entry after the one that opens it)."""
    def opener(ctx):
        start(ctx, folder)
        ctx.click(target(ctx, name))
        time.sleep(0.5)
        before = top_windows(ctx)
        ctx.keys("shift+f10")
        within = ctx.find(className="Flyout", controlType="Pane", depth=6, timeout=15)
        for i, entry in enumerate(entries):
            el = ctx.find(within=within, name=entry, depth=6)
            if i == len(entries) - 1:
                ctx.invoke(el)
            else:
                ctx.expand_only(el)
                time.sleep(SETTLE)
                within = None              # the submenu is its own popup
        _root["el"] = appeared(ctx, before)
    return opener


def open_with(ctx):
    """From the menu, as a user opens it. OpenWith.exe is one instance: a copy left
    with no window (one another process started, 2026-11-20) takes every later
    request and shows nothing, so a leftover one is ended first."""
    ctx.powershell("@(Get-Process -Name OpenWith -ErrorAction SilentlyContinue) | ForEach-Object { Stop-Process -Id $_.Id -Force }\n"
                   "exit 0\n")
    by_menu_entry(SAMPLE, "Notes", "Open with", "Choose another app")(ctx)
    quiet(ctx, _root["el"])


ARCHIVE = SAMPLE + "\\Sample archive.zip"
WIZARDS = [
    ("open-with", "Open with", open_with),
    ("extract-wizard", "Extract Compressed (Zipped) Folders", by_menu_entry(SAMPLE, "Sample archive", "Extract All...")),
    ("map-network-drive", "Map Network Drive", launched("rundll32.exe", "shell32.dll,SHHelpShortcuts_RunDLL", "Connect")),
    ("add-network-location", "Add Network Location", launched("rundll32.exe", "shwebsvc.dll,AddNetPlaceRunDll")),
    # The small volume: Format on C: shows only "you can't format this volume" (2026-11-20).
    ("format", "Format", lambda ctx: open_format(ctx), lambda ctx: close_no_space(ctx)),
]
for slug, label, opener, *close in WIZARDS:
    SCENES.append(dict(slug=slug, label=label, kind="dialog", open=opener, root=opened,
                       close=close[0] if close else close_window, expand=False))


def open_format(ctx):
    work(ctx, MAKE_SMALL)
    by_menu_entry(THIS_PC, {"nameSuffix": "(" + SMALL_LETTER + ":)"}, "Format...")(ctx)  # FAT keeps the label upper-case


# ------------------------------------- file operations, conflicts, errors --

WORK = SAMPLE + "\\Dialog work"          # made by the scene that needs it, removed on close
OPERATION = {"className": "OperationStatusWindow"}
# Big enough that a copy is still running when its dialog is read: it is paused at once.
LARGE_BYTES = 2 << 30                    # within the reference VM's free space; .bin has no type, so its name shows the extension
MAKE_WORK = r"""
$w = '%s'
New-Item -ItemType Directory -Path $w -Force | Out-Null
""" % WORK
REMOVE_WORK = r"""
# Not this script: its own command line holds the marker it looks for.
@(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" | Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like '*inventory-hold*' }) |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
if (Test-Path -LiteralPath '%s') { Remove-Item -LiteralPath '%s' -Recurse -Force }
if (Test-Path -LiteralPath '%s') { Write-Output 'the work folder is still there'; exit 1 }
exit 0
""" % (WORK, WORK, WORK)


def work(ctx, script=""):
    ctx.powershell(MAKE_WORK + script)


def copy_into(ctx, source_folder, names, dest):
    """Copy items with Ctrl+C in one folder and Ctrl+V in another, as a user does."""
    start(ctx, source_folder)
    ctx.click(target(ctx, names[0]))
    ctx.hold("ctrl")
    try:
        for n in names[1:]:
            ctx.click(target(ctx, n))
    finally:
        ctx.release("ctrl")
    ctx.keys("ctrl+c")
    start(ctx, dest)
    ctx.click_at(*list_background(ctx))
    before = top_windows(ctx)
    ctx.keys("ctrl+v")
    return before


def list_background(ctx):
    x, y, w, h = items(ctx).bounds
    return x + w // 2, y + h - 20


def operation_window(ctx, before):
    return appeared(ctx, before, wait=30)


def open_copy_progress(detailed, stacked=False):
    """A large copy, paused as soon as its dialog shows; the detailed form is
    "More details" (2026-11-20 build: one window for every running operation)."""
    def opener(ctx):
        work(ctx, "fsutil file createnew '%s\\Large file.bin' %d | Out-Null\n"
                  "if ($LASTEXITCODE) { Write-Output 'fsutil could not make the large file'; exit 1 }\n"
                  "New-Item -ItemType Directory -Path '%s\\Copy target' -Force | Out-Null\n"
                  "New-Item -ItemType Directory -Path '%s\\Second target' -Force | Out-Null\n"
                  % (WORK, LARGE_BYTES, WORK, WORK))
        before = copy_into(ctx, WORK, ["Large file.bin"], WORK + "\\Copy target")
        win = operation_window(ctx, before)
        press(ctx, win, controlType="Button", namePrefix="Pause")
        if stacked:
            ctx.keys("alt+tab")
            copy_into(ctx, WORK, ["Large file.bin"], WORK + "\\Second target")
            time.sleep(SETTLE)
            press(ctx, win, controlType="Button", namePrefix="Pause")
        set_details(ctx, win, detailed)
        _root["el"] = win
    return opener


def set_details(ctx, win, detailed):
    """The compact or the detailed form. The window keeps the last one chosen, for
    every later operation (2026-11-20), so a scene sets the one it records."""
    want, other = ("More details", "Fewer details") if detailed else ("Fewer details", "More details")
    names = {k.name for k in descendants(ctx, win, 4) if k.control_type == "Button"}
    if want in names:
        press(ctx, win, controlType="Button", name=want)
    elif other not in names:
        raise core.Incomplete(f"the operation window shows neither {want!r} nor {other!r}")


def close_operation(ctx):
    """Cancel every operation in the window by its own tile (closing the window
    with two running asks "This will cancel all transfers in progress"), then the
    scene's windows, then the work folder."""
    el = _root.get("el")
    if el is not None and el.class_name == OPERATION["className"] and not ctx.live.gone(el.el):
        for _ in range(CLOSE_PASSES):
            tiles = [k for k in descendants(ctx, el, 6) if k.control_type == "Button" and k.name == "Cancel tile"]
            if not tiles:
                break
            ctx.invoke(tiles[0])
            time.sleep(1)
    close_dialogs(ctx)
    ctx.powershell(REMOVE_WORK)


def open_conflict(compare=False):
    """The same file copied onto itself in another folder: Replace or Skip Files,
    and "Compare info for both files", the chooser (its button name on 2026-11-20)."""
    def opener(ctx):
        work(ctx, "Copy-Item -LiteralPath '%s\\Sample document.txt' -Destination '%s'\n" % (SAMPLE, WORK))
        before = copy_into(ctx, SAMPLE, ["Sample document"], WORK)
        win = operation_window(ctx, before)
        if compare:
            shown = top_windows(ctx)
            press(ctx, win, controlType="Button", name="Compare info for both files")
            win = appeared(ctx, shown, wait=15)
        _root["el"] = win
    return opener


# Windows 11 hides the folder merge prompt by default ("Hide folder merge
# conflicts", on): the scene turns it off for its copy and back to the default
# on close. The child files differ, so no file conflict stands in for it.
SHOW_MERGE = r"""
Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name HideMergeConflicts -Value 0 -Type DWord
"""
HIDE_MERGE = r"""
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' -Name HideMergeConflicts -ErrorAction SilentlyContinue
"""
MERGE_TEXT = "already contains a folder named"
# The prompt's message sits in elements the control view does not list
# (2026-11-20), so the opener keeps its lines and METRICS records them, each
# creation time replaced by what it is.
MERGE_LINES = (MERGE_TEXT, "If any files have the same names", "Do you still want to merge")
WHEN = re.compile(r"\d{1,2}/\d{1,2}/\d{4} \d{1,2}:\d{2}( [AP]M)?")
DIRECTION_MARKS = dict.fromkeys(map(ord, "\u200e\u200f"))   # Windows wraps the date's parts in them
_merge_text = []


def open_folder_merge(ctx):
    """A folder copied onto a folder of the same name: Confirm Folder Replace."""
    work(ctx, SHOW_MERGE + "New-Item -ItemType Directory -Path '%s\\General items' -Force | Out-Null\n"
              "Set-Content -LiteralPath '%s\\General items\\Other item.txt' -Value 'Other item'\n" % (WORK, WORK))
    before = copy_into(ctx, SAMPLE, ["General items"], WORK)
    win = operation_window(ctx, before)
    time.sleep(SETTLE)
    names = [k.name for k in descendants(ctx, win, 6)]
    # One element can hold every paragraph (2026-11-20): keep each text once.
    texts = list(dict.fromkeys(n for n in names if n and any(key in n for key in MERGE_LINES)))
    if not all(any(key in t for t in texts) for key in MERGE_LINES):
        raise core.Incomplete(f"the folder copy showed no folder merge prompt: {names[:40]}")
    _merge_text[:] = texts
    _root["el"] = win


def close_folder_merge(ctx):
    try:
        close_operation(ctx)
    finally:
        ctx.powershell(HIDE_MERGE)


HOLDER = "C:\\Users\\ref\\AppData\\Local\\Temp\\inventory-hold.ps1"   # its name is the marker REMOVE_WORK ends


def hold_open(path):
    """A process holding a file open with no sharing: what makes "File In Use"
    show. Refused when the file is not held after all."""
    return hold(ps_quote(path), "None") + (
        "try { [IO.File]::Open(%s, 'Open', 'Read', 'ReadWrite').Close(); Write-Output 'the file is not held'; exit 1 }"
        " catch { }\n"
        "exit 0\n" % ps_quote(path))          # -Command exits 1 after a caught error otherwise


def hold(path_expr, share):
    """Start the holder on the file a PowerShell expression names. It runs as a
    script file; a script passed on the command line lost its lines to
    Start-Process and held nothing (2026-11-20)."""
    return ("Set-Content -LiteralPath '%s' -Encoding ASCII -Value @'\n"
            "param($Path, $Share)\n"
            "$h = [IO.File]::Open($Path, 'Open', 'Read', $Share)\n"
            "Start-Sleep 900\n"
            "'@\n"
            "Start-Process powershell -WindowStyle Hidden -ArgumentList "
            "'-NoProfile','-File','%s','-Path',('\"' + %s + '\"'),'-Share','%s'\n"
            "Start-Sleep 3\n" % (HOLDER, HOLDER, path_expr, share))


def open_in_use(ctx):
    work(ctx, "Set-Content -LiteralPath '%s\\Held file.txt' -Value 'Sample content' -Encoding ASCII\n" % WORK
         + hold_open(WORK + "\\Held file.txt"))
    start(ctx, WORK)
    ctx.click(target(ctx, "Held file"))
    before = top_windows(ctx)
    ctx.keys("delete")
    _root["el"] = operation_window(ctx, before)


PROTECTED = "C:\\Program Files\\Inventory sample"


def open_access_denied(ctx):
    """A file in Program Files deleted from File Explorer, which runs without
    elevation: "File Access Denied" with its administrator (Continue) button. The
    fixture runs elevated, so it can make the file."""
    ctx.powershell("New-Item -ItemType Directory -Path '%s' -Force | Out-Null\n"
                   "Set-Content -LiteralPath '%s\\Protected file.txt' -Value 'Sample content' -Encoding ASCII\n"
                   "exit 0\n" % (PROTECTED, PROTECTED))
    start(ctx, PROTECTED)
    ctx.click(target(ctx, "Protected file"))
    before = top_windows(ctx)
    ctx.keys("delete")
    _root["el"] = operation_window(ctx, before)


def close_access_denied(ctx):
    close_operation(ctx)
    ctx.powershell("Remove-Item -LiteralPath '%s' -Recurse -Force -ErrorAction SilentlyContinue\nexit 0\n" % PROTECTED)


def open_delete(permanent):
    """Shift+Delete always asks; Delete asks once the confirmation setting is on."""
    def opener(ctx):
        work(ctx, "Set-Content -LiteralPath '%s\\Delete me.txt' -Value 'Sample content' -Encoding ASCII\n" % WORK
             + ("" if permanent else CONFIRM_RECYCLE))
        start(ctx, WORK)
        ctx.click(target(ctx, "Delete me"))
        before = top_windows(ctx)
        ctx.keys("shift+delete" if permanent else "delete")
        _root["el"] = appeared(ctx, before)
    return opener


# The Recycle Bin's "Display delete confirmation dialog" is bit 2 of ShellState's fifth byte.
CONFIRM_RECYCLE = r"""
$k = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer'
$s = (Get-ItemProperty -LiteralPath $k -Name ShellState).ShellState
$s[4] = $s[4] -band 0xFB
Set-ItemProperty -LiteralPath $k -Name ShellState -Value $s
"""
# A Recycle Bin of 1 MB on every volume, and a file larger than it.
SMALL_BIN = r"""
foreach ($v in Get-ChildItem 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\BitBucket\Volume') {
    Set-ItemProperty -LiteralPath $v.PSPath -Name MaxCapacity -Value 1 -Type DWord
    Set-ItemProperty -LiteralPath $v.PSPath -Name NukeOnDelete -Value 0 -Type DWord
}
fsutil file createnew '%s\Delete me.txt' 4194304 | Out-Null
""" % WORK


def open_too_large(ctx):
    work(ctx, SMALL_BIN)
    start(ctx, WORK)
    ctx.click(target(ctx, "Delete me"))
    before = top_windows(ctx)
    ctx.keys("delete")
    _root["el"] = appeared(ctx, before)


OPERATIONS = [
    ("copy-progress", "File operation: compact", open_copy_progress(False)),
    ("copy-progress-detailed", "File operation: more details", open_copy_progress(True)),
    ("copy-progress-stacked", "File operation: two at once", open_copy_progress(True, stacked=True)),
    ("replace-or-skip", "Replace or Skip Files", open_conflict()),
    ("file-conflict", "File Conflict: the compare chooser", open_conflict(compare=True)),
    ("folder-merge", "A folder copied onto a folder of its name", open_folder_merge, close_folder_merge),
    ("file-in-use", "File In Use", open_in_use),
    ("access-denied", "File Access Denied", open_access_denied, close_access_denied),
    ("delete-to-recycle-bin", "Delete File: to the Recycle Bin", open_delete(False)),
    ("delete-permanently", "Delete File: permanently", open_delete(True)),
    ("too-large-for-recycle-bin", "File Too Large for Recycle Bin", open_too_large),
]
for slug, label, opener, *close in OPERATIONS:
    SCENES.append(dict(slug=slug, label=label, kind="dialog", open=opener, root=opened,
                       close=close[0] if close else close_operation, expand=False))


# A 16 MB volume to run out of space on: a virtual disk diskpart makes (the
# capture runs as the reference account, an administrator), detached on close.
SMALL_DISK = "C:\\Users\\ref\\AppData\\Local\\Temp\\inventory-small.vhdx"
SMALL_LETTER = "S"
# The letter must be free before the disk is made, and the volume on it must be
# the disk's own: a letter another volume holds would take the copy and the format.
MAKE_SMALL = r"""
if (Test-Path '%s:\') { throw '%s: is in use: the small volume needs that letter free' }
$script = Join-Path $env:TEMP 'inventory-small.txt'
@"
create vdisk file="%s" maximum=16 type=fixed
select vdisk file="%s"
attach vdisk
create partition primary
format fs=fat quick label=Small
assign letter=%s
"@ | Set-Content -LiteralPath $script -Encoding ASCII
$out = diskpart /s $script
if ($LASTEXITCODE) { throw ('diskpart could not make the small volume (it needs an administrator): ' + ($out -join ' ')) }
$part = Get-DiskImage -ImagePath '%s' | Get-Disk | Get-Partition | Where-Object DriveLetter -eq '%s'
if (-not $part) { throw '%s: is not the small volume' }
fsutil file createnew '%s\Large file.bin' 33554432 | Out-Null
""" % (SMALL_LETTER, SMALL_LETTER, SMALL_DISK, SMALL_DISK, SMALL_LETTER, SMALL_DISK, SMALL_LETTER, SMALL_LETTER, WORK)
REMOVE_SMALL = r"""
if (Test-Path -LiteralPath '%s') {
    $script = Join-Path $env:TEMP 'inventory-small-off.txt'
    "select vdisk file=`"%s`"`r`ndetach vdisk" | Set-Content -LiteralPath $script -Encoding ASCII
    $out = diskpart /s $script
    if ($LASTEXITCODE) { Write-Output ('diskpart could not detach the small volume: ' + ($out -join ' ')); exit 1 }
    Remove-Item -LiteralPath '%s' -Force
}
exit 0
""" % (SMALL_DISK, SMALL_DISK, SMALL_DISK)


def open_no_space(ctx):
    work(ctx, MAKE_SMALL)
    before = copy_into(ctx, WORK, ["Large file.bin"], SMALL_LETTER + ":\\")
    _root["el"] = operation_window(ctx, before)


def close_no_space(ctx):
    close_operation(ctx)
    ctx.powershell(REMOVE_SMALL)


SCENES.append(dict(slug="not-enough-space", label="Not enough space", kind="dialog", open=open_no_space, root=opened,
                   close=close_no_space, expand=False))

def METRICS(records):
    return view_metrics(records) + merge_message(records)


def merge_message(records):
    """Confirm Folder Replace's message, read by the opener."""
    if not any(r["id"] == f"win11.{SURFACE}.folder-merge" for r in records):
        return []
    text = " ".join(" ".join(t.translate(DIRECTION_MARKS).split()) for t in _merge_text)
    if not all(key in text for key in MERGE_LINES):
        raise ValueError(f"the folder merge prompt's message was not read: {text!r}")
    text = WHEN.sub("(the folder's creation time)", text)
    if re.search(r"\d", text):
        raise ValueError(f"the folder merge message still holds a number: {text!r}")
    return [{"id": "folder-merge.message", "parent": "folder-merge", "kind": "state", "label": "Folder merge message",
             "order": 900, "behaviour": text}]


def view_metrics(records):
    """The View tab's advanced settings the walk could not reach: each entry of the
    tree is one fixed setting (a radio choice under one is a control), placed where
    the walk would have placed it. The rows the walk did see must sit at the ids
    computed here, or the scheme is wrong and nothing is written."""
    p = f"win11.{SURFACE}."
    trees = [r for r in records if r["id"].startswith(p + "folder-options-view.") and r.get("controlType") == "tree"]
    if len(trees) != 1:
        raise ValueError(f"the View tab holds {len(trees)} trees, not one")
    if not _view_tree:
        raise ValueError("the View tab's advanced settings tree was not read")
    tree = trees[0]["id"][len(p):]
    walked = {r["id"][len(p):]: r for r in records}
    order = {}
    for r in records:
        if r.get("parent", "").startswith(p + tree):
            key = r["parent"][len(p):]
            order[key] = max(order.get(key, -1), r["order"])
    out, seen = [], set()
    for names, control_type in _view_tree:
        rid = ".".join([tree] + [core.slug(n) for n in names])
        parent = rid.rsplit(".", 1)[0]
        if rid in seen:
            raise ValueError(f"two entries of the View tab's tree share the id {rid}")
        seen.add(rid)
        if rid in walked:
            continue
        if parent != tree and parent not in walked and parent not in seen:
            raise ValueError(f"{rid}: its parent entry was neither walked nor read")
        order[parent] = order.get(parent, -1) + 1
        item = {"id": rid, "parent": parent, "label": names[-1], "order": order[parent],
                "controlType": control_type.lower()}
        item["kind"] = "setting" if control_type == "TreeItem" else "control"
        out.append(item)
    on_screen = [r for r in walked if r.startswith(tree + ".") and walked[r]["kind"] == "setting"]
    missing = sorted(set(on_screen) - seen)
    if missing:
        raise ValueError(f"walked entries not at their computed ids: {missing[:3]}")
    return out


ROOT = "dialogs"
HAND = [
    {"id": ROOT, "parent": "frame", "kind": "group", "label": "Dialogs", "order": 600,
     "behaviour": "The dialogs File Explorer opens: Properties sheets, Folder Options, file operations, "
                  "conflicts and errors, and the wizards."},
    {"id": f"{ROOT}.taskbar-progress", "parent": ROOT, "kind": "state", "label": "Taskbar progress", "order": 0,
     "behaviour": "While a file operation runs, the File Explorer button on the taskbar shows its progress, in "
                  "the states ITaskbarList3 defines: normal (green), paused (yellow), error (red). Not read from "
                  "the reference: UI Automation does not expose a taskbar button's progress."},
    {"id": f"{ROOT}.long-paths", "parent": ROOT, "kind": "state", "label": "Long paths", "order": 2,
     "behaviour": "A copy into a folder whose path passes 260 characters completes with no message: File Explorer "
                  "wrote a 328-character path with LongPathsEnabled off (reference build, 2026-11-20). The "
                  "Destination Path Too Long dialog did not show."},
    {"id": f"{ROOT}.eject-in-use", "parent": ROOT, "kind": "state", "label": "Eject while in use", "order": 1,
     "behaviour": "Eject on the DVD drive while a file on its disc is held open showed no dialog and left the disc "
                  "loaded (the reference VM's virtual drive, 2026-11-20)."},
]
