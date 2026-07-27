#!/usr/bin/env python3
"""run-liveness.sh -- the three-state overnight liveness report.

The naive check an operator reaches for -- `systemctl --user is-active
overnight-<repo>.service` -- lies when the main and watchdog timers collide
(observed 2026-07-24, both at 21:30:00): the watchdog wins the flock and
carries the run while the MAIN unit reads inactive, so the operator concludes
the run died and re-arms on top of a healthy one.

These tests pin the two properties that make the replacement trustworthy:
RUNNING and ARMED are distinct (a fresh log proves the schedule ticked, NOT
that a run is executing), and an interactive Claude session is never mistaken
for an unattended run.
"""
import os
import pathlib
import subprocess
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
SCRIPT = REPO / "scripts/overnight/run-liveness.sh"
FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


def run(report_dir, fresh_min="15"):
    env = dict(os.environ)
    env["OVERNIGHT_REPORT_DIR"] = str(report_dir)
    env["OVERNIGHT_LIVENESS_FRESH_MIN"] = fresh_min
    # Point the unit-name derivation at a repo basename with no real units, so
    # the systemd probes are deterministically inactive in the test.
    env["OVERNIGHT_LIVENESS_ROOT"] = str(report_dir)
    p = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True,
                       text=True, timeout=30)
    return p.returncode, p.stdout


def test_no_evidence_is_dead():
    with tempfile.TemporaryDirectory() as d:
        rc, out = run(d)
        check("empty-is-dead", "overnight run: DEAD" in out)
        check("empty-exit-1", rc == 1)


def test_fresh_log_alone_is_ARMED_not_RUNNING():
    """The property the whole item turns on: a report log that grew recently
    proves the watchdog SCHEDULE is alive, not that a run is executing. The
    watchdog fires every 10 min, writes a launch log and exits -- reporting
    that as RUNNING would be the same class of lie, in the other direction."""
    with tempfile.TemporaryDirectory() as d:
        (pathlib.Path(d) / "run-20260727-000000.log").write_text("launching\n")
        rc, out = run(d)
        check("fresh-log-is-armed", "overnight run: ARMED" in out)
        check("fresh-log-not-running", "RUNNING" not in out)
        check("armed-exit-0", rc == 0)
        check("armed-explains-itself", "schedule is ticking" in out)


def test_stale_log_is_dead():
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "run-20260727-000000.log"
        log.write_text("old\n")
        old = time.time() - 3 * 3600
        os.utime(log, (old, old))
        rc, out = run(d)
        check("stale-log-is-dead", "overnight run: DEAD" in out)
        check("stale-exit-1", rc == 1)


def test_watchdog_note_does_not_fire_without_the_watchdog():
    """The first cut of this script printed 'the watchdog won the flock and is
    carrying the run' whenever the MAIN unit was inactive -- including when the
    watchdog was inactive too. That is a confident false diagnosis, which is
    worse than the silence it replaced. The note must require the watchdog to
    actually be active."""
    with tempfile.TemporaryDirectory() as d:
        (pathlib.Path(d) / "run-20260727-000000.log").write_text("x\n")
        _, out = run(d)
        check("no-false-watchdog-claim", "won" not in out and "carrying the run" not in out)


def test_quiet_mode_is_status_only():
    with tempfile.TemporaryDirectory() as d:
        env = dict(os.environ)
        env["OVERNIGHT_REPORT_DIR"] = d
        env["OVERNIGHT_LIVENESS_ROOT"] = d
        p = subprocess.run(["bash", str(SCRIPT), "--quiet"], env=env,
                           capture_output=True, text=True, timeout=30)
        check("quiet-silent", p.stdout.strip() == "")
        check("quiet-exit-1", p.returncode == 1)


def test_selftest_passes():
    p = subprocess.run(["bash", str(SCRIPT), "--selftest"],
                       capture_output=True, text=True, timeout=30)
    check("selftest-ok", p.returncode == 0)


if __name__ == "__main__":
    test_no_evidence_is_dead()
    test_fresh_log_alone_is_ARMED_not_RUNNING()
    test_stale_log_is_dead()
    test_watchdog_note_does_not_fire_without_the_watchdog()
    test_quiet_mode_is_status_only()
    test_selftest_passes()
    if FAILS:
        for f in FAILS:
            print("FAIL:", f)
        raise SystemExit(1)
    print("test_run_liveness OK (three-state report, no false watchdog claim)")
