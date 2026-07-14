#!/usr/bin/env python3
# Protects P4.5/P4.6 (2026-07-12 plan): the MID-SECTION context-cap rotation.
# _rollover_failures_wip() accepts a WIP tree (committed, unpushed, unstamped, no
# receipts) but KEEPS the work-safety checks (committed-clean + no pending review
# + no bg job); the `rollover-wip` verb fires ONLY when the P4.1 rotate_hint is set
# AND that gate passes, hard-forbidding a mid-fix-loop / review-wait rotation. It
# does NOT touch the shipped _rollover_failures() ship gate.
#
# STATE_PATH resolves to the REAL repo at import, so this monkeypatches it (and
# repo_root / _emit_anchor) -- it must never touch live state.
import importlib.util
import json
import subprocess
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/run_phase_guard.py"


def _load(state_file, repo_dir):
    spec = importlib.util.spec_from_file_location("rpg_wip_test", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.STATE_PATH = state_file
    mod.repo_root = lambda: repo_dir
    mod._emit_anchor = lambda s: None
    return mod


def _git_repo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "t@t"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "t"], check=True)
    (root / "src").mkdir()
    (root / "src/x.c").write_text("int x;\n")
    # .claude/state/ is gitignored in the real repo, so the gates never see
    # runtime state files -- mirror that here (else they read as untracked work).
    (root / ".gitignore").write_text(".claude/state/\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "i"], check=True,
                   capture_output=True)
    (root / ".claude/state").mkdir(parents=True)
    return root


def _review(root, received):
    (root / ".claude/state/last-codex-review.json").write_text(
        json.dumps({"received": received}))


# ----------------------------------------------------- WIP gate internals (P4.5)
def test_wip_gate_accepts_committed_clean_with_received_review():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, True)
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        # committed-clean + received review -> the work-safety checks pass
        # (ignore any bg-job noise from the host; we assert the two we control).
        assert not any("WIP not committed" in f or "outstanding Codex review" in f
                       for f in fails), fails


def test_wip_gate_rejects_uncommitted_edit_midfixloop():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, True)
        (root / "src/x.c").write_text("int x; int y;\n")   # UNCOMMITTED (mid-fix)
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        assert any("WIP not committed" in f for f in fails), fails


def test_wip_gate_rejects_outstanding_review():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, False)          # pending review
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        assert any("outstanding Codex review" in f for f in fails), fails


def test_wip_gate_tolerates_autogen_and_never_checks_pushed():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d); _review(root, True)
        (root / "docs/test-coverage").mkdir(parents=True)
        (root / "docs/test-coverage/coverage.md").write_text("regen\n")  # auto-gen
        mod = _load(pathlib.Path(d) / "s.json", root)
        fails = mod._rollover_failures_wip(root, {"active": True})
        # auto-gen tolerated; unpushed / receipts / graph are NEVER checked here
        assert not any("WIP not committed" in f or "not pushed" in f
                       or "receipt" in f or "todo-graph" in f for f in fails), fails


# ------------------------------------------------ rollover-wip firing verb (P4.6)
def _hint(root, on=True):
    (root / ".claude/state/rotate-hint.json").write_text(
        json.dumps({"count": 200, "hint": on}))


def test_rollover_wip_refused_without_hint():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        mod._rollover_failures_wip = lambda r, s: []        # gate would pass
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5})
        assert mod.cli(["rollover-wip"]) == 1               # no hint -> refuse


def test_rollover_wip_verified_with_hint_and_clean_gate():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        mod._rollover_failures_wip = lambda r, s: []
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5})
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 0
        st = mod.load_state()
        assert st["rollover"]["pending"] is True and st["rollover"]["wip"] is True
        assert not (root / ".claude/state/rotate-hint.json").exists()  # counter reset


def test_rollover_wip_refused_when_gate_fails():
    with tempfile.TemporaryDirectory() as d:
        root = _git_repo(d)
        mod = _load(root / ".claude/state/s.json", root)
        mod._rollover_failures_wip = lambda r, s: ["WIP not committed -- tree not clean"]
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5})
        _hint(root, True)
        assert mod.cli(["rollover-wip"]) == 1               # hint set but gate fails


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: p45-wip-rollover")
