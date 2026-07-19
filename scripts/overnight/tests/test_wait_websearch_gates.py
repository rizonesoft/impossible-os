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


def test_r4_main_session_blocked_subagent_passes():
    mod = _load("websearch_offload_gate")
    main_payload = {"tool_name": "WebSearch",
                    "tool_input": {"query": "win11 quota semantics"},
                    "transcript_path": "/x/session-abc.jsonl"}
    rc, err = _run(mod, main_payload)
    assert rc == 2 and "websearch-offload BLOCK" in err, (rc, err)
    # WebFetch routes the same way
    rc, err = _run(mod, {**main_payload, "tool_name": "WebFetch"})
    assert rc == 2, (rc, err)
    # a researcher subagent's search passes untouched (no deadlock)
    agent_payload = {**main_payload,
                     "transcript_path": "/x/subagents/agent-123.jsonl"}
    rc, err = _run(mod, agent_payload)
    assert rc == 0 and err.strip() == "", (rc, err)


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
