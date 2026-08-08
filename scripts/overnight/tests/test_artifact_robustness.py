#!/usr/bin/env python3
# Protects I1 (2026-07-13 findings): a broken build must not pass silently.
# Two surfaces:
#   1. artifact_pipe_guard.py warns when run-artifact.sh is piped into an
#      exit-masking stage (tail/head/...), stays silent on standalone / guarded
#      / `| python3` parse shapes.
#   2. run-artifact.sh writes an authoritative `.claude/state/last-artifact.json`
#      (real exit, un-maskable) and seeds the failure ledger on a nonzero result.
import json
import os
import subprocess
import sys
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
GUARD = ROOT / ".claude" / "hooks" / "artifact_pipe_guard.py"
RUN_ARTIFACT = HERE.parent / "run-artifact.sh"


def _guard(cmd: str):
    """Run the pipe guard with a Bash payload; return the emitted systemMessage
    (or '' if silent)."""
    payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": cmd}})
    r = subprocess.run([sys.executable, str(GUARD)], input=payload,
                       text=True, capture_output=True)
    if not r.stdout.strip():
        return ""
    return json.loads(r.stdout).get("systemMessage", "")


def test_guard_warns_on_tail():
    msg = _guard("bash scripts/overnight/run-artifact.sh build1 -- "
                 "bash scripts/build.sh 2>&1 | tail -20")
    assert "not a block" in msg and "MASKING" in msg, repr(msg)


def test_guard_warns_on_grep_and_head():
    assert _guard("run-artifact.sh t -- make | grep -i error")
    assert _guard("run-artifact.sh t -- make | head -5")


def test_guard_silent_standalone():
    assert _guard("bash scripts/overnight/run-artifact.sh build1 -- "
                  "bash scripts/build.sh") == ""


def test_guard_silent_on_pipefail_guarded():
    # A guarded pipeline propagates the real status -- must not nag.
    assert _guard("set -o pipefail; run-artifact.sh t -- make | tail -5") == ""
    assert _guard("run-artifact.sh t -- make | tail -5; "
                  "exit ${PIPESTATUS[0]}") == ""


def test_guard_silent_on_json_parse():
    # Piping into python3/jq is legitimate envelope parsing (reads .exit).
    assert _guard("run-artifact.sh t -- make | python3 -c "
                  "'import json,sys; print(json.load(sys.stdin)[\"exit\"])'") == ""
    assert _guard("run-artifact.sh t -- make | jq .exit") == ""


def test_guard_ignores_unrelated_pipes():
    assert _guard("bash scripts/build.sh 2>&1 | tail -20") == ""


def _run_artifact(workdir: pathlib.Path, label: str, *cmd):
    r = subprocess.run(["bash", str(RUN_ARTIFACT), label, "--", *cmd],
                       cwd=str(workdir), text=True, capture_output=True)
    return r


def test_run_artifact_failure_writes_authoritative_exit():
    with tempfile.TemporaryDirectory() as d:
        wd = pathlib.Path(d)
        r = _run_artifact(wd, "buildfail", "bash", "-c", "echo boom; exit 3")
        # The script itself propagates the real exit (standalone invocation).
        assert r.returncode == 3, r.returncode
        # Un-maskable sink records the true exit.
        rec = json.loads((wd / ".claude/state/last-artifact.json").read_text())
        assert rec["exit"] == 3, rec
        assert rec["label"] == "buildfail"
        # Ledger seeded so a recurrence is detected.
        ledger = wd / ".claude/state/failure-ledger.jsonl"
        assert ledger.is_file(), "nonzero result must seed the failure ledger"
        entries = [json.loads(x) for x in ledger.read_text().splitlines() if x.strip()]
        assert any(e.get("kind") == "buildfail" for e in entries), entries


def test_run_artifact_success_no_ledger():
    with tempfile.TemporaryDirectory() as d:
        wd = pathlib.Path(d)
        r = _run_artifact(wd, "buildok", "bash", "-c", "echo fine; true")
        assert r.returncode == 0, r.returncode
        rec = json.loads((wd / ".claude/state/last-artifact.json").read_text())
        assert rec["exit"] == 0, rec
        # A green run must NOT write the failure ledger.
        assert not (wd / ".claude/state/failure-ledger.jsonl").exists()


def test_run_artifact_marks_in_flight_state():
    """A backgrounded launch returns to the harness when the SUBSHELL forks, so
    the tool result says `exit code 0` while the command is still running --
    observed 2026-08-08 against a suite whose envelope later said FAIL. Nothing
    here can change what the harness reports, so the SINK has to be able to say
    "not finished": `state` is `running` with `exit` null until the command
    returns. The assertion is the whole point of the fix, so it reads the sink
    MID-RUN rather than after."""
    import time
    with tempfile.TemporaryDirectory() as d:
        wd = pathlib.Path(d)
        sink = wd / ".claude/state/last-artifact.json"
        proc = subprocess.Popen(
            ["bash", str(RUN_ARTIFACT), "slow", "--", "sleep", "3"],
            cwd=str(wd), text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        try:
            deadline = time.time() + 5
            rec = None
            while time.time() < deadline:
                if sink.is_file():
                    try:
                        rec = json.loads(sink.read_text())
                    except ValueError:
                        rec = None
                    if rec:
                        break
                time.sleep(0.05)
            assert rec is not None, "no in-flight record was written"
            assert rec["state"] == "running", rec
            assert rec["exit"] is None, rec
            assert rec["label"] == "slow", rec
        finally:
            proc.wait(timeout=30)
        done = json.loads(sink.read_text())
        assert done["state"] == "complete" and done["exit"] == 0, done


def test_run_artifact_failure_recurrence_increments():
    with tempfile.TemporaryDirectory() as d:
        wd = pathlib.Path(d)
        for _ in range(2):
            _run_artifact(wd, "flaky", "bash", "-c", "echo same-error >&2; exit 1")
        ledger = wd / ".claude/state/failure-ledger.jsonl"
        entries = [json.loads(x) for x in ledger.read_text().splitlines() if x.strip()]
        flaky = [e for e in entries if e.get("kind") == "flaky"]
        assert flaky and flaky[-1].get("count", 1) >= 2, entries


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: artifact-robustness")
