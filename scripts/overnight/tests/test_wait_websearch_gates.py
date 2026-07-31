#!/usr/bin/env python3
"""Tests for R3 (codex_wait_discipline.py) + R4 (websearch_offload_gate.py).

R3: headless verdict waits must be ONE long call (--max >= 300 AND a Bash tool
timeout that outlives it), not the 100s poll loop (51 polls x ~350K cached
tokens measured in run-20260719-022200). R4: headless MAIN-session
WebSearch/WebFetch reroutes to the researcher agents; subagent calls pass
(the researchers do the searching). Both are inert outside the headless run
(OVERNIGHT_SEQUENCER_RUN=1 discriminator) and fail-open on garbage input.
"""
import importlib.util
import io
import tempfile
from pathlib import Path
import json
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
HOOKS = HERE.parents[2] / ".claude/hooks"


def _load(name):
    if str(HOOKS) not in sys.path:
        sys.path.insert(0, str(HOOKS))
    spec = importlib.util.spec_from_file_location(name, HOOKS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(mod, payload, headless=True):
    old_env = os.environ.get("OVERNIGHT_SEQUENCER_RUN")
    if headless:
        os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
    else:
        os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
    old = (sys.stdin, sys.stdout, sys.stderr)
    sys.stdin, sys.stdout, sys.stderr = (io.StringIO(json.dumps(payload)),
                                         io.StringIO(), io.StringIO())
    try:
        rc = mod.main()
        err = sys.stderr.getvalue()
    finally:
        sys.stdin, sys.stdout, sys.stderr = old
        if old_env is None:
            os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
        else:
            os.environ["OVERNIGHT_SEQUENCER_RUN"] = old_env
    return rc, err


def _bash(cmd, timeout=None):
    ti = {"command": cmd}
    if timeout is not None:
        ti["timeout"] = timeout
    return {"tool_name": "Bash", "tool_input": ti}


WAIT = "bash scripts/overnight/wait-for-codex-verdict.sh"


def test_r3_short_poll_blocked():
    mod = _load("codex_wait_discipline")
    # bare call (script default --max 100) -> BLOCK
    rc, err = _run(mod, _bash(f"{WAIT} .claude/overnight/reviews/x.out"))
    assert rc == 2 and "codex-wait-discipline BLOCK" in err, (rc, err)
    # --max long enough but NO tool timeout -> the ~120s default kill would
    # truncate the wait (B1) -> still BLOCK
    rc, err = _run(mod, _bash(f"{WAIT} --max 540 x.out"))
    assert rc == 2, (rc, err)
    # --max below the floor even with a timeout -> BLOCK
    rc, err = _run(mod, _bash(f"{WAIT} --max 100 x.out", timeout=600000))
    assert rc == 2, (rc, err)


def test_r3_long_wait_passes():
    mod = _load("codex_wait_discipline")
    rc, err = _run(mod, _bash(f"{WAIT} --max 540 x.out", timeout=600000))
    assert rc == 0 and err.strip() == "", (rc, err)
    rc, err = _run(mod, _bash(f"{WAIT} --max=300 x.out y.out", timeout=400000))
    assert rc == 0, (rc, err)


def test_r3_inert_interactive_and_unrelated():
    mod = _load("codex_wait_discipline")
    # interactive session (no env discriminator): bare short poll is fine
    rc, err = _run(mod, _bash(f"{WAIT} x.out"), headless=False)
    assert rc == 0 and err.strip() == "", (rc, err)
    # unrelated Bash commands never gate
    rc, err = _run(mod, _bash("git status"))
    assert rc == 0, (rc, err)


def _isolated_valve(tmp):
    """Point the R4 valve at a fixture file.

    MANDATORY for every R4 test that reaches the valve. Without it the tests
    write the LIVE `.claude/state/websearch-offload-valve.json`, counts survive
    between suite runs, and the third consecutive run would see the valve
    release a call the test asserts is blocked -- a self-inflicted instance of
    the flaky-control-plane-test class filed on 2026-07-31.
    """
    os.environ["WEBSEARCH_VALVE_STATE"] = str(tmp)


def test_r4_main_session_blocked_subagent_passes():
    mod = _load("websearch_offload_gate")
    with tempfile.TemporaryDirectory() as td:
        _isolated_valve(Path(td) / "valve.json")
        try:
            main_payload = {"tool_name": "WebSearch", "session_id": "t1",
                            "tool_input": {"query": "win11 quota semantics"},
                            "transcript_path": "/x/session-abc.jsonl"}
            rc, err = _run(mod, main_payload)
            assert rc == 2 and "websearch-offload BLOCK" in err, (rc, err)
            # the block names the valve, so a misclassified researcher learns
            # that retrying is the escape rather than giving up uncited
            assert "releases it after" in err, err
            # WebFetch routes the same way (and is a distinct valve key)
            rc, err = _run(mod, {**main_payload, "tool_name": "WebFetch"})
            assert rc == 2, (rc, err)
            # a researcher subagent's search passes untouched (no deadlock)
            agent_payload = {**main_payload,
                             "transcript_path": "/x/subagents/agent-123.jsonl"}
            rc, err = _run(mod, agent_payload)
            assert rc == 0 and err.strip() == "", (rc, err)
        finally:
            os.environ.pop("WEBSEARCH_VALVE_STATE", None)


def test_r4_subagent_identified_by_payload_not_parent_transcript():
    """The 2026-07-31 defect: a subagent payload carries the PARENT transcript.

    Keying on transcript_path alone read every researcher as main-session and
    blocked it -- three consecutive sections came back uncited. Each positive
    marker must independently identify the caller.
    """
    mod = _load("websearch_offload_gate")
    with tempfile.TemporaryDirectory() as td:
        _isolated_valve(Path(td) / "valve.json")
        try:
            base = {"tool_name": "WebSearch", "session_id": "t2",
                    "tool_input": {"query": "junit error semantics"},
                    "transcript_path": "/x/parent-session.jsonl"}
            for marker in ({"agent_transcript_path": "/x/subagents/agent-z.jsonl"},
                           {"agent_id": "agent-z"},
                           {"agent_type": "parity-research-analyst"},
                           {"subagent_type": "web-research-analyst"}):
                rc, err = _run(mod, {**base, **marker})
                assert rc == 0 and err.strip() == "", (sorted(marker), rc, err)
            # the parent transcript alone is NOT identity -> still gated
            rc, _ = _run(mod, base)
            assert rc == 2, rc
        finally:
            os.environ.pop("WEBSEARCH_VALVE_STATE", None)


def test_r4_valve_releases_the_same_call_after_max_blocks():
    """ANTI-WEDGE: a headless run cannot unset the env, so a wrongly blocked
    call must not be blocked forever."""
    mod = _load("websearch_offload_gate")
    with tempfile.TemporaryDirectory() as td:
        _isolated_valve(Path(td) / "valve.json")
        try:
            call = {"tool_name": "WebSearch", "session_id": "t3",
                    "tool_input": {"query": "taef hlk conventions"},
                    "transcript_path": "/x/session-abc.jsonl"}
            seen = [_run(mod, call)[0] for _ in range(mod.MAX_BLOCKS + 2)]
            assert seen[:mod.MAX_BLOCKS] == [2] * mod.MAX_BLOCKS, seen
            assert seen[mod.MAX_BLOCKS:] == [0, 0], seen
            # releasing one call must NOT disable the gate for another
            other = {**call, "tool_input": {"query": "a different question"}}
            assert _run(mod, other)[0] == 2, "valve leaked across calls"
        finally:
            os.environ.pop("WEBSEARCH_VALVE_STATE", None)


def test_r4_valve_is_session_scoped_and_fails_open():
    mod = _load("websearch_offload_gate")
    with tempfile.TemporaryDirectory() as td:
        _isolated_valve(Path(td) / "valve.json")
        try:
            call = {"tool_name": "WebSearch", "session_id": "s-old",
                    "tool_input": {"query": "same question"},
                    "transcript_path": "/x/session-abc.jsonl"}
            for _ in range(mod.MAX_BLOCKS):
                assert _run(mod, call)[0] == 2
            # a NEW session starts a fresh budget -- no leak across runs
            assert _run(mod, {**call, "session_id": "s-new"})[0] == 2
        finally:
            os.environ.pop("WEBSEARCH_VALVE_STATE", None)
    # a state path that cannot be persisted must fail OPEN, never block
    # forever (the wedge the valve exists to prevent)
    os.environ["WEBSEARCH_VALVE_STATE"] = "/proc/nonexistent-dir/valve.json"
    try:
        rc, _ = _run(mod, {"tool_name": "WebSearch", "session_id": "s-x",
                           "tool_input": {"query": "q"},
                           "transcript_path": "/x/session-abc.jsonl"})
        assert rc == 0, f"unpersistable valve state must release, got {rc}"
    finally:
        os.environ.pop("WEBSEARCH_VALVE_STATE", None)


def test_r4_embedded_selftest_passes():
    """The hook's own 20-case fixture suite, run as part of the gate suite."""
    import subprocess
    hook = Path(__file__).resolve().parents[3] / ".claude/hooks/websearch_offload_gate.py"
    r = subprocess.run([sys.executable, str(hook), "--selftest"],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr


def test_r4_inert_interactive():
    mod = _load("websearch_offload_gate")
    payload = {"tool_name": "WebSearch", "tool_input": {"query": "q"},
               "transcript_path": "/x/session-abc.jsonl"}
    rc, err = _run(mod, payload, headless=False)
    assert rc == 0 and err.strip() == "", (rc, err)


def test_both_fail_open_on_garbage():
    for name in ("codex_wait_discipline", "websearch_offload_gate"):
        mod = _load(name)
        old = (sys.stdin, sys.stdout, sys.stderr)
        os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
        sys.stdin, sys.stdout, sys.stderr = (io.StringIO("not json"),
                                             io.StringIO(), io.StringIO())
        try:
            rc = mod.main()
        finally:
            sys.stdin, sys.stdout, sys.stderr = old
            os.environ.pop("OVERNIGHT_SEQUENCER_RUN", None)
        assert rc == 0, (name, rc)


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: R3 wait-discipline + R4 websearch-offload gates")
