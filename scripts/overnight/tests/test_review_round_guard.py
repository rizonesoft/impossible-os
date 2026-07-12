#!/usr/bin/env python3
# P2.3 wires review_round_guard.py into the review fix-loop (per-section round
# counter + STALL-based convergence cap: stop after K rounds with no NEW
# Critical/High, plus a high ceiling as an infinite-loop guard -- NEVER a fixed
# round cap, so a productive deep review runs as long as it keeps finding bugs).
# This puts it under the deterministic suite so the wiring can't silently rot.
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parents[2] / ".claude" / "hooks" / "review_round_guard.py"


def _bump(state, slice_id, progress):
    r = subprocess.run([sys.executable, str(SCRIPT), "--bump", slice_id,
                        "--progress", progress, "--state", str(state)],
                       text=True, capture_output=True)
    return r.returncode, r.stdout


def test_builtin_selftest_passes():
    r = subprocess.run([sys.executable, str(SCRIPT), "--selftest"],
                       text=True, capture_output=True)
    assert r.returncode == 0, (r.stdout, r.stderr)


def test_productive_review_never_caps():
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        for _ in range(20):                       # every round finds something new
            rc, _ = _bump(st, "todo/x.md#7", "new")
            assert rc == 0, "a productive round must not cap"


def test_stall_caps_after_3_no_new():
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        _bump(st, "S", "new")
        assert _bump(st, "S", "none")[0] == 0     # streak 1
        assert _bump(st, "S", "none")[0] == 0     # streak 2
        rc, out = _bump(st, "S", "none")          # streak 3 -> capped
        assert rc == 2 and "CAPPED" in out, (rc, out)


def test_new_finding_resets_stall():
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        _bump(st, "S", "none"); _bump(st, "S", "none")   # streak 2
        rc, _ = _bump(st, "S", "new")                    # reset
        assert rc == 0
        assert _bump(st, "S", "none")[0] == 0            # streak back to 1, not capped


if __name__ == "__main__":
    test_builtin_selftest_passes()
    test_productive_review_never_caps()
    test_stall_caps_after_3_no_new()
    test_new_finding_resets_stall()
    print("PASS: review-round-guard")
