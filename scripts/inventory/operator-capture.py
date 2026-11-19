#!/usr/bin/env python3
"""Capture Settings pages on the operator's own Windows machine and publish them.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 27.

    python3 scripts/inventory/operator-capture.py [--pages bluetooth,camera,graphics,wifi] [--from DIR] [--dry-run]

The reference VM has no Bluetooth radio, camera, discrete GPU or Wi-Fi adapter,
so these pages are captured on the operator's machine, which runs the same
build (reference.json). operator-host.ps1 does the capture there: it gives the
session the profile's theme and accent for the capture only, opens each page
maximized on the monitor with the profile's size and scale, and leaves each
page's UI Automation tree and a screenshot of that monitor. This script turns
the trees into hand records (source hand:<build>: read from the operator's
machine, not by the VM's capture tool), and publishes them with the shots
through capture.publish, all or nothing, into settings/host.json.

What names the operator never leaves the run folder: the account header (name,
email, picture), the machine name, a path under the user's profile, and the
monitor's taskbar (pinned apps, weather) are blanked in each shot, and a label
that holds one of them is rewritten with a placeholder. A run on a machine
whose build is not reference.json's is refused.
"""
import argparse
import functools
from collections import Counter
import importlib.util
import json
import pathlib
import re
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


capture = _load("capture", HERE / "capture.py")
core = capture.core

SURFACE, FILE, PROFILE = "settings", "host", "fhd-100"
PAGES = ("bluetooth", "camera", "graphics", "wifi")
TITLES = {"bluetooth": "Devices", "camera": "Cameras", "graphics": "Graphics", "wifi": "Wi-Fi"}   # the last breadcrumb
ROOT = f"win11.{SURFACE}.hardware"
HOST_SCRIPT = HERE / "operator-host.ps1"
HOST_TIMEOUT = 600                     # deadline: four pages at about 15 s each, plus the theme change and restore
IDENTITY_VERSION = 9   # after the last page's Wi-Fi read, names read losslessly, views with their scroll,
                       # shot bracketed by two tree reads, nothing shown above Settings, a fresh folder
                       # per run, the profile's size, scale, text size and contrast recorded
PENDING = "operator-pending-restore.json"   # beside the run folder; operator-host.ps1 writes and removes it
# One host capture at a time on this machine: beside the pending-restore file
# every checkout shares, so two checkouts cannot both change the theme.
HOST_LOCK = capture.WORK / "operator-host.lock"
RESTORE_TIMEOUT = 300                  # deadline: ten registry values and a broadcast at 1 s a slow window
TASKBAR = 48                           # px: the taskbar strip at the bottom of the monitor at 96 dpi
CONTENT_ID = "ItemsControlScrollViewer"   # the page's content, apart from the navigation pane
ACCOUNT_ID = "UserProfileControlButton"   # the account header: name, email, picture
# Controls a page record keeps (UI Automation control types, raw view).
CONTROL_TYPES = {"Button", "CheckBox", "ComboBox", "Edit", "Hyperlink", "Slider", "RadioButton", "List"}
# Buttons that only repeat their card (the card is recorded as the setting).
CARD_BUTTON_IDS = {"EntityItemButton"}   # a card's own button, named as the card: not a second control
EXPANDER = "ExpanderToggleButton"       # under the same automation ID, the card's "Show more settings": kept
HARDWARE_ID = re.compile(r"\\\\\?\\|#\{|VID_|PID_", re.I)   # a device instance path: this machine's, not the design's
BLANK = (243, 243, 243)                # the light theme's window background


def host_cmd(out, *args):
    """The host script's command line; the paths are resolved here, before any theme change."""
    return ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", capture.winpath(HOST_SCRIPT),
            "-Out", capture.winpath(out), *args]


def run_host(pages):
    out = capture.WORK / f"operator-{time.strftime('%Y%m%d-%H%M%S')}"
    cmd, recover_cmd = host_cmd(out, "-Pages", ",".join(pages)), host_cmd(out, "-Restore")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=HOST_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise core.Incomplete(f"operator-host.ps1 ran past {HOST_TIMEOUT} s; the theme was restored") from None
    finally:
        # A host stopped before its own finally block ran (killed at the deadline,
        # or by a Ctrl-C here) leaves its pending-restore file: replayed now.
        # After a clean run there is none, and this does nothing.
        restore(out, recover_cmd)
    print(r.stdout.strip())
    if r.returncode:
        raise core.Incomplete(f"operator-host.ps1 failed ({r.returncode}): {r.stderr.strip()[:400]}")
    return out


def restore(out, cmd=None):
    """Put the operator's theme and accent back from the pending-restore file an
    interrupted capture left beside `out` (none left is a no-op)."""
    if not (out.parent / PENDING).exists():
        return
    cmd = cmd or host_cmd(out, "-Restore")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=RESTORE_TIMEOUT)
    if r.returncode:
        raise core.Incomplete(f"the operator's theme could not be restored from {out}: {r.stderr.strip()[:400]}")


def host_build():
    r = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                        "$k = Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion';"
                        " \"$($k.CurrentBuild).$($k.UBR)\""], capture_output=True, text=True, timeout=60)
    return r.stdout.strip()


def walk(node, parent=None):
    yield node, parent
    for k in node.get("children") or []:
        yield from walk(k, node)


def find(node, automation_id):
    return next((n for n, _ in walk(node) if n.get("automationId") == automation_id), None)


def shown(text):
    """Text a reader sees: an icon font's glyph (Private Use Area) is not."""
    return any(not c.isspace() and not 0xE000 <= ord(c) <= 0xF8FF for c in text or "")


def personal_strings(tree, host_names):
    """What names the operator: the account header's texts and the machine's names."""
    out = set(s for s in host_names if s)
    head = find(tree, ACCOUNT_ID)
    for n, _ in walk(head) if head else ():
        if n.get("controlType") == "Text" and n.get("name"):
            out.add(n["name"])
    return sorted(out, key=len, reverse=True)


@functools.lru_cache(maxsize=None)
def _pattern(personal):
    # One pass, longest first: a placeholder put in is never matched again.
    return re.compile("|".join(re.escape(s) for s in sorted(personal, key=len, reverse=True)), re.I)


def scrub(text, personal, placeholder="<personal>"):
    personal = tuple(s for s in personal if s)
    return _pattern(personal).sub(placeholder, text) if personal else text


def identity(out):
    """The machine and user the run read, saved by the host at capture time."""
    path = out / "identity.json"
    if not path.is_file():
        raise core.Incomplete(f"{path} is missing: the run cannot be redacted, capture again")
    who = json.loads(path.read_text(encoding="utf-8-sig"))
    if who.get("version") != IDENTITY_VERSION:
        raise core.Incomplete(f"{path} is from an older capture (format {who.get('version')}, this reads "
                              f"{IDENTITY_VERSION}): capture again")
    names = [who.get("computer"), who.get("user"), who.get("profile")]
    if not all(names):
        raise core.Incomplete(f"{path} does not name the machine, the user and the profile path")
    # The Wi-Fi names the host read: each connected interface's SSID and each saved
    # profile. A run from before the host read both cannot say which names to mask.
    nets = []
    for key in ("ssids", "profiles"):
        got = who.get(key)
        if isinstance(got, str):       # ConvertTo-Json writes a one-item list as its item
            got = [got]
        if not isinstance(got, list):
            raise core.Incomplete(f"{path} does not list the machine's Wi-Fi {key}: capture again")
        nets += [n for n in got if n]
    return names + nets


@functools.lru_cache(maxsize=None)
def profile():
    """The capture profile's settings (docs/design/inventory/profiles.json)."""
    return json.loads((capture.INVENTORY / "profiles.json").read_text(encoding="utf-8"))[PROFILE]


def wrong_pages(dumps):
    """Pages whose tree is not the page asked for (its title, its content)."""
    out = {}
    for p, d in dumps.items():
        crumbs = [n.get("name") for n, _ in walk(d["top"])
                  if n.get("className") == "Microsoft.UI.Xaml.Controls.BreadcrumbBarItem" and n.get("name")]
        if not crumbs or crumbs[-1] != TITLES[p] or not find(d["top"], CONTENT_ID):
            out[p] = crumbs[-1] if crumbs else None
        else:
            # Records and shot are published as the profile: read at another size,
            # scale, text size or contrast theme they are not.
            prof = profile()
            seen = {"width": d["monitor"][2], "height": d["monitor"][3], "dpi": d.get("dpi"),
                    "textScale": d.get("textScale"), "highContrast": d.get("highContrast")}
            off = {k: v for k, v in seen.items() if v != prof[k]}
            if off:
                out[p] = ", ".join(f"{k} {v}" for k, v in off.items())
    return out


def rel(bounds, monitor):
    if not bounds:
        return None
    x, y, w, h = bounds
    return {"x": x - monitor[0], "y": y - monitor[1], "width": w, "height": h}


def ancestors(node, ups, stop):
    """The nodes above `node` up to (not including) `stop`, nearest first."""
    out, up = [], ups.get(id(node))
    while up is not None and up is not stop:
        out.append(up)
        up = ups.get(id(up))
    return out


def entries_of(lst, ups):
    """A list's entries: its outermost cards (a card inside an entry is part of it)."""
    card = "NamedContainerAutomationPeer"
    return [k for k, _ in walk(lst) if k is not lst and k.get("className") == card
            and not any(a.get("className") == card for a in ancestors(k, ups, lst))]


def spans(seen):
    """Distinct entries among (top, bottom) page spans: a span overlapping another
    is the same entry seen again (UI Automation clips one an edge of the
    viewport cuts, so its span can be part of the whole)."""
    out = []
    for top, bottom in sorted(seen):
        if out and top < out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], bottom))
        else:
            out.append((top, bottom))
    return out


def entry_title(entry):
    """An entry's title as shown: its first visible text."""
    return next((t["name"] for t, _ in walk(entry) if t.get("controlType") == "Text" and shown(t.get("name"))),
                entry.get("name") or "")


def entry_texts(tree):
    """The texts and images an entry shows of its own (its title, a path, a
    status, an app's icon): this machine's device, app or adapter. A card nested
    in the entry is Windows' own."""
    ups = {id(k): up for k, up in walk(tree)}
    out = []
    for lst, _ in walk(tree):
        if lst.get("controlType") != "List":
            continue
        for e in entries_of(lst, ups):
            own = [t for t, _ in walk(e) if t.get("controlType") in ("Text", "Image") and not any(
                a.get("className") == "NamedContainerAutomationPeer" for a in ancestors(t, ups, e))]
            out += own
            # The entry's leading strip, up to its title: its icon is there, and
            # not every icon is in the tree (an app's often is not).
            title = next((t for t in own if t.get("controlType") == "Text" and t.get("bounds")), None)
            eb = e.get("bounds")
            if title and eb and not e.get("offscreen"):
                inner = next((k["bounds"] for k, _ in walk(e) if k is not e and k.get("bounds")
                              and k.get("className") == "NamedContainerAutomationPeer"), None)
                bottom = inner[1] if inner else eb[1] + eb[3]
                out.append({"bounds": [eb[0], eb[1], title["bounds"][0] - eb[0], bottom - eb[1]]})
    return out


def page_records(page, dump, build, personal):
    """The page's records: the page, then its cards (setting), headings
    (group) and controls, in reading order. Device lists keep their count and
    their first entry as the entry's template: the devices are this machine's."""
    mon = dump["monitor"]
    base = {"surface": SURFACE, "source": f"hand:{build}", "capturedFrom": build, "captureProfile": PROFILE,
            "session": "console"}
    crumbs = [n for n, _ in walk(dump["top"]) if n.get("className") == "Microsoft.UI.Xaml.Controls.BreadcrumbBarItem"
              and n.get("name") and not n.get("offscreen")]
    title = crumbs[-1]["name"] if crumbs else page
    pid = f"{ROOT}.{page}"
    out = [{"id": pid, "parent": ROOT, "kind": "page", "label": title, "order": PAGES.index(page), **base,
            "openedBy": dump["uri"], "disposition": "build",
            "behaviour": f"Settings page {' > '.join(c['name'] for c in crumbs)}, opened by {dump['uri']}."}]
    seen, used = {}, set()
    # A list is known by its id across the two trees; its template is its first
    # entry wherever that is first realized, and its count the most either showed.
    list_rid, list_count, templated, placed = {}, {}, {}, {}
    for tree in (dump["top"], *(dump.get("views") or ())):
        content = find(tree, CONTENT_ID) if tree else None
        if not content:
            continue
        scrolled = tree.get("scrolled") or 0
        skip, lists, ups = set(), {}, {id(k): up for k, up in walk(content)}
        rid_of = {}                       # id(node) -> the record it is, in this view
        template = {}                     # id(entry) recorded as its list's template -> (list, title)
        for n, up in walk(content):
            if id(n) in skip or n is content:
                continue
            ctype, name, aid = n.get("controlType"), n.get("name") or "", n.get("automationId") or ""
            if ctype == "ScrollBar":
                # The content pane's own scroll bar: the frame's, not the page's,
                # and its buttons show only while it is expanded.
                for d, _ in walk(n):
                    skip.add(id(d))
                continue
            if not shown(name):
                name = ""
            in_list = any(id(a) in lists for a in ancestors(n, ups, content))
            if ctype == "List":
                # The entries are this machine's (devices, apps, adapters): the
                # first is kept as the entry's template and the rest are not
                # records, in every tree the page was read in.
                entries = entries_of(n, ups)
                lkey = aid or "List"
                lists[id(n)] = lkey
                # The entries realized differ between the two views: counted by title
                # across both, each title as many times as one view shows it (two
                # monitors of one model are two entries).
                list_count[lkey] = list_count.get(lkey, Counter()) | Counter(entry_title(e) for e in entries)
                # Each entry's place on the page (its screen span plus the scroll):
                # two entries of one title are two, however far apart.
                for e in entries:
                    eb = e.get("bounds")
                    if eb and not e.get("offscreen"):
                        placed.setdefault(lkey, {}).setdefault(entry_title(e), []).append(
                            (eb[1] + scrolled, eb[1] + eb[3] + scrolled))
                def span(e):
                    eb = e.get("bounds")
                    return (eb[1] + scrolled, eb[1] + eb[3] + scrolled) if eb and not e.get("offscreen") else None

                def same_entry(e, known):
                    # The same title at an overlapping place on the page (either
                    # place unknown: the title alone).
                    title, place = known
                    here = span(e)
                    return entry_title(e) == title and (place is None or here is None
                                                        or (here[0] < place[1] and place[0] < here[1]))
                if entries and (lkey not in templated or same_entry(entries[0], templated[lkey])):
                    # The template is the first entry first realized; in a later
                    # view the same entry is it again, so its bounds can be
                    # measured where it is whole.
                    first = entries[0]
                    known = templated.get(lkey)
                    templated[lkey] = (entry_title(first), span(first) or (known[1] if known else None))
                    template[id(first)] = (lkey, templated[lkey][0])
                    entries = entries[1:]
                for k in entries:
                    for d, _ in walk(k):
                        skip.add(id(d))
            # A second automation peer of the control above it (a search box's
            # inner Edit, an expander's inner toggle): the same control, once.
            if (up is not None and up.get("controlType") == ctype and up.get("name") == n.get("name")
                    and up.get("bounds") == n.get("bounds")):
                continue
            kind = None
            if ctype == "Group" and name and n.get("className") == "NamedContainerAutomationPeer":
                kind = "setting"
            elif ctype == "Text" and name and (aid.endswith("GroupTitleTextBlock") or up is content):
                kind = "group"
            elif ctype in CONTROL_TYPES and name and (aid not in CARD_BUTTON_IDS or n.get("className") == EXPANDER):
                kind = "control"
            elif ctype == "List":
                kind = "control"
            if kind is None:
                continue
            if kind == "setting":
                # A card's label is its title as shown; its automation name is a
                # sentence (state, category, battery).
                name = next((k["name"] for k, _ in walk(n) if k.get("controlType") == "Text" and shown(k.get("name"))),
                            name)
            owner_entry = None
            if id(n) in template:
                # Its title is this machine's device, app or adapter name.
                name = "Entry"
            else:
                # A control in the template entry can repeat that name (the Wi-Fi
                # radio is named for its adapter): replaced whole, before any
                # personal string inside it is.
                owner_entry = next((template[id(a)] for a in ancestors(n, ups, content) if id(a) in template), None)
                if owner_entry and owner_entry[1]:
                    name = scrub(name, [owner_entry[1]], "Entry")
            label = scrub(name, personal) if name else "List"
            # A list entry's id names this machine's device or app (a device
            # path, an install path, a package name): not part of the design.
            uia = "" if in_list or HARDWARE_ID.search(aid) else aid
            # A control is one per card (each card's expander is its own) and one
            # per list's template; two automation peers of one button, or one
            # control seen in both views, collapse to one record. The card is
            # keyed by its shown title, the same in both views.
            card = next((a for a in ancestors(n, ups, content) if a.get("className") == "NamedContainerAutomationPeer"
                         and a is not n), None)
            key = (("template", template[id(n)][0]) if id(n) in template
                   else (kind, uia or label, owner_entry[0] if owner_entry else None,
                         entry_title(card) if card is not None and kind == "control" else None))
            b = rel(n.get("bounds"), mon)
            b = b if b and not n.get("offscreen") else None
            if key in seen:
                rid_of[id(n)] = seen[key]["id"]
                # UI Automation clips a control an edge of the viewport cuts, so a
                # control's own size is the largest any view shows: measured there
                # (offscreen or cut in the views before it).
                had = seen[key].get("bounds")
                if b and (not had or b["width"] * b["height"] > had["width"] * had["height"]):
                    seen[key]["bounds"] = b
                continue
            rec = {"kind": kind, "label": label, **base, "controlType": ctype.lower(), "disposition": "build"}
            if uia:
                rec["uiaId"] = uia
            if n.get("className"):
                rec["className"] = n["className"]
            if b:
                rec["bounds"] = b
            if id(n) in template:
                rec["behaviour"] = "One entry of the list, recorded as its template; its title is this machine's."
            slug = core.slug(uia.removeprefix("SystemSettings_") if uia else label)[:60] or kind
            rid, i = f"{pid}.{slug}", 2
            while rid in used:
                rid, i = f"{pid}.{slug}-{i}", i + 1
            used.add(rid)
            if ctype == "List":
                list_rid[lists[id(n)]] = rid
            # Its parent is the nearest record above it: the card, list or
            # template entry it belongs to, else the page.
            owner = next((rid_of[id(a)] for a in ancestors(n, ups, content) if id(a) in rid_of), pid)
            out.append({"id": rid, "parent": owner, "order": len(out) - 1, **rec})
            seen[key] = out[-1]
            rid_of[id(n)] = rid
    for r in out:
        lkey = next((k for k, v in list_rid.items() if v == r["id"]), None)
        if lkey is not None:
            total = sum(max(list_count[lkey][t], len(spans(placed.get(lkey, {}).get(t, ()))))
                        for t in list_count[lkey])
            r["behaviour"] = (f"A list of this machine's entries ({total} seen on the operator's machine); "
                              "the first is recorded as the entry's template.")
    return out


def records(dumps, build, personal):
    out = [surface_record(build)]
    for page in PAGES:
        if page in dumps:
            out += page_records(page, dumps[page], build, personal)
    return out


def surface_record(build):
    return {"id": ROOT, "surface": SURFACE, "kind": "surface", "order": 0, "source": f"hand:{build}",
            "capturedFrom": build, "captureProfile": PROFILE, "session": "console",
            "label": "Settings: pages that need hardware",
            "behaviour": "The Settings window on the pages the reference VM cannot show (no radio, camera, "
                         "discrete GPU or Wi-Fi adapter), captured on the operator's machine at the reference build. "
                         "Its own differences from the profile: three monitors (the shot is the 1920x1080, 96 dpi "
                         "one), the en-ZA number format, and the wallpaper's tint in the window's Mica background; "
                         "the account header, the machine name, the taskbar and each list entry's own texts and icon "
                         "(this machine's devices, apps and adapters) are blanked in the shots. Each page's shot is its "
                         "top; the page is read again at each half-viewport step down to its end, and a control "
                         "not whole in the top view has its bounds from the scrolled view that shows it largest.",
            "disposition": "build"}


def blanks(dump, personal):
    """Rectangles (monitor pixels) to blank in the page's shot."""
    mon = dump["monitor"]
    out = [(0, mon[3] - TASKBAR, mon[2], TASKBAR)]
    tree = dump["top"]
    head = find(tree, ACCOUNT_ID)
    if head and head.get("bounds"):
        b = rel(head["bounds"], mon)
        out.append((b["x"], b["y"], b["width"], b["height"]))
    def personal_in(n):
        return any(s.casefold() in (n.get("name") or "").casefold() for s in personal)
    # A text naming the operator, and a control naming them whose caption no text
    # under it carries (a button that paints its own name).
    named = [n for n, _ in walk(tree) if personal_in(n)
             and (n.get("controlType") == "Text"
                  or not any(personal_in(t) for t, _ in walk(n) if t is not n and t.get("controlType") == "Text"))
             and n is not head]
    for n in named + entry_texts(tree):
        if n.get("bounds") and not n.get("offscreen"):
            b = rel(n["bounds"], mon)
            out.append((b["x"], b["y"], b["width"], b["height"]))
    return out


def moved(dump, personal):
    """True when the tree read before the screen copy masks other pixels than
    the one read after it (a list that changed while the page was copied), or
    the run did not read it."""
    before = dump.get("before")
    return before is None or blanks({**dump, "top": before}, personal) != blanks(dump, personal)


def stale_pages(dumps, build):
    """Pages not read at `build` (a replayed run from before an update, or one that recorded none)."""
    return {p: d.get("build") for p, d in dumps.items() if d.get("build") != build}


def leaks(recs, personal):
    """Records whose text read from this machine still holds a personal string:
    the label (a UI Automation name), and the ID part made from it when the
    control has no automation ID. The rest is fixed: Windows' own automation
    IDs (a device path is never kept), the page and surface records, and this
    script's own text; a saved network named like a page ("WiFi") does not
    block the run on those."""
    out = []
    for r in recs:
        if r["kind"] in ("surface", "page"):
            continue
        own = r.get("label", "")
        if "uiaId" not in r:
            own += " " + r["id"].rsplit(".", 1)[-1]
        if any(s.casefold() in own.casefold() or (core.slug(s) and core.slug(s) in own) for s in personal):
            out.append(r["id"])
    return out


def redact(src, dst, rects):
    from PIL import Image, ImageDraw
    im = Image.open(src).convert("RGB")
    d = ImageDraw.Draw(im)
    for x, y, w, h in rects:
        d.rectangle([x, y, x + w - 1, y + h - 1], fill=BLANK)
    im.save(dst)
    return im.size


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pages", default=",".join(PAGES))
    ap.add_argument("--from", dest="src", help="re-measure an earlier run's folder instead of capturing again")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    pages = list(dict.fromkeys(p for p in args.pages.split(",") if p))
    if set(pages) - set(PAGES):
        ap.error(f"--pages: one of {', '.join(PAGES)}")
    if set(pages) != set(PAGES) and not args.dry_run:
        # host.json is published whole: a subset would remove the other pages.
        ap.error("--pages: a subset is for --dry-run; publishing takes every page")
    # Measuring holds the host's own lock; only checking against the inventory
    # and publishing hold the shared one, so a VM capture can run meanwhile.
    try:
        with capture.exclusive(HOST_LOCK):
            out = measure(args, pages)
    except BlockingIOError:
        print(f"operator-capture: another host capture holds {HOST_LOCK}; run one at a time")
        return 4
    if isinstance(out, int):
        return out
    try:
        with capture.exclusive(capture.LOCK):
            return check_and_publish(args, pages, out)
    except BlockingIOError:
        print(f"operator-capture: another capture holds {capture.LOCK}; the run is kept: "
              f"publish it later with --from {out}")
        return 4


def measure(args, pages):
    """The run folder to publish from (a fresh capture, or --from's), or an exit code."""
    # An interrupted capture's theme first, whatever else this run then refuses.
    try:
        restore(capture.WORK / "operator-recover")
    except core.Incomplete as exc:
        print(f"operator-capture: {exc}; nothing captured")
        return 3
    build = json.loads((capture.INVENTORY / "reference.json").read_text(encoding="utf-8"))["build"]
    # A replay is checked page by page against the build each page recorded, below.
    have = build if args.src else host_build()
    if have != build:
        print(f"operator-capture: the machine runs {have}, the reference is {build}; nothing captured")
        return 3
    try:
        return pathlib.Path(args.src) if args.src else run_host(pages)
    except core.Incomplete as exc:
        print(f"operator-capture: {exc}; nothing published")
        return 3


def check_and_publish(args, pages, out):
    if capture.recover(capture.JOURNAL):
        print("operator-capture: rolled back a publication that was interrupted; the previous one is restored")
    build = json.loads((capture.INVENTORY / "reference.json").read_text(encoding="utf-8"))["build"]
    try:
        names = identity(out)
    except core.Incomplete as exc:
        print(f"operator-capture: {exc}; nothing published")
        return 3
    # One page at a time: only its records and its redacted shot outlive it.
    lay = out / "shots" / SURFACE
    lay.mkdir(parents=True, exist_ok=True)
    recs, states, personal = [surface_record(build)], [], None
    for p in [p for p in PAGES if p in pages]:
        dump = {p: json.loads((out / f"{p}.json").read_text(encoding="utf-8-sig"))}
        # Each page carries the build it was read at: a replayed run is evidence for that build only.
        stale = stale_pages(dump, build)
        if stale:
            print(f"operator-capture: pages not read at {build}: {stale}; nothing published")
            return 3
        wrong = wrong_pages(dump)
        if wrong:
            print(f"operator-capture: pages that are not the page or the profile asked for: {wrong}; nothing published")
            return 3
        if personal is None:
            personal = personal_strings(dump[p]["top"], names)
        if moved(dump[p], personal):
            print(f"operator-capture: the {p} page changed while it was copied; nothing published")
            return 3
        recs += page_records(p, dump[p], build, personal)
        name = f"{p}.{PROFILE}.png"
        w, h = redact(out / f"{p}.png", lay / name, blanks(dump[p], personal))
        states.append({"file": name, "width": w, "height": h})
    leaked = leaks(recs, personal)
    if leaked:
        print(f"operator-capture: records still name the operator ({leaked[:5]}); nothing published")
        return 3
    rel_path = pathlib.PurePosixPath(SURFACE) / f"{FILE}.json"
    code, report = capture.check_scratch({rel_path: recs})
    print(report.strip())
    if code:
        print("operator-capture: the result fails check-inventory.py; nothing published")
        return 1
    if args.dry_run:
        print(f"operator-capture: dry run, {len(recs)} records checked, redacted shots in {lay}; nothing published")
        return 0
    capture.publish(capture.INVENTORY / rel_path, recs, SURFACE, out, states, capture.SHOTS_MANIFEST,
                    capture.STORE, profile=PROFILE, file=FILE, journal=capture.JOURNAL)
    print(f"operator-capture: published {len(recs)} records and {len(states)} shots to {capture.INVENTORY / rel_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
