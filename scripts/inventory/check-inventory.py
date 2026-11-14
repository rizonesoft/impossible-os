#!/usr/bin/env python3
"""Validate the Windows 11 inventory under docs/design/inventory/.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 1.

Every docs/design/inventory/<surface>/<file>.json is JSON Lines: one object per
line, no blank lines. Two kinds of file:

  <file>.json            canonical records, validated against schema.json
  <file>.<profile>.json  profile observations: an existing record measured under
                         another capture profile (the schema's
                         $defs/observation), keyed by (id, profile)

<baseline>/baseline.json is a completeness baseline (section 5): line 1 its
header, every other line one row with a unique id. The header names a baseline
baseline_core.OWNERS knows, its owner surface, its source, and either the build
it was read from (which must be the build reference.json names) or the upstream
it was imported from. A record's baselineId (<baseline>:<row id>) must name a
row. A record captured from another build than a build-read baseline is stale:
it never counts as a match and its row stays open, without being an error, so
a new reference build is reached by reading the baselines again and then
capturing again. The table under "Which baseline rows are absent?" in REFERENCE.md lists
rows documented as absent, each with a reason. --baselines prints, per
baseline, its rows and how many are matched, absent, pending (the owner surface
has no records yet) or unmatched (the owner was captured and the row was not).

Facts only (section 4): a label longer than 80 characters is a sentence, not a
name; no string anywhere in a record or observation holds an http link; a
behaviour is our own description, so it never repeats the label. names.json,
the trade mark names Impossible OS replaces, must list each reference name once
with our name and the reason.

Decisions (section 6): every record whose kind is not a catalogue kind carries
a disposition, unless --allow-undecided (a fresh capture, before its hand
fields are written). consistency.json, the consistency ledger, is an array of
the schema's $defs/ledgerEntry: each variant names a record, and a flow or
home entry's design names a heading of a docs/design page. A replaced record
is evidence-bound: its inconsistency names an entry with two or more variants,
and the record is one of them or sits under one. Nothing is lost by replacing:
a replaced setting is mergedInto a setting, a replaced page or dialogTab into a
page, the target neither replaced nor excluded; and a built record whose
captured home is replaced or excluded names where it lives in placedUnder (a
page or group that is itself built), or sits under a record that does. A place
or a merge target is followed to its end: it never sits under a replaced or
excluded record and never leads back to itself. A home entry's placement maps a
record of the replaced home (a Control Panel category) to the built page that
takes what has no page of its own.

Cross-record rules over the whole tree: a duplicate id, a parent that names no
record, a parent cycle, an order used twice under one parent, an itemCount that
differs from the number of child records, an observation whose id names no
record or whose profile differs from its file name, and two observations of one
id under one profile.

    python3 scripts/inventory/check-inventory.py [--surface NAME] [--baselines] [--json] [--root DIR] [--index]
                                                 [--allow-undecided] [--design-root DIR]

--design-root is the folder a ledger design is read from: the parent of the
inventory unless given (a scratch copy of the inventory alone names the real one).

--index checks the staged inventory instead of the working tree, so a commit is
judged by the bytes it records (the pre-commit lint passes it); the design pages
are read as staged too.

Exit 0 when clean, 1 on any error, 2 on a usage error.
"""
import argparse
import importlib.util
import json
import math
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
except ImportError:  # fail-direction: a missing validator is an error, never a pass
    print("check-inventory: the jsonschema package is required (scripts/setup-deps.sh installs it)", file=sys.stderr)
    sys.exit(2)

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO / "docs" / "design" / "inventory"
PRINT_CAP = 50
LABEL_MAX = 80
LINK_MARKS = ("http://", "https://")
GIT_TIMEOUT = 120  # deadline: the staged inventory is a few hundred small files
NAME_FIELDS = ("reference", "ours", "reason")
BASELINE_FILE = "baseline.json"
ABSENT_TITLE = "Which baseline rows are absent?"
ABSENT_COLUMNS = ["Baseline", "Row", "Build", "Reason"]
ABSENT_BUILD = re.compile(r"^\d+\.\d+$")
HEADER_FIELDS = {"baseline", "owner", "source", "build", "upstream", "files"}
COMMIT = re.compile(r"^[0-9a-f]{40}$")
DELIMITER = re.compile(r"^:?-{3,}:?$")
LEDGER_FILE = "consistency.json"
# Kinds that record a measurement or a binding, not a thing to build or leave out.
CATALOGUE_KINDS = frozenset({"shortcut", "gesture", "metric", "motion", "sound", "cursor", "state"})
# A replaced record of the key kind is held by a record of the value kind.
MERGE_KINDS = {"setting": "setting", "page": "page", "dialogTab": "page"}
PLACE_KINDS = ("page", "group")
GONE = ("replaced", "excluded")

_spec = importlib.util.spec_from_file_location("baseline_core", pathlib.Path(__file__).resolve().parent / "baseline_core.py")
baseline_core = importlib.util.module_from_spec(_spec)
_fence = importlib.util.spec_from_file_location("todo_fence", pathlib.Path(__file__).resolve().parents[1] / "todo_fence.py")
todo_fence = importlib.util.module_from_spec(_fence)
_fence.loader.exec_module(todo_fence)
_spec.loader.exec_module(baseline_core)


def strings(value, at=""):
    """Yield (json path, string) for every string inside a parsed record."""
    if isinstance(value, str):
        yield at or "(record)", value
    elif isinstance(value, dict):
        for k, v in value.items():
            yield from strings(v, f"{at}/{k}" if at else k)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from strings(v, f"{at}/{i}" if at else str(i))


def fact_errors(obj, kind):
    """Yield (rule, message) for each section 4 rule a parsed object breaks."""
    rid = obj.get("id", "(no id)")
    for at, text in strings(obj):
        if any(m in text.lower() for m in LINK_MARKS):
            yield "link", f"{rid}: link in record at {at}; links to Microsoft are never recorded"
    if kind != "record":
        return
    label = obj["label"]
    if len(label) > LABEL_MAX:
        yield "label-sentence", f"{rid}: label is a sentence ({len(label)} characters; a label is a name of {LABEL_MAX} or fewer)"
    behaviour = obj.get("behaviour")
    if behaviour is not None and behaviour.strip().casefold() == label.strip().casefold():
        yield "behaviour-is-label", f"{rid}: behaviour repeats the label; describe what the control does, in our own words"


def check_names(root, err):
    path = root / "names.json"
    if not path.exists():
        return
    try:
        names = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        err(path, 1, "names", f"not JSON: {exc}")
        return
    if not isinstance(names, list) or not names:
        err(path, 1, "names", "names.json is a non-empty array of {reference, ours, reason}")
        return
    seen = {}
    for i, entry in enumerate(names):
        where = f"entry {i}"
        if not isinstance(entry, dict) or set(entry) != set(NAME_FIELDS) or not all(
                isinstance(entry[f], str) and entry[f].strip() for f in NAME_FIELDS):
            err(path, 1, "names", f"{where}: needs exactly reference, ours and reason, each a non-empty string")
            continue
        key = entry["reference"].strip().casefold()
        if key in seen:
            err(path, 1, "names", f"{where}: {entry['reference']} is also listed as entry {seen[key]}")
        seen.setdefault(key, i)
        if entry["ours"].strip().casefold() == key:
            err(path, 1, "names", f"{where}: our name for {entry['reference']} is the same name")


_site = None


def design_anchors(path):
    """The heading and anchor ids of a design page, by the site build's own rule
    (scripts/site/build.py), so a ledger link resolves exactly as a docs link does."""
    global _site
    if _site is None:
        here = pathlib.Path(__file__).resolve().parents[1] / "site"
        sys.path.insert(0, str(here))
        try:
            spec = importlib.util.spec_from_file_location("site_build", here / "build.py")
            _site = importlib.util.module_from_spec(spec)
            sys.modules["site_build"] = _site   # its dataclasses look their module up by name
            spec.loader.exec_module(_site)
        finally:
            sys.path.remove(str(here))
    # No cache file: a ledger names a handful of small pages, parsed once each.
    return _site.AnchorCache(pathlib.Path(tempfile.gettempdir()) / "inventory-no-anchor-cache").anchors(
        path.read_text(encoding="utf-8"))


def load_ledger(root, design_root, entry_v, err, report):
    """{entry id: entry} from consistency.json; faults are reported when `report`."""
    path = root / LEDGER_FILE
    out = {}
    if not path.is_file():
        return out

    def fault(message):
        if report:
            err(path, 1, "ledger", message)

    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        fault(f"not JSON: {exc}")
        return out
    if not isinstance(entries, list):
        fault(f"{LEDGER_FILE} is an array of ledger entries")
        return out
    pages = {}
    for i, entry in enumerate(entries):
        name = entry.get("id") if isinstance(entry, dict) else None
        name = name if isinstance(name, str) else f"entry {i}"
        bad = sorted(entry_v.iter_errors(entry), key=lambda e: list(e.absolute_path))
        if bad:
            for e in bad:
                at = "/".join(str(x) for x in e.absolute_path) or "(entry)"
                fault(f"{name}: {at}: {e.message}")
            continue
        if name in out:
            fault(f"{name} is defined twice")
            continue
        out[name] = entry
        if "design" in entry:
            page, _, anchor = entry["design"].partition("#")
            if page not in pages:
                target = design_root / page
                try:
                    pages[page] = design_anchors(target) if target.is_file() else None
                except (OSError, UnicodeDecodeError):
                    pages[page] = None
            if pages[page] is None:
                fault(f"{name}: design names {page}, which is not a page under {design_root.name}/")
            elif anchor not in pages[page]:
                fault(f"{name}: design names #{anchor}, which {page} does not have")
    return out


def classify(path):
    """Return ('record', None), ('observation', profile), ('baseline', None) or
    ('bad-baseline', None) for a baseline* name other than baseline.json."""
    if path.name.startswith("baseline"):
        return ("baseline" if path.name == BASELINE_FILE else "bad-baseline"), None
    stem = path.name[: -len(".json")]
    if "." in stem:
        return "observation", stem.rsplit(".", 1)[1]
    return "record", None


class NonFinite(ValueError):
    pass


def _constant(name):
    raise NonFinite(name)


def _float(text):
    value = float(text)
    if not math.isfinite(value):
        raise NonFinite(text)
    return value


def load(root, surface):
    """Yield (path, line, kind, profile, in scope, object or None, error or None).

    Every folder's canonical records are read, because parents and observations
    resolve across the whole tree. Observation files outside the selected surface
    are skipped unopened: nothing in scope can depend on them.
    """
    for folder in sorted(p for p in root.iterdir() if p.is_dir()):
        mine = surface is None or folder.name == surface
        for path in sorted(folder.glob("*.json")):
            kind, profile = classify(path)
            if kind in ("baseline", "bad-baseline") or (kind == "observation" and not mine):
                continue
            with path.open(encoding="utf-8") as fh:
                for n, line in enumerate(fh, 1):
                    if not line.strip():
                        yield path, n, kind, profile, mine, None, "blank-line: a record file holds one object per line and no blank lines"
                        continue
                    try:
                        obj = json.loads(line, parse_constant=_constant, parse_float=_float)
                    except NonFinite as exc:
                        yield path, n, kind, profile, mine, None, f"non-finite: {exc} is not a finite JSON number"
                        continue
                    except json.JSONDecodeError as exc:
                        yield path, n, kind, profile, mine, None, f"json: {exc.msg}"
                        continue
                    yield path, n, kind, profile, mine, obj, None


def parse_line(line):
    """(object, None) or (None, error) for one JSON Lines line."""
    if not line.strip():
        return None, "blank-line: a record file holds one object per line and no blank lines"
    try:
        return json.loads(line, parse_constant=_constant, parse_float=_float), None
    except NonFinite as exc:
        return None, f"non-finite: {exc} is not a finite JSON number"
    except json.JSONDecodeError as exc:
        return None, f"json: {exc.msg}"


def header_errors(h, name, build):
    """Messages for each rule a baseline header breaks."""
    if not isinstance(h, dict):
        yield "line 1 is the header object"
        return
    extra = set(h) - HEADER_FIELDS
    if extra:
        yield f"unknown header field(s): {', '.join(sorted(extra))}"
    if h.get("baseline") != name:
        yield f"header baseline is {h.get('baseline')!r}, but the file is {name}/{BASELINE_FILE}"
    want = baseline_core.OWNERS.get(name)
    if want is None:
        yield f"{name} is not a baseline baseline_core.OWNERS knows"
    elif h.get("owner") != want:
        yield f"owner is {h.get('owner')!r}; the {name} baseline is accounted for by surface {want}"
    if not isinstance(h.get("source"), str) or not h["source"].strip():
        yield "source is a non-empty string naming what was read"
    if ("build" in h) == ("upstream" in h):
        yield "a header has exactly one of build (read from the reference machine) and upstream (imported)"
    elif name in baseline_core.VM_BASELINES and "build" not in h:
        yield f"{name} is read from the reference machine, so its header names the build, not an upstream"
    elif name in baseline_core.OWNERS and name not in baseline_core.VM_BASELINES and "upstream" not in h:
        yield f"{name} is imported, so its header names the upstream, not a build"
    elif "build" in h:
        if build is None:
            yield "the header names a build but there is no reference.json to compare it with"
        elif h["build"] != build:
            yield f"read from build {h['build']}, but reference.json names {build}: read the baseline again"
    else:
        up = h["upstream"]
        if not (isinstance(up, dict) and set(up) == {"repo", "commit", "license"}
                and all(isinstance(up[k], str) and up[k] for k in up) and COMMIT.match(up["commit"])):
            yield "upstream is {repo, commit (40 hex digits), license}"
    files = h.get("files", [])
    if not (isinstance(files, list) and all(isinstance(f, dict) and isinstance(f.get("path"), str)
                                             and isinstance(f.get("sha256"), str) for f in files)):
        yield "files is a list of {path, sha256}"


def load_baselines(root, surface, build, err):
    """{name: {"header", "rows": {id: line}, "path", "mine"}} for every baseline file."""
    out = {}
    for path in sorted(root.glob("*/baseline*.json")):
        # A baseline belongs to the surface that accounts for it (known-folders
        # to explorer), and to its own folder when it is named as the surface.
        folder = path.parent.name
        mine = surface is None or surface in (folder, baseline_core.OWNERS.get(folder))
        if path.name != BASELINE_FILE:
            if mine:
                err(path, 1, "baseline-name", f"a baseline file is named {BASELINE_FILE}, not {path.name}")
            continue
        name = path.parent.name
        header, rows = None, {}
        with path.open(encoding="utf-8") as fh:
            for n, line in enumerate(fh, 1):
                obj, error = parse_line(line)
                if error:
                    if mine:
                        err(path, n, error.split(":", 1)[0], error)
                    continue
                if n == 1:
                    header = obj
                    if mine:
                        for message in header_errors(obj, name, build):
                            err(path, 1, "baseline-header", message)
                    continue
                rid = obj.get("id") if isinstance(obj, dict) else None
                if not isinstance(rid, str) or not rid:
                    if mine:
                        err(path, n, "baseline-row", "a row is an object with a non-empty string id")
                    continue
                if rid in rows:
                    if mine:
                        err(path, n, "baseline-duplicate", f"row {rid} is also at line {rows[rid]}")
                    continue
                rows[rid] = n
                if mine:
                    for at, text in strings(obj):
                        if any(m in text.lower() for m in LINK_MARKS):
                            err(path, n, "link", f"{rid}: link in row at {at}; links to Microsoft are never recorded")
        if mine and header is not None and not rows:
            err(path, 1, "baseline-empty", f"{name} has a header and no rows")
        if mine and header is None:
            err(path, 1, "baseline-header", "the file is empty; line 1 is the header")
        out[name] = {"header": header if isinstance(header, dict) else {}, "rows": rows, "path": path, "mine": mine}
    return out


def load_absent(root, baselines, err, surface, build):
    """({(baseline, row): (line, reason)}, {(baseline, row)} gone stale) from the
    REFERENCE.md absent table.

    Only a sound entry is returned, so an entry that names nothing, gives no
    reason or repeats another never counts a row as absent. An entry names the
    build the absence was checked on; one checked on another build than
    `build` (reference.json's) is stale, like a match from another build: it
    counts nothing and the row is open until it is checked again, without
    being an error. An entry is reported
    under the surface that owns its baseline, as the baseline itself is; a
    table that cannot be read is reported under every surface."""
    def scoped(name):
        return surface is None or surface in (name, baseline_core.OWNERS.get(name))

    path = root / "REFERENCE.md"
    out, old = {}, set()
    if not path.is_file():
        return out, old
    lines = path.read_text(encoding="utf-8").split("\n")
    # What a heading is, and where a section ends, is the repository's one
    # shared rule (scripts/todo_fence.py): it allows CommonMark's indent, which
    # a column-0 prefix test misses.
    start = next((n for n, line in enumerate(lines) if todo_fence.h2_title(line) == ABSENT_TITLE), None)
    if start is None:
        return out, old
    table = []
    for n in range(start + 1, len(lines)):
        if todo_fence.is_h2(lines[n]):
            break
        if lines[n].startswith("|"):
            table.append((n + 1, [c.strip() for c in lines[n].strip().strip("|").split("|")]))
    if not table:
        return out, old
    if table[0][1] != ABSENT_COLUMNS:
        err(path, table[0][0], "absent-table", f"the absent table's columns are {' | '.join(ABSENT_COLUMNS)}")
        return out, old
    if len(table) < 2 or len(table[1][1]) != 4 or not all(DELIMITER.match(c) for c in table[1][1]):
        err(path, table[0][0], "absent-table", "the header row is followed by a delimiter row (| --- | --- | --- | --- |)")
        return out, old
    first = {}
    for n, cells in table[2:]:
        if len(cells) != 4:
            err(path, n, "absent-table", "an absent row has four cells: baseline, row, build, reason")
            continue
        name, row, checked, reason = (c.strip("`") for c in cells)
        problem = None
        if name not in baselines:
            problem = "absent-orphan", f"{name} is not a baseline"
        elif not ABSENT_BUILD.match(checked):
            problem = "absent-build", f"{name}:{row} names no build the absence was checked on (like 26300.9711)"
        elif checked != build:
            # Staleness is judged before the row, as for a match: a row the new
            # build no longer has is not this old entry's error.
            old.add((name, row))
            continue
        elif row not in baselines[name]["rows"]:
            problem = "absent-orphan", f"{name} has no row {row}"
        elif not reason.strip():
            problem = "absent-reason", f"{name}:{row} is listed absent without a reason"
        elif (name, row) in first:
            problem = "absent-duplicate", f"{name}:{row} is also listed at line {first[(name, row)]}"
        first.setdefault((name, row), n)
        if problem:
            if scoped(name):
                err(path, n, *problem)
            continue
        out[(name, row)] = (n, reason)
    return out, old


def check(root, surface=None, allow_undecided=False, design_root=None):
    schema = json.loads((root / "schema.json").read_text(encoding="utf-8"))
    record_v = Draft202012Validator(schema)
    registry = Registry().with_resource(schema["$id"], Resource.from_contents(schema))
    obs_v = Draft202012Validator({"$ref": schema["$id"] + "#/$defs/observation"}, registry=registry)
    entry_v = Draft202012Validator({"$ref": schema["$id"] + "#/$defs/ledgerEntry"}, registry=registry)

    errors = []

    def err(path, line, rule, message):
        errors.append({"file": str(path.relative_to(root)), "line": line, "rule": rule, "message": message})

    def where(path, line):
        return f"{path.relative_to(root)}:{line}"

    def peers(members, me, show):
        # One named peer and a count: a group of n lines costs O(n), not O(n^2).
        other = next(m for m in members if m != me)
        more = len(members) - 2
        return show(other) + (f" and {more} more" if more else "")

    by_id = {}       # id -> [(path, line, obj, in scope)], file order
    observations = []
    count = 0
    for path, n, kind, profile, mine, obj, parse_error in load(root, surface):
        if parse_error:
            if mine:
                err(path, n, parse_error.split(":", 1)[0], parse_error)
            continue
        validator = record_v if kind == "record" else obs_v
        schema_errors = sorted(validator.iter_errors(obj), key=lambda e: list(e.absolute_path))
        if schema_errors:
            if mine:
                for e in schema_errors:
                    at = "/".join(str(x) for x in e.absolute_path) or "(record)"
                    err(path, n, "schema", f"{at}: {e.message}")
            continue
        if mine:
            for rule, message in fact_errors(obj, kind):
                err(path, n, rule, message)
        if kind == "record":
            by_id.setdefault(obj["id"], []).append((path, n, obj, mine))
            count += mine
        else:
            observations.append((path, n, profile, obj))

    # A conflict is reported at every in-scope member of it, so checking one
    # surface finds a clash with another surface whichever side sorts first.
    for rid, members in by_id.items():
        if len(members) > 1:
            for member in members:
                path, n, _, mine = member
                if mine:
                    err(path, n, "duplicate-id", f"{rid} is also defined at {peers(members, member, lambda m: where(m[0], m[1]))}")
    records = {rid: members[0] for rid, members in by_id.items()}

    children = {}
    for rid, (path, n, obj, mine) in records.items():
        parent = obj.get("parent")
        if parent is None:
            continue
        children.setdefault(parent, []).append(rid)
        if parent not in records and mine:
            err(path, n, "orphan-parent", f"{rid}: parent {parent} names no record")

    # Cycles: walk each chain once; a node met again on the same walk closes a
    # cycle, which is reported at its first in-scope member.
    state = {}
    for start in records:
        walk = []
        node = start
        while node in records and node not in state:
            state[node] = 1
            walk.append(node)
            node = records[node][2].get("parent")
        if node in records and state[node] == 1:
            cycle = walk[walk.index(node):]
            at = next((c for c in cycle if records[c][3]), None)
            if at is not None:
                path, n, _, _ = records[at]
                i = cycle.index(at)
                err(path, n, "parent-cycle", " -> ".join(cycle[i:] + cycle[:i] + [at]))
        for w in walk:
            state[w] = 2

    for parent, kids in children.items():
        by_order = {}
        for rid in kids:
            by_order.setdefault(records[rid][2]["order"], []).append(rid)
        for order, same in by_order.items():
            if len(same) > 1:
                for rid in same:
                    path, n, _, mine = records[rid]
                    if mine:
                        err(path, n, "duplicate-order", f"{rid}: order {order} under {parent} is also used by {peers(same, rid, str)}")

    for rid, (path, n, obj, mine) in records.items():
        if "itemCount" in obj and mine:
            have = len(children.get(rid, []))
            if have != obj["itemCount"]:
                err(path, n, "item-count", f"{rid}: itemCount {obj['itemCount']} but {have} child record(s) present")

    # Observations are read only from the selected surface, so every one is in scope.
    seen = {}
    for path, n, profile, obj in observations:
        if obj["captureProfile"] != profile:
            err(path, n, "observation-profile", f"{obj['id']}: captureProfile {obj['captureProfile']} in a file named for {profile}")
        if obj["id"] not in records:
            err(path, n, "observation-orphan", f"{obj['id']} names no record")
        seen.setdefault((obj["id"], obj["captureProfile"]), []).append((path, n))
    for (oid, prof), members in seen.items():
        if len(members) > 1:
            for member in members:
                path, n = member
                err(path, n, "duplicate-observation", f"{oid} at {prof} is also observed at {peers(members, member, lambda m: where(*m))}")

    if surface is None:
        check_names(root, err)

    # Decisions: the ledger, then what each record's disposition must prove.
    ledger = load_ledger(root, design_root or root.parent, entry_v, err, surface is None)
    variants = {}
    for name, entry in ledger.items():
        variants[name] = {v for v in entry["variants"] if v in records}
        if surface is None:
            for v in entry["variants"]:
                if v not in records:
                    err(root / LEDGER_FILE, 1, "ledger-variant", f"{name}: variant {v} names no record")

    def disposition(rid):
        return records[rid][2].get("disposition")

    # Where a record ends up in Impossible OS: follow placedUnder where a record
    # has one and its captured parent otherwise. "gone" when that chain meets a
    # replaced or excluded record (nothing built holds it), "cycle" when it never
    # ends (a place under itself, or under what sits under it), "missing" when it
    # names a record that does not exist, else "ok". One
    # walk per chain, and a set beside the walk so a long chain stays linear.
    fate = {}

    def fate_of(rid):
        walk, on_walk = [], set()
        node = rid
        while node in records and node not in fate:
            if node in on_walk:
                for w in walk:
                    fate[w] = "cycle"
                break
            obj = records[node][2]
            if obj.get("disposition") in GONE:
                fate[node] = "gone"
                break
            walk.append(node)
            on_walk.add(node)
            node = obj.get("placedUnder", obj.get("parent"))
        # A chain ends at a record with no parent. One that names a record which
        # does not exist ends nowhere, whichever surface the bad name is in.
        found = fate.get(node, "ok" if node is None else "missing")
        for w in walk:
            fate.setdefault(w, found)
        return fate.get(rid, "missing")

    under = {}   # (entry, record) -> the record is a variant of the entry or sits under one

    def under_variant(name, rid):
        walk, on_walk = [], set()
        node = rid
        while node in records and (name, node) not in under and node not in on_walk:
            if node in variants[name]:
                under[(name, node)] = True
                break
            walk.append(node)
            on_walk.add(node)
            node = records[node][2].get("parent")
        found = under.get((name, node), False)
        for w in walk:
            under[(name, w)] = found
        return under.get((name, rid), False)

    def unbuilt(target):
        """Why nothing built holds `target`, or None."""
        if disposition(target) in GONE:
            return f"which is itself {disposition(target)}"
        if fate_of(target) == "cycle":
            return "whose place leads back to itself"
        if fate_of(target) == "gone":
            return "which sits under a replaced or excluded record"
        if fate_of(target) == "missing":
            return "whose place or parent names no record"
        return None

    if surface is None:
        for name, entry in ledger.items():
            for source, page in entry.get("placement", {}).items():
                for rid in (source, page):
                    if rid not in records:
                        err(root / LEDGER_FILE, 1, "ledger-placement", f"{name}: placement names {rid}, which is no record")
                if page in records and (records[page][2]["kind"] != "page" or unbuilt(page)):
                    err(root / LEDGER_FILE, 1, "ledger-placement", f"{name}: placement sends {source} to {page}, which is not a built page")

    for rid, (path, n, obj, mine) in records.items():
        if not mine:
            continue
        state = obj.get("disposition")
        if state is None and obj["kind"] not in CATALOGUE_KINDS and not allow_undecided:
            err(path, n, "disposition-missing", f"{rid}: a {obj['kind']} record carries a disposition (build, equivalent, replaced or excluded)")
        # What each disposition needs. Here and not in the schema: a schema
        # conditional is interpreted for every record (measured: half again the time).
        if (state == "excluded") != ("reason" in obj):
            err(path, n, "disposition-fields", f"{rid}: an excluded record gives its reason, and only an excluded record has one")
        if state == "replaced" and "inconsistency" not in obj:
            err(path, n, "disposition-fields", f"{rid}: a replaced record names its ledger entry in inconsistency")
        if state == "replaced" and obj["kind"] in MERGE_KINDS and "mergedInto" not in obj:
            err(path, n, "disposition-fields", f"{rid}: a replaced {obj['kind']} names the record that holds it in mergedInto")
        if state != "replaced" and "mergedInto" in obj:
            err(path, n, "disposition-fields", f"{rid}: only a replaced record is mergedInto another")
        if state in GONE and "placedUnder" in obj:
            err(path, n, "disposition-fields", f"{rid}: a {state} record is not built, so it has no placedUnder")
        name = obj.get("inconsistency")
        if name is None:
            pass
        elif name not in ledger:
            err(path, n, "inconsistency-undefined", f"{rid}: inconsistency {name} is not an entry of {LEDGER_FILE}")
        elif state == "replaced":
            if len(variants[name]) < 2:
                err(path, n, "replaced-evidence", f"{rid}: replaced under {name}, which names {len(variants[name])} captured variant(s); "
                    "a record is replaced only where two captured records disagree")
            elif not under_variant(name, rid):
                err(path, n, "replaced-evidence", f"{rid}: replaced under {name}, but it is not a variant of that entry and sits under none")
        target = obj.get("mergedInto")
        if target is not None:
            want = MERGE_KINDS.get(obj["kind"])
            if target not in records:
                err(path, n, "merged-target", f"{rid}: mergedInto {target} names no record")
            elif unbuilt(target):
                err(path, n, "merged-target", f"{rid}: mergedInto {target}, {unbuilt(target)}; nothing holds the option")
            elif want is None:
                err(path, n, "merged-target", f"{rid}: only a page, dialogTab or setting is mergedInto another record")
            elif records[target][2]["kind"] != want:
                err(path, n, "merged-target", f"{rid}: a replaced {obj['kind']} is held by a {want}, and {target} is a {records[target][2]['kind']}")
        place = obj.get("placedUnder")
        if place is not None:
            if place not in records:
                err(path, n, "placed-target", f"{rid}: placedUnder {place} names no record")
            elif records[place][2]["kind"] not in PLACE_KINDS:
                err(path, n, "placed-target", f"{rid}: placedUnder names a page or group, and {place} is a {records[place][2]['kind']}")
            elif fate_of(place) == "cycle":
                err(path, n, "placed-target", f"{rid}: placedUnder {place} leads back to itself; a place ends at a built surface")
            elif unbuilt(place):
                err(path, n, "placed-target", f"{rid}: placedUnder {place}, {unbuilt(place)}; a place is built")
        elif state in ("build", "equivalent") and obj["kind"] not in CATALOGUE_KINDS and fate_of(rid) == "gone":
            err(path, n, "unplaced", f"{rid}: built, but its captured home is replaced or excluded; name where it lives in placedUnder")

    # Baselines: every row is matched by a record, documented absent, or open.
    ref = root / "reference.json"
    build = json.loads(ref.read_text(encoding="utf-8")).get("build") if ref.is_file() else None
    baselines = load_baselines(root, surface, build, err)
    matched = {name: set() for name in baselines}
    stale = {name: set() for name in baselines}
    for rid, (path, n, obj, mine) in records.items():
        if obj.get("baselineMissing") and "baselineId" in obj:
            if mine:
                err(path, n, "baseline-both", f"{rid}: baselineMissing says no row lists it, but it names {obj['baselineId']}")
            continue
        if "baselineId" not in obj:
            continue
        name, _, row = obj["baselineId"].partition(":")
        b = baselines.get(name)
        orphan = f"{rid}: baselineId {obj['baselineId']} names no baseline row"
        if b is None:
            if mine:
                err(path, n, "baseline-orphan", orphan)
            continue
        # Against the reference build, for every baseline: an imported one
        # (controls) has no build of its own, and the capture is what must be current.
        if build is not None and obj["capturedFrom"] != build:
            # Staleness is judged before the row: a row the new build no longer
            # has is not this old record's error, and the re-capture unbinds it.
            # A capture of another build is stale evidence: it never counts, and
            # the row stays open until the surface is captured again. It is not an
            # error, or a new reference build could be reached from neither side:
            # the new baselines would fail on the old records, and a re-capture
            # on the old baselines.
            stale[name].add(row)
            continue
        if row not in b["rows"]:
            if mine:
                err(path, n, "baseline-orphan", orphan)
            continue
        # Any surface's record may match a row (the settings index lists the classic
        # Control Panel entries, which are captured under control-panel); the owner
        # is who answers for the rows still open, not who alone may match one.
        matched[name].add(row)
    absent, old = load_absent(root, baselines, err, surface, build)
    for name, row in old:
        stale[name].add(row)
    for (name, row), (n, _) in list(absent.items()):
        if row in matched[name]:
            # A matched row is counted once, as matched.
            del absent[(name, row)]
            if baselines[name]["mine"]:
                err(root / "REFERENCE.md", n, "absent-matched", f"{name}:{row} is listed absent and a record matches it")
    captured = {p.parent.name for p, _, obj, _ in records.values()}
    summary = {}
    for name, b in baselines.items():
        rows = set(b["rows"])
        gone = {r for (bn, r) in absent if bn == name} & rows
        left = len(rows - matched[name] - gone)
        owner = b["header"].get("owner")
        open_ = left if owner in captured else 0
        summary[name] = {"owner": owner, "rows": len(rows), "matched": len(matched[name]), "absent": len(gone),
                         "pending": left - open_, "unmatched": open_, "stale": len((stale[name] & rows) - matched[name] - gone),
                         # Old evidence for a row this build no longer has: nothing to count, but
                         # named, so the entry is reconciled instead of forgotten.
                         "removed": sorted(stale[name] - rows)}

    errors.sort(key=lambda e: (e["file"], e["line"], e["rule"]))
    return count, len(observations), errors, summary


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--surface", help="check one folder; parents still resolve across the whole inventory")
    ap.add_argument("--json", action="store_true", help="print a JSON result for a tool")
    ap.add_argument("--baselines", action="store_true", help="print each baseline's rows: matched, absent, pending, unmatched")
    ap.add_argument("--index", action="store_true", help="check the staged inventory, not the working tree")
    ap.add_argument("--allow-undecided", action="store_true",
                    help="accept a record with no disposition yet (a fresh capture, before its hand fields)")
    ap.add_argument("--design-root", help="the folder a ledger design is read from (default: the inventory's parent)")
    ap.add_argument("--root", default=str(DEFAULT_ROOT), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    if args.index and args.design_root:
        # The staged design pages are the ones beside the staged inventory; another
        # folder would be judged by working-tree bytes, which --index exists to rule out.
        print("check-inventory: --index reads the design pages staged beside the inventory; it takes no --design-root", file=sys.stderr)
        return 2
    if args.index:
        return from_index(args)
    root = pathlib.Path(args.root)
    if not (root / "schema.json").is_file():
        print(f"check-inventory: {root}/schema.json not found", file=sys.stderr)
        return 2
    if args.surface is not None and (pathlib.PurePath(args.surface).name != args.surface or args.surface in ("", ".", "..")):
        print(f"check-inventory: --surface takes a folder name such as taskbar, not a path: {args.surface!r}", file=sys.stderr)
        return 2
    if args.surface and not (root / args.surface).is_dir():
        print(f"check-inventory: no surface folder {args.surface}", file=sys.stderr)
        return 2
    design_root = pathlib.Path(args.design_root) if args.design_root else None
    records, obs, errors, summary = check(root, args.surface, args.allow_undecided, design_root)
    if args.baselines:
        for name in sorted(set(baseline_core.OWNERS) - set(summary)):
            errors.append({"file": f"{name}/{BASELINE_FILE}", "line": 0, "rule": "baseline-missing",
                           "message": f"no {name} baseline: run scripts/inventory/baselines.py"})
    if args.json:
        out = {"records": records, "observations": obs, "errors": errors}
        if args.baselines:
            out["baselines"] = summary
        print(json.dumps(out, indent=1))
    else:
        if args.baselines:
            for name, c in sorted(summary.items()):
                print(f"baseline {name}: {c['rows']} rows, {c['matched']} matched, {c['absent']} absent, "
                      f"{c['pending']} pending, {c['unmatched']} unmatched (owner {c['owner']})"
                      + (f"; {c['stale']} of the open rows have a match or an absence from another build, "
                         "capture or check again" if c["stale"] else "")
                      + (f"; evidence from another build names {len(c['removed'])} row(s) this build no longer has: "
                         f"{', '.join(c['removed'])}" if c["removed"] else ""))
        for e in errors[:PRINT_CAP]:
            print(f"{e['file']}:{e['line']}: {e['rule']}: {e['message']}")
        if len(errors) > PRINT_CAP:
            print(f"... {len(errors) - PRINT_CAP} more error(s)")
        print(f"inventory: {records} records, {len(errors)} errors")
    return 1 if errors else 0


def from_index(args):
    """Check the inventory as staged: written out of the index into a scratch tree."""
    root = pathlib.Path(args.root).resolve()

    def git(*cmd):
        return subprocess.run(["git", *cmd], capture_output=True, text=True, check=True, timeout=GIT_TIMEOUT).stdout

    try:
        top = pathlib.Path(git("-C", str(root.parent), "rev-parse", "--show-toplevel").strip())
        rel = root.relative_to(top).as_posix()
        # The design pages beside the inventory are staged bytes too: a ledger
        # design is judged against the page this commit records.
        pages = f":(glob){root.parent.relative_to(top).as_posix()}/*.md"
        staged = git("-C", str(top), "ls-files", "-z", "--", rel, pages).split("\0")[:-1]
        scratch = pathlib.Path(tempfile.mkdtemp(prefix="inventory-index-"))
        try:
            if staged:
                git("-C", str(top), "checkout-index", "-f", "--prefix", f"{scratch}/", "--", *staged)
            argv = ["--root", str(scratch / rel)]
            if args.surface:
                argv += ["--surface", args.surface]
            if args.json:
                argv.append("--json")
            if args.baselines:
                argv.append("--baselines")
            if args.allow_undecided:
                argv.append("--allow-undecided")
            return main(argv)
        finally:
            shutil.rmtree(scratch)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as exc:
        detail = (getattr(exc, "stderr", "") or str(exc)).strip()
        print(f"check-inventory: --index cannot read the staged inventory: {detail}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
