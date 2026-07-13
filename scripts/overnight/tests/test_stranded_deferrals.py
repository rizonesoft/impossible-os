#!/usr/bin/env python3
"""P6.2 stranded-deferral audit (read-only). Runs the hook's own selftest
(item-level detection, cross-TODO + owner-shipped + awaiting/self exclusions)
and asserts the CLI is read-only (exit 0, no tree mutation)."""
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TOOL = REPO / "scripts/overnight/stranded_deferrals.py"


def test_selftest_passes():
    r = subprocess.run([sys.executable, str(TOOL), "--selftest"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_real_tree_runs_and_is_readonly():
    # Against the live tree it must exit 0 and emit valid JSON without editing
    # anything (a read-only audit).
    import json
    before = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain",
                             "todo"], capture_output=True, text=True).stdout
    r = subprocess.run([sys.executable, str(TOOL), "--json"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    data = json.loads(r.stdout)
    assert isinstance(data, list)
    for e in data:  # schema of each candidate
        assert {"source", "line", "target", "section", "action", "stranded"} <= set(e)
        assert e["action"] in ("flip", "clean", "blocked", "review")
        assert isinstance(e["stranded"], bool)
    after = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain",
                            "todo"], capture_output=True, text=True).stdout
    assert before == after, "audit must not modify any TODO file"


if __name__ == "__main__":
    test_selftest_passes()
    test_real_tree_runs_and_is_readonly()
    print("PASS: stranded-deferral audit (P6.2)")
