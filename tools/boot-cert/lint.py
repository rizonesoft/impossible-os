#!/usr/bin/env python3
"""Boot certification matrix lint (release gate, not advisory) -- TODO-28.

Validates tools/boot-cert/boot-cert.yml against boot-cert.schema.json, enforces
cross-field invariants JSON Schema cannot express, and proves every
boot-platform TODO maps to at least one certification row. Exits non-zero on any
failure so scripts/test-tooling.sh (and CI) gate on it.

Design-review adoptions:
  D1 -- a row that is required for any tier must carry machine-readable evidence:
        auto/semi rows need a non-empty source_artifact; manual rows need a
        non-empty manual_evidence. A required gate with no evidence source is
        rejected, not silently allowed.
  D2 -- the every-TODO-has-a-row coverage set comes from the LIVE
        todo/01-boot-platform/TODO-NN-*.md directory listing, never from the
        (potentially stale) build/todo-cache.json, so a newly added boot TODO
        cannot pass the zero-row check by being absent from a cache.
"""
import sys
import os
import glob
import json
import re
import argparse

try:
    import yaml
except ImportError:
    sys.stderr.write("boot-cert lint: pyyaml required (pip install pyyaml)\n")
    sys.exit(2)
try:
    import jsonschema
except ImportError:
    sys.stderr.write("boot-cert lint: jsonschema required (pip install jsonschema)\n")
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TODO_GLOB = os.path.join(REPO_ROOT, "todo", "01-boot-platform", "TODO-[0-9][0-9]-*.md")
TODO_RE = re.compile(r"/(TODO-[0-9]{2})-")


class _DupKeyError(Exception):
    pass


class _StrictLoader(yaml.SafeLoader):
    """SafeLoader that raises on duplicate mapping keys. yaml.safe_load silently
    keeps the last duplicate, which would let a second required_for mapping erase
    a required row's obligations before the gate ever validates them."""


_MERGE_TAG = "tag:yaml.org,2002:merge"


def _scan_literal_dups(loader, map_node, _active=None):
    """Recursively reject literal duplicate / non-hashable keys in a MappingNode
    and in every `<<` merge-source node (mapping, or sequence of mappings). Runs
    BEFORE flatten_mapping so a valid `<<` merge + explicit override survives,
    while a literal duplicate -- including one inside an inline merge source --
    is rejected. yaml.safe_load would silently last-wins-overwrite these. A
    cyclic merge alias (a row anchoring itself as its own merge source) is
    rejected via the active-path set rather than recursing into RecursionError."""
    if _active is None:
        _active = set()
    if id(map_node) in _active:
        raise _DupKeyError("recursive merge source")
    _active.add(id(map_node))
    seen = set()
    for key_node, value_node in map_node.value:
        if getattr(key_node, "tag", None) == _MERGE_TAG:
            srcs = value_node.value if isinstance(value_node, yaml.SequenceNode) else [value_node]
            for src in srcs:
                if isinstance(src, yaml.MappingNode):
                    _scan_literal_dups(loader, src, _active)
            continue
        try:
            key = loader.construct_object(key_node, deep=True)
        except Exception:
            raise _DupKeyError("unconstructable mapping key")
        try:
            if key in seen:
                raise _DupKeyError(f"duplicate mapping key '{key}'")
            seen.add(key)
        except TypeError:
            raise _DupKeyError("non-hashable mapping key")
    _active.discard(id(map_node))


def _no_duplicate_keys(loader, node, deep=False):
    _scan_literal_dups(loader, node)
    loader.flatten_mapping(node)
    try:
        return {loader.construct_object(k, deep=deep): loader.construct_object(v, deep=deep)
                for k, v in node.value}
    except TypeError:
        raise _DupKeyError("non-hashable mapping key after merge")


_StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _no_duplicate_keys)


def _load_yaml(path):
    with open(path, "r", encoding="utf-8") as fh:
        return yaml.load(fh, Loader=_StrictLoader)


def _json_no_dup(pairs):
    # json.load is last-wins on duplicate object names, which would let a result
    # file carry an invalid platform_class/tier first and a valid one second,
    # masking the field the gate validates. Reject duplicates instead.
    obj = {}
    for k, v in pairs:
        if k in obj:
            raise _DupKeyError(f"duplicate JSON key '{k}'")
        obj[k] = v
    return obj


def _load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh, object_pairs_hook=_json_no_dup)


def boot_platform_todos():
    """Live directory listing of boot-platform TODOs, never the todo-graph cache."""
    todos = set()
    for p in glob.glob(TODO_GLOB):
        m = TODO_RE.search(p.replace(os.sep, "/"))
        if m:
            todos.add(m.group(1))
    return todos


def lint_matrix(matrix, schema):
    """Returns a list of error strings (empty == pass)."""
    errors = []
    validator = jsonschema.Draft202012Validator(schema)
    for e in sorted(validator.iter_errors(matrix), key=lambda x: list(x.path)):
        loc = "/".join(str(p) for p in e.path) or "(root)"
        errors.append(f"schema: {loc}: {e.message}")
    if errors:
        return errors  # shape is broken; cross-field checks would be noise

    declared_platforms = set(matrix["platform_classes"])
    declared_tiers = set(matrix["tiers"])
    seen_ids = set()
    for row in matrix["rows"]:
        rid = row["id"]
        if rid in seen_ids:
            errors.append(f"row '{rid}': duplicate id")
        seen_ids.add(rid)

        # every platform a row claims must be a declared class (a row must not
        # assert applicability to an unsupported platform, even when optional)
        row_platforms = set(row["platforms"])
        for plat in sorted(row_platforms):
            if plat not in declared_platforms:
                errors.append(f"row '{rid}': platforms[] entry '{plat}' not in top-level platform_classes")

        # required_for platform-classes must be declared AND in the row's platforms
        req = row.get("required_for") or {}
        req_any = False
        for tier, classes in req.items():
            if tier not in declared_tiers:
                errors.append(f"row '{rid}': required_for tier '{tier}' not in top-level tiers")
            for cls in classes:
                req_any = True
                if cls not in declared_platforms:
                    errors.append(f"row '{rid}': required_for.{tier} class '{cls}' not in top-level platform_classes")
                if cls not in row_platforms:
                    errors.append(f"row '{rid}': required_for.{tier} class '{cls}' not in this row's platforms[]")

        # D1: a required row must carry machine-readable / manual evidence
        if req_any:
            lvl = row["automation_level"]
            if lvl in ("auto", "semi"):
                if not (row.get("source_artifact") or "").strip():
                    errors.append(f"row '{rid}': required for a tier + automation_level={lvl} but source_artifact is empty (no evidence the release gate can read)")
            else:  # manual
                if not (row.get("manual_evidence") or "").strip():
                    errors.append(f"row '{rid}': required for a tier + automation_level=manual but manual_evidence is empty")
    return errors


def lint_coverage(matrix):
    """Every live boot-platform TODO maps to >=1 row. Returns (errors, table)."""
    errors = []
    todos = boot_platform_todos()
    if not todos:
        errors.append("coverage: found zero TODO-NN files under todo/01-boot-platform/ (glob failed?)")
        return errors, []
    by_owner = {}
    for row in matrix["rows"]:
        by_owner.setdefault(row["owner_todo"], []).append(row["id"])
    table = []
    for todo in sorted(todos):
        rows = by_owner.get(todo, [])
        table.append((todo, len(rows)))
        if not rows:
            errors.append(f"coverage: {todo} maps to zero certification rows")
    for owner in sorted(by_owner):
        if owner not in todos:
            errors.append(f"coverage: row owner '{owner}' has no matching TODO-NN file")
    return errors, table


def main():
    ap = argparse.ArgumentParser(description="boot certification matrix lint")
    ap.add_argument("--matrix", default=os.path.join(HERE, "boot-cert.yml"))
    ap.add_argument("--schema", default=os.path.join(HERE, "boot-cert.schema.json"))
    ap.add_argument("--result-schema", default=os.path.join(HERE, "result.schema.json"))
    ap.add_argument("--results", nargs="*", default=None, help="optional result JSON files to validate")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    errors = []

    try:
        matrix = _load_yaml(args.matrix)
        schema = _load_json(args.schema)
    except (OSError, yaml.YAMLError, json.JSONDecodeError, _DupKeyError, RecursionError) as e:
        sys.stderr.write(f"boot-cert lint: cannot load matrix/schema: {type(e).__name__}: {e}\n")
        return 1

    # the result schema must itself be a valid JSON Schema
    try:
        rschema = _load_json(args.result_schema)
        jsonschema.Draft202012Validator.check_schema(rschema)
    except (OSError, json.JSONDecodeError, jsonschema.exceptions.SchemaError, _DupKeyError) as e:
        errors.append(f"result-schema: invalid ({e})")
        rschema = None

    if not isinstance(matrix, dict) or "rows" not in matrix:
        errors.append("matrix: top-level object with a 'rows' list expected")
        matrix = {"rows": []}
    else:
        errors += lint_matrix(matrix, schema)
        cov_errors, table = lint_coverage(matrix)
        errors += cov_errors

    # a present-but-empty --results flag is zero evidence, not a no-op pass
    if args.results == []:
        errors.append("results: --results given with no files (no certification evidence)")
    result_files = args.results or []

    # optional: validate any provided result files
    if rschema is not None:
        rv = jsonschema.Draft202012Validator(rschema)
        rows_by_id = {r["id"]: r for r in matrix.get("rows", []) if isinstance(r, dict) and "id" in r}
        declared = set(matrix.get("platform_classes", [])) if isinstance(matrix, dict) else set()
        declared_t = set(matrix.get("tiers", [])) if isinstance(matrix, dict) else set()
        for rf in result_files:
            try:
                obj = _load_json(rf)
            except (OSError, json.JSONDecodeError, _DupKeyError) as e:
                errors.append(f"result '{rf}': cannot load ({e})")
                continue
            if isinstance(obj, list) and not obj:
                errors.append(f"result '{rf}': empty result array (no certification evidence)")
                continue
            for o in (obj if isinstance(obj, list) else [obj]):
                for e in rv.iter_errors(o):
                    errors.append(f"result '{rf}': {e.message}")
                if not isinstance(o, dict):
                    continue
                rid = o.get("row_id")
                row = rows_by_id.get(rid)
                if row is None:
                    errors.append(f"result '{rf}': row_id '{rid}' not in matrix")
                    continue
                # a result must not carry evidence for a platform the row never
                # certified, nor an undeclared platform class
                pc = o.get("platform_class")
                if pc is not None and pc not in declared:
                    errors.append(f"result '{rf}': platform_class '{pc}' not in matrix platform_classes")
                elif pc is not None and pc not in set(row.get("platforms", [])):
                    errors.append(f"result '{rf}': platform_class '{pc}' not in row '{rid}' platforms[]")
                tr = o.get("tier")
                if tr is not None and declared_t and tr not in declared_t:
                    errors.append(f"result '{rf}': tier '{tr}' not in matrix tiers")

    if not args.quiet and isinstance(matrix, dict) and matrix.get("rows"):
        _, table = lint_coverage(matrix)
        print("boot-cert coverage (TODO -> rows):")
        for todo, n in table:
            print(f"  [{'ok ' if n else 'GAP'}] {todo}: {n} row(s)")

    if errors:
        sys.stderr.write(f"boot-cert lint: {len(errors)} error(s)\n")
        for e in errors:
            sys.stderr.write(f"  - {e}\n")
        return 1
    if not args.quiet:
        print(f"boot-cert lint: OK ({len(matrix['rows'])} rows, {len(boot_platform_todos())} TODOs covered)")
    return 0


if __name__ == "__main__":
    # Single diagnostic boundary: a release gate must always exit cleanly (1 on
    # any failure), never escape as a Python traceback -- e.g. deeply nested JSON
    # in --result-schema / --results raising RecursionError mid-load.
    try:
        sys.exit(main())
    except Exception as _e:  # noqa: BLE001 -- gate must not traceback
        sys.stderr.write(f"boot-cert lint: unhandled {type(_e).__name__}: {_e}\n")
        sys.exit(1)
