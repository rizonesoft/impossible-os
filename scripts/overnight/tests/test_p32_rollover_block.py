#!/usr/bin/env python3
# Protects P3.2 (2026-07-12 plan): a REFUSED rollover must BLOCK starting the
# next section (the un-rotated section-to-section advance the flow invariant
# forbids), not "continue in-session" into it. run_phase_guard.py records the
# refusal and blocks a `cursor` advance to a different section until a rollover
# verifies (which clears the flag) or the operator clears.
#
# STATE_PATH resolves to the REAL repo at import, so this test monkeypatches it
# (and repo_root / _emit_anchor) to a temp dir -- it must never touch live state.
import importlib.util
import pathlib
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/run_phase_guard.py"


def _load(state_file, repo_dir):
    spec = importlib.util.spec_from_file_location("rpg_test", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.STATE_PATH = state_file          # redirect load_state/save_state
    mod.repo_root = lambda: repo_dir     # keep subprocess paths off the real repo
    mod._emit_anchor = lambda state: None
    return mod


def test_cursor_advance_blocked_after_refused_rollover():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/TODO-22.md", "section_idx": 5,
                        "rollover_refused": {"file": "todo/TODO-22.md",
                                             "section_idx": 5, "epoch": 1}})
        # advancing to the NEXT section un-rotated -> BLOCKED
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-22.md", "6"]) == 1
        # staying on the CURRENT section (repair) -> allowed
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-22.md", "5"]) == 0
        # advancing to a different FILE un-rotated -> BLOCKED
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-23.md", "1"]) == 1


def test_no_flag_allows_advance():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/TODO-22.md", "section_idx": 5})
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-22.md", "6"]) == 0


def test_refuse_sets_flag_and_verify_clears():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5})
        # a refused rollover records the flag...
        mod._rollover_failures = lambda root, state: ["tree not clean (x [tracked-source])"]
        assert mod.cli(["rollover"]) == 1
        assert mod.load_state()["rollover_refused"]["section_idx"] == 5
        # ...and a verified rollover clears it.
        mod._rollover_failures = lambda root, state: []
        assert mod.cli(["rollover"]) == 0
        assert "rollover_refused" not in mod.load_state()


def test_operator_clear_removes_flag():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5,
                        "rollover_refused": {"file": "todo/T.md", "section_idx": 5,
                                             "epoch": 1}})
        assert mod.cli(["clear", "operator override"]) == 0
        assert "rollover_refused" not in mod.load_state()


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: p32-rollover-block")
