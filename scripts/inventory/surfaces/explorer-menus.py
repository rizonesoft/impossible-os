"""Surface module: File Explorer's context menus and the menus behind them.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 12 (the
File Explorer Context Menus and Commands item). Writes
docs/design/inventory/explorer/menus.json:

    python3 scripts/inventory/capture.py --surface explorer --file menus --module explorer-menus --prepare explorer:sample

The window is the frame module's (shared/explorer_window.py), on the fixture's
sample tree (fixtures/explorer.ps1). For each trigger the scenes walk the modern
menu (a Flyout pane in its own popup window: an icon row and a list) and the
classic menu behind "Show more options" (a Win32 #32768 menu). The classic
menus are also read through their own menu handle, which UI Automation does not
show: each entry's access key, its shortcut and whether it is the default
(bold) entry. That read is the verb table of METRICS.

The reference machine has no removable USB drive: its DVD drive with the
installation disc in it stands in, a removable medium with Eject.
"""
import ctypes
import json
import time

import capture_core as core
from explorer_window import (
    SAMPLE, SETTLE, _root, close_all, close_menu, command_bar, drag, fresh, in_bar, items, opened, overflow, user32)

SURFACE = "explorer"
ROOT = "menus"
# Among the frame record's children: its parts run from 0, its metrics from
# 200, its known folders are 300 and the views 400.
MENUS_ORDER = 500
# Share with lists the machine's installed share targets, and fills in after it
# opens: Phone Link was there on one run and missing from three menus on the next
# (2026-11-21). Its rows are the machine's data; the submenu is recorded, not them.
ENVIRONMENT_LISTS = [{"automationId": "Windows.ModernShareFlyout"}]
VALUES = []
IDENTITY = []
# A modern menu's icon row is a command bar and its list an overflow popup;
# both report offscreen while their entries are on screen, so the walk reads through them.
HOSTS = [{"className": "ApplicationBar"}, {"automationId": "OverflowPopup"}]
LABELS = [({"controlType": "Image", "name": ""}, "Icon")]

THIS_PC = "shell:MyComputerFolder"
LIBRARIES = "shell:Libraries"
BIN = "shell:RecycleBinFolder"
EMPTY = "Empty folder"
# The triggers: (slug, label, the folder it is in, the item right-clicked).
# None right-clicks the folder's background; a list selects several first.
TRIGGERS = [
    ("document", "A document", SAMPLE, "Sample document"),
    ("image", "An image", SAMPLE, "Sample image"),
    ("archive", "A compressed folder", SAMPLE, "Sample archive"),
    ("program", "A program", SAMPLE, "Sample program"),
    ("shortcut", "A shortcut", SAMPLE, "Sample shortcut"),
    ("folder", "A folder", SAMPLE, "General items"),
    ("background", "The folder background", SAMPLE, None),
    ("several", "Several items", SAMPLE, ["Sample document", "Notes"]),
    ("drive", "A drive", THIS_PC, "Windows (C:)"),
    ("removable", "A removable drive", THIS_PC, {"namePrefix": "DVD Drive (D:)"}),
    ("navigation", "A navigation pane entry", SAMPLE, "navigation"),
    ("library", "A library", LIBRARIES, "Sample library"),
    ("recycled", "A Recycle Bin item", BIN, "Sample deleted file"),
]
# The command bar's See more menu where it gains extras: (slug, label, folder, the item selected first).
SEE_MORE = [
    ("this-pc", "This PC", THIS_PC, None),
    ("drive", "A drive selected", THIS_PC, "Windows (C:)"),
    ("removable", "A removable drive selected", THIS_PC, {"namePrefix": "DVD Drive (D:)"}),
    ("recycle-bin", "The Recycle Bin", BIN, None),
    ("archive", "Inside a compressed folder", SAMPLE + "\\Sample archive.zip", None),
]
# A disc image for Mount: built in a folder of its own with IMAPI2's file
# system image (part of Windows), selected, and removed with the folder on close.
DISC_DIR = "C:\\Users\\ref\\AppData\\Local\\Temp\\Inventory disc"
DISC = "Sample disc"
MAKE_DISC = r"""
$dir = '%s'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$src = Join-Path $dir 'content.txt'
Set-Content -LiteralPath $src -Value 'Sample content' -Encoding ASCII
$fs = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$fs.FileSystemsToCreate = 3
$fs.VolumeName = 'SAMPLE'
$fs.Root.AddTree($src, $false)
$stream = $fs.CreateResultImage().ImageStream
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Runtime.InteropServices; using System.Runtime.InteropServices.ComTypes;
public static class DiscWriter {
    public static void Save(object o, string path) {
        IStream s = (IStream)o; byte[] buf = new byte[65536]; IntPtr n = Marshal.AllocHGlobal(4);
        try { using (FileStream f = File.Create(path)) { while (true) {
            s.Read(buf, buf.Length, n); int got = Marshal.ReadInt32(n); if (got == 0) break; f.Write(buf, 0, got); } } }
        finally { Marshal.FreeHGlobal(n); }
    }
}
'@
[DiscWriter]::Save($stream, (Join-Path $dir '%s.iso'))
Remove-Item -LiteralPath $src -Force
if (-not (Test-Path -LiteralPath (Join-Path $dir '%s.iso'))) { throw 'the disc image was not written' }
""" % (DISC_DIR, DISC, DISC)
REMOVE_DISC = "Remove-Item -LiteralPath '%s' -Recurse -Force -ErrorAction Stop" % DISC_DIR
# The registered verbs of the classes the shell-commands baseline reads, with
# the values that decide where each shows: its label (MUIVerb resolved, else the
# key's default value), Extended (only with Shift), ProgrammaticAccessOnly
# (never in a menu), LegacyDisable, HideBasedOnVelocityId (a feature flag) and AppliesTo.
VERB_CLASSES = ["*", "Directory", "Directory\\Background", "Drive"]
READ_VERBS = r"""
Add-Type -Namespace Inv -Name Str -MemberDefinition @'
[DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
public static extern int SHLoadIndirectString(string src, System.Text.StringBuilder buf, int cch, IntPtr r);
'@
function Label($v) {
    if (-not $v) { return $null }
    if ($v -notlike '@*') { return [string]$v }
    $b = New-Object System.Text.StringBuilder 512
    if ([Inv.Str]::SHLoadIndirectString($v, $b, 512, [IntPtr]::Zero) -eq 0) { return $b.ToString() }
    return $null
}
# PowerShell 5.1 passes a JSON array down a pipeline as one object; the variable holds its items.
$classes = ConvertFrom-Json '%s'
$out = foreach ($c in $classes) {
    $key = "Registry::HKEY_CLASSES_ROOT\$c\shell"
    if (-not (Test-Path -LiteralPath $key)) { continue }
    foreach ($k in Get-ChildItem -LiteralPath $key) {
        $p = Get-ItemProperty -LiteralPath $k.PSPath
        $names = @($p.PSObject.Properties.Name)
        [pscustomobject]@{
            class = $c; verb = $k.PSChildName
            label = (Label $p.MUIVerb); default = (Label $p.'(default)')
            extended = $names -contains 'Extended'; programmatic = $names -contains 'ProgrammaticAccessOnly'
            legacyDisable = $names -contains 'LegacyDisable'; velocity = [string]$p.HideBasedOnVelocityId
            appliesTo = [string]$p.AppliesTo
        }
    }
}
@($out) | ConvertTo-Json -Compress -Depth 3
""" % json.dumps(VERB_CLASSES).replace("'", "''")
# Extended verbs show only when Shift is held: the classic menu then opens at once.
EXTENDED = [("background", "The folder background"), ("folder", "A folder"), ("drive", "A drive")]
# The modern menu's icon row, by the entries' automation ids, in its order.
ICON_ROW = [("cut", "Cut"), ("copy", "Copy"), ("paste", "Paste"), ("rename", "Rename"),
            ("windows-modernshare", "Share"), ("delete", "Delete")]
ROW_HEIGHT = 48                          # an icon row button is this tall; a list entry is 32
NAV_TREE = {"automationId": "100", "controlType": "Tree"}
NAV_ENTRY = "Documents (pinned)"         # a Quick access pin: its name carries "(pinned)" (frame.json)
MENU_CLASS = "#32768"
MENU_WAIT = 15
SUBMENU_WAIT = 5                         # s for a classic submenu to open

# Win32 menu reads (winuser.h).
MN_GETHMENU = 0x01E1
MF_BYPOSITION = 0x0400
MF_GRAYED, MF_DISABLED = 0x0001, 0x0002
MF_POPUP, MF_SEPARATOR, MF_DEFAULT = 0x0010, 0x0800, 0x1000
MENU_TEXT_MAX = 256
SEND_TIMEOUT_MS = 5000
SMTO_ABORTIFHUNG = 0x0002

_m = {"classic": {}}                     # read by the openers, published by METRICS

if user32:
    from ctypes import wintypes
    _H = ctypes.c_void_p
    user32.SendMessageTimeoutW.restype = ctypes.c_size_t
    user32.SendMessageTimeoutW.argtypes = [_H, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t, wintypes.UINT,
                                          wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
    user32.GetMenuItemCount.argtypes = [_H]
    user32.GetMenuState.restype = wintypes.UINT
    user32.GetMenuState.argtypes = [_H, wintypes.UINT, wintypes.UINT]
    user32.FindWindowExW.restype = _H
    user32.FindWindowExW.argtypes = [_H, _H, wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.IsWindowVisible.argtypes = [_H]
    user32.GetMenuStringW.argtypes = [_H, wintypes.UINT, wintypes.LPWSTR, ctypes.c_int, wintypes.UINT]


# ------------------------------------------------------------ the menus --

def modern(ctx):
    """The modern menu: a Flyout pane in its own popup window (the desktop's
    menus are the same, desktop.py)."""
    return ctx.find(className="Flyout", controlType="Pane", depth=6, timeout=MENU_WAIT)


def classic_windows(ctx):
    """The classic menus on screen, newest first (top-level z-order)."""
    return [w for w in ctx.live.children(ctx.live.uia.GetRootElement(), ctx.live.uia.CreateTrueCondition())
            if w.class_name == MENU_CLASS and not w.offscreen]


def classic_handles():
    """The visible classic menu windows, from Win32 itself: a submenu the shell
    opens is not always listed under the UI Automation root (Send to, 2026-11-20)."""
    out, h = [], None
    for _ in range(64):
        h = user32.FindWindowExW(None, h, MENU_CLASS, None)
        if not h:
            return out
        if user32.IsWindowVisible(h):
            out.append(h)
    raise core.Incomplete("more than 64 classic menu windows")


def classic(ctx):
    return ctx.find_window(className=MENU_CLASS, timeout=MENU_WAIT)


def target(ctx, what):
    """The element right-clicked for a trigger."""
    if what == "navigation":
        tree = in_bar(ctx, **NAV_TREE)
        return ctx.find(within=tree, controlType="TreeItem", name=NAV_ENTRY, depth=6)
    spec = what if isinstance(what, dict) else {"name": what}
    # This PC's list holds its drives inside groups, a level below the rows of a folder.
    return ctx.find(within=items(ctx), controlType="ListItem", depth=4, **spec)


def right_click(ctx, folder, what):
    fresh(ctx, folder)
    if what is None:
        x, y, w, h = items(ctx).bounds
        ctx.click_at(x + w // 2, y + h - 20, button="right")
    elif isinstance(what, list):
        ctx.click(target(ctx, what[0]))
        ctx.hold("ctrl")
        try:
            for name in what[1:]:
                ctx.click(target(ctx, name))
        finally:
            ctx.release("ctrl")
        time.sleep(0.5)
        ctx.click(target(ctx, what[-1]), button="right")
    else:
        ctx.click(target(ctx, what), button="right")
    time.sleep(SETTLE)


def open_modern(folder, what):
    def opener(ctx):
        right_click(ctx, folder, what)
        _root["el"] = modern(ctx)
    return opener


def read_verbs(ctx):
    """The registered verbs, read once per capture."""
    if "verbs" in _m:
        return
    rows = json.loads(ctx.powershell(READ_VERBS) or "[]")
    if not rows:
        raise core.Incomplete("no shell verb was read from the registry")
    _m["verbs"] = rows


def open_classic(slug, folder, what):
    def opener(ctx):
        read_verbs(ctx)
        right_click(ctx, folder, what)
        # A synthesized click on this entry is ignored; Invoke reaches it (desktop.py, 2026-11-16).
        ctx.invoke(ctx.find(within=modern(ctx), automationId="expandtoclassic", depth=4))
        w = classic(ctx)
        _m["classic"][slug] = read_menu(w)
        _root["el"] = w
    return opener


def open_extended(slug, folder, what):
    def opener(ctx):
        ctx.hold("shift")
        try:
            right_click(ctx, folder, what)
        finally:
            ctx.release("shift")
        w = classic(ctx)
        _m["classic"][f"extended-{slug}"] = read_menu(w)
        _root["el"] = w
    return opener


def open_send_to(ctx):
    """The document's classic menu, then a click on Send to: the submenu opens
    as a second classic menu. (Its access key N is shared with another entry
    on that menu, so the key only moves the selection, 2026-11-20.)"""
    open_classic("send-to-parent", SAMPLE, "Sample document")(ctx)
    main = _root["el"]
    entry = ctx.find(within=main, controlType="MenuItem", name="Send to", depth=3)
    ctx.click(entry)
    first = main.el.CurrentNativeWindowHandle
    for attempt in ("click", "right"):
        if attempt == "right":
            ctx.keys("right")             # the key that opens a selected entry's submenu
        deadline = time.monotonic() + SUBMENU_WAIT
        while time.monotonic() < deadline:
            sub = [h for h in classic_handles() if h != first]
            if sub:
                _m["classic"]["send-to"] = read_menu(sub[0])
                _root["el"] = ctx.live.wrap(ctx.live.uia.ElementFromHandle(sub[0]))
                return
            time.sleep(0.3)
    raise core.Incomplete(f"Send to opened no submenu: menu at {main.bounds}, entry at {entry.bounds}, "
                          f"classic menu windows {len(classic_handles())}")


def open_drop_menu(ctx):
    """Drag the document onto the empty folder with the right button held: the
    drop menu (copy here, move here, create shortcuts here, cancel)."""
    fresh(ctx, SAMPLE)
    a, b = target(ctx, "Sample document").bounds, target(ctx, EMPTY).bounds
    drag(ctx, (a[0] + a[2] // 2, a[1] + a[3] // 2), (b[0] + b[2] // 2, b[1] + b[3] // 2), button="right")
    w = classic(ctx)
    _m["classic"]["drop"] = read_menu(w)
    _root["el"] = w


def open_see_more(folder, what):
    def opener(ctx):
        fresh(ctx, folder)
        if what is not None:
            ctx.click(target(ctx, what))
            time.sleep(SETTLE)
        ctx.click(in_bar(ctx, automationId="MoreButton"))
        _root["el"] = overflow(ctx)
    return opener


def open_disc(ctx):
    ctx.powershell(MAKE_DISC)
    open_see_more(DISC_DIR, DISC)(ctx)


def close_disc(ctx):
    close_menu(ctx)
    close_all(ctx)
    ctx.powershell(REMOVE_DISC)


def with_disc(opener):
    """An opener that first writes the disc image (Mount shows only on one)."""
    def o(ctx):
        ctx.powershell(MAKE_DISC)
        opener(ctx)
    return o


def removing_disc(close):
    def c(ctx):
        close(ctx)
        close_all(ctx)
        ctx.powershell(REMOVE_DISC)
    return c


def disc_selected(ctx):
    """The disc image selected: the command bar gains Mount (2026-11-20: it is not in See more)."""
    fresh(ctx, DISC_DIR)
    ctx.click(target(ctx, DISC))
    time.sleep(SETTLE)
    _root["el"] = command_bar(ctx)


def close_modern(ctx):
    ctx.keys("esc")
    time.sleep(SETTLE)


def close_classic(ctx):
    for _ in range(3):
        if not classic_windows(ctx):
            break
        ctx.keys("esc")
        time.sleep(0.5)
    ctx.gone(className=MENU_CLASS)
    close_all(ctx)


# --------------------------------------------------- the classic menu read --

def read_menu(w):
    """Each entry of a classic menu (a live element or a window handle), read
    through its menu handle: the text with its access key mark and shortcut, and its state."""
    if not user32:
        raise core.Incomplete("the menu handle can only be read on Windows")
    hwnd = w if isinstance(w, int) else w.el.CurrentNativeWindowHandle
    res = ctypes.c_size_t()
    if not user32.SendMessageTimeoutW(hwnd, MN_GETHMENU, 0, 0, SMTO_ABORTIFHUNG, SEND_TIMEOUT_MS, ctypes.byref(res)):
        raise core.Incomplete("the classic menu did not hand over its menu")
    hmenu = res.value
    n = user32.GetMenuItemCount(hmenu)
    if n <= 0:
        raise core.Incomplete(f"the classic menu reports {n} entries")
    out = []
    buf = ctypes.create_unicode_buffer(MENU_TEXT_MAX)
    for i in range(n):
        state = user32.GetMenuState(hmenu, i, MF_BYPOSITION)
        if state == 0xFFFFFFFF:
            raise core.Incomplete(f"the classic menu's entry {i} could not be read")
        if state & MF_SEPARATOR:
            out.append({"separator": True})
            continue
        user32.GetMenuStringW(hmenu, i, buf, MENU_TEXT_MAX, MF_BYPOSITION)
        out.append({"text": buf.value, "default": bool(state & MF_DEFAULT), "submenu": bool(state & MF_POPUP),
                    "enabled": not state & (MF_GRAYED | MF_DISABLED)})
    return out


def parse_entry(text):
    """(label, access key, shortcut) of a classic menu text: "&Open\\tCtrl+O".
    One ampersand marks the access key; two stand for a literal one."""
    body, _, shortcut = text.partition("\t")
    label, access, i = [], None, 0
    while i < len(body):
        c = body[i]
        if c == "&" and i + 1 < len(body):
            if body[i + 1] == "&":
                label.append("&")
            else:
                access = access or body[i + 1].upper()
                label.append(body[i + 1])
            i += 2
            continue
        label.append(c)
        i += 1
    return "".join(label).strip(), access, shortcut.strip() or None


# ---------------------------------------------------------------- scenes --

SCENES = [dict(slug=f"{slug}-menu", label=f"{label}: context menu", kind="contextMenu", open=open_modern(folder, what),
               root=opened, close=close_modern) for slug, label, folder, what in TRIGGERS]
SCENES += [dict(slug=f"classic-{slug}-menu", label=f"{label}: classic context menu", kind="contextMenu",
                open=open_classic(slug, folder, what), root=opened, close=close_classic, expand=False)
           for slug, label, folder, what in TRIGGERS]
SCENES += [dict(slug=f"extended-{slug}-menu", label=f"{label}: classic context menu with Shift", kind="contextMenu",
                open=open_extended(slug, folder, what), root=opened, close=close_classic, expand=False)
           for slug, label in EXTENDED for s, _, folder, what in TRIGGERS if s == slug]
SCENES += [dict(slug=f"see-more-{slug}", label=f"See more menu: {label}", kind="menu", open=open_see_more(folder, what),
                root=opened, close=close_menu) for slug, label, folder, what in SEE_MORE]
SCENES += [dict(slug="see-more-disc-image", label="See more menu: A disc image selected", kind="menu", open=open_disc,
                root=opened, close=close_disc)]
SCENES += [dict(slug="disc-image-menu", label="A disc image: context menu", kind="contextMenu",
                open=with_disc(open_modern(DISC_DIR, DISC)), root=opened, close=removing_disc(close_modern)),
           dict(slug="classic-disc-image-menu", label="A disc image: classic context menu", kind="contextMenu",
                open=with_disc(open_classic("disc-image", DISC_DIR, DISC)), root=opened,
                close=removing_disc(close_classic), expand=False),
           dict(slug="disc-image-command-bar", label="Command bar with a disc image selected", kind="state",
                open=with_disc(disc_selected), root=opened, close=removing_disc(lambda ctx: None), expand=False)]
SCENES += [dict(slug="send-to", label="Send to", kind="menu", open=open_send_to, root=opened, close=close_classic,
                expand=False),
           dict(slug="drop-menu", label="Right-drag drop menu", kind="contextMenu", open=open_drop_menu, root=opened,
                close=close_classic, expand=False)]


# --------------------------------------------------------------- records --

def verb_records(classic):
    """The verb table: one record per entry label across every classic menu,
    with its access key, shortcut, the triggers that show it and those where it
    is the default (bold) entry."""
    if not classic:
        raise ValueError("no classic menu was read")
    verbs = {}
    for menu, entries in classic.items():
        if menu == "send-to-parent":
            continue
        if not entries:
            raise ValueError(f"the classic menu {menu} has no entries")
        for e in entries:
            if e.get("separator"):
                continue
            label, access, shortcut = parse_entry(e["text"])
            if not label:
                continue
            # One verb, one record: "Open with" and "Open with..." are the same entry (2026-11-20).
            v = verbs.setdefault(plain(e["text"]), {"label": label, "forms": set(), "access": set(),
                                                    "shortcut": set(), "on": [], "default": []})
            v["forms"].add(label)
            if access:
                v["access"].add(access)
            if shortcut:
                v["shortcut"].add(shortcut)
            v["on"].append(menu)
            if e["default"]:
                v["default"].append(menu)
    out = [{"id": f"{ROOT}.verbs", "parent": ROOT, "kind": "group", "label": "Verb table", "order": 0,
            "behaviour": "Every entry of the classic menus read through their menu handles: the access key, the "
                         "shortcut, the menus that show it and those where it is the default (bold) entry."}]
    seen = set()
    for i, (key, v) in enumerate(sorted(verbs.items())):
        label = v["label"]
        slug = core.slug(key)
        if not slug or slug in seen:
            raise ValueError(f"the verb {label!r} makes no id or a repeated one")
        seen.add(slug)
        parts = [f"Access key {'/'.join(sorted(v['access']))}" if v["access"] else "No access key",
                 f"shown on {', '.join(sorted(set(v['on'])))}"]
        if len(v["forms"]) > 1:
            parts.append(f"written {' and '.join(repr(x) for x in sorted(v['forms']))} on different menus")
        if v["default"]:
            parts.append(f"the default (bold) entry on {', '.join(sorted(set(v['default'])))}")
        r = {"id": f"{ROOT}.verbs.{slug}", "parent": f"{ROOT}.verbs", "kind": "menuItem", "label": label,
             "order": i, "behaviour": "; ".join(parts) + "."}
        if len(v["shortcut"]) == 1:
            r["shortcut"], r["shortcutScope"] = next(iter(v["shortcut"])), "control"
        elif v["shortcut"]:
            raise ValueError(f"the verb {label!r} shows several shortcuts: {sorted(v['shortcut'])}")
        out.append(r)
    return out


CLASS_SLUGS = {"*": "all-files", "Directory": "folder", "Directory\\Background": "folder-background", "Drive": "drive"}
# The classic menus each registry class's verbs can appear on: a label match on
# another class's menu is another verb of the same name, not this one.
CLASS_MENUS = {
    "*": {"document", "image", "archive", "program", "shortcut", "several", "recycled", "disc-image"},
    "Directory": {"folder", "library", "navigation", "extended-folder"},
    "Directory\\Background": {"background", "extended-background"},
    "Drive": {"drive", "removable", "extended-drive"},
}


def plain(label):
    """A menu label compared across sources: no access key mark, no trailing ellipsis, any case."""
    text = parse_entry(label)[0] if label else ""
    return text.rstrip(".\u2026").strip().lower()


def registered_records(verbs, classic, records=()):
    """One record per registered verb of the baseline's classes, matched to its
    shell-commands row: where the classic menus showed it, or which registry
    value keeps it out of them. A classic menu entry is known by its label only,
    so where two verbs of one class share a label, the modern menus' items (whose
    ids are their verbs) decide which one the menu holds; when they cannot, the
    record says so rather than credit both (review, 2026-11-21: both BitLocker
    verbs, whose conditions exclude each other, were "shown on drive")."""
    if not verbs:
        raise ValueError("the registered verbs were not read")
    # Each trigger's modern menu items by verb: a twin is told apart on a classic
    # trigger only by that trigger's own modern menu (review, 2026-11-21: the
    # drive menu's evidence credited the removable menu too).
    modern = {}
    for r in records:
        parent, _, last = r["id"].rpartition(".")
        if r.get("kind") == "menuItem" and parent.startswith("win11.explorer.") and parent.endswith("-menu"):
            modern.setdefault(parent[len("win11.explorer."):-len("-menu")], set()).add(last)
    sharing = {}
    for v in verbs:
        label = v.get("label") or v.get("default")
        if label:
            sharing.setdefault((v["class"], plain(label)), []).append(v["verb"])
    shown = {}
    for menu, entries in classic.items():
        if menu == "send-to-parent":                # the document's menu again, read to reach Send to
            continue
        for e in entries:
            if not e.get("separator"):
                shown.setdefault(plain(e["text"]), set()).add(menu)
    out = [{"id": f"{ROOT}.registered", "parent": ROOT, "kind": "group", "label": "Registered verbs", "order": 2,
            "behaviour": "The verbs registered under the file, folder, folder background and drive classes, and "
                         "where each shows."}]
    seen = set()
    for i, v in enumerate(sorted(verbs, key=lambda v: (v["class"], v["verb"].lower()))):
        if v["class"] not in CLASS_SLUGS:
            raise ValueError(f"a verb of the unread class {v['class']}")
        rid = f"{ROOT}.registered.{CLASS_SLUGS[v['class']]}.{core.slug(v['verb'])}"
        if rid in seen:
            raise ValueError(f"two verbs make the id {rid}")
        seen.add(rid)
        label = v.get("label") or v.get("default")
        on = sorted(shown.get(plain(label), set()) & CLASS_MENUS[v["class"]]) if label else []
        twins = [t for t in sharing.get((v["class"], plain(label)), []) if t != v["verb"]] if label else []
        unsure = []
        if twins:
            mine = []
            for menu in on:
                held = [t for t in [v["verb"], *twins] if core.slug(t) in modern.get(menu, ())]
                if held == [v["verb"]]:
                    mine.append(menu)
                elif len(held) != 1:
                    unsure.append(menu)
            on = mine
        if unsure:
            text = (f"on {', '.join(unsure)} it shares its label with {', '.join(twins)}, "
                    "and the menus do not say which of them is shown")
            how = f"Shown on {', '.join(on)}; {text}" if on else text[0].upper() + text[1:]
        elif on:
            how = f"Shown on {', '.join(on)}"
            if v.get("extended"):
                how += ", with Shift held only"
        elif v.get("programmatic"):
            how = "Registered for programmatic use only (ProgrammaticAccessOnly): no menu shows it"
        elif v.get("velocity"):
            how = f"Hidden on the reference build by its feature flag (HideBasedOnVelocityId {v['velocity']})"
        elif v.get("legacyDisable"):
            how = "Disabled (LegacyDisable): no menu shows it"
        elif v.get("appliesTo"):
            how = f"Shown only where {v['appliesTo']}; no item read met that condition"
        else:
            how = "Registered, but no menu read showed it: its command handler hides it on this machine"
        if on and v.get("appliesTo"):
            how += f"; it applies only where {v['appliesTo']}"
        out.append({"id": rid, "parent": f"{ROOT}.registered", "kind": "menuItem" if on else "state",
                    "label": label or v["verb"], "order": i, "baselineId": f"shell-commands:verb:{v['class']}/{v['verb']}",
                    "behaviour": how + "."})
    return out


def icon_row_records(records):
    """For each icon row button, the triggers whose modern menu shows it."""
    present = {}
    for r in records:
        parent = r.get("parent") or ""
        b = r.get("bounds") or {}
        if not parent.endswith("-menu") or ".classic-" in parent or ".extended-" in parent:
            continue
        if b.get("height") != ROW_HEIGHT:
            continue
        trigger = parent.rsplit(".", 1)[-1][:-len("-menu")]
        present.setdefault(r["id"].rsplit(".", 1)[-1], set()).add(trigger)
    if not present:
        raise ValueError("no modern menu showed an icon row")
    out = [{"id": f"{ROOT}.icon-row", "parent": ROOT, "kind": "group", "label": "Icon row", "order": 1,
            "behaviour": "The row of buttons at the top of the modern menu; each is present only where it applies."}]
    for i, (seg, label) in enumerate(ICON_ROW):
        on = sorted(present.get(seg, ()))
        out.append({"id": f"{ROOT}.icon-row.{seg}", "parent": f"{ROOT}.icon-row", "kind": "state", "label": label,
                    "order": i, "behaviour": f"Shown on: {', '.join(on)}." if on else "Shown on none of the triggers."})
    return out


def METRICS(records):
    out = [{"id": ROOT, "parent": "frame", "kind": "group", "label": "Context menus", "order": MENUS_ORDER}]
    return (out + verb_records(_m["classic"]) + registered_records(_m.get("verbs"), _m["classic"], records)
            + icon_row_records(records))


HAND = []
