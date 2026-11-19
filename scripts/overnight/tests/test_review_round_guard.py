#!/usr/bin/env python3
# P2.3 wires review_round_guard.py into the review fix-loop (per-section round
# counter + STALL-based convergence cap: stop after K rounds with no NEW
# Critical/High, plus a high ceiling as an infinite-loop guard -- NEVER a fixed
# round cap, so a productive deep review runs as long as it keeps finding bugs).
# This puts it under the deterministic suite so the wiring can't silently rot.
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parents[2] / ".claude" / "hooks" / "review_round_guard.py"


def _bump(state, slice_id, progress, highest="P2", paths=("src/kernel/sched.c",)):
    # The stall cap is fixed at 3 here: these tests cover the guard's behaviour, and the repository pin's own
    # numbers (which the operator changes) are covered by test_review_pin.py. The default paths make the
    # section deep (the new-finding rule); the severity tests pass tooling paths instead.
    listed = pathlib.Path(state).with_suffix(".paths")
    listed.write_text("".join(f"{p}\n" for p in paths))
    r = subprocess.run([sys.executable, str(SCRIPT), "--bump", slice_id, "--highest", highest,
                        "--progress", progress, "--paths-file", str(listed), "--state", str(state)],
                       text=True, capture_output=True,
                       env={**os.environ, "OVERNIGHT_REVIEW_NOPROGRESS_CAP": "3"})
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


TOOLING = ("scripts/inventory/capture.py",)


def test_a_new_edge_case_of_the_same_severity_stalls_outside_the_deep_paths():
    """Operator, 2026-11-19: a section outside boot and kernel at round 23, a new P2 on every fix, never stalled."""
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        assert _bump(st, "S", "new", "P2", TOOLING)[0] == 0      # first round: progress
        assert _bump(st, "S", "new", "P2", TOOLING)[0] == 0      # streak 1, though the finding is new
        assert _bump(st, "S", "new", "P2", TOOLING)[0] == 0      # streak 2
        rc, out = _bump(st, "S", "new", "P2", TOOLING)           # streak 3 -> capped
        assert rc == 2 and "CAPPED" in out and "severity rule" in out, (rc, out)


def test_a_falling_severity_is_progress():
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        _bump(st, "S", "new", "P1", TOOLING)
        _bump(st, "S", "new", "P1", TOOLING)                      # streak 1
        rc, out = _bump(st, "S", "none", "P2", TOOLING)           # P1 -> P2 resets
        assert rc == 0 and "streak 0/" in out, out


def test_a_kernel_section_keeps_the_new_finding_rule():
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        for _ in range(6):
            rc, out = _bump(st, "S", "new", "P2", ("src/kernel/sched.c", "docs/x.md"))
            assert rc == 0 and "deep section" in out, out


def _raw(state, *args):
    env = {k: v for k, v in os.environ.items() if k != "REVIEW_BASE"}
    r = subprocess.run([sys.executable, str(SCRIPT), "--bump", "S", *args, "--state", str(state)],
                       text=True, capture_output=True, env={**env, "OVERNIGHT_REVIEW_NOPROGRESS_CAP": "3"})
    return r.returncode, r.stdout, r.stderr


def test_without_a_section_scope_the_longer_rule_counts():
    """No --base, no REVIEW_BASE and no paths file: the section's files are unknown, so it is taken as deep."""
    with tempfile.TemporaryDirectory() as d:
        for _ in range(5):
            rc, out, _ = _raw(pathlib.Path(d) / "rr", "--highest", "P2", "--progress", "new")
            assert rc == 0 and "deep section" in out, out


def test_a_legacy_reply_without_highest_is_counted_by_the_new_finding_rule():
    with tempfile.TemporaryDirectory() as d:
        rc, out, _ = _raw(pathlib.Path(d) / "rr", "--progress", "new")
        assert rc == 0 and "deep section" in out, out


def test_a_dangling_option_is_refused_before_the_state_changes():
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        rc, _, err = _raw(st, "--highest", "P2", "--paths-file")
        assert rc == 1 and "--paths-file needs a value" in err, err
        assert not st.exists(), "a refused bump wrote the counter"


def test_a_dotted_kernel_path_is_deep():
    with tempfile.TemporaryDirectory() as d:
        for _ in range(5):
            rc, out = _bump(pathlib.Path(d) / "rr", "S", "new", "P2", ("./src/kernel/x.c",))
            assert rc == 0 and "deep section" in out, out


def test_a_worse_severity_is_no_progress():
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        _bump(st, "S", "new", "P2", TOOLING)
        rc, out = _bump(st, "S", "new", "P1", TOOLING)
        assert rc == 0 and "streak 1/" in out, out


def test_a_switch_to_the_deep_rule_starts_its_streak_again():
    """Codex review 2026-11-19: a severity streak of two carried into the first deep round capped it."""
    with tempfile.TemporaryDirectory() as d:
        st = pathlib.Path(d) / "rr"
        for _ in range(3):
            _bump(st, "S", "new", "P2", TOOLING)                  # severity streak 2
        rc, out = _bump(st, "S", "none", "P2")                     # first deep round, nothing new
        assert rc == 0 and "streak 1/" in out, out


if __name__ == "__main__":
    test_builtin_selftest_passes()
    test_productive_review_never_caps()
    test_stall_caps_after_3_no_new()
    test_new_finding_resets_stall()
    test_a_new_edge_case_of_the_same_severity_stalls_outside_the_deep_paths()
    test_a_falling_severity_is_progress()
    test_a_kernel_section_keeps_the_new_finding_rule()
    test_without_a_section_scope_the_longer_rule_counts()
    test_a_legacy_reply_without_highest_is_counted_by_the_new_finding_rule()
    test_a_dangling_option_is_refused_before_the_state_changes()
    test_a_dotted_kernel_path_is_deep()
    test_a_worse_severity_is_no_progress()
    test_a_switch_to_the_deep_rule_starts_its_streak_again()
    print("PASS: review-round-guard")
