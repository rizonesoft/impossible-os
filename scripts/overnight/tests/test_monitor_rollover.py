#!/usr/bin/env python3
"""Contract test for overnight-monitor.sh rollover survival.

`tail -F latest.log` does NOT re-resolve when the symlink is repointed on GNU
coreutils 9.x (verified 9.4): the monitor went silent at every watchdog
relaunch. This pins the fixed behavior -- the monitor must keep following after
the latest.log symlink is repointed to a new run-*.log, and emit a rollover
marker. Justifies overnight-monitor.sh's place on the deterministic canary
allowlist (a read-only bystander tool, not run-control logic).
"""
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
MONITOR = HERE.parent / "overnight-monitor.sh"


def test_monitor_survives_rollover():
    with tempfile.TemporaryDirectory() as d:
        rd = pathlib.Path(d)
        (rd / "run-A.log").write_text("A1\n")
        os.symlink("run-A.log", rd / "latest.log")

        env = dict(os.environ, OVERNIGHT_REPORT_DIR=str(rd),
                   OVERNIGHT_MONITOR_LINES="50")
        proc = subprocess.Popen(
            ["bash", str(MONITOR), "--no-wait"], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, preexec_fn=os.setsid)
        try:
            time.sleep(1.5)
            with open(rd / "run-A.log", "a") as fh:
                fh.write("A2\n")
            time.sleep(1.5)
            # Rollover: new run log + repoint the symlink atomically.
            (rd / "run-B.log").write_text("B1\n")
            tmp = rd / ".latest.tmp"
            os.symlink("run-B.log", tmp)
            os.replace(tmp, rd / "latest.log")
            time.sleep(2.0)
            with open(rd / "run-B.log", "a") as fh:
                fh.write("B2\n")
            time.sleep(2.5)
        finally:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            try:
                out = proc.communicate(timeout=5)[0]
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                out = proc.communicate()[0]

    for needle in ("A1", "A2", "B1", "B2", "rollover"):
        assert needle in out, f"missing {needle!r} in monitor output:\n{out}"


if __name__ == "__main__":
    test_monitor_survives_rollover()
    print("PASS: overnight-monitor rollover survival")
