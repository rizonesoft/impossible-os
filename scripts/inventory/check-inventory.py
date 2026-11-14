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

Cross-record rules over the whole tree: a duplicate id, a parent that names no
record, a parent cycle, an order used twice under one parent, an itemCount that
differs from the number of child records, an observation whose id names no
record or whose profile differs from its file name, and two observations of one
id under one profile.

    python3 scripts/inventory/check-inventory.py [--surface NAME] [--json] [--root DIR]

Exit 0 when clean, 1 on any error, 2 on a usage error.
"""
import argparse
import json
import math
import pathlib
import sys

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource
except ImportError:  # fail-direction: a missing validator is an error, never a pass
    print("check-inventory: the jsonschema package is required (scripts/setup-deps.sh installs it)", file=sys.stderr)
    sys.exit(2)

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO / "docs" / "design" / "inventory"
PRINT_CAP = 50


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
        if kind == "record":
            by_id.setdefault(obj["id"], []).append((path, n, obj, mine))
            count += mine
        else:
            observations.append((path, n, profile, obj))

    # A conflict is reported at every in-scope member of it, so checking one
    # surface finds a clash with another surface whichever side sorts first.
    for rid, members in by_id.items():
        if len(members) > 1:
            for path, n, _, mine in members:
                if mine:
                    others = ", ".join(where(p, m) for p, m, _, _ in members if (p, m) != (path, n))
                    err(path, n, "duplicate-id", f"{rid} is also defined at {others}")
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
                        others = ", ".join(o for o in same if o != rid)
                        err(path, n, "duplicate-order", f"{rid}: order {order} under {parent} is also used by {others}")

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
            for path, n in members:
                others = ", ".join(where(p, m) for p, m in members if (p, m) != (path, n))
                err(path, n, "duplicate-observation", f"{oid} at {prof} is also observed at {others}")

    errors.sort(key=lambda e: (e["file"], e["line"], e["rule"]))
    return count, len(observations), errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--surface", help="check one folder; parents still resolve across the whole inventory")
    ap.add_argument("--json", action="store_true", help="print a JSON result for a tool")
    ap.add_argument("--root", default=str(DEFAULT_ROOT), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    root = pathlib.Path(args.root)
    if not (root / "schema.json").is_file():
        print(f"check-inventory: {root}/schema.json not found", file=sys.stderr)
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


if __name__ == "__main__":
    sys.exit(main())
