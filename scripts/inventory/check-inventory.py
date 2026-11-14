#!/usr/bin/env python3
"""Validate the Windows 11 inventory under docs/design/inventory/.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 1.

Every docs/design/inventory/<surface>/<file>.json is JSON Lines: one object per
line, no blank lines. Two kinds of file:

  <file>.json            canonical records, validated against schema.json
  <file>.<profile>.json  profile observations: an existing record measured under
                         another capture profile (the schema's
                         $defs/observation), keyed by (id, profile)

A file whose name starts with "baseline" holds completeness-baseline rows, which
section 5 checks; this checker leaves it alone.

Facts only (section 4): a label longer than 80 characters is a sentence, not a
name; no string anywhere in a record or observation holds an http link; a
behaviour is our own description, so it never repeats the label. names.json,
the trade mark names Impossible OS replaces, must list each reference name once
with our name and the reason.

Cross-record rules over the whole tree: a duplicate id, a parent that names no
record, a parent cycle, an order used twice under one parent, an itemCount that
differs from the number of child records, an observation whose id names no
record or whose profile differs from its file name, and two observations of one
id under one profile.

    python3 scripts/inventory/check-inventory.py [--surface NAME] [--json] [--root DIR] [--index]

--index checks the staged inventory instead of the working tree, so a commit is
judged by the bytes it records (the pre-commit lint passes it).

Exit 0 when clean, 1 on any error, 2 on a usage error.
"""
import argparse
import json
import math
import pathlib
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


def classify(path):
    """Return ('record', None), ('observation', profile) or ('baseline', None)."""
    if path.name.startswith("baseline"):
        return "baseline", None
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
            if kind == "baseline" or (kind == "observation" and not mine):
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


def check(root, surface=None):
    schema = json.loads((root / "schema.json").read_text(encoding="utf-8"))
    record_v = Draft202012Validator(schema)
    registry = Registry().with_resource(schema["$id"], Resource.from_contents(schema))
    obs_v = Draft202012Validator({"$ref": schema["$id"] + "#/$defs/observation"}, registry=registry)

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

    errors.sort(key=lambda e: (e["file"], e["line"], e["rule"]))
    return count, len(observations), errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--surface", help="check one folder; parents still resolve across the whole inventory")
    ap.add_argument("--json", action="store_true", help="print a JSON result for a tool")
    ap.add_argument("--index", action="store_true", help="check the staged inventory, not the working tree")
    ap.add_argument("--root", default=str(DEFAULT_ROOT), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
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
    records, obs, errors = check(root, args.surface)
    if args.json:
        print(json.dumps({"records": records, "observations": obs, "errors": errors}, indent=1))
    else:
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
        staged = git("-C", str(top), "ls-files", "-z", "--", rel).split("\0")[:-1]
        scratch = pathlib.Path(tempfile.mkdtemp(prefix="inventory-index-"))
        try:
            if staged:
                git("-C", str(top), "checkout-index", "-f", "--prefix", f"{scratch}/", "--", *staged)
            argv = ["--root", str(scratch / rel)]
            if args.surface:
                argv += ["--surface", args.surface]
            if args.json:
                argv.append("--json")
            return main(argv)
        finally:
            shutil.rmtree(scratch)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as exc:
        detail = (getattr(exc, "stderr", "") or str(exc)).strip()
        print(f"check-inventory: --index cannot read the staged inventory: {detail}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
