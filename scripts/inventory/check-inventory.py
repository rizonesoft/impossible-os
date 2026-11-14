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
import pathlib
import sys

try:
    from jsonschema import Draft202012Validator
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


def load(root, surface):
    """Yield (path, line number, object or None, error text or None)."""
    folders = [root / surface] if surface else sorted(p for p in root.iterdir() if p.is_dir())
    for folder in folders:
        for path in sorted(folder.glob("*.json")):
            kind, profile = classify(path)
            if kind == "baseline":
                continue
            with path.open(encoding="utf-8") as fh:
                for n, line in enumerate(fh, 1):
                    if not line.strip():
                        yield path, n, kind, profile, None, "blank-line: a record file holds one object per line and no blank lines"
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError as exc:
                        yield path, n, kind, profile, None, f"json: {exc.msg}"
                        continue
                    yield path, n, kind, profile, obj, None


def check(root, surface=None):
    schema = json.loads((root / "schema.json").read_text(encoding="utf-8"))
    record_v = Draft202012Validator(schema)
    obs_schema = dict(schema["$defs"]["observation"])
    obs_schema["$defs"] = schema["$defs"]
    obs_schema["properties"] = dict(obs_schema["properties"])
    obs_schema["properties"]["measure"] = schema["properties"]["measure"]
    obs_v = Draft202012Validator(obs_schema)

    errors = []

    def err(path, line, rule, message):
        errors.append({"file": str(path.relative_to(root)) if path else None, "line": line, "rule": rule, "message": message})

    # Parents and observations resolve across the whole tree even with --surface.
    scope = list(load(root, surface))
    whole = scope if surface is None else list(load(root, None))
    in_scope = {(str(p), n) for p, n, *_ in scope}

    records = {}       # id -> (path, line, obj)
    observations = {}  # (id, profile) -> (path, line)
    obs_list = []
    for path, n, kind, profile, obj, parse_error in whole:
        mine = (str(path), n) in in_scope
        if parse_error:
            if mine:
                err(path, n, parse_error.split(":", 1)[0], parse_error)
            continue
        validator = record_v if kind == "record" else obs_v
        schema_errors = sorted(validator.iter_errors(obj), key=lambda e: list(e.absolute_path))
        if schema_errors:
            if mine:
                for e in schema_errors:
                    where = "/".join(str(x) for x in e.absolute_path) or "(record)"
                    err(path, n, "schema", f"{where}: {e.message}")
            continue
        if kind == "record":
            if obj["id"] in records:
                if mine:
                    first = records[obj["id"]]
                    err(path, n, "duplicate-id", f"{obj['id']} is already defined at {first[0].relative_to(root)}:{first[1]}")
                continue
            records[obj["id"]] = (path, n, obj)
        else:
            obs_list.append((path, n, profile, obj, mine))

    children = {}
    for rid, (path, n, obj) in records.items():
        parent = obj.get("parent")
        if parent is None:
            continue
        children.setdefault(parent, []).append(rid)
        if parent not in records and (str(path), n) in in_scope:
            err(path, n, "orphan-parent", f"{rid}: parent {parent} names no record")

    # Cycles: walk each chain once; a node seen twice on the current walk is a cycle.
    state = {}  # id -> 1 on the current walk, 2 finished
    for start in records:
        walk = []
        node = start
        while node in records and state.get(node) is None:
            state[node] = 1
            walk.append(node)
            node = records[node][2].get("parent")
        if node in records and state.get(node) == 1:
            path, n, _ = records[node]
            if (str(path), n) in in_scope:
                cycle = walk[walk.index(node):]
                err(path, n, "parent-cycle", " -> ".join(cycle + [node]))
        for w in walk:
            state[w] = 2

    for parent, kids in children.items():
        seen = {}
        for rid in sorted(kids, key=lambda k: (str(records[k][0]), records[k][1])):
            path, n, obj = records[rid]
            o = obj["order"]
            if o in seen and (str(path), n) in in_scope:
                err(path, n, "duplicate-order", f"{rid}: order {o} under {parent} is already used by {seen[o]}")
            seen.setdefault(o, rid)

    for rid, (path, n, obj) in records.items():
        if "itemCount" in obj and (str(path), n) in in_scope:
            have = len(children.get(rid, []))
            if have != obj["itemCount"]:
                err(path, n, "item-count", f"{rid}: itemCount {obj['itemCount']} but {have} child record(s) present")

    for path, n, profile, obj, mine in obs_list:
        if not mine:
            continue
        if obj["captureProfile"] != profile:
            err(path, n, "observation-profile", f"{obj['id']}: captureProfile {obj['captureProfile']} in a file named for {profile}")
        if obj["id"] not in records:
            err(path, n, "observation-orphan", f"{obj['id']} names no record")
        key = (obj["id"], obj["captureProfile"])
        if key in observations:
            first = observations[key]
            err(path, n, "duplicate-observation", f"{obj['id']} at {obj['captureProfile']} is already observed at {first[0].relative_to(root)}:{first[1]}")
        else:
            observations[key] = (path, n)

    count = sum(1 for p, n, kind, _, obj, e in scope if kind == "record" and obj is not None and e is None)
    return count, len([o for o in obs_list if o[4]]), errors


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
