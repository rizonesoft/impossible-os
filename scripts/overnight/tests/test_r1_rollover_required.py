#!/usr/bin/env python3
# Protects R1 (2026-07-19): a section SHIP demands a verified rollover before
# the cursor may advance. P3.2 only traps a REFUSED rollover; R1 traps the
# never-ATTEMPTED one (measured 2026-07-19: multi-section sessions reached
# ~600K context; cache reads were 75% of the night's cost). Deferrals add no
# ship stamp and advance freely; the fresh post-rotation worker passes because
# the ship predates last_rollover_epoch.
#
# STATE_PATH resolves to the REAL repo at import, so this test monkeypatches it
# (and repo_root / _emit_anchor) to a temp dir -- it must never touch live state.
import importlib.util
import pathlib
import subprocess
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/run_phase_guard.py"


def _load(state_file, repo_dir):
    spec = importlib.util.spec_from_file_location("rpg_r1_test", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.STATE_PATH = state_file
    mod.repo_root = lambda: repo_dir
    mod._emit_anchor = lambda state: None
    return mod


def test_skill_start_blocked_when_ship_pending():
    # The LOAD-BEARING R1 chokepoint (finder C, 2026-07-19): the SECTIONS loop
    # never re-calls `cursor` between sections of one file, so the guard blocks
    # the section-STARTER skills directly via evaluate(ship_pending=sha).
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        state = {"active": True, "phase": "SECTIONS",
                 "file": "todo/TODO-25.md", "section_idx": 1}
        for sk in ("implement-todo-section", "implement-todo-item",
                   "implement-ssdt-range"):
            allow, msg = mod.evaluate("Skill", {"skill": sk}, state,
                                      headless=True, ship_pending="deadbeef")
            assert not allow and "[SEQ-R1]" in msg, (sk, allow, msg)
        # review + close skills legitimately run POST-ship: never R1-blocked
        for sk in ("review-todo-section", "verify-todo-section"):
            allow, msg = mod.evaluate("Skill", {"skill": sk}, state,
                                      headless=True, ship_pending="deadbeef")
            assert allow, (sk, msg)
        allow, msg = mod.evaluate(
            "Skill", {"skill": "complete-todo-file"},
            {**state, "phase": "FILE_CLOSE"},
            headless=True, ship_pending="deadbeef")
        assert allow, msg
        # no pending ship -> starters pass
        allow, msg = mod.evaluate(
            "Skill", {"skill": "implement-todo-section"}, state,
            headless=True, ship_pending="")
        assert allow, msg
        # interactive sessions never gated
        allow, msg = mod.evaluate(
            "Skill", {"skill": "implement-todo-section"}, state,
            headless=False, ship_pending="deadbeef")
        assert allow, msg


def test_cursor_blocked_when_ship_after_rotation():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/TODO-25.md",
                        "section_idx": 1, "last_rollover_epoch": 1000})
        mod._ship_stamp_since = lambda root, epoch: "deadbeefcafe"
        # advance to the next section un-rotated -> BLOCKED
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-25.md", "2"]) == 1
        # a different FILE un-rotated -> BLOCKED
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-26.md", "1"]) == 1
        # staying on the CURRENT section (repair / re-stamp) -> allowed
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-25.md", "1"]) == 0


def test_cursor_allowed_when_no_ship_since_rotation():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/TODO-25.md",
                        "section_idx": 1, "last_rollover_epoch": 1000})
        mod._ship_stamp_since = lambda root, epoch: ""  # deferral / fresh worker
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-25.md", "2"]) == 0


def test_missing_epoch_fails_open():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/TODO-25.md",
                        "section_idx": 1})
        # no last_rollover_epoch (mid-migration state): _ship_stamp_since
        # fails open on the missing epoch, so the advance is allowed.
        assert mod.cli(["cursor", "02-kernel-core", "todo/TODO-25.md", "2"]) == 0


def test_start_and_verified_rollover_set_epoch():
    with tempfile.TemporaryDirectory() as d:
        mod = _load(pathlib.Path(d) / "state.json", pathlib.Path(d))
        assert mod.cli(["start", "2026-07-19T00:00:00"]) == 0
        assert isinstance(mod.load_state().get("last_rollover_epoch"), int)
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 1,
                        "last_rollover_epoch": 1})
        mod._rollover_failures = lambda root, state: []
        assert mod.cli(["rollover"]) == 0
        assert mod.load_state()["last_rollover_epoch"] >= int(time.time()) - 60


def _git(cwd, *args):
    subprocess.run(["git", "-C", str(cwd), *args], check=True,
                   capture_output=True, text=True)


def test_ship_stamp_since_real_git():
    with tempfile.TemporaryDirectory() as d:
        repo = pathlib.Path(d)
        mod = _load(repo / "state.json", repo)
        _git(repo, "init", "-q")
        _git(repo, "config", "user.email", "t@t")
        _git(repo, "config", "user.name", "t")
        (repo / "todo").mkdir()
        f = repo / "todo" / "T.md"
        f.write_text("## 1. Section\n", encoding="utf-8")
        _git(repo, "add", "-A")
        _git(repo, "commit", "-q", "-m", "base")
        f.write_text("## 1. Section\n\n> **Verified:** 2026-07-19 suite green\n",
                     encoding="utf-8")
        _git(repo, "add", "-A")
        _git(repo, "commit", "-q", "-m", "review: T section 1")
        now = int(time.time())
        # rotation BEFORE the stamp commit -> the ship is post-rotation: found
        assert mod._ship_stamp_since(repo, now - 3600) != ""
        # rotation AFTER the stamp commit -> nothing newer ships: clean
        assert mod._ship_stamp_since(repo, now + 3600) == ""
        # missing epoch -> fail-open
        assert mod._ship_stamp_since(repo, None) == ""


def test_defer_commit_does_not_trip_r1():
    # A deferral edit ([/] + Deferred stamp prose, NO **Verified:** /
    # **Quality reviewed:** line) after the rotation must NOT block the
    # advance -- deferrals legitimately advance in-session.
    with tempfile.TemporaryDirectory() as d:
        repo = pathlib.Path(d)
        mod = _load(repo / "state.json", repo)
        _git(repo, "init", "-q")
        _git(repo, "config", "user.email", "t@t")
        _git(repo, "config", "user.name", "t")
        (repo / "todo").mkdir()
        f = repo / "todo" / "T.md"
        f.write_text("## 1. Section\n", encoding="utf-8")
        _git(repo, "add", "-A")
        _git(repo, "commit", "-q", "-m", "base")
        f.write_text("## 1. Section\n- [/] deferred item (XREF: todo/X.md)\n",
                     encoding="utf-8")
        _git(repo, "add", "-A")
        _git(repo, "commit", "-q", "-m", "todo: defer item")
        assert mod._ship_stamp_since(repo, int(time.time()) - 3600) == ""


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: r1-rollover-required")
