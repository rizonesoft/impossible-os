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
import contextlib
import json
import os
import pathlib
import re
import struct
import time
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
MAX_VISITS = 4 * MAX_RECORDS   # elements read, excluded and hosting ones included
POSITIONAL = "--"              # marks a segment told apart only by position; slug() never makes it


class Incomplete(Exception):
    """The capture did not see the whole surface; nothing may be published from it."""


def clean(text):
    """A name as one line: whitespace runs, line breaks included, become one space."""
    return " ".join((text or "").split())


def name_of(el):
    """el's name as a label: a Win32 menu entry carries its shortcut after a tab
    ("Undo Copy\tCtrl+Z", seen 2026-11-16 on the classic desktop menu), which is
    not part of the name."""
    return clean((el.name or "").split("\t", 1)[0])


def tab_shortcut(el):
    """The shortcut a Win32 menu entry carries after a tab in its name, or ""."""
    parts = (el.name or "").split("\t", 1)
    return clean(parts[1]) if len(parts) == 2 else ""


def slug(text):
    s = re.sub(r"[^a-z0-9]+", "-", (text or "").lower()).strip("-")
    return s[:60].strip("-")


FIXTURE_VARIANTS = re.compile(r"^# Variants: (.+)$", re.M)


def fixture_problem(fixtures_dir, spec):
    """None when `spec` (``name`` or ``name:variant``) names a fixture script in
    `fixtures_dir` and, with a variant, one its ``# Variants:`` line declares;
    otherwise what is wrong, in one line."""
    name, _, variant = str(spec).partition(":")
    if name != slug(name):
        return f"fixture {spec!r}: the name must be a slug"
    path = pathlib.Path(fixtures_dir) / f"{name}.ps1"
    if not path.is_file():
        return f"fixture {spec!r}: no script {path.name} in scripts/inventory/fixtures/"
    m = FIXTURE_VARIANTS.search(path.read_text(encoding="utf-8"))
    declared = [v.strip() for v in m.group(1).split(",")] if m else []
    if not variant:
        return f"fixture {spec!r}: name one of its variants ({', '.join(declared)})" if declared else None
    if variant not in declared:
        return f"fixture {spec!r}: {name}.ps1 has no variant {variant!r} (it has: {', '.join(declared) or 'none'})"
    return None


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


# The profile values set-profile.ps1 applies in ref's session; any other value
# that differs from fhd-100 needs a changed machine. ref-vm.ps1 holds the same
# list ($LiveKeys); a test keeps the two equal.
LIVE_KEYS = ("dpi", "appsLight", "systemLight", "highContrast", "textScale", "secondDpi", "secondSide")


def checkpoint_for(name, profiles):
    """The reference VM checkpoint a capture under profile `name` restores: clean
    for fhd-100 and the live profiles, profile-<name> for a machine profile
    (another resolution or display language, derived by ref-vm.ps1 -Derive). A
    two-display profile never runs on the reference VM (standin_variant)."""
    base = profiles["fhd-100"]
    machine = [k for k, v in profiles[name].items() if k not in LIVE_KEYS and str(v) != str(base.get(k))]
    return f"profile-{name}" if machine else "clean"


def session_type(seen):
    """The schema's session for a capture whose profile probe saw `seen`, or None.
    Every capture runs in the machine's console session (the reference VM's or the
    stand-in machine's), so that is the answer; a remote session is refused (None),
    because inside the guest an enhanced session cannot be told from a remote one
    and the record could not name it truly."""
    return None if seen.get("remote") else "console"


def standin_variant(name, profile, variants, requested=None):
    """(variant, problem) for capturing under profile `name`. A profile with more
    than one display needs the stand-in variant with that many heads (Hyper-V
    gives the reference VM one); a one-display profile runs on the reference VM
    unless `requested` names a one-head variant. Exactly one of the two is None,
    except (None, None) for the reference VM."""
    n = profile.get("monitors", 1)
    if requested is not None:
        heads = variants[requested].get("displays", 1)
        if heads != n:
            return None, f"--standin {requested} has {heads} display(s); profile {name} needs {n}"
        return requested, None
    if n == 1:
        return None, None
    fits = sorted(k for k, v in variants.items() if v.get("displays", 1) == n)
    if len(fits) != 1:
        return None, f"profile {name} needs {n} displays; {len(fits)} stand-in variants have that many heads, want one"
    return fits[0], None


def matches(element, spec):
    """True when every field the spec names equals the element's. `namePrefix`
    matches the start of the name, for an element whose name is a fixed word
    followed by a live value (the tray's "Volume No audio device is installed").
    `classPrefix` matches the first word of the class, for a web page's element
    whose class list grows with its state (search's best match row adds
    "selected" and "sa_hv" under the pointer, 2026-11-17). `automationIdPrefix`
    matches the start of the automation id, for a web page's list whose id
    carries its category and a URL-encoded query (the emoji panel's
    "expanded-view-list-Emoji-Most%20frequently%20used", 2026-11-18)."""
    fields = {"controlType": element.control_type, "automationId": element.automation_id,
              "className": element.class_name, "name": name_of(element)}

    def one(k, v):
        if k == "namePrefix":
            return fields["name"].startswith(v)
        if k == "automationIdPrefix":
            return (fields["automationId"] or "").startswith(v)
        if k == "classPrefix":
            words, want = (fields["className"] or "").split(), v.split()
            return words[:len(want)] == want
        return fields[k] == v
    return all(one(k, v) for k, v in spec.items())


def _segment(el, identity=(), values=()):
    for spec, seg in identity:
        if "nth" not in spec and matches(el, spec):
            return seg
    aid = (el.automation_id or "").strip()
    # A number-only automation id is a runtime index (Win32 menu commands), not a name.
    if aid and not aid.isdigit():
        s = slug(aid)
        if s:
            return s
    # A live value never names an id: it would publish the value and move with it.
    # Nor does a sentence: help text is withheld from the label, so from the id too.
    label = name_of(el)
    withheld = len(label) > LABEL_MAX or any(matches(el, spec) for spec in values)
    name = "" if withheld else slug(label)
    return name or slug(el.class_name) or slug(el.control_type) or "element"


def outermost(found, parent_of):
    """The keys of `found` with no ancestor in `found`.

    found maps a runtime id to an element; parent_of(element) gives its parent's
    runtime id, or None at the top. A popup search finds nested containers too
    (a list inside a list); reading only the outermost keeps a nested list's rows
    out of the entries, and behind a marked environment list where it belongs.
    An ancestry that cannot be settled (a chain past MAX_DEPTH, a cycle, or a
    parent_of that raises) fails the capture rather than keeping the container:
    keeping it could read a marked list's rows.
    """
    keep = []
    for key, el in found.items():
        # The whole chain to the root first, so a loop is found even through
        # another found container; only then is the container judged nested.
        node, chain = el, [key]
        for _ in range(MAX_DEPTH):
            up = parent_of(node)
            if up is None:
                break
            if up[0] in chain:
                raise Incomplete("a popup's parent chain loops; its nesting cannot be settled")
            chain.append(up[0])
            node = up[1]
        else:
            raise Incomplete(f"a popup's parent chain is deeper than {MAX_DEPTH}; its nesting cannot be settled")
        nested = any(k in found for k in chain[1:])
        if not nested:
            keep.append(key)
    return keep


def excluded(el):
    """Why the walk leaves el out entirely, or None: the same rule for walk and dump."""
    if el.offscreen:
        return "offscreen"
    if el.control_type in NEVER or el.class_name in NEVER_CLASSES:
        return "tooltip"
    if el.control_type == "Text" and len(name_of(el)) > LABEL_MAX:
        return "prose"                 # a description under a setting is not a label
    return None


def clip_box(boxes, screen):
    """The box around `boxes` (x, y, width, height each), clipped to `screen`
    (left, top, right, bottom), as (x0, y0, x1, y1). A box wholly off the
    screen leaves nothing to show and fails the capture."""
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[0] + b[2] for b in boxes), max(b[1] + b[3] for b in boxes)
    x0, y0, x1, y1 = max(x0, screen[0]), max(y0, screen[1]), min(x1, screen[2]), min(y1, screen[3])
    if x1 <= x0 or y1 <= y0:
        raise Incomplete("the screenshot's box lies wholly off the screen")
    return x0, y0, x1, y1


def is_hosting(el):
    return el.control_type == "Pane" and not name_of(el) and not (el.automation_id or "").strip()


def extent(el):
    """el's bounds, or the box around its on-screen children when el has no area
    of its own: a WinUI menu's popup window reports an empty, offscreen box while
    its entries are on screen (seen 2026-11-16 on 26300.9974)."""
    x, y, w, h = el.bounds
    if w > 0 and h > 0:
        return el.bounds
    boxes = [k.bounds for k in el.children() if not k.offscreen and k.bounds[2] > 0 and k.bounds[3] > 0]
    if not boxes:
        raise Incomplete(f"{clean(el.name) or el.class_name or 'the root'} has no area and no child on screen")
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[0] + b[2] for b in boxes), max(b[1] + b[3] for b in boxes)
    return (x0, y0, x1 - x0, y1 - y0)


class Capture:
    def __init__(self):
        self.records = []
        self.unnamed = []
        self.ambiguous = []
        self.skipped = {"offscreen": 0, "tooltip": 0, "prose": 0, "hosting": 0, "elsewhere": 0, "caption": 0}
        self.states = []
        self.measured = []            # records of unkept scenes (Scene.keep)


def walk(root, *, surface, build, profile, session, root_slug, root_label, root_kind="surface",
         environment_lists=(), values=(), identity=(), shot=None, expand=True, hosts=(), labels=(),
         omit=(), count=None, kinds=()):
    """Capture one surface. Returns a Capture or raises Incomplete.

    session is the schema's session the capture ran in (session_type), written
    on every record.

    identity is the surface module's IDENTITY: (spec, segment) pairs that name an
    element whose automation id, name and class do not tell it from a sibling.
    Each segment must be a slug, so it can never look positional.

    expand=False walks without opening submenus (a scene whose openers do not
    open under UI Automation: the classic desktop menu, 2026-11-16).

    hosts are specs of containers that are always read through, never recorded,
    even when they report offscreen: a WinUI menu's command bar and overflow
    popup do while their entries are on screen (2026-11-16).

    labels are (spec, label) pairs: the label of an element with no usable name
    that the spec matches (an entry's icon), so it is not reported unnamed.

    kinds are (spec, kind) pairs: the record kind of an element the spec
    matches, where its control type names the wrong one (search's action lists
    are web ListItems that act as menu entries, 2026-11-17).

    omit names elements another file of the surface records (the taskbar's own
    buttons, inside the window the tray file walks): each is left out with its
    subtree, counted as skipped "elsewhere".
    """
    for _, seg in identity:
        if seg != slug(seg) or not seg:
            raise ValueError(f"IDENTITY segment {seg!r} is not a slug (lower-case letters, digits, single hyphens)")
    cap = Capture()
    origin = extent(root)

    def rel(b):
        return {"x": int(b[0] - origin[0]), "y": int(b[1] - origin[1]), "width": max(0, int(b[2])), "height": max(0, int(b[3]))}

    def emit(el, rid, parent, order, kind, label=None):
        if len(cap.records) >= MAX_RECORDS:
            raise Incomplete(f"more than {MAX_RECORDS} elements; the surface module must name a smaller root")
        name = name_of(el)
        live = any(matches(el, spec) for spec in values)
        given = next((lab for spec, lab in labels if matches(el, spec)), None)
        if label is None and given is not None and not (name and len(name) <= LABEL_MAX and not live):
            label = given
        if label is None:
            if name and len(name) <= LABEL_MAX and not live:
                label = name
            else:
                label = clean(el.automation_id or el.class_name or el.control_type or "element")
                why = "name is a live value" if live else ("no name" if not name else "name is a sentence")
                cap.unnamed.append({"id": rid, "uiaId": el.automation_id or "", "className": el.class_name or "",
                                    "controlType": el.control_type, "why": why})
        r = {"id": rid, "surface": surface, "kind": kind, "label": label, "order": order,
             "source": f"uia:{build}", "capturedFrom": build, "captureProfile": profile, "session": session,
             "controlType": (el.control_type or "Custom").lower()}
        if parent:
            r["parent"] = parent
        if el.automation_id:
            r["uiaId"] = el.automation_id
        if el.class_name:
            r["className"] = el.class_name
        r["bounds"] = rel(origin if el is root else el.bounds)
        shortcut = clean(getattr(el, "accelerator", "")) or tab_shortcut(el)
        if shortcut:
            r["shortcut"] = shortcut
        if not getattr(el, "enabled", True):
            r["enabled"] = False          # absent means enabled at rest
        cap.records.append(r)
        return r

    visits = [0]

    def interface_children(el, depth):
        """el's children with hosting panes replaced by their own children.

        Exclusion comes first, so an excluded element (a teaching tip, a tooltip,
        anything offscreen) is pruned with its subtree even when it is also an
        unnamed pane. Every element read counts against MAX_VISITS, including the
        ones that are never recorded.
        """
        out = []
        for k in el.children(limit=MAX_VISITS - visits[0]):
            visits[0] += 1
            if visits[0] > MAX_VISITS:
                raise Incomplete(f"more than {MAX_VISITS} elements read; the surface module must name a smaller root")
            if any(matches(k, spec) for spec in omit):
                cap.skipped["elsewhere"] += 1
                continue
            if any(matches(k, spec) for spec in hosts):
                if depth > MAX_DEPTH:
                    raise Incomplete(f"hosting panes nest deeper than {MAX_DEPTH} levels")
                cap.skipped["hosting"] += 1
                out.extend(interface_children(k, depth + 1))
                continue
            if not keep(k):
                continue
            # A text that repeats its parent's name is the parent's own caption (a
            # WinUI menu item's TextBlock, 2026-11-17), recorded with the parent.
            if k.control_type == "Text" and el.name and k.name == el.name:
                cap.skipped["caption"] += 1
                continue
            if is_hosting(k) and not is_environment(k):   # a marked list is never flattened open
                if depth > MAX_DEPTH:
                    raise Incomplete(f"hosting panes nest deeper than {MAX_DEPTH} levels")
                cap.skipped["hosting"] += 1
                out.extend(interface_children(k, depth + 1))
            else:
                out.append(k)
        return out

    def ids_for(parent_id, kids):
        """Ids for siblings. Look-alike siblings are told apart by position, and
        every one of them carries its number behind a double hyphen, the first
        included (twin--1, twin--2). slug() folds every run of other characters
        to one hyphen and IDENTITY segments must be slugs, so no name can make
        "--": a positional id is recognisable in any capture, old or new, and a
        note written on one is never carried (see merge)."""
        segs = [_segment(k, identity, values) for k in kids]
        # An IDENTITY spec with "nth" names the nth of the siblings that match the
        # rest of it, counting from 1 in tree order: for twins whose order is what
        # they are (a keyboard's left and right Control keys, 2026-11-18), never
        # for a list whose rows can move.
        for spec, seg in identity:
            if "nth" not in spec:
                continue
            rest = {k: v for k, v in spec.items() if k != "nth"}
            hits = [i for i, k in enumerate(kids) if matches(k, rest)]
            # Only twins: a lone match keeps its own name.
            if len(hits) > 1 and len(hits) >= spec["nth"]:
                segs[hits[spec["nth"] - 1]] = seg
        total = {}
        for s in segs:
            total[s] = total.get(s, 0) + 1
        seen = {}
        out = []
        for s in segs:
            if total[s] == 1:
                out.append(s)
                continue
            seen[s] = seen.get(s, 0) + 1
            rid = f"{s}{POSITIONAL}{seen[s]}"
            out.append(rid)
            cap.ambiguous.append(f"{parent_id}.{rid}")
        return [f"{parent_id}.{rid}" for rid in out]

    def keep(el):
        why = excluded(el)
        if why:
            cap.skipped[why] += 1
        return why is None

    def is_environment(el):
        return any(matches(el, spec) for spec in environment_lists)

    def visit(el, rid, parent, order, depth):
        if depth > MAX_DEPTH:
            raise Incomplete(f"{rid}: deeper than {MAX_DEPTH} levels")
        env = is_environment(el)
        kind = next((k for spec, k in kinds if matches(el, spec)), None) or KIND_BY_TYPE.get(el.control_type, "control")
        if env:
            r = emit(el, rid, parent, order, kind)
            r["controlType"] = "list"       # rows are the machine's own data; never recorded
            return
        r = emit(el, rid, parent, order, kind)
        if el.control_type in CONTENT:
            return
        kids = interface_children(el, depth)
        if depth == 0 and shot:
            # The resting surface, named after its root: the root and its entries,
            # which a menu's flyout places outside its own box.
            cap.states.append(shot(root_slug, el, kids, origin))
        for i, (k, kid) in enumerate(zip(kids, ids_for(rid, kids))):
            visit(k, kid, rid, i, depth + 1)
        if expand and el.can_expand and el.control_type in EXPANDABLE:
            try:                  # closed again even when the opening itself fails
                items = [k for k in el.expand(skip=is_environment) if keep(k)]
                # The id below the surface, dots kept: ids are unique, so states are.
                state = f"{rid.split('.', 2)[2]}-open"
                if shot:
                    cap.states.append(shot(state, el, items))
                base = len(kids)
                for i, (k, kid) in enumerate(zip(items, ids_for(rid, items))):
                    if is_environment(k):          # a popup list of the machine's own data: no rows
                        emit(k, kid, rid, base + i, KIND_BY_TYPE.get(k.control_type, "control"))["controlType"] = "list"
                    else:
                        emit(k, kid, rid, base + i, "menuItem")
                r["itemCount"] = len(kids) + len(items)
            finally:
                el.collapse()

    visit(root, f"win11.{surface}.{root_slug}", None, 0, 0)
    root_record = cap.records[0]
    root_record["kind"] = root_kind
    root_record["label"] = root_label
    # A menu that is the scene's root has its entries counted as an opened one
    # does; count=True does the same for another root of entries (a flyout card).
    if count if count is not None else root_kind in ("menu", "contextMenu"):
        root_record["itemCount"] = sum(1 for r in cap.records if r.get("parent") == root_record["id"])
    cap.unnamed = [u for u in cap.unnamed if u["id"] != root_record["id"]]
    return cap


class Scene:
    """One root a surface module walks: its own open, root and close.

    A module lists several in SCENES (a surface and each of its context menus);
    a module without SCENES is one scene made from ROOT_SLUG, ROOT_LABEL,
    ROOT_KIND, open, root and close. IDENTITY, VALUES and ENVIRONMENT_LISTS come
    from the module unless a scene names its own.
    """

    FIELDS = ("slug", "label", "kind", "open", "root", "close", "identity", "values", "environment_lists", "keep",
              "expand", "hosts", "labels", "omit", "count", "kinds")

    def __init__(self, mod, **spec):
        unknown = set(spec) - set(self.FIELDS)
        if unknown:
            raise ValueError(f"scene {spec.get('slug')!r}: unknown field(s) {sorted(unknown)}")
        self.slug = spec["slug"]
        if self.slug != slug(self.slug) or not self.slug:
            raise ValueError(f"scene slug {self.slug!r} is not a slug (lower-case letters, digits, single hyphens)")
        self.label = spec["label"]
        self.kind = spec.get("kind", "surface")
        self.open, self.root, self.close = spec["open"], spec["root"], spec["close"]
        self.identity = tuple(spec.get("identity", getattr(mod, "IDENTITY", ())))
        self.values = tuple(spec.get("values", getattr(mod, "VALUES", ())))
        self.environment_lists = tuple(spec.get("environment_lists", getattr(mod, "ENVIRONMENT_LISTS", ())))
        # keep=False: a measuring scene (the desktop at another icon size); its
        # records reach METRICS and are not published, nor are its screenshots.
        self.keep = bool(spec.get("keep", True))
        self.expand = bool(spec.get("expand", True))
        self.hosts = tuple(spec.get("hosts", getattr(mod, "HOSTS", ())))
        self.labels = tuple(spec.get("labels", getattr(mod, "LABELS", ())))
        self.omit = tuple(spec.get("omit", getattr(mod, "OMIT", ())))
        self.count = spec.get("count")          # None: count a menu or context menu root's entries
        self.kinds = tuple(spec.get("kinds", getattr(mod, "KINDS", ())))


def scenes_of(mod):
    """The module's scenes, in order; their slugs are distinct."""
    if hasattr(mod, "SCENES"):
        scenes = [Scene(mod, **spec) for spec in mod.SCENES]
    else:
        scenes = [Scene(mod, slug=mod.ROOT_SLUG, label=mod.ROOT_LABEL, kind=getattr(mod, "ROOT_KIND", "surface"),
                        open=mod.open, root=mod.root, close=mod.close)]
    if not scenes:
        raise ValueError("SCENES is empty")
    seen = set()
    for sc in scenes:
        if sc.slug in seen:
            raise ValueError(f"two scenes share the slug {sc.slug!r}")
        seen.add(sc.slug)
    return scenes


RESTART_EXPLORER = ("Stop-Process -Name explorer -Force; Start-Sleep -Seconds 3; "
                    # Explorer usually restarts itself; start it when it has not.
                    "if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }")


def restart_explorer(ctx, bar="Shell_TrayWnd", wait=90, clock=time.monotonic):
    """Restart Explorer, which reads several taskbar settings only when it
    starts (the combine and multi-display modes, the clock's seconds), and wait
    until the bar in window class `bar` holds its Start button again."""
    ctx.powershell(RESTART_EXPLORER)
    deadline = clock() + wait
    while True:
        try:
            ctx.find(within=ctx.find_window(className=bar, timeout=5), automationId="StartButton", depth=4, timeout=5)
            return
        except Incomplete:
            if clock() > deadline:
                raise Incomplete(f"the {bar} bar did not come back within {wait} s of restarting Explorer")


@contextlib.contextmanager
def failing_step(where):
    """Note `where` on an exception that leaves the block."""
    try:
        yield
    except Exception as exc:
        exc.add_note(where)
        raise


SCENE_TRIES = 3                    # opens of one scene a transient provider failure may cost


def run_scenes(scenes, ctx, walk_scene, on_open=None, transient=lambda exc: False):
    """Open, walk to a settled capture, and close each scene in turn.

    Returns (Capture, passes by slug); an unkept scene's records are in the
    Capture's `measured`, not its `records`. A scene is closed even when its walk
    fails, and the first failure ends the capture: a later scene may depend on
    the state an earlier one left. Each scene's root takes its place in the list
    as its order, so the file keeps the scenes in the order the module gives.
    on_open(scene) runs after a scene opens, before its walk (a dump hook).
    transient(exc) names a provider failure that says nothing about the
    surface (a web page's provider dropping one call, 2026-11-18): the scene is
    closed and opened again, up to SCENE_TRIES times, never past that.
    """
    total, passes = Capture(), {}
    for n, sc in enumerate(scenes):
        # A failure names the scene and the step it failed in (2026-11-17: a UI
        # Automation error from a nine-scene module named neither).
        for attempt in range(1, SCENE_TRIES + 1):
            opened = False
            try:
                with failing_step(f"scene {sc.slug}: opening"):
                    sc.open(ctx)
                opened = True
                with failing_step(f"scene {sc.slug}: walking"):
                    cap, passes[sc.slug] = settle(lambda sc=sc: walk_scene(sc))
                    if on_open:
                        on_open(sc, cap)
                failed = None
            except Exception as exc:
                if attempt == SCENE_TRIES or not transient(exc):
                    raise
                failed = exc
                if not opened:
                    # A retry starts from a closed scene: an open that failed
                    # part way may have left its window up. A close that fails
                    # here raises, with the transient failure as its context.
                    with failing_step(f"scene {sc.slug}: closing before a retry"):
                        sc.close(ctx)
            finally:
                # Only an opened scene is closed: a close that fails on a scene
                # that never opened would hide why it did not.
                if opened:
                    with failing_step(f"scene {sc.slug}: closing"):
                        sc.close(ctx)
            if failed is None:
                break
        cap.records[0]["order"] = n
        if not sc.keep:
            total.measured.extend(cap.records)
            continue
        total.records.extend(cap.records)
        total.unnamed.extend(cap.unnamed)
        total.ambiguous.extend(cap.ambiguous)
        total.states.extend(cap.states)
        for k, v in cap.skipped.items():
            total.skipped[k] = total.skipped.get(k, 0) + v
    return total, passes


# Fields an authored record may carry beyond the ones authored() writes.
# baselineId: a record a module builds from what it read (Start's app list,
# scrolled through, 2026-11-17) names its baseline row like a walked one.
AUTHORED_FIELDS = ("kind", "label", "order", "behaviour", "openedBy", "shortcut", "shortcutScope", "overridable",
                   "measure", "motion", "state", "enabled", "itemCount", "controlType", "uiaId", "baselineId", "colours")


def authored(items, *, surface, build, source, profile=None, session=None):
    """Records a surface module writes itself: HAND (what UI Automation cannot
    read, source hand:<build>) and METRICS (numbers computed from captured
    bounds, source uia:<build>). Each item names its id and parent below
    win11.<surface>.; a recapture writes them again, so they never need to be
    carried across a merge."""
    out = []
    for it in items:
        extra = set(it) - set(AUTHORED_FIELDS) - {"id", "parent"}
        if extra:
            raise ValueError(f"{it.get('id')}: field(s) {sorted(extra)} cannot be authored")
        r = {"id": f"win11.{surface}.{it['id']}", "surface": surface, "kind": it["kind"], "label": it["label"],
             "order": it["order"], "source": f"{source}:{build}", "capturedFrom": build}
        if it.get("parent"):
            r["parent"] = f"win11.{surface}.{it['parent']}"
        if profile:
            r["captureProfile"] = profile
        if session:
            r["session"] = session
        r.update({k: v for k, v in it.items() if k not in ("id", "parent", "kind", "label", "order")})
        out.append(r)
    return out


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


# What a person (or a later matching pass) adds to a captured record and the walk
# never produces: a re-capture carries these and refreshes everything else. Where
# the capture itself writes one of them (a surface module's HAND text, an app's
# baseline link), the capture's value wins: the module is that field's author.
HAND_FIELDS = ("behaviour", "disposition", "inconsistency", "reason", "mergedInto", "placedUnder", "baselineId", "baselineMissing",
               "toggleStates")


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
        # Positional in this capture or in the one that wrote the note: both carry "--".
        parts = rid.split(".")
        return POSITIONAL in rid or any(".".join(parts[:n]) in amb for n in range(1, len(parts) + 1))

    by_id = {r["id"]: r for r in old}
    out = []
    for r in new:
        prev = by_id.get(r["id"])
        hand = {k: prev[k] for k in HAND_FIELDS if prev and k in prev}
        if hand and under_ambiguous(r["id"]):
            raise ValueError(f"{r['id']}: its hand-written fields cannot be carried, the id is ambiguous "
                             "(siblings share a name); give the element a name in the surface module's IDENTITY")
        # A field the capture wrote itself (a module's hand record, an app's
        # baseline link) has its author here: only what a person added is
        # carried. None is the capture owning a field that has no value this
        # time (an app out of its folder has no folder text): it is removed, not
        # refilled from the old copy. A capture's match also replaces a carried
        # "no row lists it", since the two cannot both stand.
        carried = {k: v for k, v in hand.items() if k not in r
                   and not (k == "baselineMissing" and r.get("baselineId"))}
        out.append({k: v for k, v in {**r, **carried}.items() if not (k in HAND_FIELDS and v is None)})
    seen = {r["id"] for r in new}
    dropped = [r["id"] for r in old if r["id"] not in seen and any(k in r for k in HAND_FIELDS)]
    return out, dropped


def unbind(records, live, owned=False):
    """Drop each baselineId whose baseline is in `live` ({name: set of row ids})
    and no longer holds the row.

    Returns (records, unbound ids). A re-capture on a new reference build
    carries the old matches; a row that build no longer has cannot be matched,
    so the record goes back to unmatched for a person to bind again. A match
    to a baseline that is not there at all is kept: a missing file is not a
    build that dropped its rows, and check-inventory.py refuses the orphan."""
    out, unbound = [], []
    for r in records:
        name, _, row = (r.get("baselineId") or "").partition(":")
        if name in live and row not in live[name]:
            # owned: the capture's own match, kept as None (no value, still the
            # capture's) so merge cannot refill the superseded old match.
            r = {**r, "baselineId": None} if owned else {k: v for k, v in r.items() if k != "baselineId"}
            unbound.append(r["id"])
        out.append(r)
    return out, unbound


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

    def children(self, limit=None):
        kids = self.d.get("children", [])
        if limit is not None and len(kids) > limit:
            raise Incomplete(f"{self.name or self.automation_id}: {len(kids)} children, more than the {limit} left to read")
        return [FixtureElement(c) for c in kids]

    def expand(self, skip=None):
        """The entries, as the live adapter returns them: an entry marked
        "popup" is a popup container, flattened into its children unless `skip`
        accepts it, when it is returned itself."""
        if self.d.get("expandFails"):
            raise Incomplete(f"{self.name or self.automation_id}: the flyout never settled")
        self.open = True
        out = []
        for c in self.d.get("expands", []):
            box = FixtureElement(c)
            if c.get("popup") and not (skip and skip(box)):
                out.extend(FixtureElement(k) for k in c.get("children", []))
            else:
                out.append(box)
        return out

    def collapse(self):
        self.open = False


class _Budget:
    def __init__(self, limit):
        self.left = limit

    def take(self):
        self.left -= 1
        if self.left < 0:
            raise Incomplete(f"more than {MAX_RECORDS} elements to dump; the surface module must name a smaller root")


def dump(el, depth=0, entry=False, environment_lists=(), budget=None, expand=True, hosts=(), omit=()):
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
        # A host is kept whatever it reports: the walk reads through it.
        kids = [dump(k, depth + 1, environment_lists=environment_lists, budget=budget, expand=expand, hosts=hosts,
                     omit=omit)
                for k in el.children()
                if not any(matches(k, spec) for spec in omit)
                and (not excluded(k) or any(matches(k, spec) for spec in hosts))]
        if kids:
            d["children"] = kids
    if expand and el.can_expand and el.control_type in EXPANDABLE and not el.offscreen:
        try:
            skip = (lambda c: any(matches(c, spec) for spec in environment_lists)) if environment_lists else None
            d["expands"] = [dump(k, depth + 1, entry=True, budget=budget) for k in el.expand(skip=skip) if not excluded(k)]
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
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
            for r in records:
                fh.write(json.dumps(r, ensure_ascii=False) + "\n")
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    except BaseException:
        pathlib.Path(tmp).unlink(missing_ok=True)
        raise


def read_records(path):
    with open(path, encoding="utf-8") as fh:
        return [json.loads(line) for line in fh if line.strip()]
