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



def test_gotcha_expiry_prune_is_tolerated_but_an_addition_is_not():
    """v05 item 1 (2026-07-31). `runner-doctor.py` prunes expired live-gotchas
    entries at EVERY real launch (overnight-launch.sh:158) and correctly never
    commits, so the deletion sits as working-tree dirt -- classified
    `tracked-source`, which refuses the next ship rollover with "tree not
    clean". First hit at arm time 00:05; the recurrence is scheduled, not
    hypothetical (two live entries expire 2026-08-15, and a watchdog relaunch
    that day prunes them with nobody attending).

    DELETION-ONLY is the entire safety argument and is CHECKED, not assumed: the
    pruner can only drop lines, so a diff that adds anything is the run or an
    operator writing a hazard and must keep blocking."""
    sp = subprocess
    mod = rpg
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        sp.run(["git", "init", "-q", str(root)], check=True)
        for k, v in (("user.email", "t@t"), ("user.name", "t")):
            sp.run(["git", "-C", str(root), "config", k, v], check=True)
        rel = ".claude/state/live-gotchas.md"
        (root / ".claude/state").mkdir(parents=True)
        f = root / rel
        orig = ("- 2026-01-01: keep me\n"
                "- 2026-01-02: drop me (expires 2020-01-01)\n"
                "- 2026-01-03: keep me too\n")
        f.write_text(orig, encoding="utf-8")
        sp.run(["git", "-C", str(root), "add", "-A"], check=True)
        sp.run(["git", "-C", str(root), "commit", "-qm", "seed"], check=True,
               capture_output=True)

        def status():
            return sp.run(["git", "-C", str(root), "status", "--porcelain",
                           "--", rel], capture_output=True,
                          text=True).stdout.rstrip("\n")

        kept = "- 2026-01-01: keep me\n- 2026-01-03: keep me too\n"
        # 1. pure prune -> tolerated
        f.write_text(kept, encoding="utf-8")
        assert mod._dirty_owner(status(), root) == "gotcha-prune"
        assert "gotcha-prune" in mod._TOLERATED_OWNERS
        # 2. prune PLUS an added hazard -> still blocks
        f.write_text(kept + "- 2026-07-31: a new hazard\n", encoding="utf-8")
        assert mod._dirty_owner(status(), root) == "tracked-source"
        # 3. a pure addition -> still blocks
        sp.run(["git", "-C", str(root), "checkout", "--", rel], check=True)
        f.write_text(orig + "- 2026-07-31: appended\n", encoding="utf-8")
        assert mod._dirty_owner(status(), root) == "tracked-source"
        # 4. a STAGED prune -> still blocks (index column set)
        sp.run(["git", "-C", str(root), "checkout", "--", rel], check=True)
        f.write_text(kept, encoding="utf-8")
        sp.run(["git", "-C", str(root), "add", rel], check=True)
        assert mod._dirty_owner(status(), root) == "tracked-source"
        # 5. an unrelated tracked file with a deletion-only diff -> NOT tolerated
        sp.run(["git", "-C", str(root), "reset", "-q", "--", rel], check=True)
        sp.run(["git", "-C", str(root), "checkout", "--", rel], check=True)
        other = root / "src.c"
        other.write_text("a\nb\n", encoding="utf-8")
        sp.run(["git", "-C", str(root), "add", "-A"], check=True)
        sp.run(["git", "-C", str(root), "commit", "-qm", "s2"], check=True,
               capture_output=True)
        other.write_text("a\n", encoding="utf-8")
        line = sp.run(["git", "-C", str(root), "status", "--porcelain",
                       "--", "src.c"], capture_output=True,
                      text=True).stdout.rstrip("\n")
        assert mod._dirty_owner(line, root) == "tracked-source"

if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: rollover-tolerance (B4/F2)")