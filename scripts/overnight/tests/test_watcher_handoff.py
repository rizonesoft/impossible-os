#!/usr/bin/env python3
"""REAL systemd watcher-handoff integration test (2026-07-11 live-incident).

The state-machine tests (test_phase_guard_wait.py) never spawn the actual
watcher, so they all passed despite the fatal cgroup-escape handoff bug: the
watcher launched the runner with a backgrounded `setsid ... &` inside its own
KillMode=control-group transient scope, and systemd killed the runner the
instant the watcher exited. This test spawns the REAL watcher via the REAL
`run_phase_guard.py wait` path and asserts the corrected contract:

    wait declared -> artifact completes -> EXACTLY ONE surviving launcher runs
    to completion -> the wake is consumed.

"Survives to completion" is the crux: a stub launcher records a start marker,
sleeps PAST the point the watcher process would have exited under the old
`setsid &` bug, then records a done marker and consumes the wake. If the
cgroup-escape defect regresses, the launcher is killed between the two markers
and the done marker never appears.

Skips gracefully when a user systemd manager / systemd-run is unavailable
(headless CI without a session bus).
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent.parent.parent


def _systemd_run_available() -> bool:
    if shutil.which("systemd-run") is None:
        return False
    try:
        r = subprocess.run(["systemd-run", "--user", "--collect",
                            "--unit=seq-watcher-probe-%d" % os.getpid(),
                            "/bin/true"], capture_output=True, timeout=10)
        return r.returncode == 0
    except Exception:
        return False


def main() -> int:
    if not _systemd_run_available():
        print("SKIP: systemd-run --user unavailable (no user session bus)")
        return 0

    with tempfile.TemporaryDirectory() as d:
        fx = pathlib.Path(d) / "fx"
        (fx / ".claude/hooks").mkdir(parents=True)
        (fx / ".claude/state").mkdir(parents=True)
        (fx / ".claude/overnight").mkdir(parents=True)
        (fx / "scripts/overnight").mkdir(parents=True)
        # git-init so repo_root() resolves to fx deterministically (the guard
        # computes STATE_PATH from the nearest .git, not cwd).
        subprocess.run(["git", "init", "-q", str(fx)], check=True)
        # Real guard + watcher + triage.
        for rel in (".claude/hooks/run_phase_guard.py",
                    ".claude/hooks/sequencer_triage.py",
                    "scripts/overnight/session-watcher.sh"):
            (fx / rel).write_bytes((REPO / rel).read_bytes())
        (fx / "scripts/overnight/session-watcher.sh").chmod(0o755)
        (fx / ".claude/state/sequencer-armed").write_text("")

        # STUB launcher: proves survival across the watcher's would-be exit.
        marker = fx / ".claude/overnight/launcher-markers.txt"
        stub = fx / "scripts/overnight/overnight-launch.sh"
        stub.write_text(
            "#!/usr/bin/env bash\n"
            "set -u\n"
            f'M="{marker}"\n'
            'echo "START pid=$$ $(date +%s.%N)" >> "$M"\n'
            # Sleep well past the watcher's exit point under the old bug.
            "sleep 3\n"
            # Consume the wake via the real guard (like the launcher's `wake`).
            f'python3 "{fx}/.claude/hooks/run_phase_guard.py" wake '
            '>> "$M.wake" 2>&1 || true\n'
            'echo "DONE pid=$$ $(date +%s.%N)" >> "$M"\n')
        stub.chmod(0o755)

        def guard(*args):
            return subprocess.run(
                [sys.executable, str(fx / ".claude/hooks/run_phase_guard.py"),
                 *args], capture_output=True, text=True, cwd=str(fx),
                env={**os.environ, "SEQ_WATCHER_POLL_SECS": "1"})

        guard("start", "2026-07-11")
        art = fx / "review.out"
        art.write_text("reviewing...\n")

        # Declare the wait -> spawns the REAL watcher in its own systemd scope.
        r = guard("wait", "120", "integration review", str(art), "Turn completed")
        if r.returncode != 0:
            print("FAIL: wait declaration failed:", r.stderr)
            return 1
        st = json.loads((fx / ".claude/state/sequencer-run.json").read_text())
        unit = (st.get("waiting") or {}).get("watcher_unit")
        if not unit:
            print("FAIL: no watcher_unit recorded (systemd path not taken)")
            return 1

        # Complete the artifact; the 1s-poll watcher resolves, then execs the
        # stub launcher (which must survive to write DONE).
        time.sleep(1.5)
        art.write_text("reviewing...\nTurn completed after 900s\n")

        # Wait for the launcher to run to completion (START + DONE).
        deadline = time.time() + 40
        while time.time() < deadline:
            if marker.exists() and "DONE" in marker.read_text():
                break
            time.sleep(1)

        fails = []
        text = marker.read_text() if marker.exists() else ""
        starts = text.count("START")
        dones = text.count("DONE")
        if starts == 0:
            fails.append("launcher never ran (watcher handoff failed entirely)")
        if starts != 1:
            fails.append(f"expected EXACTLY ONE launcher, got {starts}")
        if dones != 1:
            fails.append(f"launcher did not survive to completion "
                         f"(DONE count={dones}; cgroup-escape regression?)")
        # Wake consumed: no active wait, woke_from_wait recorded.
        st = json.loads((fx / ".claude/state/sequencer-run.json").read_text())
        if st.get("waiting"):
            fails.append("wait not consumed (still declared after wake)")
        if not st.get("woke_from_wait"):
            fails.append("woke_from_wait note not left by wake")

        # Cleanup any lingering unit.
        subprocess.run(["systemctl", "--user", "stop", unit],
                       capture_output=True, timeout=10)

        if fails:
            for f in fails:
                print("FAIL:", f)
            print("--- launcher markers ---\n" + text)
            return 1
    print("PASS: watcher handoff (survives exec, exactly one launcher, wake consumed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
