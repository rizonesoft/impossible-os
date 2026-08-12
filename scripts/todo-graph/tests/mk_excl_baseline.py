#!/usr/bin/env python3
"""Build the Check 7 exclusion fixtures' baselines from the real one.

Section 43. `test_build.sh` needs several deliberately-mutated copies of
`scripts/lint/stub-lint-baseline.json` to prove that the commit-gate exclusion
set reaches the rc-7 coverage gate symmetrically. Keeping them here rather than
in inline heredocs is not tidiness: the suite drives these through
`python3 - <<PY` blocks, and a nested heredoc inside one silently terminates the
outer one, so the fixture that "ran" is not the fixture that was written.

  mk_excl_baseline.py <cache> <dst> from-cache   walk the cache and write a
                                                 baseline that matches it exactly,
                                                 then move one occurrence out of
                                                 the busiest owner AND out of
                                                 `total` (prints the owner)
  mk_excl_baseline.py <src> <dst> move-one       the same move, but starting from
                                                 an existing baseline file
  mk_excl_baseline.py <src> <dst> drop-by-owner  the per-owner map removed
  mk_excl_baseline.py <src> <dst> tamper-sum     one owner's count inflated, so
                                                 the sums no longer match
  mk_excl_baseline.py <src> -    list-owners     every owner, one per line
"""
import json
import sys


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src, dst, mode = argv

    if mode == "from-cache":
        # DERIVED FROM THE CACHE, NOT FROM THE CHECKED-IN BASELINE. The fixtures
        # need `total`/`resolved`/`by_owner` to agree with the walk EXACTLY, and
        # the tracked baseline is only guaranteed to agree with the corpus at the
        # moment it was recorded. `test-tooling.sh` rebuilds caches while this
        # nested suite runs, so a fixture pinned to the tracked numbers fails for
        # a reason that has nothing to do with what it tests.
        import importlib.util
        import os
        from pathlib import Path
        repo = Path(__file__).resolve().parents[3]
        sys.path.insert(0, str(repo / "scripts" / "todo-graph"))
        spec = importlib.util.spec_from_file_location(
            "_csbs", str(repo / "scripts" / "lint" / "check_stub_behind_stamp.py"))
        helper = importlib.util.module_from_spec(spec)
        sys.modules["_csbs"] = helper
        spec.loader.exec_module(helper)
        import cache_schema as _cs
        import ref_resolution as _rr
        nodes, _info = _cs.load_and_validate(Path(src), repo / "todo")
        with _rr.walk_scope():
            _f, cov = helper._walk(nodes, repo)
        by = {k: dict(v) for k, v in cov["by_owner"].items() if v["occurrences"]}
        if not by or cov["resolved"] < 1:
            # A corpus that resolves nothing cannot carry a positive floor, and
            # every fixture below would then refuse for that reason instead.
            print("NO-USABLE-CORPUS")
            return 0
        owner = max(by, key=lambda k: by[k]["occurrences"])
        doc = {"resolved": cov["resolved"], "total": cov["occurrences"] - 1,
               "recorded": "fixture", "why": "fixture", "by_owner": by}
        doc["by_owner"][owner]["occurrences"] -= 1
        open(dst, "w", encoding="utf-8").write(json.dumps(doc))
        print(owner)
        return 0

    doc = json.loads(open(src, encoding="utf-8").read())
    by = doc.get("by_owner") or {}
    if not by:
        print("NO-BY-OWNER")
        return 0

    if mode == "list-owners":
        print("\n".join(by))
        return 0

    if mode == "move-one":
        # The BUSIEST owner, so the fixture stays meaningful if a small owner is
        # later removed from the corpus. `resolved` is deliberately untouched:
        # this fixture measures the POPULATION gate, and moving the floor too
        # would let a coverage refusal masquerade as the result.
        owner = max(by, key=lambda k: by[k]["occurrences"])
        doc["total"] -= 1
        doc["by_owner"][owner]["occurrences"] -= 1
        open(dst, "w", encoding="utf-8").write(json.dumps(doc))
        print(owner)
        return 0

    if mode == "drop-by-owner":
        doc.pop("by_owner", None)
        open(dst, "w", encoding="utf-8").write(json.dumps(doc))
        return 0

    if mode == "tamper-sum":
        doc["by_owner"][next(iter(by))]["occurrences"] += 5
        open(dst, "w", encoding="utf-8").write(json.dumps(doc))
        return 0

    sys.stderr.write(f"unknown mode {mode!r}\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
