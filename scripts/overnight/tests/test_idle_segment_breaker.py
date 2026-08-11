#!/usr/bin/env python3
"""The 2026-08-11 idle-rollover incident, closed from both ends.

WHAT HAPPENED. A section shipped at 12:04 (commit 9ebeff0b7, the
mutating-repair-tools fence adoption) and its session rolled over (VERIFIED,
12:08). The next TWO segments each landed on the clean shipped tree, concluded
a rollover was owed, re-verified it (the check is pure tree state -- clean,
pushed, receipts -- all true forever on a shipped tree), were told to END the
turn, and shipped nothing. The HEAD-blind no-ship counter counted both and the
breaker stopped a healthy run at streak 2. Meanwhile run-outcome.py had
classified both segments `checkpoint` -- "commit-free BY DESIGN" -- and left
ITS streak untouched. Two counters, one informed and one blind; the blind one
pulled the trigger.

TWO FIXES, TESTED TOGETHER BECAUSE THEY ONLY WORK TOGETHER:
  1. run_phase_guard.py refuses a rollover when HEAD has not moved since the
     segment's own start, and the refusal REDIRECTS to work -- a bare refusal
     would swap the VERIFIED loop for a REFUSED loop (same no-ship, same
     breaker), since the guard's instructions are followed verbatim.
  2. noship-update.sh consults the outcome classification: `checkpoint` /
     `snoozed` segments leave the streak UNTOUCHED (not reset -- a dead loop
     interleaved with rollovers must still trip on its own segments).
Fix 2 alone would hide a pure rollover loop forever; fix 1 alone leaves the
blocked-deferral segment counting against the limit. Each covers the other's
blind spot, and the refusal-direction tests below pin both.
"""
import contextlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
HOOK = REPO / ".claude/hooks/run_phase_guard.py"
NOSHIP = REPO / "scripts/overnight/noship-update.sh"


# ---------------------------------------------------------------------------
# Fix 1: the guard's idle-rollover refusal
# ---------------------------------------------------------------------------

def _guard(state_file, repo_dir):
    spec = importlib.util.spec_from_file_location("rpg_idle", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.STATE_PATH = state_file
    mod.repo_root = lambda: repo_dir
    mod._emit_anchor = lambda state: None
    return mod


def _run(mod, argv):
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        rc = mod.cli(argv)
    return rc, err.getvalue()


def test_idle_rollover_is_refused_with_redirect():
    """The incident itself: HEAD unmoved since segment start -> refuse, and the
    message must carry the redirect (the load-bearing half) while setting
    NEITHER state flag -- `pending` would permit the stop this refusal exists
    to prevent, and `rollover_refused` (the refused-checkpoint marker) would
    BLOCK the cursor advance the redirect demands."""
    with tempfile.TemporaryDirectory() as d:
        mod = _guard(pathlib.Path(d) / "s.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5,
                        "segment_start_head": "abc123"})
        mod._current_head = lambda root: "abc123"
        mod._rollover_failures = lambda root, state: []  # would otherwise VERIFY
        rc, err = _run(mod, ["rollover"])
        assert rc == 1, "an idle rollover must be refused"
        assert "nothing to roll over" in err
        assert "CONTINUE WORKING" in err, "the redirect is the load-bearing half"
        st = mod.load_state()
        assert "rollover" not in st, "refusal must not permit a stop"
        assert "rollover_refused" not in st, \
            "refused-checkpoint flag would wedge the cursor advance the redirect demands"


def test_earned_rollover_still_verifies():
    """REFUSAL-DIRECTION CONTROL, the one that matters most: a segment that
    DID work (HEAD moved) must roll over exactly as before. If this fails, the
    fix traded an idle loop for a run that can never rotate."""
    with tempfile.TemporaryDirectory() as d:
        mod = _guard(pathlib.Path(d) / "s.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5,
                        "segment_start_head": "abc123"})
        mod._current_head = lambda root: "def456"
        mod._rollover_failures = lambda root, state: []
        rc, err = _run(mod, ["rollover"])
        assert rc == 0 and "rollover VERIFIED" in err
        assert (mod.load_state().get("rollover") or {}).get("pending") is True


def test_unverified_checkpoint_still_refuses_on_its_own_terms():
    """CONTROL: with HEAD moved but a dirty checkpoint, the ORIGINAL refusal
    path (repair and retry) must be reached -- the idle check must not swallow
    it. These two refusals give opposite instructions; conflating them would
    send a broken checkpoint off to start the next section."""
    with tempfile.TemporaryDirectory() as d:
        mod = _guard(pathlib.Path(d) / "s.json", pathlib.Path(d))
        mod.save_state({"active": True, "file": "todo/T.md", "section_idx": 5,
                        "segment_start_head": "abc123"})
        mod._current_head = lambda root: "def456"
        mod._rollover_failures = lambda root, state: ["tree not clean (x)"]
        rc, err = _run(mod, ["rollover"])
        assert rc == 1
        assert "checkpoint not verified" in err
        assert "nothing to roll over" not in err
        assert mod.load_state().get("rollover_refused"), \
            "refused-checkpoint flag must still be set on this path"


def test_missing_baseline_or_head_fails_open():
    """FAIL-OPEN CONTROLS: a state file predating this change (no baseline) or
    an unreadable HEAD must fall through to normal verification -- refusing
    work on a broken probe is how a fix becomes a wedge."""
    for seg_head, cur in ((None, "abc123"), ("abc123", None)):
        with tempfile.TemporaryDirectory() as d:
            mod = _guard(pathlib.Path(d) / "s.json", pathlib.Path(d))
            st = {"active": True, "file": "todo/T.md", "section_idx": 5}
            if seg_head:
                st["segment_start_head"] = seg_head
            mod.save_state(st)
            mod._current_head = lambda root, _c=cur: _c
            mod._rollover_failures = lambda root, state: []
            rc, err = _run(mod, ["rollover"])
            assert rc == 0, f"fail-open violated for baseline={seg_head!r} head={cur!r}"
            assert "nothing to roll over" not in err


def test_mark_rotation_and_start_record_the_baseline():
    """The baseline must be stamped at BOTH segment entrances: mark-rotation
    (every relaunch) and `start` (the first segment, when mark-rotation is
    still a no-op because the run is not yet active)."""
    with tempfile.TemporaryDirectory() as d:
        mod = _guard(pathlib.Path(d) / "s.json", pathlib.Path(d))
        mod._current_head = lambda root: "seg777"
        mod.save_state({"active": True})
        _run(mod, ["mark-rotation"])
        assert mod.load_state().get("segment_start_head") == "seg777"
    with tempfile.TemporaryDirectory() as d:
        mod = _guard(pathlib.Path(d) / "s.json", pathlib.Path(d))
        mod._current_head = lambda root: "first99"
        _run(mod, ["start", "2026-08-11"])
        assert mod.load_state().get("segment_start_head") == "first99"


# ---------------------------------------------------------------------------
# Fix 2: the outcome-aware no-ship streak
# ---------------------------------------------------------------------------

def _fixture_repo():
    d = pathlib.Path(tempfile.mkdtemp(prefix="noship-fx."))
    (d / ".claude/overnight").mkdir(parents=True)
    subprocess.run(["git", "-C", str(d), "init", "-q"], check=True)
    subprocess.run(["git", "-C", str(d), "-c", "user.email=t@t", "-c",
                    "user.name=t", "commit", "-q", "--allow-empty", "-m", "i"],
                   check=True)
    return d


def _head(d):
    return subprocess.run(["git", "-C", str(d), "rev-parse", "HEAD"],
                          capture_output=True, text=True).stdout.strip()


def _noship(d, start_head, outcome):
    r = subprocess.run(["bash", str(NOSHIP), str(d), start_head, outcome],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    f = d / ".claude/overnight/noship-streak"
    return (f.read_text().strip() if f.exists() else None), r.stdout


def test_streak_rules_all_directions():
    import shutil
    d = _fixture_repo()
    try:
        h1 = _head(d)
        subprocess.run(["git", "-C", str(d), "-c", "user.email=t@t", "-c",
                        "user.name=t", "commit", "-q", "--allow-empty", "-m",
                        "work"], check=True)
        h2 = _head(d)
        # Rule 1: HEAD moved -> reset, whatever the outcome says.
        (d / ".claude/overnight/noship-streak").write_text("1\n")
        v, out = _noship(d, h1, '{"outcome":"unproductive"}')
        assert v is None and "reset" in out
        # Rule 2: checkpoint, unmoved -> UNTOUCHED (absent stays absent...)
        v, out = _noship(d, h2, '{"outcome":"checkpoint"}')
        assert v is None and "untouched" in out
        # ...and REFUSAL: an existing streak is NOT reset by a checkpoint --
        # a dead loop interleaved with rollovers must still trip on its own
        # segments (run-outcome.py's own stated rule, now honoured here too).
        (d / ".claude/overnight/noship-streak").write_text("1\n")
        v, _ = _noship(d, h2, '{"outcome":"checkpoint"}')
        assert v == "1", "checkpoint must neither increment NOR reset"
        v, _ = _noship(d, h2, '{"outcome":"snoozed"}')
        assert v == "1"
        # Rule 3: a genuinely idle segment still counts -- the breaker keeps
        # its teeth. This is the case that must never be exempted.
        v, _ = _noship(d, h2, '{"outcome":"unproductive"}')
        assert v == "2"
        # REFUSAL: an unreadable verdict counts too (fail toward stopping).
        v, _ = _noship(d, h2, "not json")
        assert v == "3"
        v, _ = _noship(d, h2, "")
        assert v == "4"
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_launcher_delegates_to_the_helper():
    """PIN: the launcher must call noship-update.sh rather than keeping a
    private copy of the rules -- two implementations of one counter is how the
    original blind spot happened (run-outcome.py knew; the launcher did not)."""
    src = (REPO / "scripts/overnight/overnight-launch.sh").read_text()
    assert "noship-update.sh" in src
    assert 'END_HEAD" != "${START_HEAD' not in src, \
        "the launcher's inline HEAD-test copy of the streak logic is back"


if __name__ == "__main__":
    fails = []
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS {name}")
            except Exception as exc:
                fails.append(name)
                print(f"  FAIL {name}: {exc}")
    sys.exit(1 if fails else 0)
