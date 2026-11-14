"""The pure half of the UI Automation capture: the walk, record identity, the
profile comparison, the merge and the fixture tree. Standard library only, so the
guest tool (capture-ui.py, Windows Python with comtypes) and the Linux tests run
the same code.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 3.

An element is any object with: name, automation_id, class_name, control_type
(the UIA control type name, such as "MenuItem"), bounds ((x, y, width, height) in
screen pixels), offscreen, accelerator (the UIA AcceleratorKey, "" when none),
enabled, children() and can_expand; an expandable element also
has expand() (returns the entries it shows, raising Incomplete when they never
settle) and collapse(). LiveElement in capture-ui.py and FixtureElement here are
the two implementations.

The walk reads structure only: a name, an automation id, a class, a control type,
a rectangle, a shortcut and whether the element is enabled. It never reads a
value, a text range, a toggle or a selection, so nothing a person typed or chose
can reach a record. Two module lists cover what structure alone cannot tell:
ENVIRONMENT_LISTS (lists whose rows are the machine's own data: recorded once,
never entered) and VALUES (elements whose name is a live value, such as a caret
position or a character count: recorded, their name withheld).

An unnamed Pane with no automation id is hosting plumbing (a XAML island's
bridge and input site), not interface: the walk records its children in its
place, so ids follow what a user sees.
"""
import json
import os
import re
import struct
import zlib

# UIA control type ids (UIAutomationClient.h), so a fixture and a live tree name
# types the same way.
CONTROL_TYPES = {
    50000: "Button", 50001: "Calendar", 50002: "CheckBox", 50003: "ComboBox", 50004: "Edit",
    50005: "Hyperlink", 50006: "Image", 50007: "ListItem", 50008: "List", 50009: "Menu",
    50010: "MenuBar", 50011: "MenuItem", 50012: "ProgressBar", 50013: "RadioButton",
    50014: "ScrollBar", 50015: "Slider", 50016: "Spinner", 50017: "StatusBar", 50018: "Tab",
    50019: "TabItem", 50020: "Text", 50021: "ToolBar", 50022: "ToolTip", 50023: "Tree",
    50024: "TreeItem", 50025: "Custom", 50026: "Group", 50027: "Thumb", 50028: "DataGrid",
    50029: "DataItem", 50030: "Document", 50031: "SplitButton", 50032: "Window", 50033: "Pane",
    50034: "Header", 50035: "HeaderItem", 50036: "Table", 50037: "TitleBar", 50038: "Separator",
    50039: "SemanticZoom", 50040: "AppBar",
}
KIND_BY_TYPE = {
    "Menu": "menu", "MenuBar": "menu", "MenuItem": "menuItem", "Group": "group",
    "Pane": "region", "ToolBar": "region", "StatusBar": "region", "TitleBar": "region",
    "AppBar": "region", "Tab": "region",
}
# Openers the walk expands one level: menus, flyouts, combo boxes and split buttons.
EXPANDABLE = {"MenuItem", "SplitButton", "ComboBox", "Button", "AppBar"}
# Controls whose children are content, not interface: recorded, never entered.
CONTENT = {"Edit", "Document"}
NEVER = {"ToolTip"}            # tooltips are help text (section 4)
NEVER_CLASSES = {"Microsoft.UI.Xaml.Controls.TeachingTip"}   # so are teaching tips
LABEL_MAX = 80                 # longer is a sentence (section 4)
MAX_PASSES = 4                 # a surface gets this many walks to repeat itself
MAX_DEPTH = 40
MAX_RECORDS = 20000


class Incomplete(Exception):
    """The capture did not see the whole surface; nothing may be published from it."""


def clean(text):
    """A name as one line: whitespace runs, line breaks included, become one space."""
    return " ".join((text or "").split())


def slug(text):
    s = re.sub(r"[^a-z0-9]+", "-", (text or "").lower()).strip("-")
    return s[:60].strip("-")


def compare_profile(seen, want, name):
    """Messages for each value of profile `name` that does not hold, in the profile's order."""
    out = []
    for key, expected in want.items():
        got = seen.get(key)
        if key == "dpi":
            if got != expected:
                out.append(f"profile {name}: scaling is {round((got or 0) * 100 / 96)}, expected {round(expected * 100 / 96)}")
        elif str(got) != str(expected):
            out.append(f"profile {name}: {key} is {got}, expected {expected}")
    return out


def matches(element, spec):
    """True when every field the spec names equals the element's."""
    fields = {"controlType": element.control_type, "automationId": element.automation_id,
              "className": element.class_name, "name": clean(element.name)}
    return all(fields[k] == v for k, v in spec.items())


def _segment(el, identity=(), values=()):
    for spec, seg in identity:
        if matches(el, spec):
            return seg
    aid = (el.automation_id or "").strip()
    # A number-only automation id is a runtime index (Win32 menu commands), not a name.
    if aid and not aid.isdigit():
        s = slug(aid)
        if s:
            return s
    # A live value never names an id: it would publish the value and move with it.
    name = "" if any(matches(el, spec) for spec in values) else slug(clean(el.name))
    return name or slug(el.class_name) or slug(el.control_type) or "element"


def excluded(el):
    """Why the walk leaves el out entirely, or None: the same rule for walk and dump."""
    if el.offscreen:
        return "offscreen"
    if el.control_type in NEVER or el.class_name in NEVER_CLASSES:
        return "tooltip"
    if el.control_type == "Text" and len(clean(el.name)) > LABEL_MAX:
        return "prose"                 # a description under a setting is not a label
    return None


def is_hosting(el):
    return el.control_type == "Pane" and not clean(el.name) and not (el.automation_id or "").strip()


class Capture:
    def __init__(self):
        self.records = []
        self.unnamed = []
        self.ambiguous = []
        self.skipped = {"offscreen": 0, "tooltip": 0, "prose": 0, "hosting": 0}
        self.states = []


def walk(root, *, surface, build, profile, root_slug, root_label, root_kind="surface",
         environment_lists=(), values=(), identity=(), shot=None):
    """Capture one surface. Returns a Capture or raises Incomplete.

    identity is the surface module's IDENTITY: (spec, segment) pairs that name an
    element whose automation id, name and class do not tell it from a sibling.
    """
    cap = Capture()
    origin = root.bounds

    def rel(b):
        return {"x": int(b[0] - origin[0]), "y": int(b[1] - origin[1]), "width": max(0, int(b[2])), "height": max(0, int(b[3]))}

    def emit(el, rid, parent, order, kind, label=None):
        if len(cap.records) >= MAX_RECORDS:
            raise Incomplete(f"more than {MAX_RECORDS} elements; the surface module must name a smaller root")
        name = clean(el.name)
        live = any(matches(el, spec) for spec in values)
        if label is None:
            if name and len(name) <= LABEL_MAX and not live:
                label = name
            else:
                label = clean(el.automation_id or el.class_name or el.control_type or "element")
                why = "name is a live value" if live else ("no name" if not name else "name is a sentence")
                cap.unnamed.append({"id": rid, "uiaId": el.automation_id or "", "className": el.class_name or "",
                                    "controlType": el.control_type, "why": why})
        r = {"id": rid, "surface": surface, "kind": kind, "label": label, "order": order,
             "source": f"uia:{build}", "capturedFrom": build, "captureProfile": profile,
             "controlType": (el.control_type or "Custom").lower()}
        if parent:
            r["parent"] = parent
        if el.automation_id:
            r["uiaId"] = el.automation_id
        if el.class_name:
            r["className"] = el.class_name
        r["bounds"] = rel(el.bounds)
        shortcut = clean(getattr(el, "accelerator", ""))
        if shortcut:
            r["shortcut"] = shortcut
        if not getattr(el, "enabled", True):
            r["enabled"] = False          # absent means enabled at rest
        cap.records.append(r)
        return r

    def interface_children(el, depth):
        """el's children with hosting panes replaced by their own children."""
        out = []
        for k in el.children():
            if is_hosting(k) and not k.offscreen:
                if depth > MAX_DEPTH:
                    raise Incomplete(f"hosting panes nest deeper than {MAX_DEPTH} levels")
                cap.skipped["hosting"] += 1
                out.extend(interface_children(k, depth + 1))
            elif keep(k):
                out.append(k)
        return out

    def ids_for(parent_id, kids):
        segs = [_segment(k, identity, values) for k in kids]
        seen = {}
        out = []
        for s in segs:
            seen[s] = seen.get(s, 0) + 1
            out.append(s if seen[s] == 1 else f"{s}-{seen[s]}")
        ambiguous = {s for s, n in seen.items() if n > 1}
        for s, rid in zip(segs, out):
            if s in ambiguous:
                cap.ambiguous.append(f"{parent_id}.{rid}")
        return [f"{parent_id}.{rid}" for rid in out]

    def keep(el):
        why = excluded(el)
        if why:
            cap.skipped[why] += 1
        return why is None

    def visit(el, rid, parent, order, depth):
        if depth > MAX_DEPTH:
            raise Incomplete(f"{rid}: deeper than {MAX_DEPTH} levels")
        env = any(matches(el, spec) for spec in environment_lists)
        kind = KIND_BY_TYPE.get(el.control_type, "control")
        if env:
            r = emit(el, rid, parent, order, kind)
            r["controlType"] = "list"       # rows are the machine's own data; never recorded
            return
        r = emit(el, rid, parent, order, kind)
        if el.control_type in CONTENT:
            return
        kids = interface_children(el, depth)
        for i, (k, kid) in enumerate(zip(kids, ids_for(rid, kids))):
            visit(k, kid, rid, i, depth + 1)
        if el.can_expand and el.control_type in EXPANDABLE:
            try:                  # closed again even when the opening itself fails
                items = [k for k in el.expand() if keep(k)]
                state = f"{rid.split('.', 2)[2]}-open".replace(".", "-")
                if shot:
                    cap.states.append(shot(state, el, items))
                base = len(kids)
                for i, (k, kid) in enumerate(zip(items, ids_for(rid, items))):
                    emit(k, kid, rid, base + i, "menuItem")
                r["itemCount"] = len(kids) + len(items)
            finally:
                el.collapse()

    if shot:
        cap.states.append(shot("rest", root, []))
    visit(root, f"win11.{surface}.{root_slug}", None, 0, 0)
    root_record = cap.records[0]
    root_record["kind"] = root_kind
    root_record["label"] = root_label
    cap.unnamed = [u for u in cap.unnamed if u["id"] != root_record["id"]]
    return cap


def settle(walk_once, max_passes=MAX_PASSES):
    """Walk until two passes in a row record the same thing; return (capture, passes).

    The first open of a menu after launch can differ from every later one (seen on
    Notepad 2026-11-14: the File menu 18 px narrower, the Edit menu one entry
    longer, the editor missing), so one walk is not evidence. A surface that never
    repeats itself is incomplete.
    """
    prev = walk_once()
    for n in range(2, max_passes + 1):
        cap = walk_once()
        if cap.records == prev.records:
            return cap, n
        prev = cap
    raise Incomplete(f"the surface did not repeat itself in {max_passes} walks")


def confirm_dump(tree, cap, **walk_args):
    """Raise Incomplete unless walking the dumped tree gives exactly the captured records."""
    again = walk(FixtureElement(tree), **walk_args)
    if again.records != cap.records:
        raise Incomplete("the surface changed between the last walk and the dump")


HAND_FIELDS = ("behaviour", "disposition", "inconsistency")


def merge(old, new, ambiguous):
    """Carry the hand-written fields of `old` onto `new` by id.

    Returns (records, dropped): dropped lists the annotated old ids the capture no
    longer sees. Raises ValueError when an annotation would land on an ambiguous id
    or on any id beneath one, because positional ids can move a note onto a
    different control: reorder two look-alike groups and each group's children
    swap ids too.
    """
    amb = set(ambiguous)

    def under_ambiguous(rid):
        parts = rid.split(".")
        return any(".".join(parts[:n]) in amb for n in range(1, len(parts) + 1))

    by_id = {r["id"]: r for r in old}
    out = []
    for r in new:
        prev = by_id.get(r["id"])
        hand = {k: prev[k] for k in HAND_FIELDS if prev and k in prev}
        if hand and under_ambiguous(r["id"]):
            raise ValueError(f"{r['id']}: its hand-written fields cannot be carried, the id is ambiguous "
                             "(siblings share a name); give the element a name in the surface module's IDENTITY")
        out.append({**r, **hand})
    seen = {r["id"] for r in new}
    dropped = [r["id"] for r in old if r["id"] not in seen and any(k in r for k in HAND_FIELDS)]
    return out, dropped


class FixtureElement:
    """An element read from a recorded tree (capture-ui.py --dump), for tests."""

    def __init__(self, d):
        self.d = d
        self.name = d.get("name", "")
        self.automation_id = d.get("automationId", "")
        self.class_name = d.get("className", "")
        self.control_type = d.get("controlType", "Custom")
        self.bounds = tuple(d.get("bounds", (0, 0, 0, 0)))
        self.offscreen = bool(d.get("offscreen", False))
        self.accelerator = d.get("accelerator", "")
        self.enabled = d.get("enabled", True)
        self.can_expand = "expands" in d or d.get("expandFails", False)
        self.open = False

    def children(self):
        return [FixtureElement(c) for c in self.d.get("children", [])]

    def expand(self):
        if self.d.get("expandFails"):
            raise Incomplete(f"{self.name or self.automation_id}: the flyout never settled")
        self.open = True
        return [FixtureElement(c) for c in self.d.get("expands", [])]

    def collapse(self):
        self.open = False


class _Budget:
    def __init__(self, limit):
        self.left = limit

    def take(self):
        self.left -= 1
        if self.left < 0:
            raise Incomplete(f"more than {MAX_RECORDS} elements to dump; the surface module must name a smaller root")


def dump(el, depth=0, entry=False, environment_lists=(), budget=None):
    """The fixture form of a tree: structure only, the same fields the walk reads.

    It visits exactly what the walk visits: an element the walk excludes (offscreen,
    a tooltip, prose) is left out with its subtree, an environment list is kept
    with no rows, and an expansion's entries are recorded flat, with no children
    and no expansion of their own. It stops past MAX_RECORDS elements.
    """
    budget = budget or _Budget(MAX_RECORDS)
    budget.take()
    d = {"name": el.name or "", "automationId": el.automation_id or "", "className": el.class_name or "",
         "controlType": el.control_type, "bounds": list(el.bounds)}
    if el.offscreen:
        d["offscreen"] = True
    if el.accelerator:
        d["accelerator"] = el.accelerator
    if not el.enabled:
        d["enabled"] = False
    if entry:
        return d
    if depth > MAX_DEPTH:
        raise Incomplete(f"{d['name'] or d['automationId']}: deeper than {MAX_DEPTH} levels")
    if any(matches(el, spec) for spec in environment_lists):
        return d                       # rows are the machine's own data
    if el.control_type not in CONTENT:
        kids = [dump(k, depth + 1, environment_lists=environment_lists, budget=budget)
                for k in el.children() if not excluded(k)]
        if kids:
            d["children"] = kids
    if el.can_expand and el.control_type in EXPANDABLE and not el.offscreen:
        try:
            d["expands"] = [dump(k, depth + 1, entry=True, budget=budget) for k in el.expand() if not excluded(k)]
        finally:
            el.collapse()
    return d


def write_png(path, width, height, rgb):
    """Write 8-bit RGB pixels (rows top to bottom) as a PNG file."""
    stride = width * 3
    raw = b"".join(b"\0" + rgb[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        fh.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        fh.write(chunk(b"IEND", b""))


def write_records(path, records):
    """Write one record per line, replacing the file only once the new one is whole."""
    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        for r in records:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def read_records(path):
    with open(path, encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]
