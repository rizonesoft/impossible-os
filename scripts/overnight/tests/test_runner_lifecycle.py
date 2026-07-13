#!/usr/bin/env python3
"""Lifecycle-contract + regression-pin tests for the overnight runner.

Why this file exists (2026-07-11): the "structural-wait" apparatus -- a session
that declared a wait, EXITED, and relied on a bespoke background watcher to
relaunch it -- broke the runner repeatedly. Every bug lived in the process-
lifecycle layer (cgroup escape, premature wake, non-atomic state, pid-1
watchers, a self-cancelling wake, a deadlocked flock) while the unit tests
stayed green. The apparatus was ripped out and replaced with an IN-SESSION
blocking poll (a sleeping shell costs ~0 model tokens), leaving the *:0/10
systemd watchdog as the sole relaunch mechanism.

These tests are the guardrail that keeps it gone:

  * The removed guard verbs (wait / wait-ready / wake) stay removed.
  * No session-watcher artifact and no watcher-spawn code comes back.
  * No source references a `wait`/`wake` guard verb (the drift class that left
    a stale "declare a structural wait (`wait` verb)" line in the Stop message
    for hours after the rip-out).
  * The sequencer skill still prescribes the in-session blocking poll.
  * The launcher's flock is the ONLY concurrency guard, and DRYRUN still exits
    cleanly (delegated to the sibling test-launch.sh, invoked by run-all.sh).

Hermetic: guard-CLI checks run from a throwaway fixture so real run state is
never touched; regression-pin checks read the REAL control-plane files (that is
the point -- they protect those files). Plain asserts, stdlib only, matching the
house style of the other scripts/overnight/tests/*.py.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent  # scripts/overnight/tests -> repo root

GUARD = REPO / ".claude/hooks/run_phase_guard.py"
SKILL = REPO / ".claude/skills/overnight-sequencer/SKILL.md"

# The removed verbs -- if any of these is re-added, the whole exit-and-relaunch
# failure mode can come back with it.
REMOVED_VERBS = ("wait", "wait-ready", "wake")

# Files whose wording (not just behavior) must not reference the dead verbs.
# Historical "removed 2026-07-11" notes are allowed; a LIVE instruction to use
# the verb is not.
CONTROL_PLANE_SOURCES = (
    ".claude/hooks/run_phase_guard.py",
    ".claude/hooks/runner_status.py",
    "scripts/overnight/overnight-launch.sh",
    "scripts/overnight/run-outcome.py",
    "scripts/overnight/review-broker-codex-dispatch.sh",
    ".claude/skills/overnight-sequencer/SKILL.md",
    "docs/infrastructure/hook-codes.md",
)


def _mk_fixture(d: pathlib.Path) -> pathlib.Path:
    """A throwaway repo carrying only the guard, so a removed-verb CLI call
    resolves repo_root here and can never mutate the real run state."""
    fx = d / "fx"
    (fx / ".claude/hooks").mkdir(parents=True)
    (fx / ".claude/state").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(fx)], check=True)
    for hook in ("run_phase_guard.py", "sequencer_triage.py"):
        (fx / ".claude/hooks" / hook).write_bytes(
            (REPO / ".claude/hooks" / hook).read_bytes())
    return fx


def _guard(fx, *args):
    return subprocess.run(
        [sys.executable, str(fx / ".claude/hooks/run_phase_guard.py"), *args],
        capture_output=True, text=True, cwd=str(fx))


def test_removed_wait_verbs_are_rejected():
    """Each removed verb must be an unknown command (nonzero exit), NOT a
    silently-accepted no-op that could mask a re-introduction."""
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        for verb in REMOVED_VERBS:
            r = _guard(fx, verb)
            assert r.returncode != 0, \
                f"guard verb {verb!r} unexpectedly succeeded (exit 0)"
            assert "unknown command" in r.stderr.lower(), \
                f"guard verb {verb!r} not reported as unknown: {r.stderr!r}"


def test_no_watcher_artifact_or_spawn_code():
    """The bespoke watcher script and its spawn/cancel helpers stay deleted."""
    assert not (REPO / "scripts/overnight/session-watcher.sh").exists(), \
        "session-watcher.sh is back -- the exit-and-relaunch watcher was removed"
    guard_src = GUARD.read_text()
    for banned in ("_spawn_watcher", "_cancel_watcher", "WAITING_REVIEW",
                   "_wait_satisfied", "_wait_expired", "_artifact_matches"):
        assert banned not in guard_src, \
            f"{banned} re-appeared in run_phase_guard.py (watcher apparatus)"


def test_no_source_references_a_dead_guard_verb():
    """Regression pin for the exact drift that outlived the rip-out: a Stop
    message still told sessions to 'declare a structural wait (`wait` verb)'.
    No control-plane source may instruct use of a removed verb."""
    # Matches `run_phase_guard.py wait`, `... wake`, and "`wait` verb" phrasing.
    dead_call = re.compile(
        r"run_phase_guard\.py\s+(wait|wait-ready|wake)\b"
        r"|`(wait|wake|wait-ready)`\s+verb")
    offenders = []
    for rel in CONTROL_PLANE_SOURCES:
        p = REPO / rel
        if not p.exists():
            continue
        for i, line in enumerate(p.read_text().splitlines(), 1):
            # Allow explicit historical/removal notes.
            low = line.lower()
            if "removed 2026-07-11" in low or "were removed" in low \
               or "was removed" in low or "verbs + watcher" in low:
                continue
            if dead_call.search(line):
                offenders.append(f"{rel}:{i}: {line.strip()}")
    assert not offenders, "live references to removed guard verbs:\n" + \
        "\n".join(offenders)


def test_skill_prescribes_in_session_poll():
    """The sequencer skill must document the in-session blocking poll, not an
    exit-and-wait. This is the doctrine the whole rip-out rests on."""
    src = SKILL.read_text()
    assert "Turn completed" in src, \
        "SKILL.md lost the in-session poll sentinel ('Turn completed')"
    # B1: the in-session poll is the canonical waiter (bounded so a bare call is
    # never killed at the 2m default), not a hand-rolled sleep loop.
    assert "wait-for-codex-verdict.sh" in src, \
        "SKILL.md lost the canonical in-session waiter (wait-for-codex-verdict.sh)"
    assert re.search(r"NEVER exits to wait", src, re.IGNORECASE), \
        "SKILL.md lost the never-exit-to-wait doctrine (the anti-deadlock rule)"


def test_launcher_flock_is_sole_concurrency_guard():
    """The launcher must take the flock and must NOT have grown a bespoke
    session-lifecycle relaunch mechanism (setsid/nohup/disown/backgrounded
    self-exec). The *:0/10 watchdog is the only sanctioned relaunch."""
    src = (REPO / "scripts/overnight/overnight-launch.sh").read_text()
    assert re.search(r"flock\s+-n\s+9", src), \
        "launcher lost its flock concurrency guard"
    # A launcher that re-spawns itself detached is the anti-pattern we removed.
    for banned in (r"\bsetsid\b", r"\bnohup\b", r"\bdisown\b"):
        assert not re.search(banned, src), \
            f"launcher grew a detached-relaunch primitive: {banned}"


if __name__ == "__main__":
    test_removed_wait_verbs_are_rejected()
    test_no_watcher_artifact_or_spawn_code()
    test_no_source_references_a_dead_guard_verb()
    test_skill_prescribes_in_session_poll()
    test_launcher_flock_is_sole_concurrency_guard()
    print("PASS: runner-lifecycle regression pins")
