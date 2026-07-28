#!/usr/bin/env python3
# Protects B4/F2 (2026-07-13 findings): the runner's own full-suite/graph runs
# rewrite auto-gen docs (coverage.*, COUNT.md, todo-graph.md), which dirtied the
# tree and (F2) forced a separate commit + receipt re-record before every
# rollover, while (B4) the bare "N change(s)" message mis-attributed them to the
# operator. Fix: _rollover_failures tolerates an auto-gen-ONLY delta and names
# the path + probable owner for any real blocking change.
import importlib.util
import json
import subprocess
import pathlib
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]

_spec = importlib.util.spec_from_file_location(
    "rpg", REPO / ".claude/hooks/run_phase_guard.py")
rpg = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rpg)


def _repo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "t@t"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "t"], check=True)
    (root / "src/kernel").mkdir(parents=True)
    (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")
    # coverage.* are TRACKED in the real repo; the suite regenerates them into an
    # UNSTAGED modification (` M`) -- which is the only tolerated auto-gen shape.
    (root / "docs/test-coverage").mkdir(parents=True)
    (root / "docs/test-coverage/coverage.md").write_text("orig\n")
    (root / "docs/test-coverage/coverage.json").write_text("{}\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "init"], check=True)
    return root


def test_dirty_owner_classification():
    # F4: the XY status matters -- ONLY an unstaged modification of an allowlisted
    # generated file is auto-gen.
    assert rpg._dirty_owner(" M docs/test-coverage/coverage.md") == "auto-gen"
    assert rpg._dirty_owner(" M COUNT.md") == "auto-gen"
    # untracked / staged / deleted / conflicted are NEVER auto-gen (even allowlisted)
    assert rpg._dirty_owner("?? docs/test-coverage/coverage.json") == "untracked"
    assert rpg._dirty_owner("M  docs/test-coverage/coverage.md") == "tracked-source"
    assert rpg._dirty_owner("A  docs/test-coverage/coverage.md") == "tracked-source"
    assert rpg._dirty_owner(" D docs/test-coverage/coverage.md") == "tracked-source"
    assert rpg._dirty_owner("UU COUNT.md") == "conflict"
    assert rpg._dirty_owner("?? scratch.tmp") == "untracked"
    assert rpg._dirty_owner(" M src/kernel/x.c") == "tracked-source"
    assert rpg._dirty_path(" M a.c -> b.c") == "b.c"


def _tree_not_clean(fails):
    return [f for f in fails if "tree not clean" in f]


def test_coverage_only_delta_tolerated():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        # UNSTAGED modification of the tracked coverage files (` M`) -> tolerated.
        (root / "docs/test-coverage/coverage.md").write_text("regenerated\n")
        (root / "docs/test-coverage/coverage.json").write_text('{"x":1}\n')
        fails = rpg._rollover_failures(root, {})
        assert not _tree_not_clean(fails), fails  # auto-gen-only -> not blocked


def test_staged_autogen_change_blocks():
    # F4: a STAGED coverage change is NOT tolerated (uncommitted index state).
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "docs/test-coverage/coverage.md").write_text("regen\n")
        subprocess.run(["git", "-C", str(root), "add",
                        "docs/test-coverage/coverage.md"], check=True)
        fails = rpg._rollover_failures(root, {})
        assert _tree_not_clean(fails), "a staged auto-gen change must block"


def test_source_change_blocks_and_names_owner():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")  # tracked-source
        fails = rpg._rollover_failures(root, {})
        tnc = _tree_not_clean(fails)
        assert tnc and "src/kernel/x.c [tracked-source]" in tnc[0], tnc


def test_mixed_delta_blocks_but_notes_tolerated():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        (root / "docs/test-coverage/coverage.md").write_text("regen\n")   # ' M' auto-gen
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")  # blocking
        fails = rpg._rollover_failures(root, {})
        tnc = _tree_not_clean(fails)
        assert tnc, fails
        assert "src/kernel/x.c [tracked-source]" in tnc[0], tnc
        # "pipeline-output" (2026-07-28) covers BOTH tolerated owners: the
        # auto-gen path allowlist and the content-classified `xref-repair`
        # (see test_rollover_xref_repair.py). The count is what matters.
        assert "tolerating 1 pipeline-output" in tnc[0], tnc


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: rollover-tolerance (B4/F2)")
