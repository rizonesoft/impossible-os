#!/usr/bin/env python3
"""Contract test for agent_result_cache.py (content-addressed agent reports)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".claude/hooks/agent_result_cache.py"

REPORT = ("Evidence map:\n- src/kernel/a.c:10 implements the frob path\n"
          "- include/kernel/a.h:5 contract struct\n"
          "Verify-first: src/kernel/a.c:10-40\n" * 3)


def _mk_fixture(d: pathlib.Path) -> pathlib.Path:
    fx = d / "fx"
    (fx / "src/kernel").mkdir(parents=True)
    (fx / "todo").mkdir()
    subprocess.run(["git", "init", "-q", str(fx)], check=True)
    (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
    (fx / "todo/TODO-01.md").write_text("# t\n")
    subprocess.run(["git", "-C", str(fx), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(fx), "-c", "user.email=t@t",
                    "-c", "user.name=t", "commit", "-qm", "init"],
                   check=True, capture_output=True)
    return fx


def _hook(fx, mode, payload):
    return subprocess.run([sys.executable, str(HOOK), mode],
                          input=json.dumps(payload), capture_output=True,
                          text=True, cwd=str(fx),
                          env={**os.environ})


def _payload(prompt, response=None):
    p = {"tool_name": "Agent",
         "tool_input": {"subagent_type": "kernel-explorer", "prompt": prompt}}
    if response is not None:
        p["tool_response"] = response
    return p


def test_cache_roundtrip_and_invalidation():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        prompt = "Map the frob path integration surface in src/kernel."
        # pre before any store: pass-through (rc 0)
        assert _hook(fx, "pre", _payload(prompt)).returncode == 0
        # post stores the report
        assert _hook(fx, "post", _payload(prompt, REPORT)).returncode == 0
        assert list((fx / ".claude/state/agent-cache").glob("*.json"))
        # identical dispatch -> BLOCK with the cached report
        r = _hook(fx, "pre", _payload(prompt))
        assert r.returncode == 2 and "cached kernel-explorer report" in r.stderr
        assert "frob path" in r.stderr
        # whitespace-only prompt difference still hits (normalized)
        r = _hook(fx, "pre", _payload("Map the frob   path\nintegration surface in src/kernel."))
        assert r.returncode == 2
        # different prompt -> miss
        assert _hook(fx, "pre", _payload(prompt + " fresh run: retest")).returncode == 0
        # source content change -> miss (fingerprint moved)
        (fx / "src/kernel/a.c").write_text("int a(void){return 2;}\n")
        assert _hook(fx, "pre", _payload(prompt)).returncode == 0
        # revert -> hit again (content-addressed, no clock)
        (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        assert _hook(fx, "pre", _payload(prompt)).returncode == 2


def test_non_cacheable_agent_passthrough():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        p = {"tool_name": "Agent",
             "tool_input": {"subagent_type": "checks-runner", "prompt": "build"},
             "tool_response": REPORT}
        assert _hook(fx, "post", p).returncode == 0
        assert not (fx / ".claude/state/agent-cache").exists()
        assert _hook(fx, "pre", p).returncode == 0


def _leaf_transcript(fx, prompt, report):
    """A subagent's own transcript: first user message == the dispatched prompt,
    then an assistant reply (mirrors the real background-subagent JSONL)."""
    tp = fx / "leaf.jsonl"
    tp.write_text(
        json.dumps({"message": {"role": "user", "content": prompt}}) + "\n"
        + json.dumps({"message": {"role": "assistant",
                     "content": [{"type": "text", "text": report}]}}) + "\n")
    return tp


def test_subagentstop_store_then_pre_hit():
    # The Agent tool runs subagents in the BACKGROUND, so the report only
    # arrives at SubagentStop. Storing there must produce a key a later `pre`
    # (which sees the same prompt via tool_input) hits.
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        prompt = "Map the frob path integration surface in src/kernel."
        tp = _leaf_transcript(fx, prompt, REPORT)
        # pre before store: miss
        assert _hook(fx, "pre", _payload(prompt)).returncode == 0
        # SubagentStop payload (agent_type + last_assistant_message + leaf path)
        stop = {"agent_type": "kernel-explorer",
                "last_assistant_message": REPORT,
                "agent_transcript_path": str(tp)}
        assert _hook(fx, "subagentstop", stop).returncode == 0
        assert list((fx / ".claude/state/agent-cache").glob("*.json")), \
            "SubagentStop did not store a cache entry"
        # now the identical dispatch is served from cache
        r = _hook(fx, "pre", _payload(prompt))
        assert r.returncode == 2 and "frob path" in r.stderr, r.stderr
        # non-cacheable agent type at SubagentStop is ignored
        stop2 = {"agent_type": "checks-runner",
                 "last_assistant_message": REPORT,
                 "agent_transcript_path": str(tp)}
        n0 = len(list((fx / ".claude/state/agent-cache").glob("*.json")))
        assert _hook(fx, "subagentstop", stop2).returncode == 0
        assert len(list((fx / ".claude/state/agent-cache").glob("*.json"))) == n0


def test_kill_switch_and_tiny_reports():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        prompt = "Map it."
        # tiny (<80 chars) responses are not cached (likely an error string)
        assert _hook(fx, "post", _payload(prompt, "err")).returncode == 0
        assert not list((fx / ".claude/state/agent-cache").glob("*.json")) \
            if (fx / ".claude/state/agent-cache").exists() else True
        # kill switch bypasses even a real hit
        assert _hook(fx, "post", _payload(prompt, REPORT)).returncode == 0
        env = {**os.environ, "AGENT_RESULT_CACHE_DISABLE": "1"}
        r = subprocess.run([sys.executable, str(HOOK), "pre"],
                           input=json.dumps(_payload(prompt)),
                           capture_output=True, text=True, cwd=str(fx), env=env)
        assert r.returncode == 0


if __name__ == "__main__":
    test_cache_roundtrip_and_invalidation()
    test_non_cacheable_agent_passthrough()
    test_subagentstop_store_then_pre_hit()
    test_kill_switch_and_tiny_reports()
    print("PASS: agent-result cache")
